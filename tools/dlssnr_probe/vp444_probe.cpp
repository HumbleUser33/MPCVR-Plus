// Can the D3D11 video processor take a picture whose chroma the shaders already
// rebuilt -- 4:4:4, AYUV / Y410 / Y416 -- and still do what only it does: RTX Video
// Super Resolution, RTX Video HDR, deinterlacing? Measured with the renderer's own
// CD3D11VP on the real driver. Each feature is judged by what it changes in the
// output against the same run without it, for 4:2:0 (NV12, P010) and 4:4:4 alike.

#include "stdafx.h"
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include "Helper.h"
#include "DX11Helper.h"
#include "IVideoRenderer.h"
#include "D3D11VP.h"
#include <wincodec.h>
#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowscodecs.lib")

HRESULT SaveToBMP(BYTE*, UINT, UINT, UINT, UINT, const wchar_t*) { return E_NOTIMPL; }

struct Gpu {
	CComPtr<ID3D11Device> dev;
	CComPtr<ID3D11DeviceContext> ctx;
	UINT vendor = 0;
	std::wstring name;
};

static bool CreateDevice(Gpu& g)
{
	CComPtr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
		return false;
	}
	CComPtr<IDXGIAdapter1> chosen, adapter;
	for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++, adapter.Release()) {
		DXGI_ADAPTER_DESC1 d = {};
		adapter->GetDesc1(&d);
		if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
			continue;
		}
		if (!chosen || d.VendorId == 0x10DE) {
			chosen = adapter;
			g.vendor = d.VendorId;
			g.name = d.Description;
			if (d.VendorId == 0x10DE) {
				break;
			}
		}
	}
	if (!chosen) {
		return false;
	}
	const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
	return SUCCEEDED(D3D11CreateDevice(chosen, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
		D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
		levels, (UINT)std::size(levels), D3D11_SDK_VERSION, &g.dev, nullptr, &g.ctx));
}

// A test picture in BT.709 studio-range Y'CbCr, full resolution, in 8-bit code values.
struct Picture {
	int w = 0, h = 0;
	std::vector<float> y, cb, cr;
};

static Picture MakePicture(int w, int h, int shift)
{
	Picture p;
	p.w = w;
	p.h = h;
	p.y.resize((size_t)w * h);
	p.cb.resize(p.y.size());
	p.cr.resize(p.y.size());
	for (int j = 0; j < h; j++) {
		for (int i = 0; i < w; i++) {
			const int x = i + shift;
			// Smooth colour gradients, a zone plate for fine luma, saturated shapes with hard edges.
			float r = 0.5f + 0.35f * std::sin(x * 0.013f), g = 0.5f + 0.35f * std::sin(j * 0.017f + 1.0f), b = 0.5f + 0.35f * std::sin((x + j) * 0.009f + 2.0f);
			const float dx = x - w * 0.3f, dy = j - h * 0.5f;
			if (dx * dx + dy * dy < 0.04f * w * w) {
				const float zp = 0.5f + 0.45f * std::cos((dx * dx + dy * dy) * 0.004f);
				r = g = b = zp;
			}
			if (std::abs(x - w * 0.65f) < w * 0.08f && std::abs(j - h * 0.3f) < h * 0.12f) { r = 0.85f; g = 0.1f; b = 0.1f; }
			if (std::abs(x - w * 0.8f) < w * 0.06f && std::abs(j - h * 0.65f) < h * 0.15f) { r = 0.1f; g = 0.15f; b = 0.9f; }
			if (x % 24 < 3 && j > h * 0.75f) { r = 0.1f; g = 0.8f; b = 0.2f; }
			const float yl = 0.2126f * r + 0.7152f * g + 0.0722f * b;
			const size_t k = (size_t)j * w + i;
			p.y[k] = 16.0f + 219.0f * yl;
			p.cb[k] = 128.0f + 224.0f * (b - yl) / 1.8556f;
			p.cr[k] = 128.0f + 224.0f * (r - yl) / 1.5748f;
		}
	}
	return p;
}

// Rows alternating between two pictures, as an interlaced frame whose fields differ.
static Picture Weave(const Picture& top, const Picture& bottom)
{
	Picture p = top;
	for (int j = 1; j < p.h; j += 2) {
		for (int i = 0; i < p.w; i++) {
			const size_t k = (size_t)j * p.w + i;
			p.y[k] = bottom.y[k];
			p.cb[k] = bottom.cb[k];
			p.cr[k] = bottom.cr[k];
		}
	}
	return p;
}

static CComPtr<ID3D11Texture2D> Upload(Gpu& g, DXGI_FORMAT fmt, const Picture& p)
{
	D3D11_TEXTURE2D_DESC d = {};
	d.Width = p.w;
	d.Height = p.h;
	d.MipLevels = 1;
	d.ArraySize = 1;
	d.Format = fmt;
	d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_STAGING;
	d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	CComPtr<ID3D11Texture2D> tex;
	if (FAILED(g.dev->CreateTexture2D(&d, nullptr, &tex))) {
		return nullptr;
	}
	D3D11_MAPPED_SUBRESOURCE m = {};
	if (FAILED(g.ctx->Map(tex, 0, D3D11_MAP_WRITE, 0, &m))) {
		return nullptr;
	}
	BYTE* base = (BYTE*)m.pData;
	auto q8 = [](float v) { return (BYTE)std::clamp((int)std::lround(v), 0, 255); };
	auto q10 = [](float v) { return (UINT)std::clamp((int)std::lround(v * 4.0f), 0, 1023); };
	auto q16 = [](float v) { return (UINT16)std::clamp((int)std::lround(v * 256.0f), 0, 65535); };
	auto chroma = [&](int i2, int j2, float& u, float& v) {   // a 2x2 block's mean, for 4:2:0
		u = v = 0;
		for (int dj = 0; dj < 2; dj++) {
			for (int di = 0; di < 2; di++) {
				const size_t k = (size_t)std::min(2 * j2 + dj, p.h - 1) * p.w + std::min(2 * i2 + di, p.w - 1);
				u += p.cb[k] * 0.25f;
				v += p.cr[k] * 0.25f;
			}
		}
	};
	switch (fmt) {
	case DXGI_FORMAT_NV12:
		for (int j = 0; j < p.h; j++) {
			for (int i = 0; i < p.w; i++) {
				base[(size_t)j * m.RowPitch + i] = q8(p.y[(size_t)j * p.w + i]);
			}
		}
		for (int j = 0; j < p.h / 2; j++) {
			BYTE* row = base + (size_t)(p.h + j) * m.RowPitch;
			for (int i = 0; i < p.w / 2; i++) {
				float u, v;
				chroma(i, j, u, v);
				row[2 * i] = q8(u);
				row[2 * i + 1] = q8(v);
			}
		}
		break;
	case DXGI_FORMAT_P010:
		for (int j = 0; j < p.h; j++) {
			UINT16* row = (UINT16*)(base + (size_t)j * m.RowPitch);
			for (int i = 0; i < p.w; i++) {
				row[i] = (UINT16)(q10(p.y[(size_t)j * p.w + i]) << 6);
			}
		}
		for (int j = 0; j < p.h / 2; j++) {
			UINT16* row = (UINT16*)(base + (size_t)(p.h + j) * m.RowPitch);
			for (int i = 0; i < p.w / 2; i++) {
				float u, v;
				chroma(i, j, u, v);
				row[2 * i] = (UINT16)(q10(u) << 6);
				row[2 * i + 1] = (UINT16)(q10(v) << 6);
			}
		}
		break;
	case DXGI_FORMAT_AYUV:   // V, U, Y, A
		for (int j = 0; j < p.h; j++) {
			BYTE* row = base + (size_t)j * m.RowPitch;
			for (int i = 0; i < p.w; i++) {
				const size_t k = (size_t)j * p.w + i;
				row[4 * i] = q8(p.cr[k]);
				row[4 * i + 1] = q8(p.cb[k]);
				row[4 * i + 2] = q8(p.y[k]);
				row[4 * i + 3] = 255;
			}
		}
		break;
	case DXGI_FORMAT_Y410:   // U 0-9, Y 10-19, V 20-29, A 30-31
		for (int j = 0; j < p.h; j++) {
			UINT* row = (UINT*)(base + (size_t)j * m.RowPitch);
			for (int i = 0; i < p.w; i++) {
				const size_t k = (size_t)j * p.w + i;
				row[i] = q10(p.cb[k]) | (q10(p.y[k]) << 10) | (q10(p.cr[k]) << 20) | (3u << 30);
			}
		}
		break;
	case DXGI_FORMAT_Y416:   // U, Y, V, A
		for (int j = 0; j < p.h; j++) {
			UINT16* row = (UINT16*)(base + (size_t)j * m.RowPitch);
			for (int i = 0; i < p.w; i++) {
				const size_t k = (size_t)j * p.w + i;
				row[4 * i] = q16(p.cb[k]);
				row[4 * i + 1] = q16(p.y[k]);
				row[4 * i + 2] = q16(p.cr[k]);
				row[4 * i + 3] = 65535;
			}
		}
		break;
	default:
		g.ctx->Unmap(tex, 0);
		return nullptr;
	}
	g.ctx->Unmap(tex, 0);
	return tex;
}

static DXVA2_ExtendedFormat Bt709(bool interlaced)
{
	DXVA2_ExtendedFormat fmt = {};
	fmt.SampleFormat           = interlaced ? DXVA2_SampleFieldInterleavedEvenFirst : DXVA2_SampleProgressiveFrame;
	fmt.VideoChromaSubsampling = DXVA2_VideoChromaSubsampling_MPEG2;
	fmt.NominalRange           = DXVA2_NominalRange_16_235;
	fmt.VideoTransferMatrix    = DXVA2_VideoTransferMatrix_BT709;
	fmt.VideoLighting          = DXVA2_VideoLighting_dim;
	fmt.VideoPrimaries         = DXVA2_VideoPrimaries_BT709;
	fmt.VideoTransferFunction  = DXVA2_VideoTransFunc_709;
	return fmt;
}

// The output in 0..1 per channel, R, G, B.
struct Output {
	int w = 0, h = 0;
	std::vector<float> rgb;
};

static bool ReadOutput(Gpu& g, ID3D11Texture2D* rt, Output& out)
{
	D3D11_TEXTURE2D_DESC d = {};
	rt->GetDesc(&d);
	d.Usage = D3D11_USAGE_STAGING;
	d.BindFlags = 0;
	d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	d.MiscFlags = 0;
	CComPtr<ID3D11Texture2D> st;
	if (FAILED(g.dev->CreateTexture2D(&d, nullptr, &st))) {
		return false;
	}
	g.ctx->CopyResource(st, rt);
	D3D11_MAPPED_SUBRESOURCE m = {};
	if (FAILED(g.ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
		return false;
	}
	out.w = d.Width;
	out.h = d.Height;
	out.rgb.resize((size_t)3 * d.Width * d.Height);
	for (UINT j = 0; j < d.Height; j++) {
		const UINT* row = (const UINT*)((const BYTE*)m.pData + (size_t)j * m.RowPitch);
		for (UINT i = 0; i < d.Width; i++) {
			const UINT v = row[i];
			float* o = &out.rgb[3 * ((size_t)j * d.Width + i)];
			if (d.Format == DXGI_FORMAT_R10G10B10A2_UNORM) {
				o[0] = (v & 1023) / 1023.0f;
				o[1] = ((v >> 10) & 1023) / 1023.0f;
				o[2] = ((v >> 20) & 1023) / 1023.0f;
			} else {   // B8G8R8A8
				o[0] = ((v >> 16) & 255) / 255.0f;
				o[1] = ((v >> 8) & 255) / 255.0f;
				o[2] = (v & 255) / 255.0f;
			}
		}
	}
	g.ctx->Unmap(st, 0);
	return true;
}

// Mean absolute difference in 8-bit levels.
static double Diff(const Output& a, const Output& b)
{
	if (a.rgb.size() != b.rgb.size() || a.rgb.empty()) {
		return -1;
	}
	double s = 0;
	for (size_t i = 0; i < a.rgb.size(); i++) {
		s += std::abs(a.rgb[i] - b.rgb[i]);
	}
	return 255.0 * s / a.rgb.size();
}

// How much each row stands out from the mean of its neighbours: the combing of a woven,
// not deinterlaced picture. 8-bit levels, green channel.
static double Combing(const Output& o)
{
	double s = 0;
	size_t n = 0;
	for (int j = 1; j + 1 < o.h; j++) {
		for (int i = 0; i < o.w; i++) {
			const float c = o.rgb[3 * ((size_t)j * o.w + i) + 1];
			const float u = o.rgb[3 * ((size_t)(j - 1) * o.w + i) + 1];
			const float d = o.rgb[3 * ((size_t)(j + 1) * o.w + i) + 1];
			s += std::abs(c - 0.5f * (u + d));
			n++;
		}
	}
	return 255.0 * s / n;
}

struct RunResult {
	HRESULT hr = E_FAIL;
	HRESULT hrSuperRes = E_FAIL;
	HRESULT hrTrueHDR = E_FAIL;
	DXGI_FORMAT outFmt = DXGI_FORMAT_UNKNOWN;
	Output out;
};

// One processor, fed the frames in turn, the output of the last one read back.
static RunResult Run(Gpu& g, DXGI_FORMAT fmt, const std::vector<Picture>& frames, bool interlaced, int outW, int outH,
	int superRes, bool trueHdr, bool hdrOutput, int passthrough = -1)
{
	RunResult r;
	CD3D11VP vp;
	r.hr = vp.InitVideoDevice(g.dev, g.ctx, g.vendor);
	if (FAILED(r.hr)) {
		return r;
	}
	const int w = frames[0].w, h = frames[0].h;
	DXGI_FORMAT out = hdrOutput ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
	const bool bPassthrough = (passthrough < 0) ? hdrOutput : (passthrough != 0);
	r.hr = vp.InitVideoProcessor(fmt, w, h, Bt709(interlaced), interlaced ? DEINT_Enable : DEINT_Disable, bPassthrough, out);
	if (FAILED(r.hr)) {
		return r;
	}
	r.outFmt = out;
	r.hr = vp.InitInputTextures(g.dev);
	if (FAILED(r.hr)) {
		return r;
	}
	const RECT src = { 0, 0, w, h }, dst = { 0, 0, outW, outH };
	vp.SetRectangles(&src, &dst);
	r.hrSuperRes = vp.SetSuperRes(superRes);
	r.hrTrueHDR = vp.SetRTXVideoHDR(trueHdr);

	D3D11_TEXTURE2D_DESC d = CreateTex2DDesc(out, outW, outH, Tex2D_DefaultRTarget);
	CComPtr<ID3D11Texture2D> rt;
	r.hr = g.dev->CreateTexture2D(&d, nullptr, &rt);
	if (FAILED(r.hr)) {
		return r;
	}
	const D3D11_VIDEO_FRAME_FORMAT ff = interlaced ? D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST : D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
	for (const Picture& p : frames) {
		CComPtr<ID3D11Texture2D> staged = Upload(g, fmt, p);
		if (!staged) {
			r.hr = E_OUTOFMEMORY;
			return r;
		}
		ID3D11Texture2D* in = vp.GetNextInputTexture(ff);
		g.ctx->CopyResource(in, staged);
		r.hr = vp.Process(rt, ff, false);
		if (FAILED(r.hr)) {
			return r;
		}
	}
	if (!ReadOutput(g, rt, r.out)) {
		r.hr = E_FAIL;
	}
	return r;
}

static const char* Name(DXGI_FORMAT f)
{
	switch (f) {
	case DXGI_FORMAT_NV12: return "NV12 (4:2:0, 8-bit)";
	case DXGI_FORMAT_P010: return "P010 (4:2:0, 10-bit)";
	case DXGI_FORMAT_AYUV: return "AYUV (4:4:4, 8-bit)";
	case DXGI_FORMAT_Y410: return "Y410 (4:4:4, 10-bit)";
	case DXGI_FORMAT_Y416: return "Y416 (4:4:4, 16-bit)";
	default: return "?";
	}
}

// ---------------------------------------------------------------------------
// Film frames: what reaches the screen when the chroma is rebuilt before the
// processor, against the processor rebuilding it itself.

static bool LoadHalved(IWICImagingFactory* f, const wchar_t* path, int& w, int& h, std::vector<float>& rgb)
{
	CComPtr<IWICBitmapDecoder> dec;
	CComPtr<IWICBitmapFrameDecode> frame;
	CComPtr<IWICFormatConverter> cv;
	if (FAILED(f->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))
		|| FAILED(dec->GetFrame(0, &frame)) || FAILED(f->CreateFormatConverter(&cv))
		|| FAILED(cv->Initialize(frame, GUID_WICPixelFormat64bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
		return false;
	}
	UINT W = 0, H = 0;
	cv->GetSize(&W, &H);
	std::vector<UINT16> px((size_t)W * H * 4);
	if (FAILED(cv->CopyPixels(nullptr, W * 8, (UINT)(px.size() * 2), (BYTE*)px.data()))) {
		return false;
	}
	w = (int)(W / 2) & ~1;
	h = (int)(H / 2) & ~1;
	rgb.assign((size_t)w * h * 3, 0.0f);
	for (int j = 0; j < h; j++) {
		for (int i = 0; i < w; i++) {
			for (int c = 0; c < 3; c++) {
				float s = 0;
				for (int dj = 0; dj < 2; dj++) {
					for (int di = 0; di < 2; di++) {
						s += px[(((size_t)(2 * j + dj) * W) + 2 * i + di) * 4 + c] / 65535.0f;
					}
				}
				rgb[((size_t)j * w + i) * 3 + c] = s * 0.25f;
			}
		}
	}
	return true;
}

// Y'CbCr planes in 8-bit code values, chroma at cw x ch.
struct Planes {
	int w = 0, h = 0, cw = 0, ch = 0;
	std::vector<float> y, cb, cr;
};

static Planes ToYCbCr444(const std::vector<float>& rgb, int w, int h)
{
	Planes p;
	p.w = p.cw = w;
	p.h = p.ch = h;
	p.y.resize((size_t)w * h);
	p.cb.resize(p.y.size());
	p.cr.resize(p.y.size());
	for (size_t k = 0; k < p.y.size(); k++) {
		const float r = rgb[3 * k], g = rgb[3 * k + 1], b = rgb[3 * k + 2];
		const float yl = 0.2126f * r + 0.7152f * g + 0.0722f * b;
		p.y[k] = 16.0f + 219.0f * yl;
		p.cb[k] = 128.0f + 224.0f * (b - yl) / 1.8556f;
		p.cr[k] = 128.0f + 224.0f * (r - yl) / 1.5748f;
	}
	return p;
}

// 4:2:0 as MPEG-2 sites it: chroma on the even columns, between two rows.
static Planes To420(const Planes& f)
{
	Planes p;
	p.w = f.w;
	p.h = f.h;
	p.cw = f.w / 2;
	p.ch = f.h / 2;
	p.y = f.y;
	p.cb.resize((size_t)p.cw * p.ch);
	p.cr.resize(p.cb.size());
	for (int j = 0; j < p.ch; j++) {
		for (int i = 0; i < p.cw; i++) {
			float u = 0, v = 0;
			for (int dj = 0; dj < 2; dj++) {
				for (int di = -1; di <= 1; di++) {
					const int x = std::clamp(2 * i + di, 0, f.w - 1), y = 2 * j + dj;
					const float wgt = 0.5f * (di == 0 ? 0.5f : 0.25f);
					u += wgt * f.cb[(size_t)y * f.w + x];
					v += wgt * f.cr[(size_t)y * f.w + x];
				}
			}
			p.cb[(size_t)j * p.cw + i] = u;
			p.cr[(size_t)j * p.cw + i] = v;
		}
	}
	return p;
}

// Back to full resolution with Catmull-Rom, placed where MPEG-2 puts the chroma --
// what the renderer's shaders do.
static Planes CatmullRom444(const Planes& s)
{
	Planes p;
	p.w = p.cw = s.w;
	p.h = p.ch = s.h;
	p.y = s.y;
	p.cb.resize((size_t)s.w * s.h);
	p.cr.resize(p.cb.size());
	auto weights = [](float t, float w[4]) {
		const float t2 = t * t, t3 = t2 * t;
		w[0] = 0.5f * (-t3 + 2 * t2 - t);
		w[1] = 0.5f * (3 * t3 - 5 * t2 + 2);
		w[2] = 0.5f * (-3 * t3 + 4 * t2 + t);
		w[3] = 0.5f * (t3 - t2);
	};
	for (int y = 0; y < s.h; y++) {
		const float v = (y - 0.5f) * 0.5f;
		const int j0 = (int)std::floor(v);
		float wy[4];
		weights(v - j0, wy);
		for (int x = 0; x < s.w; x++) {
			const float u = x * 0.5f;
			const int i0 = (int)std::floor(u);
			float wx[4];
			weights(u - i0, wx);
			float cb = 0, cr = 0;
			for (int b = 0; b < 4; b++) {
				const int j = std::clamp(j0 - 1 + b, 0, s.ch - 1);
				for (int a = 0; a < 4; a++) {
					const int i = std::clamp(i0 - 1 + a, 0, s.cw - 1);
					const float wgt = wx[a] * wy[b];
					cb += wgt * s.cb[(size_t)j * s.cw + i];
					cr += wgt * s.cr[(size_t)j * s.cw + i];
				}
			}
			p.cb[(size_t)y * s.w + x] = cb;
			p.cr[(size_t)y * s.w + x] = cr;
		}
	}
	return p;
}

static CComPtr<ID3D11Texture2D> UploadPlanes(Gpu& g, DXGI_FORMAT fmt, const Planes& p)
{
	D3D11_TEXTURE2D_DESC d = {};
	d.Width = p.w;
	d.Height = p.h;
	d.MipLevels = 1;
	d.ArraySize = 1;
	d.Format = fmt;
	d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_STAGING;
	d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	CComPtr<ID3D11Texture2D> tex;
	D3D11_MAPPED_SUBRESOURCE m = {};
	if (FAILED(g.dev->CreateTexture2D(&d, nullptr, &tex)) || FAILED(g.ctx->Map(tex, 0, D3D11_MAP_WRITE, 0, &m))) {
		return nullptr;
	}
	BYTE* base = (BYTE*)m.pData;
	auto q8 = [](float v) { return (BYTE)std::clamp((int)std::lround(v), 0, 255); };
	auto q10 = [](float v) { return (UINT)std::clamp((int)std::lround(v * 4.0f), 0, 1023); };
	auto q16 = [](float v) { return (UINT16)std::clamp((int)std::lround(v * 256.0f), 0, 65535); };
	for (int j = 0; j < p.h; j++) {
		BYTE* row = base + (size_t)j * m.RowPitch;
		for (int i = 0; i < p.w; i++) {
			const size_t k = (size_t)j * p.w + i;
			switch (fmt) {
			case DXGI_FORMAT_NV12:
				row[i] = q8(p.y[k]);
				break;
			case DXGI_FORMAT_P010:
				((UINT16*)row)[i] = (UINT16)(q10(p.y[k]) << 6);
				break;
			case DXGI_FORMAT_AYUV:
				row[4 * i] = q8(p.cr[k]);
				row[4 * i + 1] = q8(p.cb[k]);
				row[4 * i + 2] = q8(p.y[k]);
				row[4 * i + 3] = 255;
				break;
			case DXGI_FORMAT_Y410:
				((UINT*)row)[i] = q10(p.cb[k]) | (q10(p.y[k]) << 10) | (q10(p.cr[k]) << 20) | (3u << 30);
				break;
			case DXGI_FORMAT_Y416: {
				UINT16* o = (UINT16*)row + 4 * i;
				o[0] = q16(p.cb[k]);
				o[1] = q16(p.y[k]);
				o[2] = q16(p.cr[k]);
				o[3] = 65535;
				break;
			}
			default:
				break;
			}
		}
	}
	if (fmt == DXGI_FORMAT_NV12) {
		for (int j = 0; j < p.ch; j++) {
			BYTE* row = base + (size_t)(p.h + j) * m.RowPitch;
			for (int i = 0; i < p.cw; i++) {
				row[2 * i] = q8(p.cb[(size_t)j * p.cw + i]);
				row[2 * i + 1] = q8(p.cr[(size_t)j * p.cw + i]);
			}
		}
	}
	if (fmt == DXGI_FORMAT_P010) {
		for (int j = 0; j < p.ch; j++) {
			UINT16* row = (UINT16*)(base + (size_t)(p.h + j) * m.RowPitch);
			for (int i = 0; i < p.cw; i++) {
				row[2 * i] = (UINT16)(q10(p.cb[(size_t)j * p.cw + i]) << 6);
				row[2 * i + 1] = (UINT16)(q10(p.cr[(size_t)j * p.cw + i]) << 6);
			}
		}
	}
	g.ctx->Unmap(tex, 0);
	return tex;
}

// The same, with the processor's own options and an output size.
static bool Convert2(Gpu& g, DXGI_FORMAT fmt, const Planes& p, bool hdrOutput, bool trueHdr, int superRes,
	int outW, int outH, Output& out, std::string& error)
{
	CD3D11VP vp;
	DXGI_FORMAT o = DXGI_FORMAT_R10G10B10A2_UNORM;
	if (FAILED(vp.InitVideoDevice(g.dev, g.ctx, g.vendor))
			|| FAILED(vp.InitVideoProcessor(fmt, p.w, p.h, Bt709(false), DEINT_Disable, hdrOutput, o))
			|| FAILED(vp.InitInputTextures(g.dev))) {
		error = "video processor failed";
		return false;
	}
	const RECT src = { 0, 0, p.w, p.h }, dst = { 0, 0, outW, outH };
	vp.SetRectangles(&src, &dst);
	vp.SetSuperRes(superRes);
	vp.SetRTXVideoHDR(trueHdr);
	D3D11_TEXTURE2D_DESC d = CreateTex2DDesc(o, outW, outH, Tex2D_DefaultRTarget);
	CComPtr<ID3D11Texture2D> rt;
	CComPtr<ID3D11Texture2D> staged = UploadPlanes(g, fmt, p);
	if (!staged || FAILED(g.dev->CreateTexture2D(&d, nullptr, &rt))) {
		error = "textures failed";
		return false;
	}
	for (int n = 0; n < 3; n++) {
		g.ctx->CopyResource(vp.GetNextInputTexture(D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE), staged);
		if (FAILED(vp.Process(rt, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE, false))) {
			error = "Blt failed";
			return false;
		}
	}
	return ReadOutput(g, rt, out);
}

// The processor at the picture's size, 10-bit output, fed the same frame three times.
static bool Convert(Gpu& g, DXGI_FORMAT fmt, const Planes& p, Output& out)
{
	CD3D11VP vp;
	DXGI_FORMAT o = DXGI_FORMAT_R10G10B10A2_UNORM;
	if (FAILED(vp.InitVideoDevice(g.dev, g.ctx, g.vendor)) || FAILED(vp.InitVideoProcessor(fmt, p.w, p.h, Bt709(false), DEINT_Disable, false, o))
		|| FAILED(vp.InitInputTextures(g.dev))) {
		return false;
	}
	const RECT r = { 0, 0, p.w, p.h };
	vp.SetRectangles(&r, &r);
	D3D11_TEXTURE2D_DESC d = CreateTex2DDesc(o, p.w, p.h, Tex2D_DefaultRTarget);
	CComPtr<ID3D11Texture2D> rt;
	CComPtr<ID3D11Texture2D> staged = UploadPlanes(g, fmt, p);
	if (!staged || FAILED(g.dev->CreateTexture2D(&d, nullptr, &rt))) {
		return false;
	}
	for (int n = 0; n < 3; n++) {
		g.ctx->CopyResource(vp.GetNextInputTexture(D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE), staged);
		if (FAILED(vp.Process(rt, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE, false))) {
			return false;
		}
	}
	return ReadOutput(g, rt, out);
}

struct Score {
	double rgb = 0, chroma = 0, edges = 0;
};

// As playback_test's ScoreChroma, against the picture itself: the chroma taken back
// from R'G'B' on the whole picture, and next to the luma's edges, where it bleeds.
static Score ScoreAgainst(const Output& o, const std::vector<float>& ref)
{
	auto chroma = [](const float* p, double& y, double& cb, double& cr) {
		y = 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
		cb = (p[2] - y) / 1.8556;
		cr = (p[0] - y) / 1.5748;
	};
	double seRgb = 0, seChroma = 0, seEdges = 0;
	size_t edges = 0;
	const size_t n = (size_t)o.w * o.h;
	for (int y = 0; y < o.h; y++) {
		for (int x = 0; x < o.w; x++) {
			const size_t i = ((size_t)y * o.w + x) * 3;
			double ty, tcb, tcr, ry, rcb, rcr;
			chroma(&o.rgb[i], ty, tcb, tcr);
			chroma(&ref[i], ry, rcb, rcr);
			for (int k = 0; k < 3; k++) {
				const double d = o.rgb[i + k] - ref[i + k];
				seRgb += d * d;
			}
			const double dcb = tcb - rcb, dcr = tcr - rcr;
			seChroma += dcb * dcb + dcr * dcr;
			double ny, ncb, ncr, by, bcb, bcr;
			chroma(&ref[((size_t)y * o.w + std::min(x + 1, o.w - 1)) * 3], ny, ncb, ncr);
			chroma(&ref[((size_t)std::min(y + 1, o.h - 1) * o.w + x) * 3], by, bcb, bcr);
			if (std::abs(ny - ry) + std::abs(by - ry) > 0.05) {
				seEdges += dcb * dcb + dcr * dcr;
				edges++;
			}
		}
	}
	auto psnr = [](double mse) { return mse > 0 ? 10.0 * std::log10(1.0 / mse) : 99.0; };
	return { psnr(seRgb / (3.0 * n)), psnr(seChroma / (2.0 * n)), edges ? psnr(seEdges / (2.0 * edges)) : 99.0 };
}

// output = gain * picture + offset, per channel, least squares; offset in 8-bit levels.
static void Fit(const Output& o, const std::vector<float>& ref, double gain[3], double offset[3])
{
	for (int c = 0; c < 3; c++) {
		double sx = 0, sy = 0, sxx = 0, sxy = 0;
		const size_t n = (size_t)o.w * o.h;
		for (size_t i = 0; i < n; i++) {
			const double x = ref[3 * i + c], y = o.rgb[3 * i + c];
			sx += x;
			sy += y;
			sxx += x * x;
			sxy += x * y;
		}
		gain[c] = (n * sxy - sx * sy) / (n * sxx - sx * sx);
		offset[c] = 255.0 * (sy - gain[c] * sx) / n;
	}
}


// ---------------------------------------------------------------------------
// What the filter would do: a compute shader writes the packed 4:4:4 word into
// the texture the video processor reads. AYUV can be a render target, Y410 and
// Y416 cannot -- their only writable view is an unordered access one -- so the
// same shader covers both through an R32_UINT view.

static const char* const kPackShader = R"(
Texture2D<float4> source : register(t0);   // Y, Cb, Cr in 0..1 of the 8-bit scale
RWTexture2D<uint> packed : register(u0);

cbuffer Constants : register(b0)
{
	float scale;      // code value per unit of the 8-bit scale: 1, 4 or 1023/255
	float bits;       // 8 for AYUV, 10 for Y410
	float2 reserved;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint w, h;
	packed.GetDimensions(w, h);
	if (id.x >= w || id.y >= h) {
		return;
	}
	const float3 ycc = source.Load(int3(id.xy, 0)).rgb * 255.0;
	if (bits < 9.0) {
		// AYUV: V, U, Y, A in an R8G8B8A8 view, so V is the low byte.
		const uint3 q = (uint3)clamp(round(ycc * scale), 0.0, 255.0);
		packed[id.xy] = q.z | (q.y << 8) | (q.x << 16) | (255u << 24);
	} else {
		// Y410: U, Y, V, A in an R10G10B10A2 view.
		const uint3 q = (uint3)clamp(round(ycc * scale), 0.0, 1023.0);
		packed[id.xy] = q.y | (q.x << 10) | (q.z << 20) | (3u << 30);
	}
}
)";

// The picture as one RGBA16 texture: Y, Cb, Cr on the 8-bit scale, normalised.
static CComPtr<ID3D11Texture2D> SourceTexture(Gpu& g, const Planes& p, CComPtr<ID3D11ShaderResourceView>& srv)
{
	std::vector<UINT16> data(4 * (size_t)p.w * p.h);
	for (int j = 0; j < p.h; j++) {
		for (int i = 0; i < p.w; i++) {
			const size_t k = (size_t)j * p.w + i;
			auto q = [](float v) { return (UINT16)std::clamp((int)std::lround(v / 255.0f * 65535.0f), 0, 65535); };
			data[4 * k] = q(p.y[k]);
			data[4 * k + 1] = q(p.cb[k]);
			data[4 * k + 2] = q(p.cr[k]);
			data[4 * k + 3] = 65535;
		}
	}
	D3D11_TEXTURE2D_DESC d = CreateTex2DDesc(DXGI_FORMAT_R16G16B16A16_UNORM, p.w, p.h, Tex2D_DefaultShader);
	D3D11_SUBRESOURCE_DATA init = { data.data(), (UINT)(8 * p.w), 0 };
	CComPtr<ID3D11Texture2D> tex;
	if (FAILED(g.dev->CreateTexture2D(&d, &init, &tex)) || FAILED(g.dev->CreateShaderResourceView(tex, nullptr, &srv))) {
		return nullptr;
	}
	return tex;
}

// Writes the picture into a 4:4:4 texture the video processor can read, through a
// compute shader and an R32_UINT view. scale says how a code value of the 8-bit
// scale is written: 4 keeps the 10-bit standard (64..940), 1023/255 lands where
// this driver reads the studio range whatever the depth.
static CComPtr<ID3D11Texture2D> PackWithShader(Gpu& g, DXGI_FORMAT fmt, const Planes& p, float scale, std::string& error)
{
	CComPtr<ID3DBlob> code, errors;
	if (FAILED(D3DCompile(kPackShader, strlen(kPackShader), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &errors))) {
		error = errors ? (const char*)errors->GetBufferPointer() : "compile failed";
		return nullptr;
	}
	CComPtr<ID3D11ComputeShader> cs;
	if (FAILED(g.dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs))) {
		error = "CreateComputeShader failed";
		return nullptr;
	}

	D3D11_TEXTURE2D_DESC d = {};
	d.Width = p.w;
	d.Height = p.h;
	d.MipLevels = 1;
	d.ArraySize = 1;
	d.Format = fmt;
	d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_DEFAULT;
	d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	CComPtr<ID3D11Texture2D> tex;
	HRESULT hr = g.dev->CreateTexture2D(&d, nullptr, &tex);
	if (FAILED(hr)) {
		error = std::format("CreateTexture2D(SRV|UAV) failed 0x{:08X}", (unsigned)hr);
		return nullptr;
	}
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	uavDesc.Format = DXGI_FORMAT_R32_UINT;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
	CComPtr<ID3D11UnorderedAccessView> uav;
	hr = g.dev->CreateUnorderedAccessView(tex, &uavDesc, &uav);
	if (FAILED(hr)) {
		error = std::format("CreateUnorderedAccessView(R32_UINT) failed 0x{:08X}", (unsigned)hr);
		return nullptr;
	}

	CComPtr<ID3D11ShaderResourceView> srv;
	CComPtr<ID3D11Texture2D> source = SourceTexture(g, p, srv);
	if (!source) {
		error = "source texture failed";
		return nullptr;
	}
	struct { float scale, bits, reserved[2]; } constants = { scale, fmt == DXGI_FORMAT_AYUV ? 8.0f : 10.0f, {} };
	D3D11_BUFFER_DESC bd = { sizeof(constants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
	D3D11_SUBRESOURCE_DATA cinit = { &constants, 0, 0 };
	CComPtr<ID3D11Buffer> cb;
	if (FAILED(g.dev->CreateBuffer(&bd, &cinit, &cb))) {
		error = "constant buffer failed";
		return nullptr;
	}

	ID3D11ShaderResourceView* srvs[1] = { srv };
	ID3D11UnorderedAccessView* uavs[1] = { uav };
	ID3D11Buffer* cbs[1] = { cb };
	g.ctx->CSSetShader(cs, nullptr, 0);
	g.ctx->CSSetShaderResources(0, 1, srvs);
	g.ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
	g.ctx->CSSetConstantBuffers(0, 1, cbs);
	g.ctx->Dispatch((p.w + 7) / 8, (p.h + 7) / 8, 1);
	ID3D11ShaderResourceView* none[1] = {};
	ID3D11UnorderedAccessView* noUav[1] = {};
	g.ctx->CSSetShaderResources(0, 1, none);
	g.ctx->CSSetUnorderedAccessViews(0, 1, noUav, nullptr);
	g.ctx->CSSetShader(nullptr, nullptr, 0);
	return tex;
}

// The processor fed a texture written by the shader, at the picture's size.
static bool ConvertPacked(Gpu& g, DXGI_FORMAT fmt, const Planes& p, float scale, bool hdrOutput, bool trueHdr, int superRes,
	int outW, int outH, Output& out, std::string& error)
{
	CComPtr<ID3D11Texture2D> packed = PackWithShader(g, fmt, p, scale, error);
	if (!packed) {
		return false;
	}
	CD3D11VP vp;
	DXGI_FORMAT o = hdrOutput ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R10G10B10A2_UNORM;
	if (FAILED(vp.InitVideoDevice(g.dev, g.ctx, g.vendor))
			|| FAILED(vp.InitVideoProcessor(fmt, p.w, p.h, Bt709(false), DEINT_Disable, hdrOutput, o))
			|| FAILED(vp.InitInputTextures(g.dev))) {
		error = "video processor failed";
		return false;
	}
	const RECT src = { 0, 0, p.w, p.h }, dst = { 0, 0, outW, outH };
	vp.SetRectangles(&src, &dst);
	vp.SetSuperRes(superRes);
	vp.SetRTXVideoHDR(trueHdr);
	D3D11_TEXTURE2D_DESC d = CreateTex2DDesc(o, outW, outH, Tex2D_DefaultRTarget);
	CComPtr<ID3D11Texture2D> rt;
	if (FAILED(g.dev->CreateTexture2D(&d, nullptr, &rt))) {
		error = "render target failed";
		return false;
	}
	for (int n = 0; n < 3; n++) {
		// The ring texture is written by the shader in the filter; here the packed
		// picture is copied into it, which is the same thing for the processor.
		g.ctx->CopyResource(vp.GetNextInputTexture(D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE), packed);
		if (FAILED(vp.Process(rt, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE, false))) {
			error = "Blt failed";
			return false;
		}
	}
	return ReadOutput(g, rt, out);
}


// Everything the filter needs to know before it can hand the processor a 4:4:4
// picture: that a shader can write one, that the processor still takes it, and
// that what only the processor does still happens on it.

// Which bind flags a picture can carry and still be handed to the video processor:
// the chroma pass needs to write it, the processor needs to read it.

// What the filter asks before it sets up HDR passthrough: can the processor take a
// PQ picture of this format and give back PQ RGB? A 4:4:4 input has to answer yes
// for the chroma pass to keep HDR video on the processor.
static void HdrConvTest(Gpu& g)
{
	CComPtr<ID3D11VideoDevice> vd;
	g.dev->QueryInterface(IID_PPV_ARGS(&vd));
	D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = { D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE, {}, 1920, 1080, {}, 1920, 1080, D3D11_VIDEO_USAGE_PLAYBACK_NORMAL };
	CComPtr<ID3D11VideoProcessorEnumerator> en;
	if (!vd || FAILED(vd->CreateVideoProcessorEnumerator(&cd, &en))) {
		printf("no video device\n");
		return;
	}
	CComQIPtr<ID3D11VideoProcessorEnumerator1> en1(en);
	if (!en1) {
		printf("no ID3D11VideoProcessorEnumerator1\n");
		return;
	}
	const struct { const char* name; DXGI_COLOR_SPACE_TYPE in; DXGI_COLOR_SPACE_TYPE out; DXGI_FORMAT outFmt; } cases[] = {
		{ "PQ -> PQ RGB10",  DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, DXGI_FORMAT_R10G10B10A2_UNORM },
		{ "SDR -> SDR RGB8", DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709,    DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,    DXGI_FORMAT_B8G8R8A8_UNORM },
		{ "SDR -> SDR RGB10",DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709,    DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,    DXGI_FORMAT_R10G10B10A2_UNORM },
	};
	const DXGI_FORMAT formats[] = { DXGI_FORMAT_NV12, DXGI_FORMAT_P010, DXGI_FORMAT_AYUV, DXGI_FORMAT_Y410, DXGI_FORMAT_Y416 };
	printf("\nConversions the processor says it supports:\n");
	for (const auto& c : cases) {
		printf("  %s\n", c.name);
		for (const DXGI_FORMAT f : formats) {
			BOOL supported = FALSE;
			const HRESULT hr = en1->CheckVideoProcessorFormatConversion(f, c.in, c.outFmt, c.out, &supported);
			printf("    %-22s %s\n", Name(f), FAILED(hr) ? "call failed" : supported ? "yes" : "no");
		}
	}
}

static void ViewTest(Gpu& g)
{
	CComPtr<ID3D11VideoDevice> vd;
	g.dev->QueryInterface(IID_PPV_ARGS(&vd));
	D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = { D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE, {}, 1920, 1080, {}, 1920, 1080, D3D11_VIDEO_USAGE_PLAYBACK_NORMAL };
	CComPtr<ID3D11VideoProcessorEnumerator> en;
	if (!vd || FAILED(vd->CreateVideoProcessorEnumerator(&cd, &en))) {
		printf("no video device\n");
		return;
	}
	const struct { const char* name; UINT flags; } binds[] = {
		{ "none",           0 },
		{ "SRV",            D3D11_BIND_SHADER_RESOURCE },
		{ "UAV",            D3D11_BIND_UNORDERED_ACCESS },
		{ "SRV|UAV",        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS },
		{ "RTV",            D3D11_BIND_RENDER_TARGET },
		{ "SRV|RTV",        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET },
		{ "SRV|RTV|UAV",    D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS },
	};
	const DXGI_FORMAT formats[] = { DXGI_FORMAT_NV12, DXGI_FORMAT_AYUV, DXGI_FORMAT_Y410 };
	printf("\nTexture bind flags, then the processor input view on it:\n");
	for (const DXGI_FORMAT fmt : formats) {
		for (const auto& bind : binds) {
			D3D11_TEXTURE2D_DESC d = {};
			d.Width = 1920;
			d.Height = 1080;
			d.MipLevels = 1;
			d.ArraySize = 1;
			d.Format = fmt;
			d.SampleDesc.Count = 1;
			d.Usage = D3D11_USAGE_DEFAULT;
			d.BindFlags = bind.flags;
			CComPtr<ID3D11Texture2D> tex;
			HRESULT hr = g.dev->CreateTexture2D(&d, nullptr, &tex);
			if (FAILED(hr)) {
				printf("  %-22s %-14s texture 0x%08X\n", Name(fmt), bind.name, (unsigned)hr);
				continue;
			}
			D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC vd2 = {};
			vd2.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
			CComPtr<ID3D11VideoProcessorInputView> view;
			const HRESULT hrView = vd->CreateVideoProcessorInputView(tex, en, &vd2, &view);
			HRESULT hrUav = S_FALSE;
			if (bind.flags & D3D11_BIND_UNORDERED_ACCESS) {
				D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
				ud.Format = DXGI_FORMAT_R32_UINT;
				ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
				CComPtr<ID3D11UnorderedAccessView> uav;
				hrUav = g.dev->CreateUnorderedAccessView(tex, &ud, &uav);
			}
			printf("  %-22s %-14s texture ok, input view %s, R32_UINT view %s\n", Name(fmt), bind.name,
			       SUCCEEDED(hrView) ? "ok" : std::format("0x{:08X}", (unsigned)hrView).c_str(),
			       hrUav == S_FALSE ? "-" : SUCCEEDED(hrUav) ? "ok" : std::format("0x{:08X}", (unsigned)hrUav).c_str());
		}
	}
}

static void PackTest(Gpu& g)
{
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	CComPtr<IWICImagingFactory> f;
	if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) {
		printf("no WIC\n");
		return;
	}
	int w = 0, h = 0;
	std::vector<float> rgb;
	const wchar_t* path = L"C:\\Users\\Bruno\\Desktop\\MPCVR-DLSS5\\tools\\dlssnr_probe\\upscale_refs\\4K1.png";
	if (!LoadHalved(f, path, w, h, rgb)) {
		printf("could not load %S\n", path);
		return;
	}
	const Planes full = ToYCbCr444(rgb, w, h);
	const Planes sub = To420(full);
	const Planes cr = CatmullRom444(sub);   // what the filter's shaders would produce

	printf("\nA shader writing the picture for the processor (%dx%d):\n", w, h);
	const struct { const char* name; DXGI_FORMAT fmt; float scale; } packs[] = {
		{ "AYUV  (8-bit)",           DXGI_FORMAT_AYUV, 1.0f },
		{ "Y410  (10-bit, x4)",      DXGI_FORMAT_Y410, 4.0f },
		{ "Y410  (10-bit, x1023/255)", DXGI_FORMAT_Y410, 1023.0f / 255.0f },
		{ "Y416  (16-bit, x257)",    DXGI_FORMAT_Y416, 257.0f },
	};
	for (const auto& pack : packs) {
		Output out;
		std::string error;
		if (!ConvertPacked(g, pack.fmt, cr, pack.scale, false, false, SUPERRES_Disable, w, h, out, error)) {
			printf("  %-24s %s\n", pack.name, error.c_str());
			continue;
		}
		const Score sc = ScoreAgainst(out, rgb);
		double gain[3], offset[3];
		Fit(out, rgb, gain, offset);
		printf("  %-24s psnr rgb %6.2f chroma %6.2f edges %6.2f   gain %.4f %.4f %.4f, offset %+.2f %+.2f %+.2f\n",
		       pack.name, sc.rgb, sc.chroma, sc.edges, gain[0], gain[1], gain[2], offset[0], offset[1], offset[2]);
	}
	{
		// The same picture staged from the CPU, to show the shader wrote what it should.
		Output staged;
		if (Convert(g, DXGI_FORMAT_AYUV, cr, staged)) {
			Output shader;
			std::string error;
			if (ConvertPacked(g, DXGI_FORMAT_AYUV, cr, 1.0f, false, false, SUPERRES_Disable, w, h, shader, error)) {
				printf("  AYUV written by the shader against the same staged from the CPU: %.4f level\n", Diff(shader, staged));
			}
		}
	}

	printf("\nRTX Video HDR, SDR in and HDR10 out, what enabling it changes (8-bit levels):\n");
	const struct { const char* name; DXGI_FORMAT fmt; float scale; } hdrs[] = {
		{ "NV12 (4:2:0, today)",  DXGI_FORMAT_NV12, 1.0f },
		{ "P010 (4:2:0, today)",  DXGI_FORMAT_P010, 4.0f },
		{ "AYUV (4:4:4)",         DXGI_FORMAT_AYUV, 1.0f },
		{ "Y410 (4:4:4)",         DXGI_FORMAT_Y410, 1023.0f / 255.0f },
	};
	for (const auto& hdr : hdrs) {
		Output off, on;
		std::string error;
		bool ok = false;
		if (hdr.fmt == DXGI_FORMAT_NV12 || hdr.fmt == DXGI_FORMAT_P010) {
			ok = Convert2(g, hdr.fmt, hdr.fmt == DXGI_FORMAT_NV12 ? sub : sub, true, false, SUPERRES_Disable, w, h, off, error)
				&& Convert2(g, hdr.fmt, sub, true, true, SUPERRES_Disable, w, h, on, error);
		} else {
			ok = ConvertPacked(g, hdr.fmt, cr, hdr.scale, true, false, SUPERRES_Disable, w, h, off, error)
				&& ConvertPacked(g, hdr.fmt, cr, hdr.scale, true, true, SUPERRES_Disable, w, h, on, error);
		}
		printf("  %-24s %s\n", hdr.name, ok ? std::format("changes the picture by {:.3f}", Diff(on, off)).c_str() : error.c_str());
	}

	printf("\nSuper Resolution, %dx%d -> %dx%d, what enabling it changes (8-bit levels):\n", w / 2, h / 2, w, h);
	{
		// A half-size picture, so that the processor enlarges it twice.
		std::vector<float> reduced((size_t)(w / 2) * (h / 2) * 3);
		for (int j = 0; j < h / 2; j++) {
			for (int i = 0; i < w / 2; i++) {
				for (int c = 0; c < 3; c++) {
					float v = 0;
					for (int dj = 0; dj < 2; dj++) {
						for (int di = 0; di < 2; di++) {
							v += rgb[((size_t)(2 * j + dj) * w + 2 * i + di) * 3 + c];
						}
					}
					reduced[((size_t)j * (w / 2) + i) * 3 + c] = v * 0.25f;
				}
			}
		}
		const Planes halfFull = ToYCbCr444(reduced, w / 2, h / 2);
		const Planes half420 = To420(halfFull);
		const Planes halfCr = CatmullRom444(half420);
		for (const auto& sr : hdrs) {
			Output off, on;
			std::string error;
			bool ok = false;
			if (sr.fmt == DXGI_FORMAT_NV12 || sr.fmt == DXGI_FORMAT_P010) {
				ok = Convert2(g, sr.fmt, half420, false, false, SUPERRES_Disable, w, h, off, error)
					&& Convert2(g, sr.fmt, half420, false, false, SUPERRES_1080p, w, h, on, error);
			} else {
				ok = ConvertPacked(g, sr.fmt, halfCr, sr.scale, false, false, SUPERRES_Disable, w, h, off, error)
					&& ConvertPacked(g, sr.fmt, halfCr, sr.scale, false, false, SUPERRES_1080p, w, h, on, error);
			}
			printf("  %-24s %s\n", sr.name, ok ? std::format("changes the picture by {:.3f}", Diff(on, off)).c_str() : error.c_str());
		}
	}
}

static void FilmTest(Gpu& g)
{
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	CComPtr<IWICImagingFactory> f;
	if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) {
		printf("no WIC\n");
		return;
	}
	const struct { const char* name; int path; DXGI_FORMAT fmt; } paths[] = {
		{ "NV12 -> VP (today)",               0, DXGI_FORMAT_NV12 },
		{ "P010 -> VP (same 4:2:0, 10-bit)",  0, DXGI_FORMAT_P010 },
		{ "Catmull-Rom -> AYUV -> VP",        1, DXGI_FORMAT_AYUV },
		{ "Catmull-Rom -> Y410 -> VP",        1, DXGI_FORMAT_Y410 },
		{ "Catmull-Rom -> Y416 -> VP",        1, DXGI_FORMAT_Y416 },
		{ "4:4:4 original -> Y416 -> VP",     2, DXGI_FORMAT_Y416 },
	};
	Score sums[std::size(paths)] = {};
	int count = 0;
	WIN32_FIND_DATAW fd = {};
	const std::wstring dir = L"C:\\Users\\Bruno\\Desktop\\MPCVR-DLSS5\\tools\\dlssnr_probe\\upscale_refs\\";
	HANDLE h = FindFirstFileW((dir + L"*.png").c_str(), &fd);
	printf("\nFilm frames halved from 4K, 4:2:0 as MPEG-2 sites it, 10-bit output (PSNR dB: rgb, chroma, chroma at luma edges):\n");
	for (BOOL more = h != INVALID_HANDLE_VALUE; more; more = FindNextFileW(h, &fd)) {
		int w = 0, hh = 0;
		std::vector<float> rgb;
		if (!LoadHalved(f, (dir + fd.cFileName).c_str(), w, hh, rgb) || w < 1800) {
			continue;
		}
		const Planes full = ToYCbCr444(rgb, w, hh);
		const Planes sub = To420(full);
		const Planes cr = CatmullRom444(sub);
		printf("  %S (%dx%d)\n", fd.cFileName, w, hh);
		for (size_t k = 0; k < std::size(paths); k++) {
			Output o;
			const Planes& in = paths[k].path == 0 ? sub : paths[k].path == 1 ? cr : full;
			if (!Convert(g, paths[k].fmt, in, o)) {
				printf("    %-32s failed\n", paths[k].name);
				continue;
			}
			const Score sc = ScoreAgainst(o, rgb);
			double gain[3], offset[3];
			Fit(o, rgb, gain, offset);
			printf("    %-32s %6.2f %6.2f %6.2f   gain %.4f %.4f %.4f, offset %+.2f %+.2f %+.2f\n", paths[k].name, sc.rgb, sc.chroma, sc.edges,
			       gain[0], gain[1], gain[2], offset[0], offset[1], offset[2]);
			sums[k].rgb += sc.rgb;
			sums[k].chroma += sc.chroma;
			sums[k].edges += sc.edges;
		}
		count++;
	}
	if (h != INVALID_HANDLE_VALUE) {
		FindClose(h);
	}
	if (count) {
		printf("  mean of %d frames:\n", count);
		for (size_t k = 0; k < std::size(paths); k++) {
			printf("    %-32s %6.2f %6.2f %6.2f\n", paths[k].name, sums[k].rgb / count, sums[k].chroma / count, sums[k].edges / count);
		}
	}
}

int wmain(int argc, wchar_t* argv[])
{
	Gpu g;
	if (!CreateDevice(g)) {
		printf("no D3D11 device\n");
		return 1;
	}
	printf("GPU: %S\n", g.name.c_str());
	if (argc > 1 && !wcscmp(argv[1], L"--film")) {
		FilmTest(g);
		return 0;
	}
	if (argc > 1 && !wcscmp(argv[1], L"--pack")) {
		PackTest(g);
		return 0;
	}
	if (argc > 1 && !wcscmp(argv[1], L"--views")) {
		ViewTest(g);
		return 0;
	}
	if (argc > 1 && !wcscmp(argv[1], L"--hdrconv")) {
		HdrConvTest(g);
		return 0;
	}

	const DXGI_FORMAT formats[] = { DXGI_FORMAT_NV12, DXGI_FORMAT_P010, DXGI_FORMAT_AYUV, DXGI_FORMAT_Y410, DXGI_FORMAT_Y416 };

	// What the processor says it takes, as input and as a render target for the shaders.
	{
		D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = { D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE, {}, 960, 540, {}, 1920, 1080, D3D11_VIDEO_USAGE_PLAYBACK_NORMAL };
		CComPtr<ID3D11VideoDevice> vd;
		g.dev->QueryInterface(IID_PPV_ARGS(&vd));
		CComPtr<ID3D11VideoProcessorEnumerator> en;
		if (vd) {
			vd->CreateVideoProcessorEnumerator(&cd, &en);
		}
		printf("\nFormat support (VP input; shaders can render into it):\n");
		for (const DXGI_FORMAT f : formats) {
			UINT flags = 0, fs = 0;
			if (en) {
				en->CheckVideoProcessorFormat(f, &flags);
			}
			g.dev->CheckFormatSupport(f, &fs);
			printf("  %-22s VP input %-3s   render target %-3s\n", Name(f),
			       (flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) ? "yes" : "no",
			       (fs & D3D11_FORMAT_SUPPORT_RENDER_TARGET) ? "yes" : "no");
		}
	}

	const Picture a = MakePicture(960, 540, 0);
	const Picture b = MakePicture(960, 540, 6);

	// Plain conversion at the same size, and the reference each 4:2:0 run is judged against.
	printf("\nConversion 960x540, no scaling (mean difference against AYUV, 8-bit levels):\n");
	RunResult ref = Run(g, DXGI_FORMAT_AYUV, { a, a, a }, false, 960, 540, SUPERRES_Disable, false, false);
	for (const DXGI_FORMAT f : formats) {
		RunResult r = Run(g, f, { a, a, a }, false, 960, 540, SUPERRES_Disable, false, false);
		if (FAILED(r.hr)) {
			printf("  %-22s failed 0x%08X\n", Name(f), (unsigned)r.hr);
			continue;
		}
		printf("  %-22s ok, %.3f\n", Name(f), Diff(r.out, ref.out));
	}

	// RTX Video Super Resolution: 960x540 to 1920x1080, with and without it.
	printf("\nSuper Resolution, 960x540 -> 1920x1080 (what enabling it changes, 8-bit levels):\n");
	for (const DXGI_FORMAT f : formats) {
		RunResult off = Run(g, f, { a, a, a }, false, 1920, 1080, SUPERRES_Disable, false, false);
		RunResult on = Run(g, f, { a, a, a }, false, 1920, 1080, SUPERRES_SD, false, false);
		if (FAILED(off.hr) || FAILED(on.hr)) {
			printf("  %-22s failed 0x%08X / 0x%08X\n", Name(f), (unsigned)off.hr, (unsigned)on.hr);
			continue;
		}
		printf("  %-22s extension 0x%08X, changes the picture by %.3f\n", Name(f), (unsigned)on.hrSuperRes, Diff(on.out, off.out));
	}

	// RTX Video HDR: SDR in, HDR10 out, with and without it.
	printf("\nRTX Video HDR, 960x540 SDR -> HDR10 (what enabling it changes, 8-bit levels of the 10-bit output):\n");
	for (const DXGI_FORMAT f : formats) {
		RunResult off = Run(g, f, { a, a, a }, false, 960, 540, SUPERRES_Disable, false, true);
		RunResult on = Run(g, f, { a, a, a }, false, 960, 540, SUPERRES_Disable, true, true);
		if (FAILED(off.hr) || FAILED(on.hr)) {
			printf("  %-22s failed 0x%08X / 0x%08X\n", Name(f), (unsigned)off.hr, (unsigned)on.hr);
			continue;
		}
		printf("  %-22s extension 0x%08X, changes the picture by %.3f\n", Name(f), (unsigned)on.hrTrueHDR, Diff(on.out, off.out));
	}

	// The same extension, in the arrangement the renderer really uses: an HDR10 output
	// with the processor left in G22/P709, which is what upstream switched to so that a
	// 10-bit SDR source would work (bbc6d1d). If the driver tone maps here where it did
	// not above, the depth was never the reason -- the passthrough was.
	printf("\nRTX Video HDR, HDR10 out but no PQ passthrough (what enabling it changes):\n");
	for (const DXGI_FORMAT f : formats) {
		RunResult off = Run(g, f, { a, a, a }, false, 960, 540, SUPERRES_Disable, false, true, 0);
		RunResult on = Run(g, f, { a, a, a }, false, 960, 540, SUPERRES_Disable, true, true, 0);
		if (FAILED(off.hr) || FAILED(on.hr)) {
			printf("  %-22s failed 0x%08X / 0x%08X\n", Name(f), (unsigned)off.hr, (unsigned)on.hr);
			continue;
		}
		printf("  %-22s extension 0x%08X, changes the picture by %.3f\n", Name(f), (unsigned)on.hrTrueHDR, Diff(on.out, off.out));
	}

	// And Super Resolution beside it, in the same arrangement: 960x540 enlarged to
	// 1920x1080, with and without. This is the pair the user asks to have together.
	printf("\nRTX Video Super Resolution, 960x540 -> 1920x1080 (what enabling it changes):\n");
	for (const DXGI_FORMAT f : formats) {
		for (const int hdr : { 0, 1 }) {
			RunResult off = Run(g, f, { a, a, a }, false, 1920, 1080, SUPERRES_Disable, hdr != 0, hdr != 0, 0);
			RunResult on = Run(g, f, { a, a, a }, false, 1920, 1080, SUPERRES_1080p, hdr != 0, hdr != 0, 0);
			if (FAILED(off.hr) || FAILED(on.hr)) {
				printf("  %-22s %-16s failed 0x%08X / 0x%08X\n", Name(f), hdr ? "with RTX HDR" : "on its own",
					(unsigned)off.hr, (unsigned)on.hr);
				continue;
			}
			printf("  %-22s %-16s extension 0x%08X, changes the picture by %.3f\n", Name(f),
				hdr ? "with RTX HDR" : "on its own", (unsigned)on.hrSuperRes, Diff(on.out, off.out));
		}
	}

	// Deinterlacing: fields from two pictures 6 pixels apart. Woven, the rows comb;
	// deinterlaced, they do not.
	printf("\nDeinterlacing, fields of two pictures 6 px apart (combing, 8-bit levels; weave shown first):\n");
	const Picture woven = Weave(a, b);
	for (const DXGI_FORMAT f : formats) {
		RunResult weave = Run(g, f, { woven }, false, 960, 540, SUPERRES_Disable, false, false);
		RunResult deint = Run(g, f, { woven, woven, woven, woven }, true, 960, 540, SUPERRES_Disable, false, false);
		if (FAILED(weave.hr) || FAILED(deint.hr)) {
			printf("  %-22s failed 0x%08X / 0x%08X\n", Name(f), (unsigned)weave.hr, (unsigned)deint.hr);
			continue;
		}
		printf("  %-22s weave %.3f, deinterlaced %.3f\n", Name(f), Combing(weave.out), Combing(deint.out));
	}
	return 0;
}
