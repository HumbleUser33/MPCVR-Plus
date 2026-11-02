// What deblocking and debanding are worth on the decoded planes, and which way of
// doing them the renderer ships (Shaders/d3d11/cs_deblock.hlsl, cs_deband.hlsl,
// Source/Clean/CleanParams.h).
//
// The renderer cleans the planes a codec decoded, each at its own size, before
// anything converts or resizes them, so that is what is measured here. Every
// reference is reduced to 1920x1080 and 960x540 and made BT.709 limited 4:2:0 planes
// -- the truth -- and then degraded the ways a source is:
//   clean       the truth at 8 bits, what an encoder is given
//   grain       film grain on the picture first
//   jpeg        a JPEG round trip through WIC at quality 0.72: blocks, ringing
//   mpeg2 qN    every 8 x 8 block of each plane quantized as an MPEG-2 intra block,
//               with the default intra matrix and quantiser_scale_code N: a DVD's
//               blocks, at the strengths a DVD or a TV channel shows them
//   +4          the same with the grid moved by four pixels each way: the blocks of a
//               cropped source, where a filter that looks for the codec's grid fails
//   banding N   the smooth parts requantized to steps of N code levels: the bands a
//               starved encoder leaves in a sky or a fade
// and each method is scored against the truth it never saw, on the planes:
//   psnr      luma and chroma together, weighted by their samples (4:1:1)
//   Y, C      the luma alone, Cb and Cr alone
//   detail    the luma on the truth's most detailed quarter, in 32-pixel tiles: what
//             a filter took from the picture that was not damage
//   flat      the luma where the truth is smooth (3 x 3 range under 3 levels): where
//             bands live
//   block     the luma's steps across the codec's grid against its steps elsewhere,
//             both ways; the truth's own figure is printed beside it
//
// --tclean runs the methods the renderer ships, at their five levels, and checks the
// shaders against a reference in double precision (the port check). --tclean --study
// runs the comparison the choice was made on: both deblocking methods and their
// variants over a sweep of strengths, the order against chroma upsampling, a sweep
// of the debanding's parameters, and what each costs.

#include "Clean/CleanParams.h"

namespace temporal {
namespace clean {

constexpr float kBlack = 16.0f / 255.0f;
constexpr float kMid = 128.0f / 255.0f;

// ------------------------------------------------------------------ planes --

struct Planes {
	int W = 0, H = 0;                 // luma; the chroma is W/2 x H/2
	std::vector<float> y, cb, cr;     // normalized code values, BT.709 limited range
	int CW() const { return W / 2; }
	int CH() const { return H / 2; }
};

// The picture's own chroma at full size, before it is subsampled: what a chroma
// upsampler is trying to rebuild.
struct Chroma444 {
	std::vector<float> cb, cr;
};

static void ToPlanes(const std::vector<float>& rgba, int W, int H, Planes& p, Chroma444* full = nullptr)
{
	p.W = W & ~1;
	p.H = H & ~1;
	std::vector<float> cb((size_t)p.W * p.H), cr((size_t)p.W * p.H);
	p.y.resize((size_t)p.W * p.H);
	for (int y = 0; y < p.H; y++) {
		for (int x = 0; x < p.W; x++) {
			const float* c = &rgba[4 * ((size_t)y * W + x)];
			const float l = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
			const size_t i = (size_t)y * p.W + x;
			p.y[i] = kBlack + (219.0f / 255.0f) * l;
			cb[i] = kMid + (224.0f / 255.0f) * (c[2] - l) / 1.8556f;
			cr[i] = kMid + (224.0f / 255.0f) * (c[0] - l) / 1.5748f;
		}
	}
	const int cw = p.CW(), ch = p.CH();
	p.cb.resize((size_t)cw * ch);
	p.cr.resize((size_t)cw * ch);
	for (int y = 0; y < ch; y++) {
		for (int x = 0; x < cw; x++) {
			const size_t a = (size_t)(2 * y) * p.W + 2 * x, b = a + p.W;
			p.cb[(size_t)y * cw + x] = 0.25f * (cb[a] + cb[a + 1] + cb[b] + cb[b + 1]);
			p.cr[(size_t)y * cw + x] = 0.25f * (cr[a] + cr[a + 1] + cr[b] + cr[b + 1]);
		}
	}
	if (full) {
		full->cb = std::move(cb);
		full->cr = std::move(cr);
	}
}

static void Round8(std::vector<float>& v)
{
	for (float& f : v) {
		f = std::clamp(std::round(f * 255.0f), 0.0f, 255.0f) / 255.0f;
	}
}

static void Round8(Planes& p)
{
	Round8(p.y);
	Round8(p.cb);
	Round8(p.cr);
}

// A chroma plane brought to the luma's size, centre-sited as ToPlanes made it:
// bilinear, or Catmull-Rom, the renderer's own default.
static void Upsample(const std::vector<float>& c, int cw, int ch, int W, int H, bool catmull, std::vector<float>& out)
{
	auto at = [&](int x, int y) { return c[(size_t)std::clamp(y, 0, ch - 1) * cw + std::clamp(x, 0, cw - 1)]; };
	auto cubic = [](float t, float w[4]) {
		const float t2 = t * t, t3 = t2 * t;
		w[0] = -0.5f * t3 + t2 - 0.5f * t;
		w[1] = 1.5f * t3 - 2.5f * t2 + 1.0f;
		w[2] = -1.5f * t3 + 2.0f * t2 + 0.5f * t;
		w[3] = 0.5f * t3 - 0.5f * t2;
	};
	out.resize((size_t)W * H);
	for (int y = 0; y < H; y++) {
		const float sy = (y + 0.5f) * ch / H - 0.5f;
		const int y0 = (int)std::floor(sy);
		const float fy = sy - y0;
		for (int x = 0; x < W; x++) {
			const float sx = (x + 0.5f) * cw / W - 0.5f;
			const int x0 = (int)std::floor(sx);
			const float fx = sx - x0;
			float v;
			if (catmull) {
				float wx[4], wy[4];
				cubic(fx, wx);
				cubic(fy, wy);
				v = 0;
				for (int j = 0; j < 4; j++) {
					float r = 0;
					for (int i = 0; i < 4; i++) {
						r += wx[i] * at(x0 - 1 + i, y0 - 1 + j);
					}
					v += wy[j] * r;
				}
			} else {
				v = (1 - fy) * ((1 - fx) * at(x0, y0) + fx * at(x0 + 1, y0))
				  + fy * ((1 - fx) * at(x0, y0 + 1) + fx * at(x0 + 1, y0 + 1));
			}
			out[(size_t)y * W + x] = v;
		}
	}
}

static void ToRgb(const Planes& p, std::vector<float>& rgba)
{
	std::vector<float> cb, cr;
	Upsample(p.cb, p.CW(), p.CH(), p.W, p.H, true, cb);
	Upsample(p.cr, p.CW(), p.CH(), p.W, p.H, true, cr);
	rgba.resize(4 * (size_t)p.W * p.H);
	for (size_t i = 0; i < p.y.size(); i++) {
		const float l = (p.y[i] - kBlack) * (255.0f / 219.0f);
		const float pb = (cb[i] - kMid) * (255.0f / 224.0f) * 1.8556f;
		const float pr = (cr[i] - kMid) * (255.0f / 224.0f) * 1.5748f;
		const float r = l + pr, b = l + pb;
		const float g = (l - 0.2126f * r - 0.0722f * b) / 0.7152f;
		float* o = &rgba[4 * i];
		o[0] = std::clamp(r, 0.0f, 1.0f);
		o[1] = std::clamp(g, 0.0f, 1.0f);
		o[2] = std::clamp(b, 0.0f, 1.0f);
		o[3] = 1.0f;
	}
}

static std::vector<float> Interleave(const std::vector<float>& a, const std::vector<float>& b)
{
	std::vector<float> o(2 * a.size());
	for (size_t i = 0; i < a.size(); i++) {
		o[2 * i] = a[i];
		o[2 * i + 1] = b[i];
	}
	return o;
}

static void Split(const std::vector<float>& ab, std::vector<float>& a, std::vector<float>& b)
{
	a.resize(ab.size() / 2);
	b.resize(ab.size() / 2);
	for (size_t i = 0; i < a.size(); i++) {
		a[i] = ab[2 * i];
		b[i] = ab[2 * i + 1];
	}
}

#include "mpeg2_intra.inl"

// The smooth parts of a plane requantized to steps of `levels` code levels: where
// the truth's 3 x 3 range stays under 4 levels. Everything else at 8 bits.
static void Band(std::vector<float>& plane, const std::vector<float>& truth, int W, int H, int levels)
{
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			float lo = 1, hi = 0;
			for (int dy = -1; dy <= 1; dy++) {
				for (int dx = -1; dx <= 1; dx++) {
					const float v = truth[(size_t)std::clamp(y + dy, 0, H - 1) * W + std::clamp(x + dx, 0, W - 1)];
					lo = std::min(lo, v);
					hi = std::max(hi, v);
				}
			}
			const size_t i = (size_t)y * W + x;
			if (hi - lo < 4.0f / 255.0f) {
				plane[i] = std::round(truth[i] * 255.0f / levels) * levels / 255.0f;
			}
		}
	}
}

// ------------------------------------------------------- the CPU references --

// vf_spp.c's grids, as the shader has them.
static const uint8_t kOffset[128][2] = {
	{0,0}, {0,0},
	{0,0}, {4,4},
	{0,0}, {2,2}, {6,4}, {4,6},
	{0,0}, {5,1}, {2,2}, {7,3}, {4,4}, {1,5}, {6,6}, {3,7},
	{0,0}, {4,0}, {1,1}, {5,1}, {3,2}, {7,2}, {2,3}, {6,3},
	{0,4}, {4,4}, {1,5}, {5,5}, {3,6}, {7,6}, {2,7}, {6,7},
	{0,0}, {0,2}, {0,4}, {0,6}, {1,1}, {1,3}, {1,5}, {1,7},
	{2,0}, {2,2}, {2,4}, {2,6}, {3,1}, {3,3}, {3,5}, {3,7},
	{4,0}, {4,2}, {4,4}, {4,6}, {5,1}, {5,3}, {5,5}, {5,7},
	{6,0}, {6,2}, {6,4}, {6,6}, {7,1}, {7,3}, {7,5}, {7,7},
	{0,0}, {4,4}, {0,4}, {4,0}, {2,2}, {6,6}, {2,6}, {6,2},
	{0,2}, {4,6}, {0,6}, {4,2}, {2,0}, {6,4}, {2,4}, {6,0},
	{1,1}, {5,5}, {1,5}, {5,1}, {3,3}, {7,7}, {3,7}, {7,3},
	{1,3}, {5,7}, {1,7}, {5,3}, {3,1}, {7,5}, {3,5}, {7,1},
	{0,1}, {4,5}, {0,5}, {4,1}, {2,3}, {6,7}, {2,7}, {6,3},
	{0,3}, {4,7}, {0,7}, {4,3}, {2,1}, {6,5}, {2,5}, {6,1},
	{1,0}, {5,4}, {1,4}, {5,0}, {3,2}, {7,6}, {3,6}, {7,2},
	{1,2}, {5,6}, {1,6}, {5,2}, {3,0}, {7,4}, {3,4}, {7,0},
};

static int Mirror(int i, int n)
{
	i = (i < 0) ? -1 - i : i;
	i = (i >= n) ? 2 * n - 1 - i : i;
	return std::clamp(i, 0, n - 1);
}

// The shader's Requantize, and whether c sat within `tie` of a threshold.
static double Requantize(double c, double t, uint32_t mode, double tie, bool& isTie)
{
	const double a = std::abs(c);
	if (std::abs(a - t) < tie || (mode == Clean::DeblockMedium && std::abs(a - 2 * t) < tie)) {
		isTie = true;
	}
	const double s = c > 0 ? 1.0 : c < 0 ? -1.0 : 0.0;
	if (mode == Clean::DeblockSoft) {
		return a > t ? c - s * t : 0.0;
	}
	if (mode == Clean::DeblockMedium) {
		return a > 2 * t ? c : a > t ? 2 * (c - s * t) : 0.0;
	}
	return a > t ? c : 0.0;
}

// One channel of a plane. ties, if given, marks the pixels whose result hangs on a
// coefficient within float error of its threshold.
static void SppCpu(const std::vector<float>& in, int W, int H, int shifts, float qp, uint32_t mode,
                   std::vector<float>& out, std::vector<uint8_t>* ties = nullptr)
{
	const Dct8& d = Dct();
	const double t = 2.0 * std::max(qp, 0.0625f) - 0.125;
	std::vector<double> acc((size_t)W * H, 0.0);
	if (ties) {
		ties->assign((size_t)W * H, 0);
	}
	for (int s = 0; s < shifts; s++) {
		const int ox = kOffset[shifts + s][0], oy = kOffset[shifts + s][1];
		for (int by = oy - 8; by < H; by += 8) {
			for (int bx = ox - 8; bx < W; bx += 8) {
				double blk[64], f[64], r[64];
				for (int j = 0; j < 8; j++) {
					for (int i = 0; i < 8; i++) {
						blk[j * 8 + i] = in[(size_t)Mirror(by + j, H) * W + Mirror(bx + i, W)] * 255.0;
					}
				}
				d.Forward(blk, f);
				bool tie = false;
				for (int k = 1; k < 64; k++) {
					f[k] = Requantize(f[k], t, mode, 1e-3, tie);
				}
				d.Inverse(f, r);
				for (int j = 0; j < 8; j++) {
					for (int i = 0; i < 8; i++) {
						const int x = bx + i, y = by + j;
						if (x >= 0 && x < W && y >= 0 && y < H) {
							acc[(size_t)y * W + x] += r[j * 8 + i];
							if (tie && ties) {
								(*ties)[(size_t)y * W + x] = 1;
							}
						}
					}
				}
			}
		}
	}
	out.resize(acc.size());
	for (size_t i = 0; i < acc.size(); i++) {
		out[i] = (float)std::clamp(acc[i] / shifts / 255.0, 0.0, 1.0);
	}
}

static void Even7(const double w[7], double o[4])
{
	const double s0 = w[0] + w[6], s1 = w[1] + w[5], s2 = w[2] + w[4];
	const double s = w[3] + w[3];
	const double t3 = s - s0, t0 = s + s0, ss = s2 + s1, t2 = s2 - s1;
	o[0] = t0 + ss;
	o[2] = t0 - ss;
	o[1] = 2 * t3 + t2;
	o[3] = t3 - 2 * t2;
}

static void Pp7Cpu(const std::vector<float>& in, int W, int H, float qp, uint32_t mode,
                   std::vector<float>& out, std::vector<uint8_t>* ties = nullptr)
{
	static const double kN[4] = { 4, 5, 4, 10 };
	static const double kSN[4] = { 2, 3.16227766017, 2, 3.16227766017 };
	const double q = std::max(1.0f, qp);
	out.resize((size_t)W * H);
	if (ties) {
		ties->assign((size_t)W * H, 0);
	}
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			double v[7][4];
			for (int c = 0; c < 7; c++) {
				double col[7];
				for (int r = 0; r < 7; r++) {
					col[r] = in[(size_t)Mirror(y - 3 + r, H) * W + Mirror(x - 3 + c, W)] * 255.0;
				}
				Even7(col, v[c]);
			}
			double a = 0;
			bool tie = false;
			for (int k = 0; k < 4; k++) {
				double row[7], h[4];
				for (int c = 0; c < 7; c++) {
					row[c] = v[c][k];
				}
				Even7(row, h);
				for (int j = 0; j < 4; j++) {
					const double weight = 1.0 / (kN[j] * kN[k]);
					a += (j == 0 && k == 0) ? h[j] * weight
						: Requantize(h[j], kSN[j] * kSN[k] * q * 4 - 1, mode, 1e-2, tie) * weight;
				}
			}
			out[(size_t)y * W + x] = (float)std::clamp(a * 0.25 / 255.0, 0.0, 1.0);
			if (ties && tie) {
				(*ties)[(size_t)y * W + x] = 1;
			}
		}
	}
}

// pcg3d, as the shader and libplacebo have it.
static void Pcg3d(uint32_t s[3], double r[3])
{
	for (int k = 0; k < 3; k++) {
		s[k] = 1664525u * s[k] + 1013904223u;
	}
	s[0] += s[1] * s[2];
	s[1] += s[2] * s[0];
	s[2] += s[0] * s[1];
	for (int k = 0; k < 3; k++) {
		s[k] ^= s[k] >> 16u;
	}
	s[0] += s[1] * s[2];
	s[1] += s[2] * s[0];
	s[2] += s[0] * s[1];
	for (int k = 0; k < 3; k++) {
		r[k] = (double)(float)((float)s[k] * (1.0f / 4294967295.0f));
	}
}

// channels 1 or 2, interleaved. A pixel whose sample lands within float error of a
// texel's edge, or whose difference sits on the threshold, is marked a tie.
static void DebandCpu(const std::vector<float>& in, int W, int H, int channels, const Clean::Constants& c,
                      std::vector<float>& out, std::vector<uint8_t>* ties = nullptr)
{
	out.resize(in.size());
	if (ties) {
		ties->assign((size_t)W * H, 0);
	}
	const double t = c.threshold / 1000.0;
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			uint32_t st[3] = { (uint32_t)x, (uint32_t)y, c.seed };
			double res[2] = {}, r[3];
			for (int k = 0; k < channels; k++) {
				res[k] = in[((size_t)y * W + x) * channels + k];
			}
			bool tie = false;
			for (uint32_t i = 1; i <= c.iterations; i++) {
				Pcg3d(st, r);
				const double dist = r[0] * (i * (double)c.radius), angle = r[1] * 6.28318530717958647692;
				const double dx = dist * std::cos(angle), dy = dist * std::sin(angle);
				const double sx[4] = { +dx, -dx, -dx, +dx }, sy[4] = { +dy, +dy, -dy, -dy };
				double avg[2] = {};
				for (int n = 0; n < 4; n++) {
					const double fx = x + 0.5 + sx[n], fy = y + 0.5 + sy[n];
					if (std::abs(fx - std::round(fx)) < 1e-3 || std::abs(fy - std::round(fy)) < 1e-3) {
						tie = true;
					}
					const int qx = std::clamp((int)std::floor(fx), 0, W - 1), qy = std::clamp((int)std::floor(fy), 0, H - 1);
					for (int k = 0; k < channels; k++) {
						avg[k] += in[((size_t)qy * W + qx) * channels + k];
					}
				}
				for (int k = 0; k < channels; k++) {
					avg[k] *= 0.25;
					const double diff = std::abs(res[k] - avg[k]);
					if (std::abs(diff - t / i) < 1e-5) {
						tie = true;
					}
					res[k] = diff > t / i ? res[k] : avg[k];
				}
			}
			if (c.grain > 0) {
				Pcg3d(st, r);
				for (int k = 0; k < channels; k++) {
					const double strength = std::min(std::abs(res[k] - c.neutral[k]), c.grain / 1000.0);
					res[k] += strength * (r[k] - 0.5);
				}
			}
			for (int k = 0; k < channels; k++) {
				out[((size_t)y * W + x) * channels + k] = (float)std::clamp(res[k], 0.0, 1.0);
			}
			if (ties && tie) {
				(*ties)[(size_t)y * W + x] = 1;
			}
		}
	}
}

// --------------------------------------------------------------------- GPU --

struct ShaderKey {
	bool deband = false;
	int mode = 1;        // deblock: 0 SPP, 1 PP7
	int shifts = 8;      // SPP
	int channels = 1;
	bool operator<(const ShaderKey& o) const
	{
		return std::tie(deband, mode, shifts, channels) < std::tie(o.deband, o.mode, o.shifts, o.channels);
	}
};

class CCleanGpu
{
public:
	bool Init(ID3D11Device* dev, ID3D11DeviceContext* ctx)
	{
		m_dev = dev;
		m_ctx = ctx;
		D3D11_BUFFER_DESC d = {};
		d.ByteWidth = sizeof(Clean::Constants);
		d.Usage = D3D11_USAGE_DYNAMIC;
		d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		return SUCCEEDED(dev->CreateBuffer(&d, nullptr, &m_cb));
	}

	ID3D11ComputeShader* Shader(const ShaderKey& k, std::string& error)
	{
		auto it = m_shaders.find(k);
		if (it != m_shaders.end()) {
			return it->second;
		}
		char mode[8], shifts[8], channels[8];
		sprintf_s(mode, "%d", k.mode);
		sprintf_s(shifts, "%d", k.shifts);
		sprintf_s(channels, "%d", k.channels);
		const D3D_SHADER_MACRO defines[] = {
			{ "MODE", mode }, { "SHIFTS", shifts }, { "CHANNELS", channels }, { nullptr, nullptr }
		};
		CComPtr<ID3DBlob> code, errors;
		const HRESULT hr = D3DCompileFromFile(k.deband ? L"..\\..\\Shaders\\d3d11\\cs_deband.hlsl" : L"..\\..\\Shaders\\d3d11\\cs_deblock.hlsl",
			defines, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
		if (FAILED(hr)) {
			error = errors ? std::string((const char*)errors->GetBufferPointer(), errors->GetBufferSize())
			               : std::format("cannot compile (0x{:08X})", (unsigned)hr);
			return nullptr;
		}
		CComPtr<ID3D11ComputeShader> cs;
		if (FAILED(m_dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs))) {
			error = "CreateComputeShader";
			return nullptr;
		}
		m_shaders[k] = cs;
		return cs;
	}

	// One dispatch on a plane of 1 or 2 channels, interleaved, normalized.
	bool Run(ID3D11ComputeShader* cs, const Clean::Constants& c, const std::vector<float>& in, int channels, std::vector<float>& out)
	{
		const int W = (int)c.width, H = (int)c.height;
		if (!Prepare(W, H, channels)) {
			return false;
		}
		std::vector<uint16_t> words(in.size());
		for (size_t i = 0; i < in.size(); i++) {
			words[i] = (uint16_t)std::lround(std::clamp(in[i], 0.0f, 1.0f) * 65535.0f);
		}
		m_ctx->UpdateSubresource(m_in, 0, nullptr, words.data(), (UINT)(2 * channels * W), 0);
		Dispatch(cs, c);
		m_ctx->CopyResource(m_stage, m_out);
		D3D11_MAPPED_SUBRESOURCE mr = {};
		if (FAILED(m_ctx->Map(m_stage, 0, D3D11_MAP_READ, 0, &mr))) {
			return false;
		}
		out.resize(in.size());
		for (int y = 0; y < H; y++) {
			const uint16_t* row = (const uint16_t*)((const BYTE*)mr.pData + (size_t)mr.RowPitch * y);
			for (int x = 0; x < W * channels; x++) {
				out[(size_t)y * W * channels + x] = row[x] / 65535.0f;
			}
		}
		m_ctx->Unmap(m_stage, 0);
		return true;
	}

	// Milliseconds a dispatch, on a plane of this size, after warming up.
	double Cost(ID3D11ComputeShader* cs, const Clean::Constants& c, int channels)
	{
		if (!Prepare((int)c.width, (int)c.height, channels)) {
			return 0;
		}
		auto sync = [&]() {
			m_ctx->CopyResource(m_stage, m_out);
			D3D11_MAPPED_SUBRESOURCE mr = {};
			if (SUCCEEDED(m_ctx->Map(m_stage, 0, D3D11_MAP_READ, 0, &mr))) {
				m_ctx->Unmap(m_stage, 0);
			}
		};
		for (int i = 0; i < 32; i++) {
			Dispatch(cs, c);
		}
		sync();
		const int runs = 64;
		LARGE_INTEGER freq = {}, t0 = {}, t1 = {};
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&t0);
		for (int i = 0; i < runs; i++) {
			Dispatch(cs, c);
		}
		sync();
		QueryPerformanceCounter(&t1);
		return 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart / runs;
	}

private:
	bool Prepare(int W, int H, int channels)
	{
		if (m_in && m_W == W && m_H == H && m_channels == channels) {
			return true;
		}
		m_in.Release(); m_out.Release(); m_stage.Release(); m_srv.Release(); m_uav.Release();
		D3D11_TEXTURE2D_DESC d = {};
		d.Width = W;
		d.Height = H;
		d.MipLevels = 1;
		d.ArraySize = 1;
		d.Format = channels == 2 ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R16_UNORM;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		if (FAILED(m_dev->CreateTexture2D(&d, nullptr, &m_in)) || FAILED(m_dev->CreateShaderResourceView(m_in, nullptr, &m_srv))) {
			return false;
		}
		d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		if (FAILED(m_dev->CreateTexture2D(&d, nullptr, &m_out)) || FAILED(m_dev->CreateUnorderedAccessView(m_out, nullptr, &m_uav))) {
			return false;
		}
		d.Usage = D3D11_USAGE_STAGING;
		d.BindFlags = 0;
		d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (FAILED(m_dev->CreateTexture2D(&d, nullptr, &m_stage))) {
			return false;
		}
		m_W = W;
		m_H = H;
		m_channels = channels;
		return true;
	}

	void Dispatch(ID3D11ComputeShader* cs, const Clean::Constants& c)
	{
		D3D11_MAPPED_SUBRESOURCE mr = {};
		if (SUCCEEDED(m_ctx->Map(m_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr))) {
			memcpy(mr.pData, &c, sizeof(c));
			m_ctx->Unmap(m_cb, 0);
		}
		ID3D11ShaderResourceView* srv = m_srv;
		ID3D11UnorderedAccessView* uav = m_uav;
		ID3D11Buffer* cb = m_cb;
		m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
		m_ctx->CSSetShader(cs, nullptr, 0);
		m_ctx->CSSetShaderResources(0, 1, &srv);
		m_ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		m_ctx->CSSetConstantBuffers(0, 1, &cb);
		m_ctx->Dispatch((c.width + 15) / 16, (c.height + 15) / 16, 1);
		ID3D11ShaderResourceView* noSrv = nullptr;
		ID3D11UnorderedAccessView* noUav = nullptr;
		m_ctx->CSSetShaderResources(0, 1, &noSrv);
		m_ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
	}

	ID3D11Device* m_dev = nullptr;
	ID3D11DeviceContext* m_ctx = nullptr;
	CComPtr<ID3D11Buffer> m_cb;
	std::map<ShaderKey, CComPtr<ID3D11ComputeShader>> m_shaders;
	CComPtr<ID3D11Texture2D> m_in, m_out, m_stage;
	CComPtr<ID3D11ShaderResourceView> m_srv;
	CComPtr<ID3D11UnorderedAccessView> m_uav;
	int m_W = 0, m_H = 0, m_channels = 0;
};

// ----------------------------------------------------------------- methods --

struct Deblocker {
	const char* name;
	int mode;            // 0 SPP, 1 PP7
	int shifts;
	uint32_t threshold;  // hard, soft, medium
};

// Every combination the study weighs.
static std::vector<Deblocker> StudyDeblockers()
{
	std::vector<Deblocker> v;
	static const char* tn[3] = { "hard", "soft", "medium" };
	static char names[32][32];
	int n = 0;
	for (uint32_t t = 0; t < 3; t++) {
		sprintf_s(names[n], "PP7 %s", tn[t]);
		v.push_back({ names[n++], 1, 0, t });
	}
	for (const int s : { 4, 8, 16, 64 }) {
		for (uint32_t t = 0; t < 3; t++) {
			sprintf_s(names[n], "SPP-%d %s", s, tn[t]);
			v.push_back({ names[n++], 0, s, t });
		}
	}
	return v;
}

static Clean::Constants DeblockC(int W, int H, float qp, uint32_t mode)
{
	Clean::Constants c = {};
	c.width = W;
	c.height = H;
	c.threshold = qp;
	c.mode = mode;
	return c;
}

// The method on both planes of a 4:2:0 picture: luma alone, Cb and Cr together.
static bool DeblockPlanes(CCleanGpu& gpu, const Deblocker& m, float qp, const Planes& in, Planes& out, std::string& error)
{
	ID3D11ComputeShader* cs1 = gpu.Shader({ false, m.mode, m.shifts ? m.shifts : 8, 1 }, error);
	ID3D11ComputeShader* cs2 = gpu.Shader({ false, m.mode, m.shifts ? m.shifts : 8, 2 }, error);
	if (!cs1 || !cs2) {
		return false;
	}
	out.W = in.W;
	out.H = in.H;
	std::vector<float> cbcr;
	if (!gpu.Run(cs1, DeblockC(in.W, in.H, qp, m.threshold), in.y, 1, out.y)
			|| !gpu.Run(cs2, DeblockC(in.CW(), in.CH(), qp, m.threshold), Interleave(in.cb, in.cr), 2, cbcr)) {
		error = "dispatch";
		return false;
	}
	Split(cbcr, out.cb, out.cr);
	return true;
}

static bool DebandPlanes(CCleanGpu& gpu, const Clean::DebandLevel& l, uint32_t seed, const Planes& in, Planes& out, std::string& error)
{
	ID3D11ComputeShader* cs1 = gpu.Shader({ true, 0, 0, 1 }, error);
	ID3D11ComputeShader* cs2 = gpu.Shader({ true, 0, 0, 2 }, error);
	if (!cs1 || !cs2) {
		return false;
	}
	auto constants = [&](int W, int H, float neutral) {
		Clean::Constants c = {};
		c.width = W;
		c.height = H;
		c.seed = seed;
		c.iterations = l.iterations;
		c.threshold = l.threshold;
		c.radius = l.radius;
		c.grain = l.grain;
		c.neutral[0] = c.neutral[1] = neutral;
		return c;
	};
	out.W = in.W;
	out.H = in.H;
	std::vector<float> cbcr;
	if (!gpu.Run(cs1, constants(in.W, in.H, kBlack), in.y, 1, out.y)
			|| !gpu.Run(cs2, constants(in.CW(), in.CH(), kMid), Interleave(in.cb, in.cr), 2, cbcr)) {
		error = "dispatch";
		return false;
	}
	Split(cbcr, out.cb, out.cr);
	return true;
}

// ----------------------------------------------------------------- metrics --

// What is computed once per truth: where it is smooth, and its most detailed tiles.
struct TruthInfo {
	std::vector<uint8_t> flat;              // luma 3 x 3 range under 3 levels
	std::vector<std::pair<int, int>> detail;   // the most detailed quarter's 32-pixel tiles
};

static TruthInfo Analyse(const Planes& t)
{
	TruthInfo info;
	const int W = t.W, H = t.H;
	info.flat.assign((size_t)W * H, 0);
	for (int y = 1; y < H - 1; y++) {
		for (int x = 1; x < W - 1; x++) {
			float lo = 1, hi = 0;
			for (int dy = -1; dy <= 1; dy++) {
				for (int dx = -1; dx <= 1; dx++) {
					const float v = t.y[(size_t)(y + dy) * W + x + dx];
					lo = std::min(lo, v);
					hi = std::max(hi, v);
				}
			}
			info.flat[(size_t)y * W + x] = (hi - lo) < 3.0f / 255.0f;
		}
	}
	const int tile = 32;
	std::vector<std::tuple<double, int, int>> tiles;
	for (int ty = 0; ty + tile <= H; ty += tile) {
		for (int tx = 0; tx + tile <= W; tx += tile) {
			double e = 0;
			for (int y = ty + 1; y < ty + tile; y++) {
				for (int x = tx + 1; x < tx + tile; x++) {
					const size_t i = (size_t)y * W + x;
					e += std::abs(t.y[i] - t.y[i - 1]) + std::abs(t.y[i] - t.y[i - W]);
				}
			}
			tiles.emplace_back(e, tx, ty);
		}
	}
	std::sort(tiles.begin(), tiles.end(), [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b); });
	const size_t keep = std::max<size_t>(1, tiles.size() / 4);
	for (size_t k = 0; k < keep && k < tiles.size(); k++) {
		info.detail.emplace_back(std::get<1>(tiles[k]), std::get<2>(tiles[k]));
	}
	return info;
}

struct Score {
	double mseY = 0, mseC = 0, mseDetail = 0, mseFlat = 0, block = 0;
	double psnr = 0, psnrY = 0, psnrC = 0, detail = 0, flat = 0;
	int n = 0;
	void Add(const Score& s)
	{
		psnr += s.psnr; psnrY += s.psnrY; psnrC += s.psnrC; detail += s.detail; flat += s.flat; block += s.block;
		n++;
	}
	Score Mean() const
	{
		Score m = *this;
		if (n) {
			m.psnr /= n; m.psnrY /= n; m.psnrC /= n; m.detail /= n; m.flat /= n; m.block /= n;
		}
		return m;
	}
};

static double Psnr(double mse) { return mse > 0 ? 10.0 * std::log10(1.0 / mse) : 99.0; }

// Steps across the grid at (gx, gy) mod 8 against steps elsewhere, both ways, averaged.
static double Blockiness(const std::vector<float>& y, int W, int H, int gx, int gy)
{
	double on[2] = {}, off[2] = {};
	long long nOn[2] = {}, nOff[2] = {};
	for (int j = 0; j < H; j++) {
		for (int i = 0; i < W; i++) {
			const float v = y[(size_t)j * W + i];
			if (i > 0) {
				const double s = std::abs(v - y[(size_t)j * W + i - 1]);
				if (((i - gx) & 7) == 0) { on[0] += s; nOn[0]++; } else { off[0] += s; nOff[0]++; }
			}
			if (j > 0) {
				const double s = std::abs(v - y[(size_t)(j - 1) * W + i]);
				if (((j - gy) & 7) == 0) { on[1] += s; nOn[1]++; } else { off[1] += s; nOff[1]++; }
			}
		}
	}
	double r = 0;
	for (int k = 0; k < 2; k++) {
		r += (nOn[k] && nOff[k] && off[k] > 0) ? (on[k] / nOn[k]) / (off[k] / nOff[k]) : 0;
	}
	return r / 2;
}

static Score ScorePlanes(const Planes& o, const Planes& t, const TruthInfo& info, int gx, int gy)
{
	Score s;
	double se = 0, sf = 0, sd = 0;
	long long nf = 0, nd = 0;
	for (size_t i = 0; i < t.y.size(); i++) {
		const double d = (double)o.y[i] - t.y[i];
		se += d * d;
		if (info.flat[i]) {
			sf += d * d;
			nf++;
		}
	}
	for (const auto& tl : info.detail) {
		for (int y = tl.second; y < tl.second + 32; y++) {
			for (int x = tl.first; x < tl.first + 32; x++) {
				const double d = (double)o.y[(size_t)y * t.W + x] - t.y[(size_t)y * t.W + x];
				sd += d * d;
				nd++;
			}
		}
	}
	double sc = 0;
	for (size_t i = 0; i < t.cb.size(); i++) {
		const double a = (double)o.cb[i] - t.cb[i], b = (double)o.cr[i] - t.cr[i];
		sc += a * a + b * b;
	}
	s.mseY = se / t.y.size();
	s.mseC = sc / (2.0 * t.cb.size());
	s.psnr = Psnr((se + sc) / (t.y.size() + 2.0 * t.cb.size()));
	s.psnrY = Psnr(s.mseY);
	s.psnrC = Psnr(s.mseC);
	s.detail = Psnr(nd ? sd / nd : 0);
	s.flat = Psnr(nf ? sf / nf : 0);
	s.block = Blockiness(o.y, o.W, o.H, gx, gy);
	return s;
}

static void PrintHead(const char* first, const char* second)
{
	Out("  %-22s %8s %8s %8s %8s %8s %8s %7s\n", first, second, "psnr", "Y", "C", "detail", "flat", "block");
}

static void PrintRow(const char* first, const char* second, const Score& s)
{
	Out("  %-22s %8s %8.3f %8.3f %8.3f %8.3f %8.3f %7.3f\n", first, second, s.psnr, s.psnrY, s.psnrC, s.detail, s.flat, s.block);
}

// ---------------------------------------------------------------- the set --

struct Case {
	const char* name;
	enum Kind { CleanPic, Grain, Jpeg, Mpeg2, Banding } kind;
	int q;      // mpeg2's quantiser_scale_code, banding's step
	int grid;   // where the blocks sit, mod 8
};

static const Case kCases[] = {
	{ "clean",       Case::CleanPic, 0,  0 },
	{ "grain",       Case::Grain,    0,  0 },
	{ "jpeg 0.72",   Case::Jpeg,     0,  0 },
	{ "mpeg2 q2",    Case::Mpeg2,    2,  0 },
	{ "mpeg2 q4",    Case::Mpeg2,    4,  0 },
	{ "mpeg2 q8",    Case::Mpeg2,    8,  0 },
	{ "mpeg2 q12",   Case::Mpeg2,    12, 0 },
	{ "mpeg2 q16",   Case::Mpeg2,    16, 0 },
	{ "mpeg2 q8 +4", Case::Mpeg2,    8,  4 },
	{ "banding 2",   Case::Banding,  2,  0 },
	{ "banding 4",   Case::Banding,  4,  0 },
};

struct Picture {
	std::wstring name;
	Planes truth;
	Chroma444 truth444;
	TruthInfo info;
	std::vector<Planes> cases;     // one per kCases entry
};

static bool MakePicture(IWICImagingFactory* factory, const std::vector<float>& rgba, int W, int H, Picture& p, std::string& error)
{
	ToPlanes(rgba, W, H, p.truth, &p.truth444);
	p.info = Analyse(p.truth);
	p.cases.resize(std::size(kCases));
	for (size_t k = 0; k < std::size(kCases); k++) {
		const Case& c = kCases[k];
		Planes& d = p.cases[k];
		switch (c.kind) {
		case Case::Grain: {
			std::vector<float> g = rgba;
			AddGrain(g, W, H, 0.006f, 1234u);
			ToPlanes(g, W, H, d);
			Round8(d);
			break;
		}
		case Case::Jpeg: {
			std::vector<float> j = rgba;
			if (!JpegRoundTrip(factory, j, W, H, 0.72f, error)) {
				return false;
			}
			ToPlanes(j, W, H, d);
			Round8(d);
			break;
		}
		case Case::Mpeg2:
			d = p.truth;
			Round8(d);
			Mpeg2Intra(d.y, d.W, d.H, c.q, c.grid, c.grid);
			Mpeg2Intra(d.cb, d.CW(), d.CH(), c.q, c.grid / 2, c.grid / 2);
			Mpeg2Intra(d.cr, d.CW(), d.CH(), c.q, c.grid / 2, c.grid / 2);
			break;
		case Case::Banding:
			d = p.truth;
			Round8(d);
			Band(d.y, p.truth.y, d.W, d.H, c.q);
			Band(d.cb, p.truth.cb, d.CW(), d.CH(), c.q);
			Band(d.cr, p.truth.cr, d.CW(), d.CH(), c.q);
			break;
		default:
			d = p.truth;
			Round8(d);
			break;
		}
	}
	return true;
}

// ------------------------------------------------------------- port check --

// The shader against the reference in double precision, on a crop: within three
// steps of R16 everywhere a threshold did not decide by a hair.
static bool PortCheck(CCleanGpu& gpu, const char* what, const ShaderKey& k, const Clean::Constants& c,
                      const std::vector<float>& in, int channels)
{
	std::string error;
	ID3D11ComputeShader* cs = gpu.Shader(k, error);
	if (!cs) {
		Out("  %-34s cannot compile: %s\n", what, error.c_str());
		return false;
	}
	std::vector<float> gpuOut, cpuOut;
	if (!gpu.Run(cs, c, in, channels, gpuOut)) {
		Out("  %-34s dispatch failed\n", what);
		return false;
	}
	const int W = (int)c.width, H = (int)c.height;
	std::vector<uint8_t> ties;
	if (k.deband) {
		DebandCpu(in, W, H, channels, c, cpuOut, &ties);
	} else {
		cpuOut.resize(in.size());
		std::vector<uint8_t> tiesK;
		ties.assign((size_t)W * H, 0);
		for (int ch = 0; ch < channels; ch++) {
			std::vector<float> one((size_t)W * H), res;
			for (size_t i = 0; i < one.size(); i++) {
				one[i] = in[i * channels + ch];
			}
			if (k.mode == 0) {
				SppCpu(one, W, H, k.shifts, c.threshold, c.mode, res, &tiesK);
			} else {
				Pp7Cpu(one, W, H, c.threshold, c.mode, res, &tiesK);
			}
			for (size_t i = 0; i < one.size(); i++) {
				cpuOut[i * channels + ch] = res[i];
				ties[i] |= tiesK[i];
			}
		}
	}
	const double tol = 3.0 / 65535.0;
	double worst = 0;
	long long bad = 0, tied = 0;
	for (size_t i = 0; i < ties.size(); i++) {
		if (ties[i]) {
			tied++;
			continue;
		}
		for (int ch = 0; ch < channels; ch++) {
			const double d = std::abs((double)gpuOut[i * channels + ch] - cpuOut[i * channels + ch]);
			worst = std::max(worst, d);
			bad += d > tol;
		}
	}
	const bool ok = bad == 0;
	Out("  %-34s %s  max %.2f R16 steps, %lld over 3, %lld of %zu pixels set aside as ties\n",
		what, ok ? "ok  " : "FAIL", worst * 65535.0, bad, tied, ties.size());
	char msg[96];
	sprintf_s(msg, "%s matches its reference", what);
	Check(ok, msg);
	return ok;
}

// ------------------------------------------------------------ real encodes --

// A raw yuv420p (8 bits) or yuv420p10le picture, normalized.
static bool LoadYuv(const std::wstring& path, int W, int H, int bits, Planes& p)
{
	FILE* f = nullptr;
	if (_wfopen_s(&f, path.c_str(), L"rb") || !f) {
		return false;
	}
	p.W = W;
	p.H = H;
	const size_t ny = (size_t)W * H, nc = (size_t)(W / 2) * (H / 2);
	const int bytes = bits > 8 ? 2 : 1;
	std::vector<uint8_t> raw((ny + 2 * nc) * bytes);
	const bool ok = fread(raw.data(), 1, raw.size(), f) == raw.size();
	fclose(f);
	if (!ok) {
		return false;
	}
	const float scale = bits > 8 ? 1.0f / 1023.0f : 1.0f / 255.0f;
	auto at = [&](size_t i) { return bytes == 2 ? (float)(raw[2 * i] | (raw[2 * i + 1] << 8)) : (float)raw[i]; };
	p.y.resize(ny);
	p.cb.resize(nc);
	p.cr.resize(nc);
	for (size_t i = 0; i < ny; i++) {
		p.y[i] = at(i) * scale;
	}
	for (size_t i = 0; i < nc; i++) {
		p.cb[i] = at(ny + i) * scale;
		p.cr[i] = at(ny + nc + i) * scale;
	}
	return true;
}

// As yuv420p10le, for CAMBI: a cleaned picture keeps more than 8 bits.
static bool SaveYuv10(const std::wstring& path, const Planes& p)
{
	FILE* f = nullptr;
	if (_wfopen_s(&f, path.c_str(), L"wb") || !f) {
		return false;
	}
	auto put = [&](const std::vector<float>& v) {
		std::vector<uint16_t> w(v.size());
		for (size_t i = 0; i < v.size(); i++) {
			w[i] = (uint16_t)std::lround(std::clamp(v[i], 0.0f, 1.0f) * 1023.0f);
		}
		fwrite(w.data(), 2, w.size(), f);
	};
	put(p.y);
	put(p.cb);
	put(p.cr);
	fclose(f);
	return true;
}

// The real encodes clean_refs.py made, FFmpeg's filters on them, and ours.
static int RunFiles(CCleanGpu& gpu, const std::wstring& dir)
{
	struct FileCase {
		std::string name, encoder;
		int W = 0, H = 0, bits = 8;
		std::wstring truth, decoded;
		std::vector<std::pair<std::string, std::wstring>> anchors;
	};
	std::vector<FileCase> cases;
	{
		FILE* f = nullptr;
		if (_wfopen_s(&f, (dir + L"\\manifest.txt").c_str(), L"r") || !f) {
			Out("  no %ls\\manifest.txt: run clean_refs.py first\n", dir.c_str());
			return 1;
		}
		char line[1024];
		auto wide = [&](const char* s) { return dir + L"\\" + std::wstring(s, s + strlen(s)); };
		while (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n")] = 0;
			std::vector<std::string> p;
			for (char* ctx = nullptr, *t = strtok_s(line, "\t", &ctx); t; t = strtok_s(nullptr, "\t", &ctx)) {
				p.push_back(t);
			}
			if (p.size() == 7 && p[0] == "case") {
				FileCase c;
				c.name = p[1];
				c.encoder = p[1].substr(p[1].find('.') + 1);
				c.W = atoi(p[2].c_str());
				c.H = atoi(p[3].c_str());
				c.bits = atoi(p[4].c_str());
				c.truth = wide(p[5].c_str());
				c.decoded = wide(p[6].c_str());
				cases.push_back(std::move(c));
			} else if (p.size() == 4 && p[0] == "anchor" && !cases.empty() && cases.back().name == p[1]) {
				cases.back().anchors.emplace_back(p[2], wide(p[3].c_str()));
			}
		}
		fclose(f);
	}
	Out("\n6. Real encodes (clean_refs.py): %zu decoded pictures. Each method's mean per\n", cases.size());
	Out("   encoder; FFmpeg's filters at each of their settings, ours over a sweep (best qp)\n");
	Out("   and at the shipped levels. block is printed on the 8-pixel grid the MPEG codecs use.\n");

	CreateDirectoryW((dir + L"\\out").c_str(), nullptr);
	FILE* cambi = nullptr;
	_wfopen_s(&cambi, (dir + L"\\out\\cambi.txt").c_str(), L"w");

	static const float kQp[] = { 1, 1.5f, 2, 3, 4, 5, 6, 8, 10, 12, 16 };
	const Deblocker ours[] = {
		{ "PP7 medium", 1, 8, Clean::DeblockMedium },
		{ "PP7 soft", 1, 8, Clean::DeblockSoft },
		{ "SPP-8 medium", 0, 8, Clean::DeblockMedium },
		{ "SPP-8 soft", 0, 8, Clean::DeblockSoft },
		{ "SPP-16 hard", 0, 16, Clean::DeblockHard },
	};
	// [encoder][method label] -> scores
	std::map<std::string, std::map<std::string, Score>> table;
	std::map<std::string, std::vector<std::string>> order;
	auto add = [&](const std::string& enc, const std::string& label, const Score& s) {
		auto& m = table[enc];
		if (m.find(label) == m.end()) {
			order[enc].push_back(label);
		}
		m[label].Add(s);
	};
	std::string error;
	for (const FileCase& c : cases) {
		Planes truth, dec;
		if (!LoadYuv(c.truth, c.W, c.H, 8, truth) || !LoadYuv(c.decoded, c.W, c.H, c.bits, dec)) {
			Out("  %s: cannot read\n", c.name.c_str());
			continue;
		}
		const TruthInfo info = Analyse(truth);
		add(c.encoder, "none", ScorePlanes(dec, truth, info, 0, 0));
		for (const auto& a : c.anchors) {
			Planes o;
			if (LoadYuv(a.second, c.W, c.H, 8, o)) {
				add(c.encoder, "ffmpeg " + a.first, ScorePlanes(o, truth, info, 0, 0));
			}
		}
		for (const Deblocker& m : ours) {
			for (const float qp : kQp) {
				Planes o;
				if (DeblockPlanes(gpu, m, qp, dec, o, error)) {
					char l[64];
					sprintf_s(l, "ours %s qp%g", m.name, qp);
					add(c.encoder, l, ScorePlanes(o, truth, info, 0, 0));
				}
			}
		}
		for (int lv = 1; lv <= Clean::kLevels; lv++) {
			Planes o;
			if (DebandPlanes(gpu, Clean::kDeband[lv - 1], 1, dec, o, error)) {
				char l[64];
				sprintf_s(l, "ours deband %d", lv);
				add(c.encoder, l, ScorePlanes(o, truth, info, 0, 0));
				// CAMBI where bands are what the codec leaves: not the block codecs.
				if (cambi && c.encoder.rfind("mpeg2", 0) && c.encoder.rfind("xvid", 0)) {
					wchar_t path[MAX_PATH];
					swprintf_s(path, L"%ls\\out\\%hs.deband%d.yuv", dir.c_str(), c.name.c_str(), lv);
					SaveYuv10(path, o);
					fprintf(cambi, "%s\tours deband %d\t%ls\t%d\t%d\t10\n", c.name.c_str(), lv, path, c.W, c.H);
				}
			}
		}
		// The smoothing alone, without grain, on two encoders, and the decoded picture
		// written at 10 bits as these are: CAMBI reads grain as no banding at all, and
		// reads 10 bits differently from 8.
		if (cambi && c.W == 1920 && (c.encoder == "x264_crf38" || c.encoder == "av1_crf60")) {
			static const Clean::DebandLevel bare[] = {
				{ 1, 2, 16, 0 }, { 1, 3, 16, 0 }, { 1, 4, 16, 0 }, { 2, 4, 16, 0 }, { 2, 6, 20, 0 }, { 3, 8, 24, 0 },
			};
			wchar_t path[MAX_PATH];
			swprintf_s(path, L"%ls\\out\\%hs.none10.yuv", dir.c_str(), c.name.c_str());
			SaveYuv10(path, dec);
			fprintf(cambi, "%s\tnone, 10 bits\t%ls\t%d\t%d\t10\n", c.name.c_str(), path, c.W, c.H);
			for (const Clean::DebandLevel& l : bare) {
				Planes o;
				if (DebandPlanes(gpu, l, 1, dec, o, error)) {
					swprintf_s(path, L"%ls\\out\\%hs.bare%u_%g_%g.yuv", dir.c_str(), c.name.c_str(), l.iterations, l.threshold, l.radius);
					SaveYuv10(path, o);
					fprintf(cambi, "%s\tno grain %u/%g/%g\t%ls\t%d\t%d\t10\n", c.name.c_str(), l.iterations, l.threshold, l.radius, path, c.W, c.H);
				}
			}
		}
		if (cambi && c.encoder.rfind("mpeg2", 0) && c.encoder.rfind("xvid", 0)) {
			fprintf(cambi, "%s\tnone\t%ls\t%d\t%d\t%d\n", c.name.c_str(), c.decoded.c_str(), c.W, c.H, c.bits);
			for (const auto& a : c.anchors) {
				if (a.first.find("deband") != std::string::npos || a.first.find("gradfun") != std::string::npos
						|| a.first.find("libplacebo") != std::string::npos) {
					fprintf(cambi, "%s\tffmpeg %s\t%ls\t%d\t%d\t8\n", c.name.c_str(), a.first.c_str(), a.second.c_str(), c.W, c.H);
				}
			}
		}
		printf("  %s\n", c.name.c_str());
	}
	if (cambi) {
		fclose(cambi);
	}

	for (const auto& e : order) {
		Out("\n  --- %s ---\n", e.first.c_str());
		PrintHead("method", "");
		// Each family at its best setting first, then the levels as shipped.
		std::map<std::string, std::pair<std::string, Score>> best;
		std::vector<std::string> families;
		for (const std::string& label : e.second) {
			const Score s = table[e.first][label].Mean();
			std::string fam = label;
			const size_t q = fam.rfind("_qp") != std::string::npos ? fam.rfind("_qp") : fam.rfind(" qp");
			if (q != std::string::npos) {
				fam = fam.substr(0, q);
			}
			if (best.find(fam) == best.end()) {
				families.push_back(fam);
				best[fam] = { label, s };
			} else if (s.psnr > best[fam].second.psnr) {
				best[fam] = { label, s };
			}
		}
		for (const std::string& fam : families) {
			const auto& b = best[fam];
			std::string setting = b.first.size() > fam.size() ? b.first.substr(fam.size() + 1) : "";
			PrintRow(fam.substr(0, 22).c_str(), setting.c_str(), b.second);
		}
	}
	Out("\n  CAMBI for the debanding rows: python clean_refs.py --cambi\n");
	return 0;
}

// ------------------------------------------------------------- DLSS 5 NR --

// DLSS 5 NR at its default settings on the degraded picture, with and without the
// cleaning before it, as the renderer chains them. The network works on RGB, so every
// row here goes through RGB and back, the rows without the network too.
static int RunWithNR(ID3D11Device* dev, ID3D11DeviceContext* ctx, CDlssNR& dlss, CCleanGpu& gpu,
                     const std::vector<Picture>& pics, int (*caseIndex)(const char*))
{
	CDlssNR::Params params;
	params.iStyle = 0;
	params.iPreset = 0;
	params.fIntensity = 1.50f;
	params.fLocalTone = 0.30f;
	params.fLocalStructure = 0.50f;
	params.fSkinStructure = 0.90f;
	params.bUseAutoMask = true;
	params.bNoHistory = false;

	std::string error;
	CStabilizerPasses passes;
	if (!passes.Init(dev, error)) {
		Out("  %s\n", error.c_str());
		return 1;
	}
	CDlssStabilizer stabilizer;
	Tex2D_t texIn, texOut;
	CComPtr<ID3D11Texture2D> stage;
	int curW = 0, curH = 0;

	auto setup = [&](int W, int H) {
		if (W == curW && H == curH) {
			dlss.RequestReset();
			return true;
		}
		dlss.ReleaseFeature();
		dlss.SetGuides(CDlssNR::Guides{});
		stabilizer.Release();
		stage.Release();
		if (!MakeSharedPair(dev, texIn, texOut, W, H)
				|| FAILED(stabilizer.Create(dev, ctx, W, H, passes.InputLayout(), passes.VertexShader(),
				                            passes.SamplerPoint(), passes.SamplerLinear(), false, CDlssStabilizer::FlowSettings{}))
				|| !dlss.CreateFeature(texIn.pTexture, texOut.pTexture, W, H, params)) {
			Out("  the network at %dx%d could not start: %S\n", W, H, dlss.GetStatusLine().c_str());
			return false;
		}
		D3D11_TEXTURE2D_DESC sd = CreateTex2DDesc(DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, Tex2D_Default);
		sd.Usage = D3D11_USAGE_STAGING;
		sd.BindFlags = 0;
		sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		sd.MiscFlags = 0;
		if (FAILED(dev->CreateTexture2D(&sd, nullptr, &stage))) {
			return false;
		}
		dlss.RequestReset();
		curW = W;
		curH = H;
		return true;
	};
	// The same picture eight times over, as a still shot plays: the network and its
	// stabilizer settle, and the last one is kept.
	auto network = [&](const Planes& in, Planes& out) {
		std::vector<float> rgba, result;
		ToRgb(in, rgba);
		if (!setup(in.W, in.H)) {
			return false;
		}
		std::vector<HALF> half(rgba.size());
		DirectX::PackedVector::XMConvertFloatToHalfStream(half.data(), sizeof(HALF), rgba.data(), sizeof(float), rgba.size());
		for (int f = 0; f < 8; f++) {
			ctx->UpdateSubresource(texIn.pTexture, 0, nullptr, half.data(), 4 * sizeof(HALF) * in.W, 0);
			stabilizer.PrepareMotion(ctx, texIn.pShaderResource);
			ID3D11ShaderResourceView* nullSRVs[4] = {};
			ctx->PSSetShaderResources(0, (UINT)std::size(nullSRVs), nullSRVs);
			ctx->OMSetRenderTargets(0, nullptr, nullptr);
			if (!dlss.Evaluate(params)) {
				return false;
			}
			stabilizer.Stabilize(ctx, texIn.pShaderResource, texOut.pShaderResource, 1.0f, true);
		}
		ctx->CopyResource(stage, stabilizer.GetResult()->pTexture);
		if (!ReadRgbaHalf(ctx, stage, in.W, in.H, result)) {
			return false;
		}
		ToPlanes(result, in.W, in.H, out);
		return true;
	};
	auto throughRgb = [](const Planes& in, Planes& out) {
		std::vector<float> rgba;
		ToRgb(in, rgba);
		ToPlanes(rgba, in.W, in.H, out);
	};

	Out("\n7. DLSS 5 NR at its default settings, alone and after the cleaning, on the\n");
	Out("   1080p pictures; every row through RGB and back, as the network needs:\n");
	struct NRCase { const char* name; int deblock; int deband; };
	const NRCase nrCases[] = {
		{ "clean", 0, 0 }, { "jpeg 0.72", 2, 0 }, { "mpeg2 q8", 3, 0 }, { "mpeg2 q12", 4, 0 }, { "banding 4", 0, 3 },
	};
	for (const NRCase& nc : nrCases) {
		const int k = caseIndex(nc.name);
		Score none, cleaned, nr, both;
		for (const Picture& p : pics) {
			if (p.truth.W != 1920) {
				continue;
			}
			const Planes& d = p.cases[k];
			Planes a, c, cr, n, b;
			// the cleaning at the level suited to the damage
			c = d;
			if (nc.deblock) {
				// The light method, PP7.
				const Clean::DeblockLevel& l = Clean::DeblockLevelFor(Clean::DeblockPP7, nc.deblock);
				const Clean::DeblockShader& sh = Clean::kDeblockShader[Clean::DeblockPP7];
				const Deblocker m = { "", sh.mode, sh.shifts, l.mode };
				DeblockPlanes(gpu, m, l.qp, d, c, error);
			}
			if (nc.deband) {
				Planes t = c;
				DebandPlanes(gpu, Clean::kDeband[nc.deband - 1], 1, t, c, error);
			}
			throughRgb(d, a);
			throughRgb(c, cr);
			if (!network(d, n) || !network(c, b)) {
				Out("  the network failed: %S\n", dlss.GetStatusLine().c_str());
				return 1;
			}
			none.Add(ScorePlanes(a, p.truth, p.info, kCases[k].grid, kCases[k].grid));
			cleaned.Add(ScorePlanes(cr, p.truth, p.info, kCases[k].grid, kCases[k].grid));
			nr.Add(ScorePlanes(n, p.truth, p.info, kCases[k].grid, kCases[k].grid));
			both.Add(ScorePlanes(b, p.truth, p.info, kCases[k].grid, kCases[k].grid));
		}
		char lv[32];
		sprintf_s(lv, nc.deblock ? "deblock %d" : nc.deband ? "deband %d" : "-", nc.deblock ? nc.deblock : nc.deband);
		Out("\n  --- %s --- (cleaning: %s)\n", nc.name, lv);
		PrintHead("", "");
		PrintRow("none", "", none.Mean());
		PrintRow("cleaning", "", cleaned.Mean());
		PrintRow("DLSS 5 NR", "", nr.Mean());
		PrintRow("cleaning, then NR", "", both.Mean());
	}
	dlss.ReleaseFeature();
	dlss.SetGuides(CDlssNR::Guides{});
	return 0;
}

} // namespace clean

// ------------------------------------------------------------------- suite --

static int RunClean(ID3D11Device* dev, ID3D11DeviceContext* ctx, int maxRefs, int study, const wchar_t* files,
                    CDlssNR* nr)
{
	using namespace clean;
	Head(study ? "Artifact removal: the study" : "Artifact removal: deblocking and debanding on the decoded planes");

	const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	int rc = 0;
	{
		CComPtr<IWICImagingFactory> factory;
		Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))), "WIC factory");

		CCleanGpu gpu;
		if (!factory || !gpu.Init(dev, ctx)) {
			Check(false, "set up");
			return 1;
		}

		// --files <dir>: the real encodes alone, a report of their own.
		if (files && *files) {
			g_report = nullptr;
			fopen_s(&g_report, "temporal_results_clean_files.txt", "w");
			rc = RunFiles(gpu, files);
			if (g_report) {
				fclose(g_report);
				g_report = nullptr;
			}
			printf("\n  written to temporal_results_clean_files.txt\n");
			factory.Release();   // before COM goes, not after it
			if (SUCCEEDED(coInit)) {
				CoUninitialize();
			}
			return (rc || g_failures) ? 1 : 0;
		}

		// The references that are pictures of the world: not the line-art test, not the
		// frame that is already an AV1 encode.
		std::vector<std::wstring> files;
		{
			WIN32_FIND_DATAW fd = {};
			HANDLE h = FindFirstFileW(L"upscale_refs\\*.png", &fd);
			if (h != INVALID_HANDLE_VALUE) {
				do {
					const std::wstring n = fd.cFileName;
					if (n.find(L"BrownFox") == std::wstring::npos && n.find(L"AV1") == std::wstring::npos) {
						files.push_back(L"upscale_refs\\" + n);
					}
				} while (FindNextFileW(h, &fd));
				FindClose(h);
			}
			std::sort(files.begin(), files.end());
			if (maxRefs > 0 && files.size() > (size_t)maxRefs) {
				files.resize((size_t)maxRefs);
			}
		}

		std::vector<Picture> pics;
		std::string error;
		for (const std::wstring& f : files) {
			UpscaleRef ref;
			if (!LoadReference(factory, f.c_str(), ref, error)) {
				printf("  %ls: %s\n", f.c_str(), error.c_str());
				continue;
			}
			// 1080p from a 4K frame, and 540p; a smaller frame gives its half.
			std::vector<int> factors;
			if (ref.W >= 3840) {
				factors = { 2, 4 };
			} else {
				factors = { 2 };
			}
			for (const int fct : factors) {
				std::vector<float> reduced;
				DownscaleBox(ref.rgba, ref.W, ref.H, fct, reduced);
				Picture p;
				p.name = ref.name.empty() ? f.substr(f.find_last_of(L'\\') + 1) : ref.name;
				p.name += fct == 2 ? L" /2" : L" /4";
				if (!MakePicture(factory, reduced, ref.W / fct, ref.H / fct, p, error)) {
					printf("  %ls: %s\n", f.c_str(), error.c_str());
					rc = 1;
					continue;
				}
				pics.push_back(std::move(p));
			}
		}
		if (pics.empty()) {
			Check(false, "references");
			return 1;
		}
		printf("  %zu picture(s) from %zu reference(s)\n", pics.size(), files.size());

		const char* reportName = study == 4 ? "temporal_results_clean_nr.txt" : study == 3 ? "temporal_results_clean_ladder.txt" : study == 2 ? "temporal_results_clean_cost.txt"
			: study ? "temporal_results_clean_study.txt" : "temporal_results_clean.txt";
		g_report = nullptr;
		fopen_s(&g_report, reportName, "w");

		Out("Every reference reduced to 1920x1080 and 960x540, made BT.709 limited 4:2:0 planes\n");
		Out("(the truth), degraded, cleaned on the planes, and scored against the truth. psnr:\n");
		Out("luma and chroma by their samples. detail: the luma on the truth's most detailed\n");
		Out("quarter. flat: the luma where the truth is smooth. block: steps across the codec's\n");
		Out("grid over steps elsewhere, both ways (the truth's own figure in the 'none' row's\n");
		Out("note). %zu pictures.\n", pics.size());

		auto caseIndex = [](const char* n) {
			for (size_t k = 0; k < std::size(kCases); k++) {
				if (!strcmp(kCases[k].name, n)) {
					return (int)k;
				}
			}
			return -1;
		};

		// The score of doing nothing, per case.
		auto scoreNone = [&](size_t k) {
			Score s;
			for (const Picture& p : pics) {
				s.Add(ScorePlanes(p.cases[k], p.truth, p.info, kCases[k].grid, kCases[k].grid));
			}
			return s.Mean();
		};
		auto truthBlock = [&](size_t k) {
			double b = 0;
			for (const Picture& p : pics) {
				b += Blockiness(p.truth.y, p.truth.W, p.truth.H, kCases[k].grid, kCases[k].grid);
			}
			return b / pics.size();
		};
		auto scoreDeblock = [&](const Deblocker& m, float qp, size_t k, Score& out) {
			Score s;
			for (const Picture& p : pics) {
				Planes o;
				if (!DeblockPlanes(gpu, m, qp, p.cases[k], o, error)) {
					return false;
				}
				s.Add(ScorePlanes(o, p.truth, p.info, kCases[k].grid, kCases[k].grid));
			}
			out = s.Mean();
			return true;
		};

		// --withnr: DLSS 5 NR alone and after the cleaning, without the rest of the study.
		if (study == 4) {
			if (nr) {
				rc |= RunWithNR(dev, ctx, *nr, gpu, pics, caseIndex);
			} else {
				Out("\n7. DLSS 5 NR: the harness has no session (--nonr).\n");
			}
		}

		// --ladder: the deblocking that ships, each threshold mode over the whole ladder
		// of damage, strength by strength: where the levels are read from.
		if (study == 3) {
			static const float kQpL[] = { 0.5f, 1, 1.5f, 2, 2.5f, 3, 4, 5, 6, 8, 10, 12 };
			const char* ladder[] = { "mpeg2 q2", "mpeg2 q4", "mpeg2 q8", "mpeg2 q12", "mpeg2 q16", "mpeg2 q8 +4", "jpeg 0.72", "clean", "grain" };
			for (const int method : { (int)Clean::DeblockPP7, (int)Clean::DeblockSPP }) {
			const Clean::DeblockShader& sh = Clean::kDeblockShader[method];
			Out("\nThe deblocking %s over the ladder: psnr, and block in brackets, at each qp.\n",
				method == Clean::DeblockSPP ? "SPP" : "PP7");
			for (uint32_t mode = 0; mode < 3; mode++) {
				static const char* tn[3] = { "hard", "soft", "medium" };
				Out("\n  --- %s ---\n  %6s", tn[mode], "qp");
				for (const char* cn : ladder) {
					Out(" %16s", cn);
				}
				Out("\n  %6s", "none");
				for (const char* cn : ladder) {
					const Score s = scoreNone(caseIndex(cn));
					Out(" %8.3f (%5.3f)", s.psnr, s.block);
				}
				Out("\n");
				for (const float qp : kQpL) {
					Out("  %6.1f", qp);
					for (const char* cn : ladder) {
						Score s;
						const Deblocker m = { "", sh.mode, sh.shifts, mode };
						scoreDeblock(m, qp, caseIndex(cn), s);
						Out(" %8.3f (%5.3f)", s.psnr, s.block);
					}
					Out("\n");
				}
			}
			}
		}

		if (study == 1 || study == 2) {
			// ---- 1. which deblocking: every variant over a sweep of strengths.
			static const float kQp[] = { 1, 1.5f, 2, 3, 4, 5, 6, 8, 10, 12, 16, 20 };
			const std::vector<Deblocker> methods = StudyDeblockers();
			const char* sweepCases[] = { "mpeg2 q4", "mpeg2 q8", "mpeg2 q12", "mpeg2 q8 +4", "jpeg 0.72" };

			if (study == 1) {
				Out("\n1. Deblocking: each method at the strength that suits each damage best (psnr).\n");
			}
			for (const char* cn : sweepCases) {
				if (study != 1) {
					break;
				}
				const int k = caseIndex(cn);
				Out("\n  --- %s --- (the truth's block figure %.3f)\n", cn, truthBlock(k));
				PrintHead("method", "qp");
				PrintRow("none", "-", scoreNone(k));
				for (const Deblocker& m : methods) {
					Score best;
					float bestQp = 0;
					for (const float qp : kQp) {
						Score s;
						if (!scoreDeblock(m, qp, k, s)) {
							Out("  %s: %s\n", m.name, error.c_str());
							rc = 1;
							break;
						}
						if (!bestQp || s.psnr > best.psnr) {
							best = s;
							bestQp = qp;
						}
					}
					char q[16];
					sprintf_s(q, "%.1f", bestQp);
					PrintRow(m.name, q, best);
				}
			}

			// ---- 2. what each costs.
			Out("\n2. Deblocking: one picture's planes (luma, then Cb and Cr together), ms on this GPU:\n\n");
			Out("  %-22s %10s %10s\n", "method", "1080p", "2160p");
			for (const Deblocker& m : methods) {
				ID3D11ComputeShader* cs1 = gpu.Shader({ false, m.mode, m.shifts ? m.shifts : 8, 1 }, error);
				ID3D11ComputeShader* cs2 = gpu.Shader({ false, m.mode, m.shifts ? m.shifts : 8, 2 }, error);
				if (!cs1 || !cs2) {
					continue;
				}
				const double hd = gpu.Cost(cs1, DeblockC(1920, 1080, 6, m.threshold), 1) + gpu.Cost(cs2, DeblockC(960, 540, 6, m.threshold), 2);
				const double uhd = gpu.Cost(cs1, DeblockC(3840, 2160, 6, m.threshold), 1) + gpu.Cost(cs2, DeblockC(1920, 1080, 6, m.threshold), 2);
				Out("  %-22s %10.3f %10.3f\n", m.name, hd, uhd);
			}
			{
				ID3D11ComputeShader* d1 = gpu.Shader({ true, 0, 0, 1 }, error);
				ID3D11ComputeShader* d2 = gpu.Shader({ true, 0, 0, 2 }, error);
				for (int it = 1; it <= 3; it++) {
					Clean::Constants c = {};
					c.iterations = it; c.threshold = 4; c.radius = 16; c.grain = 4; c.seed = 1;
					c.width = 1920; c.height = 1080;
					double hd = gpu.Cost(d1, c, 1);
					c.width = 960; c.height = 540;
					hd += gpu.Cost(d2, c, 2);
					c.width = 3840; c.height = 2160;
					double uhd = gpu.Cost(d1, c, 1);
					c.width = 1920; c.height = 1080;
					uhd += gpu.Cost(d2, c, 2);
					char n[32];
					sprintf_s(n, "deband, %d iteration%s", it, it > 1 ? "s" : "");
					Out("  %-22s %10.3f %10.3f\n", n, hd, uhd);
				}
			}

			// --cost: the costs alone.
			if (study == 1) {
			// ---- 3. clean the chroma before or after it is brought to the luma's size.
			Out("\n3. The chroma: deblocked at its own size then upsampled, or upsampled then\n");
			Out("   deblocked, against the picture's own full-size chroma (psnr of Cb and Cr):\n\n");
			Out("  %-14s %-10s %10s %14s %14s\n", "case", "upsampler", "none", "clean, then up", "up, then clean");
			for (const char* cn : { "mpeg2 q8", "mpeg2 q12", "jpeg 0.72" }) {
				const int k = caseIndex(cn);
				for (const bool catmull : { false, true }) {
					// PP7 medium, the strength swept for each order on its own.
					double none = 0, bestA = 0, bestB = 0;
					for (const float qp : kQp) {
						double a = 0, b = 0, n0 = 0;
						for (const Picture& p : pics) {
							const Planes& d = p.cases[k];
							const int W = d.W, H = d.H, cw = d.CW(), ch = d.CH();
							std::vector<float> ucb, ucr, cb2, cr2;
							auto mse = [&](const std::vector<float>& x, const std::vector<float>& y) {
								double s = 0;
								for (size_t i = 0; i < x.size(); i++) {
									s += ((double)x[i] - y[i]) * ((double)x[i] - y[i]);
								}
								return s / x.size();
							};
							// none
							Upsample(d.cb, cw, ch, W, H, catmull, ucb);
							Upsample(d.cr, cw, ch, W, H, catmull, ucr);
							n0 += Psnr(0.5 * (mse(ucb, p.truth444.cb) + mse(ucr, p.truth444.cr)));
							// clean, then up
							std::vector<float> cbcr;
							ID3D11ComputeShader* cs2 = gpu.Shader({ false, 1, 8, 2 }, error);
							gpu.Run(cs2, DeblockC(cw, ch, qp, Clean::DeblockMedium), Interleave(d.cb, d.cr), 2, cbcr);
							Split(cbcr, cb2, cr2);
							std::vector<float> acb, acr;
							Upsample(cb2, cw, ch, W, H, catmull, acb);
							Upsample(cr2, cw, ch, W, H, catmull, acr);
							a += Psnr(0.5 * (mse(acb, p.truth444.cb) + mse(acr, p.truth444.cr)));
							// up, then clean
							gpu.Run(cs2, DeblockC(W, H, qp, Clean::DeblockMedium), Interleave(ucb, ucr), 2, cbcr);
							Split(cbcr, cb2, cr2);
							b += Psnr(0.5 * (mse(cb2, p.truth444.cb) + mse(cr2, p.truth444.cr)));
						}
						none = n0 / pics.size();
						bestA = std::max(bestA, a / pics.size());
						bestB = std::max(bestB, b / pics.size());
					}
					Out("  %-14s %-10s %10.3f %14.3f %14.3f\n", cn, catmull ? "Catmull" : "bilinear", none, bestA, bestB);
				}
			}

			// ---- 4. debanding: its parameters.
			Out("\n4. Debanding: iterations, threshold, radius (no grain), on the banded pictures and\n");
			Out("   on clean ones, where it should change nothing:\n");
			const int band2 = caseIndex("banding 2"), band4 = caseIndex("banding 4"), clean0 = caseIndex("clean"), grain0 = caseIndex("grain");
			std::vector<Clean::DebandLevel> tries;
			for (uint32_t it : { 1u, 2u, 3u }) {
				for (float th : { 2.0f, 3.0f, 4.0f, 6.0f, 8.0f, 12.0f }) {
					for (float r : { 8.0f, 16.0f, 24.0f }) {
						tries.push_back({ it, th, r, 0.0f });
					}
				}
			}
			for (const int k : { band2, band4, clean0, grain0 }) {
				Out("\n  --- %s ---\n", kCases[k].name);
				Out("  %-22s %8s %8s %8s %8s %8s %8s %7s\n", "it / thr / radius", "", "psnr", "Y", "C", "detail", "flat", "block");
				PrintRow("none", "-", scoreNone(k));
				for (const Clean::DebandLevel& l : tries) {
					Score s;
					bool okRun = true;
					for (const Picture& p : pics) {
						Planes o;
						if (!DebandPlanes(gpu, l, 1, p.cases[k], o, error)) {
							okRun = false;
							break;
						}
						s.Add(ScorePlanes(o, p.truth, p.info, 0, 0));
					}
					if (!okRun) {
						Out("  deband: %s\n", error.c_str());
						rc = 1;
						break;
					}
					char n[32];
					sprintf_s(n, "%u / %.0f / %.0f", l.iterations, l.threshold, l.radius);
					PrintRow(n, "", s.Mean());
				}
			}

			// ---- 5. debanding: the grain, and how much it moves from one picture to the next.
			Out("\n5. Debanding's grain, at 2 / 4 / 16: the scores, and the mean change from one\n");
			Out("   picture to the next in the truth's smooth areas (x1000 of the range):\n\n");
			Out("  %-10s %10s %10s %10s %10s %10s\n", "grain", "band2 flat", "band4 flat", "clean psnr", "detail", "flicker");
			for (float g : { 0.0f, 2.0f, 4.0f, 6.0f, 8.0f }) {
				const Clean::DebandLevel l = { 2, 4.0f, 16.0f, g };
				Score s2, s4, sc;
				double flick = 0;
				for (const Picture& p : pics) {
					Planes a, b;
					DebandPlanes(gpu, l, 1, p.cases[band2], a, error);
					s2.Add(ScorePlanes(a, p.truth, p.info, 0, 0));
					DebandPlanes(gpu, l, 1, p.cases[band4], a, error);
					s4.Add(ScorePlanes(a, p.truth, p.info, 0, 0));
					DebandPlanes(gpu, l, 1, p.cases[clean0], a, error);
					sc.Add(ScorePlanes(a, p.truth, p.info, 0, 0));
					DebandPlanes(gpu, l, 2, p.cases[clean0], b, error);
					double d = 0;
					long long n = 0;
					for (size_t i = 0; i < a.y.size(); i++) {
						if (p.info.flat[i]) {
							d += std::abs((double)a.y[i] - b.y[i]);
							n++;
						}
					}
					flick += n ? d / n : 0;
				}
				Out("  %-10.0f %10.3f %10.3f %10.3f %10.3f %10.3f\n", g, s2.Mean().flat, s4.Mean().flat,
					sc.Mean().psnr, sc.Mean().detail, 1000.0 * flick / pics.size());
			}

			// ---- 7. DLSS 5 NR, when the harness has a session.
			if (nr) {
				rc |= RunWithNR(dev, ctx, *nr, gpu, pics, caseIndex);
			} else {
				Out("\n7. DLSS 5 NR: left out (--nonr).\n");
			}
			}
		}

		// ---- the shipped levels, on every case.
		for (const int method : { (int)Clean::DeblockPP7, (int)Clean::DeblockSPP }) {
			const Clean::DeblockShader& sh = Clean::kDeblockShader[method];
			const char* mname = method == Clean::DeblockSPP ? "SPP" : "PP7";
			Out("\nThe five deblocking levels as shipped, %s, on every case:\n", mname);
			for (size_t k = 0; k < std::size(kCases); k++) {
				Out("\n  --- %s, %s --- (the truth's block figure %.3f)\n", mname, kCases[k].name, truthBlock(k));
				PrintHead("level", "qp");
				PrintRow("none", "-", scoreNone(k));
				for (int lv = 1; lv <= Clean::kLevels; lv++) {
					const Clean::DeblockLevel& l = Clean::DeblockLevelFor(method, lv);
					const Deblocker m = { "deblock", sh.mode, sh.shifts, l.mode };
					Score s;
					if (!scoreDeblock(m, l.qp, k, s)) {
						Out("  %s\n", error.c_str());
						rc = 1;
						break;
					}
					char n[16], q[16];
					sprintf_s(n, "%s %d", mname, lv);
					sprintf_s(q, "%.1f", l.qp);
					PrintRow(n, q, s);
				}
			}
		}
		Out("\nThe five debanding levels as shipped, on every case:\n");
		for (size_t k = 0; k < std::size(kCases); k++) {
			Out("\n  --- %s ---\n", kCases[k].name);
			PrintHead("level", "");
			PrintRow("none", "-", scoreNone(k));
			for (int lv = 1; lv <= Clean::kLevels; lv++) {
				Score s;
				for (const Picture& p : pics) {
					Planes o;
					if (DebandPlanes(gpu, Clean::kDeband[lv - 1], 1, p.cases[k], o, error)) {
						s.Add(ScorePlanes(o, p.truth, p.info, kCases[k].grid, kCases[k].grid));
					}
				}
				char n[16];
				sprintf_s(n, "%d", lv);
				PrintRow(n, "", s.Mean());
			}
		}

		// ---- the cost of what ships.
		{
			Out("\nWhat ships, one picture's planes in 4:2:0, on this GPU:\n\n");
			Out("  %-22s %10s %10s\n", "", "1080p", "2160p");
			ID3D11ComputeShader* d1 = gpu.Shader({ true, 0, 0, 1 }, error);
			ID3D11ComputeShader* d2 = gpu.Shader({ true, 0, 0, 2 }, error);
			// The deblocking costs the same at every level; the debanding by its iterations.
			double bh = 0, bu = 0, ph = 0, pu = 0;
			for (const int method : { (int)Clean::DeblockPP7, (int)Clean::DeblockSPP }) {
				const Clean::DeblockShader& sh = Clean::kDeblockShader[method];
				ID3D11ComputeShader* b1 = gpu.Shader({ false, sh.mode, sh.shifts, 1 }, error);
				ID3D11ComputeShader* b2 = gpu.Shader({ false, sh.mode, sh.shifts, 2 }, error);
				if (!b1 || !b2) {
					continue;
				}
				const Clean::DeblockLevel& bl = Clean::DeblockLevelFor(method, Clean::kLevels);
				const double h = gpu.Cost(b1, DeblockC(1920, 1080, bl.qp, bl.mode), 1) + gpu.Cost(b2, DeblockC(960, 540, bl.qp, bl.mode), 2);
				const double u = gpu.Cost(b1, DeblockC(3840, 2160, bl.qp, bl.mode), 1) + gpu.Cost(b2, DeblockC(1920, 1080, bl.qp, bl.mode), 2);
				Out("  %-22s %7.3f ms %7.3f ms\n", method == Clean::DeblockSPP ? "deblocking, SPP" : "deblocking, PP7", h, u);
				(method == Clean::DeblockPP7 ? bh : ph) = h;
				(method == Clean::DeblockPP7 ? bu : pu) = u;
			}
			if (d1 && d2) {
				double dh[Clean::kLevels + 1] = {}, du[Clean::kLevels + 1] = {};
				for (int lv = 1; lv <= Clean::kLevels; lv++) {
					dh[lv] = gpu.Cost(d1, Clean::DebandConstants(lv, 1920, 1080, kBlack, 1, 0), 1)
						+ gpu.Cost(d2, Clean::DebandConstants(lv, 960, 540, kMid, 1, 0), 2);
					du[lv] = gpu.Cost(d1, Clean::DebandConstants(lv, 3840, 2160, kBlack, 1, 0), 1)
						+ gpu.Cost(d2, Clean::DebandConstants(lv, 1920, 1080, kMid, 1, 0), 2);
					char n[32];
					sprintf_s(n, "debanding, level %d", lv);
					Out("  %-22s %7.3f ms %7.3f ms\n", n, dh[lv], du[lv]);
				}
				Out("  %-22s %7.3f ms %7.3f ms\n", "PP7 and deband, 3", bh + dh[3], bu + du[3]);
				Out("  %-22s %7.3f ms %7.3f ms\n", "PP7 and deband, 5", bh + dh[5], bu + du[5]);
				Out("  %-22s %7.3f ms %7.3f ms\n", "SPP and deband, 3", ph + dh[3], pu + du[3]);
				Out("  %-22s %7.3f ms %7.3f ms\n", "SPP and deband, 5", ph + dh[5], pu + du[5]);
			}
		}

		// ---- the port check: the shaders against the reference in double precision.
		Out("\nThe shaders against their reference in double precision, on a 256x144 crop:\n\n");
		{
			const Picture& p = pics[0];
			const Planes& d = p.cases[caseIndex("mpeg2 q8")];
			const int cw = 256, chh = 144;
			std::vector<float> y((size_t)cw * chh), cbcr(2 * (size_t)(cw / 2) * (chh / 2));
			for (int j = 0; j < chh; j++) {
				for (int i = 0; i < cw; i++) {
					y[(size_t)j * cw + i] = d.y[(size_t)(j + 200) * d.W + i + 300];
				}
			}
			for (int j = 0; j < chh / 2; j++) {
				for (int i = 0; i < cw / 2; i++) {
					cbcr[2 * ((size_t)j * (cw / 2) + i)] = d.cb[(size_t)(j + 100) * d.CW() + i + 150];
					cbcr[2 * ((size_t)j * (cw / 2) + i) + 1] = d.cr[(size_t)(j + 100) * d.CW() + i + 150];
				}
			}
			std::vector<std::pair<int, int>> variants = {
				{ Clean::kDeblockShader[Clean::DeblockPP7].mode, Clean::kDeblockShader[Clean::DeblockPP7].shifts },
				{ Clean::kDeblockShader[Clean::DeblockSPP].mode, Clean::kDeblockShader[Clean::DeblockSPP].shifts },
			};
			if (study) {
				variants = { { 1, 8 }, { 0, 4 }, { 0, 8 }, { 0, 16 }, { 0, 64 } };
			}
			for (const auto& v : variants) {
				for (uint32_t mode = 0; mode < 3; mode++) {
					char what[64];
					const char* tn[3] = { "hard", "soft", "medium" };
					if (v.first == 1) {
						sprintf_s(what, "PP7 %s, luma", tn[mode]);
					} else {
						sprintf_s(what, "SPP-%d %s, luma", v.second, tn[mode]);
					}
					PortCheck(gpu, what, { false, v.first, v.second, 1 }, DeblockC(cw, chh, 6, mode), y, 1);
					what[strlen(what) - 4] = 0;
					strcat_s(what, "Cb and Cr");
					PortCheck(gpu, what, { false, v.first, v.second, 2 }, DeblockC(cw / 2, chh / 2, 6, mode), cbcr, 2);
				}
			}
			for (int lv : { 1, Clean::kLevels }) {
				char what[64];
				Clean::Constants c = Clean::DebandConstants(lv, cw, chh, kBlack, 7, 0);
				sprintf_s(what, "deband level %d, luma", lv);
				PortCheck(gpu, what, { true, 0, 0, 1 }, c, y, 1);
				c = Clean::DebandConstants(lv, cw / 2, chh / 2, kMid, 7, 0);
				sprintf_s(what, "deband level %d, Cb and Cr", lv);
				PortCheck(gpu, what, { true, 0, 0, 2 }, c, cbcr, 2);
			}
		}

		if (g_report) {
			fclose(g_report);
			g_report = nullptr;
		}
		printf("\n  written to %s\n", reportName);
	}
	if (SUCCEEDED(coInit)) {
		CoUninitialize();
	}
	return (rc || g_failures) ? 1 : 0;
}

} // namespace temporal
