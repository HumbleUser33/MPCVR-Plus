// Exercises the actual filter code -- Source/DLSS/DlssNR.cpp and the real
// Tex2D_t from Source/DX11Helper.h -- against a real D3D11 device, outside
// MPC-BE.
//
// The point is that every bug that reached the player so far would have been
// caught here in seconds: the command-allocator reset that removed the device,
// the texture type that CheckCreate ignored, the fence deadlock on teardown.
// Nothing in this file is a mock. If a sequence survives here it should survive
// in the renderer.
//
// --temporal runs a different suite: it measures what motion vectors and the
// other guide inputs change in the network's output over time, on synthetic
// sequences whose true motion is known exactly. --tmask runs the same
// measurements across ControlMask values.

#include "stdafx.h"
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <DirectXPackedVector.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>
#include "Helper.h"        // DX11Helper.h needs the plane-config types
#include "DX11Helper.h"
#include "DLSS/DlssNR.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// DX11Helper.cpp carries a debug dump helper we never call. Stubbing it beats
// compiling Helper.cpp, which would drag in DirectShow, WIC and the CPU feature
// detection for no benefit here.
HRESULT SaveToBMP(BYTE* src, UINT src_pitch, UINT width, UINT height, UINT bitdepth, const wchar_t* filename)
{
	UNREFERENCED_PARAMETER(src); UNREFERENCED_PARAMETER(src_pitch);
	UNREFERENCED_PARAMETER(width); UNREFERENCED_PARAMETER(height);
	UNREFERENCED_PARAMETER(bitdepth); UNREFERENCED_PARAMETER(filename);
	return E_NOTIMPL;
}

static int g_failures = 0;

static void Head(const char* s) { printf("\n=== %s ===\n", s); }

static void Check(bool ok, const char* what)
{
	printf("  [%s] %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		g_failures++;
	}
}

static void Dump(const CDlssNR& dlss)
{
	const std::wstring info = dlss.GetInfoBlock();
	printf("%S", info.c_str());
}

// A D3D11 write into the shared input, the way the renderer's blit would be.
static void FillInput(ID3D11Device* dev, ID3D11DeviceContext* ctx,
                      ID3D11Texture2D* tex, float phase)
{
	CComPtr<ID3D11RenderTargetView> rtv;
	if (SUCCEEDED(dev->CreateRenderTargetView(tex, nullptr, &rtv))) {
		const FLOAT c[4] = { 0.25f + 0.5f * phase, 0.5f, 0.75f - 0.5f * phase, 1.0f };
		ctx->ClearRenderTargetView(rtv, c);
	}
}

// Did the network actually write something?
static bool OutputIsNonZero(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex)
{
	D3D11_TEXTURE2D_DESC d = {};
	tex->GetDesc(&d);
	d.Usage = D3D11_USAGE_STAGING;
	d.BindFlags = 0;
	d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	d.MiscFlags = 0;

	CComPtr<ID3D11Texture2D> stage;
	if (FAILED(dev->CreateTexture2D(&d, nullptr, &stage))) {
		return false;
	}
	ctx->CopyResource(stage, tex);

	D3D11_MAPPED_SUBRESOURCE mr = {};
	if (FAILED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &mr))) {
		return false;
	}
	const uint16_t* px = (const uint16_t*)mr.pData;
	bool nonZero = false;
	for (int i = 0; i < 256 && !nonZero; i++) {
		if (px[i]) nonZero = true;
	}
	ctx->Unmap(stage, 0);
	return nonZero;
}

// Exactly the sizing and texture type UpdateTexures() uses in the filter.
static bool MakeSharedPair(ID3D11Device* dev, Tex2D_t& in, Tex2D_t& out, UINT w, UINT h)
{
	HRESULT hr = in.CheckCreate(dev, DXGI_FORMAT_R16G16B16A16_FLOAT, w, h, Tex2D_DefaultShaderRTargetUAVShared);
	if (SUCCEEDED(hr)) {
		hr = out.CheckCreate(dev, DXGI_FORMAT_R16G16B16A16_FLOAT, w, h, Tex2D_DefaultShaderRTargetUAVShared);
	}
	return SUCCEEDED(hr) && in.pTexture && out.pTexture;
}

// ============================================================================
// Temporal suites (--temporal, --tmask)
// ============================================================================
// Synthetic content on purpose: the true motion of every pixel is known, so a
// warp by it is the ground truth any frame-to-frame difference is measured
// against. What is left after that warp is flicker -- in the input it is the
// noise we put there, in the output it is that noise plus whatever the network
// adds. The low-frequency version of the same measure is the shimmer itself.
//
// First sweep, measured 2026-09-13 on an RTX 3050 at default strengths:
//   - motion vectors are read as pixels, current -> previous, scale 1.0: that
//     convention alone lowered low-frequency flicker on noisy motion (1.06x ->
//     0.96x of the input); the opposite sign and normalised vectors did not
//   - they barely help overall, and slightly worsen a clean pan
//   - history without vectors measures identical to no history at all
//   - Depth changes nothing
//   - ControlMask 0 freezes the output on the previous one, apparently without
//     reprojection: perfectly stable on static content, smeared on motion

namespace temporal {

using DirectX::PackedVector::HALF;

struct Rng {
	uint32_t s;
	explicit Rng(uint32_t seed) : s(seed ? seed : 0x9E3779B9u) {}
	uint32_t Next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
	float Uniform() { return (Next() >> 8) * (1.0f / 16777216.0f); }
	float Gauss() {
		const float u1 = std::max(Uniform(), 1e-7f);
		const float u2 = Uniform();
		return std::sqrt(-2.0f * std::log(u1)) * std::cos(6.2831853f * u2);
	}
};

static float Hash(int x, int y, uint32_t seed)
{
	uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return ((h ^ (h >> 16)) & 0xFFFFFF) / 16777215.0f;
}

// Value noise on a lattice that wraps at the image size: the picture tiles, so
// a pan never reaches an edge. Cell sizes must divide both dimensions.
static float Noise(int x, int y, int cell, int W, int H, uint32_t seed)
{
	const int px = W / cell, py = H / cell;
	const int ix = x / cell, iy = y / cell;
	float tx = (float)(x % cell) / cell, ty = (float)(y % cell) / cell;
	tx = tx * tx * (3 - 2 * tx);
	ty = ty * ty * (3 - 2 * ty);
	auto L = [&](int a, int b) { return Hash(a % px, b % py, seed); };
	const float v00 = L(ix, iy), v10 = L(ix + 1, iy), v01 = L(ix, iy + 1), v11 = L(ix + 1, iy + 1);
	return (v00 * (1 - tx) + v10 * tx) * (1 - ty) + (v01 * (1 - tx) + v11 * tx) * ty;
}

struct Image {
	int W = 0, H = 0;
	std::vector<float> rgb;
	float* At(int x, int y) { return &rgb[3 * ((size_t)y * W + x)]; }
	const float* At(int x, int y) const { return &rgb[3 * ((size_t)y * W + x)]; }
};

// Soft texture at several scales, plus hard rectangles and thin lines: the
// edges are where history dragged along the wrong way shows up as ghosts.
static Image MakeBase(int W, int H, uint32_t seed)
{
	Image img;
	img.W = W;
	img.H = H;
	img.rgb.resize(3 * (size_t)W * H);
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			const float L = 0.18f + 0.45f * Noise(x, y, 120, W, H, seed)
			              + 0.18f * Noise(x, y, 40, W, H, seed + 1)
			              + 0.10f * Noise(x, y, 12, W, H, seed + 2)
			              + 0.06f * Noise(x, y, 4, W, H, seed + 3);
			float* p = img.At(x, y);
			p[0] = L * (0.90f + 0.20f * Noise(x, y, 120, W, H, seed + 4));
			p[1] = L * (0.95f + 0.10f * Noise(x, y, 120, W, H, seed + 5));
			p[2] = L * (0.85f + 0.30f * Noise(x, y, 120, W, H, seed + 6));
		}
	}

	Rng r(seed ^ 0xA5A5A5A5u);
	for (int i = 0; i < 48; i++) {
		const int x0 = (int)(r.Uniform() * W), y0 = (int)(r.Uniform() * H);
		const int w = 12 + (int)(r.Uniform() * 180), h = 8 + (int)(r.Uniform() * 120);
		const float d = (r.Uniform() - 0.5f) * 0.5f;
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				float* p = img.At((x0 + x) % W, (y0 + y) % H);
				p[0] += d; p[1] += d; p[2] += d;
			}
		}
	}
	for (int i = 0; i < 40; i++) {
		const int x0 = (int)(r.Uniform() * W), y0 = (int)(r.Uniform() * H);
		const int len = 60 + (int)(r.Uniform() * 400);
		const bool horiz = r.Uniform() < 0.5f;
		const float d = r.Uniform() < 0.5f ? -0.35f : 0.35f;
		for (int k = 0; k < len; k++) {
			float* p = horiz ? img.At((x0 + k) % W, y0) : img.At(x0, (y0 + k) % H);
			p[0] += d; p[1] += d; p[2] += d;
		}
	}
	for (float& v : img.rgb) {
		v = std::clamp(v, 0.02f, 0.98f);
	}
	return img;
}

struct Sequence {
	const char* name;
	float vx, vy;   // content motion, pixels per frame
	bool noise;     // block DC offsets, grain and coarse quantisation per frame
};

// Frame t: the base shifted by t*v with bilinear sampling, and when asked for,
// the frame-to-frame variation a low bitrate adds: an offset per 8x8 block,
// grain, and quantisation to 128 levels.
static void RenderFrame(const Image& base, const Sequence& s, int t, std::vector<float>& rgba)
{
	const int W = base.W, H = base.H;
	const float sx = s.vx * t, sy = s.vy * t;
	const int bw = (W + 7) / 8;
	Rng r(0x1234567u + (uint32_t)t * 7919u);
	std::vector<float> blockDc;
	if (s.noise) {
		blockDc.resize((size_t)bw * ((H + 7) / 8));
		for (float& d : blockDc) {
			d = (r.Uniform() * 2 - 1) * (1.5f / 255);
		}
	}

	for (int y = 0; y < H; y++) {
		const float fy = y - sy;
		const float fyf = std::floor(fy);
		const int y0 = (((int)fyf % H) + H) % H;
		const int y1 = (y0 + 1) % H;
		const float ty = fy - fyf;
		for (int x = 0; x < W; x++) {
			const float fx = x - sx;
			const float fxf = std::floor(fx);
			const int x0 = (((int)fxf % W) + W) % W;
			const int x1 = (x0 + 1) % W;
			const float tx = fx - fxf;
			const float* p00 = base.At(x0, y0);
			const float* p10 = base.At(x1, y0);
			const float* p01 = base.At(x0, y1);
			const float* p11 = base.At(x1, y1);
			float* o = &rgba[4 * ((size_t)y * W + x)];
			for (int c = 0; c < 3; c++) {
				o[c] = (p00[c] * (1 - tx) + p10[c] * tx) * (1 - ty) + (p01[c] * (1 - tx) + p11[c] * tx) * ty;
			}
			if (s.noise) {
				const float n = blockDc[(size_t)(y / 8) * bw + x / 8] + r.Gauss() * (1.5f / 255);
				for (int c = 0; c < 3; c++) {
					o[c] = std::round(std::clamp(o[c] + n, 0.0f, 1.0f) * 127.5f) / 127.5f;
				}
			}
			o[3] = 1.0f;
		}
	}
}

static inline float Luma(const float* p) { return 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]; }

// Luma at half resolution, box filtered -- enough for flicker, four times less work.
static void LumaHalfFromFloat(const std::vector<float>& rgba, int W, int H, std::vector<float>& out)
{
	const int w2 = W / 2, h2 = H / 2;
	out.resize((size_t)w2 * h2);
	for (int y = 0; y < h2; y++) {
		for (int x = 0; x < w2; x++) {
			const float s = Luma(&rgba[4 * ((size_t)(2 * y) * W + 2 * x)])
			              + Luma(&rgba[4 * ((size_t)(2 * y) * W + 2 * x + 1)])
			              + Luma(&rgba[4 * ((size_t)(2 * y + 1) * W + 2 * x)])
			              + Luma(&rgba[4 * ((size_t)(2 * y + 1) * W + 2 * x + 1)]);
			out[(size_t)y * w2 + x] = s * 0.25f;
		}
	}
}

static bool ReadLumaHalf(ID3D11DeviceContext* ctx, ID3D11Texture2D* stage, int W, int H, std::vector<float>& out)
{
	D3D11_MAPPED_SUBRESOURCE mr = {};
	if (FAILED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &mr))) {
		return false;
	}
	const int w2 = W / 2, h2 = H / 2;
	out.resize((size_t)w2 * h2);
	std::vector<float> row0(4 * (size_t)W), row1(4 * (size_t)W);
	for (int y = 0; y < h2; y++) {
		const HALF* r0 = (const HALF*)((const BYTE*)mr.pData + (size_t)mr.RowPitch * (2 * y));
		const HALF* r1 = (const HALF*)((const BYTE*)mr.pData + (size_t)mr.RowPitch * (2 * y + 1));
		DirectX::PackedVector::XMConvertHalfToFloatStream(row0.data(), sizeof(float), r0, sizeof(HALF), 4 * (size_t)W);
		DirectX::PackedVector::XMConvertHalfToFloatStream(row1.data(), sizeof(float), r1, sizeof(HALF), 4 * (size_t)W);
		for (int x = 0; x < w2; x++) {
			const float s = Luma(&row0[4 * (size_t)(2 * x)]) + Luma(&row0[4 * (size_t)(2 * x + 1)])
			              + Luma(&row1[4 * (size_t)(2 * x)]) + Luma(&row1[4 * (size_t)(2 * x + 1)]);
			out[(size_t)y * w2 + x] = s * 0.25f;
		}
	}
	ctx->Unmap(stage, 0);
	return true;
}

// Three box passes approximate a Gaussian; radius 4 at half resolution is a
// sigma of roughly 8 source pixels.
static void Blur(const std::vector<float>& in, int w, int h, int r, std::vector<float>& out)
{
	std::vector<float> a = in, b(in.size());
	for (int pass = 0; pass < 3; pass++) {
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				float s = 0;
				for (int k = -r; k <= r; k++) {
					s += a[(size_t)y * w + std::clamp(x + k, 0, w - 1)];
				}
				b[(size_t)y * w + x] = s / (2 * r + 1);
			}
		}
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				float s = 0;
				for (int k = -r; k <= r; k++) {
					s += b[(size_t)std::clamp(y + k, 0, h - 1) * w + x];
				}
				a[(size_t)y * w + x] = s / (2 * r + 1);
			}
		}
	}
	out.swap(a);
}

static float SampleClamp(const std::vector<float>& img, int w, int h, float x, float y)
{
	x = std::clamp(x, 0.0f, (float)(w - 1));
	y = std::clamp(y, 0.0f, (float)(h - 1));
	const int x0 = (int)x, y0 = (int)y;
	const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
	const float tx = x - x0, ty = y - y0;
	const float a = img[(size_t)y0 * w + x0] * (1 - tx) + img[(size_t)y0 * w + x1] * tx;
	const float b = img[(size_t)y1 * w + x0] * (1 - tx) + img[(size_t)y1 * w + x1] * tx;
	return a * (1 - ty) + b * ty;
}

// Mean |cur - prev warped by the true motion| over [x0, x1) x rows, margins excluded.
static double WarpDiff(const std::vector<float>& cur, const std::vector<float>& prev, int w, int h,
                       float dx, float dy, int margin, int x0, int x1)
{
	double s = 0;
	long long n = 0;
	const int xa = std::max(margin, x0), xb = std::min(w - margin, x1);
	for (int y = margin; y < h - margin; y++) {
		for (int x = xa; x < xb; x++) {
			s += std::abs(cur[(size_t)y * w + x] - SampleClamp(prev, w, h, x - dx, y - dy));
			n++;
		}
	}
	return n ? s / n : 0;
}

static double Mean(const std::vector<float>& v)
{
	double s = 0;
	for (float f : v) s += f;
	return v.empty() ? 0 : s / v.size();
}

enum MaskMode { MaskNone, MaskLeftHalf, MaskUniform };

struct Config {
	const char* name;
	bool  noHistory;
	int   mv;         // 0 none, 1 current->previous in pixels, 2 the opposite sign, 3 like 1 but scaled to [0,1] by MVecScale
	bool  depth;      // constant depth 0.5
	int   maskMode;   // MaskMode
	float maskValue;  // for MaskUniform
};

static FILE* g_report = nullptr;

static void Out(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	if (g_report) {
		va_start(ap, fmt);
		vfprintf(g_report, fmt, ap);
		va_end(ap);
	}
}

// The first config of either table must be "no history": the input metrics and
// the per-frame reference are taken during that run.
static const Config s_Sweep[] = {
	{ "no history",               true,  0, false, MaskNone,     0.0f },
	{ "history, no MV (today)",   false, 0, false, MaskNone,     0.0f },
	{ "MV px, current->previous", false, 1, false, MaskNone,     0.0f },
	{ "MV px, inverted sign",     false, 2, false, MaskNone,     0.0f },
	{ "MV normalised by scale",   false, 3, false, MaskNone,     0.0f },
	{ "MV px + depth 0.5",        false, 1, true,  MaskNone,     0.0f },
	{ "MV px + ControlMask left", false, 1, false, MaskLeftHalf, 0.0f },
};

// Is ControlMask a continuous weight, and is the history it keeps reprojected
// by the motion vectors? On a pan, reprojected history tracks the motion and
// stays low on flicker; unreprojected history smears.
static const Config s_MaskSweep[] = {
	{ "no history",               true,  0, false, MaskNone,    0.0f  },
	{ "mask 1.00, MV px",         false, 1, false, MaskUniform, 1.00f },
	{ "mask 0.75, MV px",         false, 1, false, MaskUniform, 0.75f },
	{ "mask 0.50, MV px",         false, 1, false, MaskUniform, 0.50f },
	{ "mask 0.25, MV px",         false, 1, false, MaskUniform, 0.25f },
	{ "mask 0.00, MV px",         false, 1, false, MaskUniform, 0.00f },
	{ "mask 0.50, no MV",         false, 0, false, MaskUniform, 0.50f },
	{ "mask 0.50, MV inverted",   false, 2, false, MaskUniform, 0.50f },
	{ "mask 0.00, no MV",         false, 0, false, MaskUniform, 0.00f },
	{ "mask 0.00, MV inverted",   false, 2, false, MaskUniform, 0.00f },
};

static int Run(ID3D11Device* dev, ID3D11DeviceContext* ctx, CDlssNR& dlss, int frames, bool strong, bool maskSuite)
{
	const int W = 1920, H = 1080;
	const int w2 = W / 2, h2 = H / 2;
	const int gw = w2 / 4, gh = h2 / 4;   // 1/8 resolution, for the per-frame reference
	const int warm = 8;

	static const Sequence seqs[] = {
		{ "S1 pan (4, 1.5) px/frame, clean",  4.0f, 1.5f, false },
		{ "S2 static, compression noise",     0.0f, 0.0f, true  },
		{ "S3 pan (2.5, 0) px/frame + noise", 2.5f, 0.0f, true  },
	};
	const Config* cfgs = maskSuite ? s_MaskSweep : s_Sweep;
	const size_t cfgCount = maskSuite ? std::size(s_MaskSweep) : std::size(s_Sweep);

	Head(maskSuite ? "Temporal suite: ControlMask" : "Temporal suite");
	Tex2D_t texIn, texOut, texMV, texDepth, texMask;
	Check(MakeSharedPair(dev, texIn, texOut, W, H), "colour pair 1920x1080 RGBA16F");
	Check(SUCCEEDED(texMV.CheckCreate(dev, DXGI_FORMAT_R16G16_FLOAT, W, H, Tex2D_DefaultShaderRTargetUAVShared)), "MVec RG16F");
	Check(SUCCEEDED(texDepth.CheckCreate(dev, DXGI_FORMAT_R32_FLOAT, W, H, Tex2D_DefaultShaderRTargetUAVShared)), "Depth R32F");
	Check(SUCCEEDED(texMask.CheckCreate(dev, DXGI_FORMAT_R8_UNORM, W, H, Tex2D_DefaultShaderRTargetUAVShared)), "ControlMask R8");
	if (!texIn.pTexture || !texOut.pTexture || !texMV.pTexture || !texDepth.pTexture || !texMask.pTexture) {
		return 1;
	}

	D3D11_TEXTURE2D_DESC sd = texOut.desc;
	sd.Usage = D3D11_USAGE_STAGING;
	sd.BindFlags = 0;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	sd.MiscFlags = 0;
	CComPtr<ID3D11Texture2D> stage;
	Check(SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &stage)), "readback staging");
	if (!stage) {
		return 1;
	}

	auto Clear = [&](ID3D11Texture2D* tex, float r, float g) {
		CComPtr<ID3D11RenderTargetView> rtv;
		if (SUCCEEDED(dev->CreateRenderTargetView(tex, nullptr, &rtv))) {
			const FLOAT c[4] = { r, g, 0, 0 };
			ctx->ClearRenderTargetView(rtv, c);
		}
	};
	Clear(texDepth.pTexture, 0.5f, 0.0f);

	CDlssNR::Params params;
	params.iStyle = 0;
	params.iPreset = 0;
	params.fIntensity = 1.50f;
	params.fLocalTone = strong ? 1.0f : 0.30f;
	params.fLocalStructure = strong ? 1.0f : 0.50f;
	params.fSkinStructure = 0.90f;
	params.bUseAutoMask = true;

	printf("  building the base texture...\n");
	const Image base = MakeBase(W, H, 20260913u);

	g_report = fopen(maskSuite ? "temporal_results_mask.txt" : "temporal_results.txt", "w");
	Out("\nDLSS 5 NR temporal measurements%s -- %d frames per run, first %d skipped, %s strengths\n",
	    maskSuite ? " (ControlMask)" : "", frames, warm,
	    strong ? "strong (tone 1.00, structure 1.00)" : "default (tone 0.30, structure 0.50)");
	Out("flicker = mean |Y(t) - warp(Y(t-1))| on luma; LF = the same on luma blurred (sigma ~8 px);\n");
	Out("ratios are output / input. dev = mean |Y - Y(no history)| at 1/8 resolution.\n");

	std::vector<float> rgba(4 * (size_t)W * H);
	std::vector<HALF> half(4 * (size_t)W * H);
	std::vector<uint8_t> maskBytes((size_t)W * H);
	std::vector<float> yIn, yOut, yInPrev, yOutPrev, bIn, bOut, bInPrev, bOutPrev;

	for (const Sequence& s : seqs) {
		const float dx = s.vx / 2, dy = s.vy / 2;   // motion at half resolution
		const int margin = 16 + (int)std::ceil(std::max(std::abs(dx), std::abs(dy))) + 12;
		std::vector<float> reference;               // per-frame output of "no history", 1/8 resolution
		double inFlicker = 0, inLF = 0;
		int inCount = 0;

		Out("\n%s\n", s.name);
		Out("  %-26s %9s %9s %9s %9s %8s %8s %7s\n",
		    "config", "flicker", "ratio", "LF", "LF ratio", "dev", "tone", "ms");

		for (size_t ci = 0; ci < cfgCount; ci++) {
			const Config& c = cfgs[ci];
			const bool isReference = (ci == 0);
			dlss.ReleaseFeature();

			CDlssNR::Guides g;
			if (c.mv) {
				const float sign = (c.mv == 2) ? 1.0f : -1.0f;
				Clear(texMV.pTexture, sign * s.vx, sign * s.vy);
				g.pMVec = texMV.pTexture;
				if (c.mv == 3) {
					g.fMVecScaleX = 1.0f / W;
					g.fMVecScaleY = 1.0f / H;
				}
			}
			if (c.depth) {
				g.pDepth = texDepth.pTexture;
			}
			if (c.maskMode != MaskNone) {
				if (c.maskMode == MaskLeftHalf) {
					std::fill(maskBytes.begin(), maskBytes.end(), (uint8_t)0);
					for (int y = 0; y < H; y++) {
						memset(&maskBytes[(size_t)y * W], 255, W / 2);
					}
				} else {
					std::fill(maskBytes.begin(), maskBytes.end(),
					          (uint8_t)std::lround(std::clamp(c.maskValue, 0.0f, 1.0f) * 255));
				}
				ctx->UpdateSubresource(texMask.pTexture, 0, nullptr, maskBytes.data(), W, 0);
				g.pControlMask = texMask.pTexture;
			}
			const bool guidesOk = dlss.SetGuides(g);
			const bool featureOk = guidesOk && dlss.CreateFeature(texIn.pTexture, texOut.pTexture, W, H, params);
			if (!guidesOk || !featureOk) {
				Out("  %-26s could not set up: %S\n", c.name, dlss.GetStatusLine().c_str());
				g_failures++;
				continue;
			}
			dlss.RequestReset();
			params.bNoHistory = c.noHistory;

			double flicker = 0, lf = 0, lfLeft = 0, lfRight = 0, dev = 0, tone = 0, ms = 0;
			int count = 0, failed = 0;
			LARGE_INTEGER freq, t0, t1;
			QueryPerformanceFrequency(&freq);

			for (int t = 0; t < frames; t++) {
				RenderFrame(base, s, t, rgba);
				DirectX::PackedVector::XMConvertFloatToHalfStream(half.data(), sizeof(HALF), rgba.data(), sizeof(float), rgba.size());
				ctx->UpdateSubresource(texIn.pTexture, 0, nullptr, half.data(), 4 * sizeof(HALF) * W, 0);

				QueryPerformanceCounter(&t0);
				const bool ok = dlss.Evaluate(params);
				QueryPerformanceCounter(&t1);
				if (!ok) {
					if (++failed <= 2) {
						Out("  %-26s frame %d: Evaluate failed -- %S\n", c.name, t, dlss.GetStatusLine().c_str());
					}
					continue;
				}
				ctx->CopyResource(stage, texOut.pTexture);
				if (!ReadLumaHalf(ctx, stage, W, H, yOut)) {
					failed++;
					continue;
				}
				LumaHalfFromFloat(rgba, W, H, yIn);
				Blur(yOut, w2, h2, 4, bOut);

				// 1/8 resolution copy, against the no-history reference
				std::vector<float> lowres((size_t)gw * gh);
				for (int y = 0; y < gh; y++) {
					for (int x = 0; x < gw; x++) {
						float acc = 0;
						for (int yy = 0; yy < 4; yy++) {
							for (int xx = 0; xx < 4; xx++) {
								acc += yOut[(size_t)(y * 4 + yy) * w2 + x * 4 + xx];
							}
						}
						lowres[(size_t)y * gw + x] = acc / 16;
					}
				}

				if (t >= warm) {
					ms += 1000.0 * (t1.QuadPart - t0.QuadPart) / freq.QuadPart;
					flicker += WarpDiff(yOut, yOutPrev, w2, h2, dx, dy, margin, 0, w2);
					lf += WarpDiff(bOut, bOutPrev, w2, h2, dx, dy, margin, 0, w2);
					lfLeft += WarpDiff(bOut, bOutPrev, w2, h2, dx, dy, margin, 0, w2 / 2 - 32);
					lfRight += WarpDiff(bOut, bOutPrev, w2, h2, dx, dy, margin, w2 / 2 + 32, w2);
					tone += Mean(yOut) - Mean(yIn);
					if (isReference) {
						Blur(yIn, w2, h2, 4, bIn);
						inFlicker += WarpDiff(yIn, yInPrev, w2, h2, dx, dy, margin, 0, w2);
						inLF += WarpDiff(bIn, bInPrev, w2, h2, dx, dy, margin, 0, w2);
						inCount++;
						reference.insert(reference.end(), lowres.begin(), lowres.end());
					} else if (reference.size() >= (size_t)(count + 1) * gw * gh) {
						double d = 0;
						const float* ref = &reference[(size_t)count * gw * gh];
						for (size_t i = 0; i < lowres.size(); i++) {
							d += std::abs(lowres[i] - ref[i]);
						}
						dev += d / lowres.size();
					}
					count++;
				} else if (isReference) {
					Blur(yIn, w2, h2, 4, bIn);
				}

				yOutPrev.swap(yOut);
				bOutPrev.swap(bOut);
				if (isReference) {
					yInPrev.swap(yIn);
					bInPrev.swap(bIn);
				}
			}

			if (!count) {
				Out("  %-26s no usable frames (%d failed)\n", c.name, failed);
				g_failures++;
				continue;
			}
			const double inF = inCount ? inFlicker / inCount : 0;
			const double inL = inCount ? inLF / inCount : 0;
			const double f = flicker / count, l = lf / count;
			Out("  %-26s %9.5f %8.2fx %9.5f %8.2fx %8.5f %+8.4f %7.1f",
			    c.name, f, inF > 0 ? f / inF : 0.0, l, inL > 0 ? l / inL : 0.0,
			    isReference ? 0.0 : dev / count, tone / count, ms / count);
			if (c.maskMode == MaskLeftHalf) {
				Out("   LF left %.5f / right %.5f", lfLeft / count, lfRight / count);
			}
			if (failed) {
				Out("   (%d failed)", failed);
			}
			Out("\n");
		}
		Out("  %-26s %9.5f %9s %9.5f\n", "(input)", inCount ? inFlicker / inCount : 0.0, "", inCount ? inLF / inCount : 0.0);
	}

	dlss.SetGuides(CDlssNR::Guides{});
	if (g_report) {
		fclose(g_report);
		g_report = nullptr;
		printf("\n  written to %s\n", maskSuite ? "temporal_results_mask.txt" : "temporal_results.txt");
	}
	return g_failures ? 1 : 0;
}

// ----------------------------------------------------------------------------
// --toracle: what ControlMask can do with a perfect map of what moves
// ----------------------------------------------------------------------------
// The ControlMask sweep showed the mask is a per-pixel history weight -- on
// static content 0.5 cuts flicker by a third, 0.25 by two thirds, 0 freezes it,
// and anything from 0.75 up measures the same as 1 -- and that the history it
// keeps is not reprojected by the motion vectors, so lowering it where things
// move smears them. The obvious use is a mask that is low only where nothing
// moves. Before building a detector, this measures the best case: a noisy static
// background with a textured object crossing it, and masks drawn from the
// object's true position.

struct OracleSequence {
	const char* name;
	float ovx, ovy;      // object motion, pixels per frame
};

enum OracleMask { OracleNone, OracleUniform, OracleObject, OracleObjectAndTrail };

struct OracleConfig {
	const char* name;
	bool  noHistory;
	int   maskMode;      // OracleMask
	float value;         // uniform value, or the background value
};

struct Rect { int x0, y0, x1, y1; };   // half-open

static Rect Expand(const Rect& r, int d, int w, int h)
{
	return { std::max(r.x0 - d, 0), std::max(r.y0 - d, 0), std::min(r.x1 + d, w), std::min(r.y1 + d, h) };
}

static Rect Union(const Rect& a, const Rect& b)
{
	return { std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1) };
}

static Rect Scale(const Rect& r, int div)
{
	return { r.x0 / div, r.y0 / div, (r.x1 + div - 1) / div, (r.y1 + div - 1) / div };
}

static bool Inside(const Rect& r, int x, int y) { return x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1; }

// Static background with a window of a second texture pasted at (ox, oy), then
// the same per-frame compression noise over everything.
static void RenderOracleFrame(const Image& bg, const Image& obj, float ox, float oy, int ow, int oh,
                              int t, std::vector<float>& rgba)
{
	const int W = bg.W, H = bg.H;
	const int bw = (W + 7) / 8;
	Rng r(0x7654321u + (uint32_t)t * 7919u);
	std::vector<float> blockDc((size_t)bw * ((H + 7) / 8));
	for (float& d : blockDc) {
		d = (r.Uniform() * 2 - 1) * (1.5f / 255);
	}

	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			float* o = &rgba[4 * ((size_t)y * W + x)];
			const float lx = x - ox, ly = y - oy;
			if (lx >= 0 && ly >= 0 && lx < ow - 1 && ly < oh - 1) {
				const int x0 = (int)lx, y0 = (int)ly;
				const float tx = lx - x0, ty = ly - y0;
				const float* p00 = obj.At(x0, y0);
				const float* p10 = obj.At(x0 + 1, y0);
				const float* p01 = obj.At(x0, y0 + 1);
				const float* p11 = obj.At(x0 + 1, y0 + 1);
				for (int c = 0; c < 3; c++) {
					o[c] = (p00[c] * (1 - tx) + p10[c] * tx) * (1 - ty) + (p01[c] * (1 - tx) + p11[c] * tx) * ty;
				}
			} else {
				const float* p = bg.At(x, y);
				o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
			}
			const float n = blockDc[(size_t)(y / 8) * bw + x / 8] + r.Gauss() * (1.5f / 255);
			for (int c = 0; c < 3; c++) {
				o[c] = std::round(std::clamp(o[c] + n, 0.0f, 1.0f) * 127.5f) / 127.5f;
			}
			o[3] = 1.0f;
		}
	}
}

// Mean |cur - prev| outside a rectangle, margins excluded. The background is
// static, so no warp.
static double OutsideDiff(const std::vector<float>& cur, const std::vector<float>& prev, int w, int h,
                          const Rect& excluded, int margin)
{
	double s = 0;
	long long n = 0;
	for (int y = margin; y < h - margin; y++) {
		for (int x = margin; x < w - margin; x++) {
			if (!Inside(excluded, x, y)) {
				s += std::abs(cur[(size_t)y * w + x] - prev[(size_t)y * w + x]);
				n++;
			}
		}
	}
	return n ? s / n : 0;
}

static int RunOracle(ID3D11Device* dev, ID3D11DeviceContext* ctx, CDlssNR& dlss, int frames, bool strong)
{
	const int W = 1920, H = 1080;
	const int w2 = W / 2, h2 = H / 2;
	const int gw = w2 / 4, gh = h2 / 4;   // 1/8 resolution, for the per-frame reference
	const int warm = 8;
	const int trailFrames = 8;
	const int OW = 480, OH = 320;          // object size, source pixels
	const float OX = 200, OY = 300;        // object position at frame 0

	static const OracleSequence seqs[] = {
		{ "S4 noisy static background, object moving (3, 1) px/frame",  3.0f, 1.0f },
		{ "S5 noisy static background, object moving (0.5, 0) px/frame", 0.5f, 0.0f },
	};
	static const OracleConfig cfgs[] = {
		{ "no history",                true,  OracleNone,           0.0f  },
		{ "history, no mask (today)",  false, OracleNone,           0.0f  },
		{ "mask 1.00 everywhere",      false, OracleUniform,        1.00f },
		{ "mask 0.60 everywhere",      false, OracleUniform,        0.60f },
		{ "mask 0.40 everywhere",      false, OracleUniform,        0.40f },
		{ "bg 0.50, object 1",         false, OracleObject,         0.50f },
		{ "bg 0.25, object 1",         false, OracleObject,         0.25f },
		{ "bg 0.25, object+trail 1",   false, OracleObjectAndTrail, 0.25f },
		{ "bg 0.00, object+trail 1",   false, OracleObjectAndTrail, 0.00f },
	};

	Head("Temporal suite: ControlMask with a perfect motion map");
	Tex2D_t texIn, texOut, texMask;
	Check(MakeSharedPair(dev, texIn, texOut, W, H), "colour pair 1920x1080 RGBA16F");
	Check(SUCCEEDED(texMask.CheckCreate(dev, DXGI_FORMAT_R8_UNORM, W, H, Tex2D_DefaultShaderRTargetUAVShared)), "ControlMask R8");
	if (!texIn.pTexture || !texOut.pTexture || !texMask.pTexture) {
		return 1;
	}

	D3D11_TEXTURE2D_DESC sd = texOut.desc;
	sd.Usage = D3D11_USAGE_STAGING;
	sd.BindFlags = 0;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	sd.MiscFlags = 0;
	CComPtr<ID3D11Texture2D> stage;
	Check(SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &stage)), "readback staging");
	if (!stage) {
		return 1;
	}

	CDlssNR::Params params;
	params.iStyle = 0;
	params.iPreset = 0;
	params.fIntensity = 1.50f;
	params.fLocalTone = strong ? 1.0f : 0.30f;
	params.fLocalStructure = strong ? 1.0f : 0.50f;
	params.fSkinStructure = 0.90f;
	params.bUseAutoMask = true;

	printf("  building the textures...\n");
	const Image bgImage = MakeBase(W, H, 20260913u);
	const Image objImage = MakeBase(W, H, 777u);   // only a window of it is used

	g_report = fopen("temporal_results_oracle.txt", "w");
	Out("\nDLSS 5 NR, ControlMask with a perfect motion map -- %d frames per run, first %d skipped, %s strengths\n",
	    frames, warm, strong ? "strong (tone 1.00, structure 1.00)" : "default (tone 0.30, structure 0.50)");
	Out("bg LF = mean |Y(t) - Y(t-1)| on blurred luma, background only (the shimmer on static areas);\n");
	Out("dev = mean |Y - Y(no history)| at 1/8 resolution on the object, on its trail (the last %d frames), and\n", trailFrames);
	Out("on the background -- on the object and the trail it is smear.\n");

	std::vector<float> rgba(4 * (size_t)W * H);
	std::vector<HALF> half(4 * (size_t)W * H);
	std::vector<uint8_t> maskBytes((size_t)W * H);
	std::vector<float> yIn, yOut, yInPrev, yOutPrev, bIn, bOut, bInPrev, bOutPrev;
	std::vector<float> lowres((size_t)gw * gh);

	for (const OracleSequence& s : seqs) {
		std::vector<float> reference;
		double inBgLF = 0;
		int inCount = 0;

		Out("\n%s\n", s.name);
		Out("  %-26s %9s %9s %9s %9s %9s %7s\n", "config", "bg LF", "bg ratio", "obj dev", "trail dev", "bg dev", "ms");

		for (size_t ci = 0; ci < std::size(cfgs); ci++) {
			const OracleConfig& c = cfgs[ci];
			const bool isReference = (ci == 0);
			dlss.ReleaseFeature();

			CDlssNR::Guides g;
			if (c.maskMode != OracleNone) {
				g.pControlMask = texMask.pTexture;
			}
			const bool guidesOk = dlss.SetGuides(g);
			const bool featureOk = guidesOk && dlss.CreateFeature(texIn.pTexture, texOut.pTexture, W, H, params);
			if (!guidesOk || !featureOk) {
				Out("  %-26s could not set up: %S\n", c.name, dlss.GetStatusLine().c_str());
				g_failures++;
				continue;
			}
			dlss.RequestReset();
			params.bNoHistory = c.noHistory;

			double bgLF = 0, objDev = 0, trailDev = 0, bgDev = 0, ms = 0;
			int count = 0, failed = 0;
			LARGE_INTEGER freq, t0, t1;
			QueryPerformanceFrequency(&freq);

			for (int t = 0; t < frames; t++) {
				const float ox = OX + s.ovx * t, oy = OY + s.ovy * t;
				const int tp = std::max(t - trailFrames, 0);
				const float oxp = OX + s.ovx * tp, oyp = OY + s.ovy * tp;
				const Rect objFull = { (int)std::floor(ox), (int)std::floor(oy), (int)std::ceil(ox) + OW, (int)std::ceil(oy) + OH };
				const Rect prevFull = { (int)std::floor(oxp), (int)std::floor(oyp), (int)std::ceil(oxp) + OW, (int)std::ceil(oyp) + OH };
				const Rect sweepFull = Union(objFull, prevFull);

				RenderOracleFrame(bgImage, objImage, ox, oy, OW, OH, t, rgba);

				if (c.maskMode == OracleUniform) {
					std::fill(maskBytes.begin(), maskBytes.end(), (uint8_t)std::lround(c.value * 255));
				} else if (c.maskMode == OracleObject || c.maskMode == OracleObjectAndTrail) {
					std::fill(maskBytes.begin(), maskBytes.end(), (uint8_t)std::lround(c.value * 255));
					const Rect keep = Expand(c.maskMode == OracleObject ? objFull : sweepFull, 16, W, H);
					for (int y = keep.y0; y < keep.y1; y++) {
						memset(&maskBytes[(size_t)y * W + keep.x0], 255, (size_t)(keep.x1 - keep.x0));
					}
				}
				if (c.maskMode != OracleNone) {
					ctx->UpdateSubresource(texMask.pTexture, 0, nullptr, maskBytes.data(), W, 0);
				}

				DirectX::PackedVector::XMConvertFloatToHalfStream(half.data(), sizeof(HALF), rgba.data(), sizeof(float), rgba.size());
				ctx->UpdateSubresource(texIn.pTexture, 0, nullptr, half.data(), 4 * sizeof(HALF) * W, 0);

				QueryPerformanceCounter(&t0);
				const bool ok = dlss.Evaluate(params);
				QueryPerformanceCounter(&t1);
				if (!ok) {
					if (++failed <= 2) {
						Out("  %-26s frame %d: Evaluate failed -- %S\n", c.name, t, dlss.GetStatusLine().c_str());
					}
					continue;
				}
				ctx->CopyResource(stage, texOut.pTexture);
				if (!ReadLumaHalf(ctx, stage, W, H, yOut)) {
					failed++;
					continue;
				}
				LumaHalfFromFloat(rgba, W, H, yIn);
				Blur(yOut, w2, h2, 4, bOut);

				for (int y = 0; y < gh; y++) {
					for (int x = 0; x < gw; x++) {
						float acc = 0;
						for (int yy = 0; yy < 4; yy++) {
							for (int xx = 0; xx < 4; xx++) {
								acc += yOut[(size_t)(y * 4 + yy) * w2 + x * 4 + xx];
							}
						}
						lowres[(size_t)y * gw + x] = acc / 16;
					}
				}

				// Regions: the object now, the band it swept over the last frames,
				// and a guard band around both kept out of the background measures.
				const Rect guardHalf = Expand(Scale(sweepFull, 2), 16, w2, h2);

				if (t >= warm) {
					ms += 1000.0 * (t1.QuadPart - t0.QuadPart) / freq.QuadPart;
					bgLF += OutsideDiff(bOut, bOutPrev, w2, h2, guardHalf, 24);
					if (isReference) {
						Blur(yIn, w2, h2, 4, bIn);
						inBgLF += OutsideDiff(bIn, bInPrev, w2, h2, guardHalf, 24);
						inCount++;
						reference.insert(reference.end(), lowres.begin(), lowres.end());
					} else if (reference.size() >= (size_t)(count + 1) * gw * gh) {
						const Rect objG = Scale(objFull, 8);
						const Rect sweepG = Scale(sweepFull, 8);
						const Rect guardG = Expand(sweepG, 2, gw, gh);
						const float* ref = &reference[(size_t)count * gw * gh];
						double dO = 0, dT = 0, dB = 0;
						long long nO = 0, nT = 0, nB = 0;
						for (int y = 0; y < gh; y++) {
							for (int x = 0; x < gw; x++) {
								const double d = std::abs(lowres[(size_t)y * gw + x] - ref[(size_t)y * gw + x]);
								if (Inside(objG, x, y)) { dO += d; nO++; }
								else if (Inside(sweepG, x, y)) { dT += d; nT++; }
								else if (!Inside(guardG, x, y)) { dB += d; nB++; }
							}
						}
						objDev += nO ? dO / nO : 0;
						trailDev += nT ? dT / nT : 0;
						bgDev += nB ? dB / nB : 0;
					}
					count++;
				} else if (isReference) {
					Blur(yIn, w2, h2, 4, bIn);
				}

				yOutPrev.swap(yOut);
				bOutPrev.swap(bOut);
				if (isReference) {
					yInPrev.swap(yIn);
					bInPrev.swap(bIn);
				}
			}

			if (!count) {
				Out("  %-26s no usable frames (%d failed)\n", c.name, failed);
				g_failures++;
				continue;
			}
			const double inL = inCount ? inBgLF / inCount : 0;
			const double l = bgLF / count;
			Out("  %-26s %9.5f %8.2fx %9.5f %9.5f %9.5f %7.1f",
			    c.name, l, inL > 0 ? l / inL : 0.0,
			    isReference ? 0.0 : objDev / count, isReference ? 0.0 : trailDev / count,
			    isReference ? 0.0 : bgDev / count, ms / count);
			if (failed) {
				Out("   (%d failed)", failed);
			}
			Out("\n");
		}
		Out("  %-26s %9.5f\n", "(input)", inCount ? inBgLF / inCount : 0.0);
	}

	dlss.SetGuides(CDlssNR::Guides{});
	if (g_report) {
		fclose(g_report);
		g_report = nullptr;
		printf("\n  written to temporal_results_oracle.txt\n");
	}
	return g_failures ? 1 : 0;
}

} // namespace temporal

#include "detect_suite.inl"
#include "effect_suite.inl"
#include "flow_suite.inl"
#include "stab_suite.inl"
#include "mpvhook.inl"
#include "upscale_suite.inl"
#include "sharpen_suite.inl"
#include "chroma_suite.inl"
#include "mpv_port_suite.inl"
#include "sr_suite.inl"
#include "sr_still_suite.inl"
#include "pipeline_suite.inl"

int wmain(int argc, wchar_t** argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	printf("CDlssNR harness -- the filter's own code, outside the player\n");
	printf("===========================================================\n");

	int frames = 300;
	int temporalFrames = 96;
	bool temporal = false;
	bool temporalStrong = false;
	bool temporalMask = false;
	const wchar_t* dllPath = L"";
	for (int i = 1; i < argc; i++) {
		if (!wcscmp(argv[i], L"--frames") && i + 1 < argc) frames = _wtoi(argv[++i]);
		else if (!wcscmp(argv[i], L"--dll") && i + 1 < argc) dllPath = argv[++i];
		else if (!wcscmp(argv[i], L"--temporal")) temporal = true;
		else if (!wcscmp(argv[i], L"--tmask")) { temporal = true; temporalMask = true; }
		else if (!wcscmp(argv[i], L"--tframes") && i + 1 < argc) temporalFrames = std::max(_wtoi(argv[++i]), 16);
		else if (!wcscmp(argv[i], L"--tstrong")) temporalStrong = true;
	}

	// ---- device, shaped like CDX11VideoProcessor::Init --------------------
	Head("D3D11 device");
	D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
	CComPtr<ID3D11Device> dev0;
	CComPtr<ID3D11DeviceContext> ctx0;
	D3D_FEATURE_LEVEL fl = {};
	HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
	                               levels, 2, D3D11_SDK_VERSION, &dev0, &fl, &ctx0);
	Check(SUCCEEDED(hr), "D3D11CreateDevice");
	if (FAILED(hr)) return 1;

	CComPtr<ID3D11Device> dev = dev0;
	CComPtr<ID3D11DeviceContext> ctx = ctx0;
	{
		CComPtr<ID3D10Multithread> mt;
		if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&mt))) && mt) {
			mt->SetMultithreadProtected(TRUE);
		}
	}

	// ---- session ----------------------------------------------------------
	// --nonr leaves DLSS 5 NR out, for the suites that do not need it: its
	// architecture hook and its session in the same NGX runtime are then absent.
	bool skipNR = false;
	for (int i = 1; i < argc; i++) {
		if (!wcscmp(argv[i], L"--nonr")) skipNR = true;
	}
	CDlssNR dlss;
	if (!skipNR) {
		Head("CDlssNR::Init");
		const bool inited = dlss.Init(dev, dllPath, true);
		Check(inited, "Init");
		Dump(dlss);
		if (!inited) {
			printf("\n  Cannot continue without a session.\n");
			return 2;
		}
	}

	auto hasArg = [&](const wchar_t* name) {
		for (int i = 1; i < argc; i++) {
			if (!wcscmp(argv[i], name)) return true;
		}
		return false;
	};
	const bool temporalOracle = hasArg(L"--toracle");
	const bool temporalDetect = hasArg(L"--tdetect");
	const bool temporalBench = hasArg(L"--tbench");
	const bool temporalPort = hasArg(L"--tport");
	const bool temporalEffect = hasArg(L"--teffect");
	const bool temporalFlow = hasArg(L"--tflow");
	const bool temporalStab = hasArg(L"--tstab");
	const bool temporalStabPort = hasArg(L"--tstabport");
	const bool temporalStabBench = hasArg(L"--tstabbench");
	const bool temporalUpscale = hasArg(L"--tupscale");
	const bool temporalUpscaleCost = hasArg(L"--tupscalecost");
	const bool temporalChroma = hasArg(L"--tchroma");
	const bool temporalSharpen = hasArg(L"--tsharpen");
	const bool temporalMpvPort = hasArg(L"--tmpvport");
	const bool temporalSR = hasArg(L"--tsr");
	const bool temporalSRQuality = hasArg(L"--tsrq");
	const bool temporalSRStill = hasArg(L"--tsrstill");
	const bool temporalSRPort = hasArg(L"--tsrport");
	const bool temporalPipeline = hasArg(L"--tpipeline");
	std::wstring srDllPath;   // empty: CDlssSR looks next to the harness, then up to the repository root
	for (int i = 1; i + 1 < argc; i++) {
		if (!wcscmp(argv[i], L"--srdll")) {
			srDllPath = argv[i + 1];
		}
	}
	std::wstring temporalImage = L"C:\\Windows\\Web\\Wallpaper\\ThemeB\\img24.jpg";
	for (int i = 1; i + 1 < argc; i++) {
		if (!wcscmp(argv[i], L"--timage")) {
			temporalImage = argv[i + 1];
		}
	}
	if (temporal || temporalOracle || temporalDetect || temporalBench || temporalPort || temporalEffect || temporalFlow
			|| temporalStab || temporalStabPort || temporalStabBench || temporalUpscale || temporalUpscaleCost || temporalChroma || temporalSharpen || temporalMpvPort || temporalSR || temporalSRQuality || temporalSRStill || temporalSRPort || temporalPipeline) {
		int srRefs = 0;   // --srrefs N: only the first N references
		for (int i = 1; i + 1 < argc; i++) {
			if (!wcscmp(argv[i], L"--srrefs")) {
				srRefs = _wtoi(argv[i + 1]);
			}
		}
		const int rc = temporalPipeline ? temporal::RunPipeline(dev, ctx, dlss, srDllPath.c_str(), temporalFrames)
			: temporalSRPort ? temporal::RunSRPort(dev, ctx, srRefs)
			: temporalSRStill ? temporal::RunSRStill(dev, ctx, srDllPath.c_str(), srRefs)
			: temporalSRQuality ? temporal::RunSRQuality(dev, ctx, srDllPath.c_str(), srRefs)
			: temporalSR ? temporal::RunSR(dev, ctx, dlss, srDllPath.c_str())
			: temporalUpscaleCost ? temporal::RunUpscaleCost(dev, ctx)
			: temporalMpvPort ? temporal::RunMpvPort(dev, ctx, srRefs)
			: temporalSharpen ? temporal::RunSharpen(dev, ctx, srRefs)
			: temporalChroma ? temporal::RunChroma(dev, ctx, srRefs)
			: temporalUpscale ? temporal::RunUpscale(dev, ctx, srRefs, hasArg(L"--nomodels"))
			: temporalStabBench ? temporal::RunStabBench(dev, ctx)
			: (temporalStab || temporalStabPort)
			? temporal::RunStab(dev, ctx, dlss, temporalImage.c_str(), temporalFrames, temporalStrong, temporalStabPort)
			: temporalFlow ? temporal::RunFlow(dev, ctx, temporalImage.c_str())
			: temporalEffect ? temporal::RunEffect(dev, ctx, dlss, temporalImage.c_str(), temporalStrong)
			: temporalBench ? temporal::RunDetectBench(dev, ctx)
			: temporalPort ? temporal::RunDetect(dev, ctx, dlss, temporalFrames, temporalStrong, true)
			: temporalDetect ? temporal::RunDetect(dev, ctx, dlss, temporalFrames, temporalStrong, false)
			: temporalOracle ? temporal::RunOracle(dev, ctx, dlss, temporalFrames, temporalStrong)
			: temporal::Run(dev, ctx, dlss, temporalFrames, temporalStrong, temporalMask);
		dlss.Shutdown();
		Head("Result");
		printf("  %d check(s) failed\n", g_failures);
		return rc;
	}

	CDlssNR::Params params;
	params.bNoHistory = true;

	Tex2D_t texIn, texOut;
	UINT w = 1920, h = 1080;

	// ---- feature ----------------------------------------------------------
	Head("First feature");
	Check(MakeSharedPair(dev, texIn, texOut, w, h), "shared texture pair");
	Check(dlss.CreateFeature(texIn.pTexture, texOut.pTexture, w, h, params), "CreateFeature");
	Dump(dlss);
	if (!dlss.IsFeatureReady()) return 3;

	// ---- the loop that matters -------------------------------------------
	// Steady playback with the things that broke the player mixed in: toggling
	// the feature, changing resolution the way a fullscreen switch does, and
	// changing the preset, which forces a rebuild.
	Head("Playback loop");
	printf("  %d frames, with toggles, resolution changes and preset changes\n", frames);

	int evaluated = 0, evalFailed = 0, blank = 0, rebuilds = 0;

	for (int f = 0; f < frames; f++) {
		if (f && f % 60 == 0) {
			// A fullscreen switch, as far as this code is concerned.
			const bool big = ((f / 60) & 1) != 0;
			const UINT nw = big ? 2560u : 1920u;
			const UINT nh = big ? 1440u : 1080u;
			if (nw != w || nh != h) {
				w = nw; h = nh;
				if (!MakeSharedPair(dev, texIn, texOut, w, h)
						|| !dlss.CreateFeature(texIn.pTexture, texOut.pTexture, w, h, params)) {
					printf("  frame %d: rebuild at %ux%u FAILED -- %S\n", f, w, h, dlss.GetStatusLine().c_str());
					g_failures++;
					break;
				}
				rebuilds++;
			}
		}

		if (f && f % 90 == 0) {
			// Toggle off and straight back on, as the checkbox does.
			dlss.ReleaseFeature();
			if (!dlss.CreateFeature(texIn.pTexture, texOut.pTexture, w, h, params)) {
				printf("  frame %d: toggle rebuild FAILED -- %S\n", f, dlss.GetStatusLine().c_str());
				g_failures++;
				break;
			}
			rebuilds++;
		}

		if (f && f % 120 == 0) {
			// Preset change: baked into the feature, so this recreates it.
			params.iPreset = (params.iPreset + 1) % 3;
			dlss.ReleaseFeature();
			if (!dlss.CreateFeature(texIn.pTexture, texOut.pTexture, w, h, params)) {
				printf("  frame %d: preset rebuild FAILED -- %S\n", f, dlss.GetStatusLine().c_str());
				g_failures++;
				break;
			}
			rebuilds++;
		}

		// Sliders move without touching the feature.
		params.fIntensity = 0.5f + 1.0f * ((f % 50) / 50.0f);

		FillInput(dev, ctx, texIn.pTexture, (f % 30) / 30.0f);

		if (dlss.Evaluate(params)) {
			evaluated++;
			if (f % 50 == 0 && !OutputIsNonZero(dev, ctx, texOut.pTexture)) {
				blank++;
			}
		} else {
			evalFailed++;
			if (evalFailed <= 3) {
				printf("  frame %d: Evaluate failed -- %S\n", f, dlss.GetStatusLine().c_str());
			}
			if (evalFailed > 10) {
				printf("  giving up after 10 failures\n");
				break;
			}
		}

		// The renderer would be doing plenty of its own D3D11 work here.
		if (const HRESULT rr = dev->GetDeviceRemovedReason(); FAILED(rr)) {
			printf("  frame %d: D3D11 DEVICE REMOVED 0x%08X\n", f, (unsigned)rr);
			g_failures++;
			break;
		}
	}

	printf("  evaluated %d, failed %d, blank readbacks %d, rebuilds %d\n",
	       evaluated, evalFailed, blank, rebuilds);
	Check(evalFailed == 0, "no Evaluate failures");
	Check(blank == 0, "output never blank");
	Check(SUCCEEDED(dev->GetDeviceRemovedReason()), "D3D11 device survived");

	// ---- teardown while work is in flight ---------------------------------
	Head("Teardown");
	FillInput(dev, ctx, texIn.pTexture, 0.5f);
	dlss.Evaluate(params);
	dlss.Shutdown();          // the sequence that used to freeze the picture
	Check(SUCCEEDED(dev->GetDeviceRemovedReason()), "device survived Shutdown");

	// The renderer keeps drawing afterwards -- make sure its timeline is alive.
	FillInput(dev, ctx, texIn.pTexture, 0.25f);
	ctx->Flush();
	Check(SUCCEEDED(dev->GetDeviceRemovedReason()), "D3D11 still usable after Shutdown");

	// ---- a second session on the same device ------------------------------
	Head("Re-init");
	const bool reinit = dlss.Init(dev, dllPath, true);
	Check(reinit, "Init again");
	if (reinit) {
		texIn.Release();
		texOut.Release();
		Check(MakeSharedPair(dev, texIn, texOut, 1280, 720), "shared pair at 1280x720");
		Check(dlss.CreateFeature(texIn.pTexture, texOut.pTexture, 1280, 720, params), "CreateFeature again");
		FillInput(dev, ctx, texIn.pTexture, 0.5f);
		Check(dlss.Evaluate(params), "Evaluate again");
		dlss.Shutdown();
	}

	Head("Result");
	printf("  %d check(s) failed\n", g_failures);
	if (g_failures) {
		Dump(dlss);
	}
	printf("\n%s\n", g_failures ? "NOT SAFE to put in the player yet."
	                            : "Clean. This is the same code the filter runs.");
	return g_failures ? 1 : 0;
}
