// --tupscale: what each upscaling method does to a real picture.
//
// Included by harness.cpp after stab_suite.inl. Reference pictures -- ideally
// true 4K frames of films, dropped into upscale_refs\ -- are reduced the way a
// video source is (a clean box reduction, then film grain or compression), then
// brought back to the reference size by each method. What the method invented
// is measured against the reference it never saw.
//
// The renderer's own resize shaders are compiled from Shaders\d3d11, with the
// constants of CDX11VideoProcessor::TextureResizeShader, so the baseline rows
// are exactly what MPC-VR does today. EfRLFN, a neural doubler, is measured on
// the same pictures and the same metrics when ONNX Runtime is present; it did
// not beat the classic filters on film sources and --tupscalecost shows why the
// renderer does not ship it.

#include <map>
#include <memory>

#ifdef HAVE_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#include <dml_provider_factory.h>
#endif

namespace temporal {

// A picture moved by part of a pixel, with the kernel the renderer resizes with.
// A prescaler that writes its result off the grid -- NNEDI3 says so itself -- is
// put back here; in a player the shift costs nothing, since the picture goes
// through a resize anyway.
void ShiftPlane(std::vector<float>& rgba, int W, int H, double dx, double dy)
{
	if (dx == 0 && dy == 0) {
		return;
	}
	auto weights = [](double t, double w[4]) {
		const double t2 = t * t, t3 = t2 * t;
		w[0] = -0.5 * t3 + t2 - 0.5 * t;
		w[1] = 1.5 * t3 - 2.5 * t2 + 1.0;
		w[2] = -1.5 * t3 + 2.0 * t2 + 0.5 * t;
		w[3] = 0.5 * t3 - 0.5 * t2;
	};
	const std::vector<float> src(rgba);
	double wx[4], wy[4];
	weights(dx - std::floor(dx), wx);
	weights(dy - std::floor(dy), wy);
	const int ox = (int)std::floor(dx), oy = (int)std::floor(dy);
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			double acc = 0;
			for (int j = 0; j < 4; j++) {
				double row = 0;
				for (int i = 0; i < 4; i++) {
					const int sx = std::clamp(x + ox - 1 + i, 0, W - 1);
					const int sy = std::clamp(y + oy - 1 + j, 0, H - 1);
					row += wx[i] * src[4 * ((size_t)sy * W + sx)];
				}
				acc += wy[j] * row;
			}
			rgba[4 * ((size_t)y * W + x)] = (float)acc;
		}
	}
}

// A picture pulled back towards the range the source really covers around each
// pixel, by `strength` of the way -- libplacebo's anti-ringing, applied after a
// doubler instead of inside a kernel. The source is the reduced picture the
// method was given; `factor` is how much bigger the output is.
void AntiRingPlane(std::vector<float>& rgba, int W, int H,
                   const std::vector<float>& lowres, int lw, int lh, int factor, float strength)
{
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			const double u = (x + 0.5) / factor - 0.5, v = (y + 0.5) / factor - 0.5;
			const int ix = (int)std::floor(u), iy = (int)std::floor(v);
			float lo = 1e9f, hi = -1e9f;
			for (int j = 0; j <= 1; j++) {
				for (int i = 0; i <= 1; i++) {
					const size_t k = (size_t)std::clamp(iy + j, 0, lh - 1) * lw + std::clamp(ix + i, 0, lw - 1);
					const float sample = Luma(&lowres[4 * k]);
					lo = std::min(lo, sample);
					hi = std::max(hi, sample);
				}
			}
			float& value = rgba[4 * ((size_t)y * W + x)];
			value += strength * (std::clamp(value, lo, hi) - value);
		}
	}
}

// A reference picture, and the largest size it can honestly stand for.
struct UpscaleRef {
	std::wstring name;
	int W = 0, H = 0;
	bool bTrue4K = false;
	std::vector<float> rgba;
};

// ---------------------------------------------------------------- pictures --

// A reference is used at its own size and never resampled: a film frame keeps
// its aspect ratio and every pixel it has. The size is trimmed to a multiple of
// 6, so that halving and thirding it stay exact, and to 3840x2160 at most,
// keeping the centre. A frame narrower than 3840 still ranks the methods, but
// does not stand for a 4K screen -- the report says which is which.
static bool LoadReference(IWICImagingFactory* factory, const wchar_t* path, UpscaleRef& ref, std::string& error)
{
	CComPtr<IWICBitmapDecoder> decoder;
	CComPtr<IWICBitmapFrameDecode> frame;
	HRESULT hr = factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
	if (SUCCEEDED(hr)) {
		hr = decoder->GetFrame(0, &frame);
	}
	UINT sw = 0, sh = 0;
	if (SUCCEEDED(hr)) {
		hr = frame->GetSize(&sw, &sh);
	}
	if (FAILED(hr) || !sw || !sh) {
		error = std::format("cannot decode the picture (0x{:08X})", (unsigned)hr);
		return false;
	}

	const int W = std::min((int)sw, 3840) / 6 * 6;
	const int H = std::min((int)sh, 2160) / 6 * 6;
	if (W < 480 || H < 270) {
		error = "the picture is too small to be a reference";
		return false;
	}

	CComPtr<IWICFormatConverter> converter;
	hr = factory->CreateFormatConverter(&converter);
	if (SUCCEEDED(hr)) {
		hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
	}
	std::vector<BYTE> bytes(4 * (size_t)W * H);
	const WICRect rect = { (INT)(sw - W) / 2, (INT)(sh - H) / 2, W, H };
	if (SUCCEEDED(hr)) {
		hr = converter->CopyPixels(&rect, 4 * (UINT)W, (UINT)bytes.size(), bytes.data());
	}
	if (FAILED(hr)) {
		error = std::format("cannot read the pixels (0x{:08X})", (unsigned)hr);
		return false;
	}

	ref.rgba.resize(bytes.size());
	for (size_t i = 0; i < bytes.size(); i++) {
		ref.rgba[i] = bytes[i] / 255.0f;
	}
	ref.W = W;
	ref.H = H;
	ref.bTrue4K = (sw >= 3840);
	return true;
}

// The pw x ph window of the reference with the most detail. A film frame is often
// soft where the lens was not focused, and a centre crop landing there shows four
// identical panels.
static POINT FindDetailWindow(const std::vector<float>& rgba, int W, int H, int pw, int ph)
{
	// Summed-area table of the luma gradient, so every window costs four lookups.
	std::vector<double> sat((size_t)(W + 1) * (H + 1), 0.0);
	for (int y = 0; y < H; y++) {
		double row = 0;
		for (int x = 0; x < W; x++) {
			double g = 0;
			if (x > 0 && y > 0) {
				const float* p = &rgba[4 * ((size_t)y * W + x)];
				g = std::abs(Luma(p) - Luma(p - 4)) + std::abs(Luma(p) - Luma(p - 4 * (size_t)W));
			}
			row += g;
			sat[(size_t)(y + 1) * (W + 1) + x + 1] = sat[(size_t)y * (W + 1) + x + 1] + row;
		}
	}
	POINT best = { (W - pw) / 2, (H - ph) / 2 };
	double bestEnergy = -1;
	for (int y = 0; y + ph <= H; y += 16) {
		for (int x = 0; x + pw <= W; x += 16) {
			const double e = sat[(size_t)(y + ph) * (W + 1) + x + pw] - sat[(size_t)y * (W + 1) + x + pw]
			               - sat[(size_t)(y + ph) * (W + 1) + x] + sat[(size_t)y * (W + 1) + x];
			if (e > bestEnergy) {
				bestEnergy = e;
				best = { x, y };
			}
		}
	}
	return best;
}

// A 2x2 board of the same crop, one panel per picture, saved as one file: easier
// to judge than four. The crop is the reference's most detailed window.
static bool SaveBoard(IWICImagingFactory* factory, const wchar_t* path,
                      const std::vector<const std::vector<float>*>& pictures, int W, int H, POINT at)
{
	const int pw = std::min(960, W), ph = std::min(540, H);
	const int x0 = std::clamp((int)at.x, 0, W - pw), y0 = std::clamp((int)at.y, 0, H - ph);
	std::vector<float> board(4 * (size_t)(2 * pw) * (2 * ph), 0.0f);
	for (size_t n = 0; n < pictures.size() && n < 4; n++) {
		if (!pictures[n]) {
			continue;
		}
		const int bx = (int)(n % 2) * pw, by = (int)(n / 2) * ph;
		for (int y = 0; y < ph; y++) {
			const float* src = &(*pictures[n])[4 * ((size_t)(y0 + y) * W + x0)];
			std::copy_n(src, 4 * (size_t)pw, &board[4 * ((size_t)(by + y) * (2 * pw) + bx)]);
		}
	}
	const Rect all = { 0, 0, 2 * pw, 2 * ph };
	return SavePng(factory, path, board, 2 * pw, all, 1.0f);
}

// Six methods side by side, close up: the most detailed 320x180 window of the
// reference, each panel blown up twice by pixel repetition so that edges, halos
// and invented texture show as they are. Three columns, two rows.
static bool SaveZoomBoard(IWICImagingFactory* factory, const wchar_t* path,
                          const std::vector<const std::vector<float>*>& pictures, int W, int H, POINT at)
{
	const int cw = 320, ch = 180, zoom = 2;
	const int pw = cw * zoom, ph = ch * zoom;
	const int x0 = std::clamp((int)at.x, 0, W - cw), y0 = std::clamp((int)at.y, 0, H - ch);
	std::vector<float> board(4 * (size_t)(3 * pw) * (2 * ph), 0.0f);
	for (size_t n = 0; n < pictures.size() && n < 6; n++) {
		if (!pictures[n]) {
			continue;
		}
		const int bx = (int)(n % 3) * pw, by = (int)(n / 3) * ph;
		for (int y = 0; y < ph; y++) {
			for (int x = 0; x < pw; x++) {
				const float* src = &(*pictures[n])[4 * ((size_t)(y0 + y / zoom) * W + (x0 + x / zoom))];
				std::copy_n(src, 4, &board[4 * ((size_t)(by + y) * (3 * pw) + bx + x)]);
			}
		}
	}
	const Rect all = { 0, 0, 3 * pw, 2 * ph };
	return SavePng(factory, path, board, 3 * pw, all, 1.0f);
}

// Integer box reduction: what a careful encoder does, and no invented detail.
static void DownscaleBox(const std::vector<float>& src, int W, int H, int factor, std::vector<float>& dst)
{
	const int w = W / factor, h = H / factor;
	dst.assign(4 * (size_t)w * h, 0.0f);
	const float norm = 1.0f / (factor * factor);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			float acc[4] = {};
			for (int j = 0; j < factor; j++) {
				const float* row = &src[4 * ((size_t)(y * factor + j) * W + (size_t)x * factor)];
				for (int i = 0; i < factor; i++) {
					for (int k = 0; k < 4; k++) {
						acc[k] += row[4 * i + k];
					}
				}
			}
			float* o = &dst[4 * ((size_t)y * w + x)];
			for (int k = 0; k < 4; k++) {
				o[k] = acc[k] * norm;
			}
		}
	}
}

// Film grain: one luma value per pixel, the same on the three channels.
static void AddGrain(std::vector<float>& rgba, int W, int H, float sigma, uint32_t seed)
{
	Rng r(seed);
	for (size_t p = 0; p + 3 < rgba.size(); p += 4) {
		const float n = r.Gauss() * sigma;
		for (int k = 0; k < 3; k++) {
			rgba[p + k] = std::clamp(rgba[p + k] + n, 0.0f, 1.0f);
		}
	}
	(void)W; (void)H;
}

// A JPEG round trip through WIC: blocking, ringing and chroma damage, close to
// what a web release leaves behind.
static bool JpegRoundTrip(IWICImagingFactory* factory, std::vector<float>& rgba, int W, int H, float quality, std::string& error)
{
	// JPEG has no alpha: the frame ends up 24bpp whatever we ask for, and a
	// 32bpp buffer written into it lands one byte off on every pixel.
	std::vector<BYTE> bgr(3 * (size_t)W * H);
	for (size_t p = 0, o = 0; p + 3 < rgba.size(); p += 4, o += 3) {
		bgr[o + 0] = (BYTE)std::lround(std::clamp(rgba[p + 2], 0.0f, 1.0f) * 255);
		bgr[o + 1] = (BYTE)std::lround(std::clamp(rgba[p + 1], 0.0f, 1.0f) * 255);
		bgr[o + 2] = (BYTE)std::lround(std::clamp(rgba[p + 0], 0.0f, 1.0f) * 255);
	}

	CComPtr<IStream> stream;
	HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
	if (SUCCEEDED(hr)) {
		CComPtr<IWICBitmapEncoder> encoder;
		CComPtr<IWICBitmapFrameEncode> frame;
		CComPtr<IPropertyBag2> props;
		hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
		if (SUCCEEDED(hr)) {
			hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
		}
		if (SUCCEEDED(hr)) {
			hr = encoder->CreateNewFrame(&frame, &props);
		}
		if (SUCCEEDED(hr)) {
			PROPBAG2 option = {};
			option.pstrName = (LPOLESTR)L"ImageQuality";
			VARIANT value = {};
			value.vt = VT_R4;
			value.fltVal = quality;
			props->Write(1, &option, &value);
			hr = frame->Initialize(props);
		}
		if (SUCCEEDED(hr)) {
			hr = frame->SetSize((UINT)W, (UINT)H);
		}
		WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
		if (SUCCEEDED(hr)) {
			hr = frame->SetPixelFormat(&format);
		}
		if (SUCCEEDED(hr) && !IsEqualGUID(format, GUID_WICPixelFormat24bppBGR)) {
			error = "the JPEG encoder refused 24bpp BGR";
			return false;
		}
		if (SUCCEEDED(hr)) {
			hr = frame->WritePixels((UINT)H, 3 * (UINT)W, (UINT)bgr.size(), bgr.data());
		}
		if (SUCCEEDED(hr)) {
			hr = frame->Commit();
		}
		if (SUCCEEDED(hr)) {
			hr = encoder->Commit();
		}
	}

	CComPtr<IWICBitmapDecoder> decoder;
	CComPtr<IWICBitmapFrameDecode> decoded;
	CComPtr<IWICFormatConverter> converter;
	if (SUCCEEDED(hr)) {
		LARGE_INTEGER zero = {};
		stream->Seek(zero, STREAM_SEEK_SET, nullptr);
		hr = factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
	}
	if (SUCCEEDED(hr)) {
		hr = decoder->GetFrame(0, &decoded);
	}
	if (SUCCEEDED(hr)) {
		hr = factory->CreateFormatConverter(&converter);
	}
	if (SUCCEEDED(hr)) {
		hr = converter->Initialize(decoded, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
	}
	std::vector<BYTE> back(4 * (size_t)W * H);
	if (SUCCEEDED(hr)) {
		hr = converter->CopyPixels(nullptr, 4 * (UINT)W, (UINT)back.size(), back.data());
	}
	if (FAILED(hr)) {
		error = std::format("JPEG round trip failed (0x{:08X})", (unsigned)hr);
		return false;
	}

	for (size_t p = 0, o = 0; p + 3 < rgba.size(); p += 4, o += 4) {
		for (int k = 0; k < 4; k++) {
			rgba[p + k] = back[o + k] / 255.0f;
		}
	}
	return true;
}

// ----------------------------------------------------------------- metrics --

struct UpscaleMetrics {
	double psnr = 0;    // luma, against the reference
	double psnrDetail = 0;   // the same on the reference's most detailed quarter
	double ssim = 0;
	double sharp = 0;   // gradient on the reference's edges, output over reference
	double halo = 0;    // overshoot beyond the reference's local range, on those edges
	double grain = 0;   // high-frequency energy in flat areas, output over reference
	double block = 0;   // steps on the source's 8-pixel grid against steps elsewhere
	long long notFinite = 0;   // pixels the method did not produce a number for
	int firstX = -1, firstY = -1, minX = 0, maxX = 0, minY = 0, maxY = 0;
	std::map<int, int> rows, columns;   // how many on each, to see the shape of the damage
};

// Anything not finite is counted and read as black: one such pixel would
// otherwise poison every metric, and a method that produces them has a bug
// worth seeing rather than hiding.
static void LumaPlane(const std::vector<float>& rgba, int W, int H, std::vector<float>& y, UpscaleMetrics* m = nullptr)
{
	y.resize((size_t)W * H);
	for (size_t p = 0, i = 0; i < y.size(); p += 4, i++) {
		const float v = Luma(&rgba[p]);
		if (std::isfinite(v)) {
			y[i] = v;
		} else {
			y[i] = 0;
			if (m) {
				const int x = (int)(i % W), row = (int)(i / W);
				if (!m->notFinite) {
					m->firstX = m->minX = m->maxX = x;
					m->firstY = m->minY = m->maxY = row;
				} else {
					m->minX = std::min(m->minX, x);
					m->maxX = std::max(m->maxX, x);
					m->minY = std::min(m->minY, row);
					m->maxY = std::max(m->maxY, row);
				}
				m->rows[row]++;
				m->columns[x]++;
				m->notFinite++;
			}
		}
	}
}

// gridPeriod is where the source's 8x8 compression blocks land in the output;
// 0 when the source was not compressed.
static UpscaleMetrics Measure(const std::vector<float>& outRgba, const std::vector<float>& refRgba, int W, int H,
                              int gridPeriod = 0)
{
	UpscaleMetrics m;

	std::vector<float> yOut, yRef;
	LumaPlane(outRgba, W, H, yOut, &m);
	LumaPlane(refRgba, W, H, yRef);

	double mse = 0;
	for (size_t i = 0; i < yRef.size(); i++) {
		const double d = yOut[i] - yRef[i];
		mse += d * d;
	}
	mse /= yRef.size();
	m.psnr = mse > 0 ? 10.0 * std::log10(1.0 / mse) : 99.0;

	// The same on the most detailed quarter of the picture, in 32-pixel tiles:
	// large soft or flat areas otherwise drown the difference the eye sees.
	{
		const int tile = 32;
		struct Tile { double energy; int x, y; };
		std::vector<Tile> tiles;
		for (int ty = 0; ty + tile <= H; ty += tile) {
			for (int tx = 0; tx + tile <= W; tx += tile) {
				double e = 0;
				for (int y = ty + 1; y < ty + tile; y++) {
					for (int x = tx + 1; x < tx + tile; x++) {
						const size_t i = (size_t)y * W + x;
						e += std::abs(yRef[i] - yRef[i - 1]) + std::abs(yRef[i] - yRef[i - W]);
					}
				}
				tiles.push_back({ e, tx, ty });
			}
		}
		std::sort(tiles.begin(), tiles.end(), [](const Tile& a, const Tile& b) { return a.energy > b.energy; });
		const size_t keep = std::max<size_t>(1, tiles.size() / 4);
		double mseDetail = 0;
		for (size_t t = 0; t < keep && t < tiles.size(); t++) {
			for (int y = tiles[t].y; y < tiles[t].y + tile; y++) {
				for (int x = tiles[t].x; x < tiles[t].x + tile; x++) {
					const double d = (double)yOut[(size_t)y * W + x] - yRef[(size_t)y * W + x];
					mseDetail += d * d;
				}
			}
		}
		mseDetail /= (double)keep * tile * tile;
		m.psnrDetail = mseDetail > 0 ? 10.0 * std::log10(1.0 / mseDetail) : 99.0;
	}

	// SSIM on 8x8 windows every 4 pixels.
	{
		const double C1 = 0.01 * 0.01, C2 = 0.03 * 0.03;
		double sum = 0;
		long long count = 0;
		for (int y0 = 0; y0 + 8 <= H; y0 += 4) {
			for (int x0 = 0; x0 + 8 <= W; x0 += 4) {
				double ma = 0, mb = 0;
				for (int y = 0; y < 8; y++) {
					for (int x = 0; x < 8; x++) {
						ma += yOut[(size_t)(y0 + y) * W + x0 + x];
						mb += yRef[(size_t)(y0 + y) * W + x0 + x];
					}
				}
				ma /= 64; mb /= 64;
				double va = 0, vb = 0, cov = 0;
				for (int y = 0; y < 8; y++) {
					for (int x = 0; x < 8; x++) {
						const double a = yOut[(size_t)(y0 + y) * W + x0 + x] - ma;
						const double b = yRef[(size_t)(y0 + y) * W + x0 + x] - mb;
						va += a * a; vb += b * b; cov += a * b;
					}
				}
				va /= 63; vb /= 63; cov /= 63;
				sum += ((2 * ma * mb + C1) * (2 * cov + C2)) / ((ma * ma + mb * mb + C1) * (va + vb + C2));
				count++;
			}
		}
		m.ssim = count ? sum / count : 0;
	}

	// Edges of the reference: sharpness and overshoot there; flat areas: grain.
	{
		auto gradient = [W](const std::vector<float>& y, int x, int j) {
			const size_t i = (size_t)j * W + x;
			return std::abs(y[i + 1] - y[i - 1]) + std::abs(y[i + W] - y[i - W]);
		};

		double edgeOut = 0, edgeRef = 0;
		double overshoot = 0;
		long long edges = 0;
		double flatOut = 0, flatRef = 0;
		long long flats = 0;
		const double edgeThreshold = 0.08;   // a clear edge in luma units
		const double flatThreshold = 0.01;

		for (int y = 1; y < H - 1; y++) {
			for (int x = 1; x < W - 1; x++) {
				const size_t i = (size_t)y * W + x;
				const double gRef = gradient(yRef, x, y);
				if (gRef > edgeThreshold) {
					edgeRef += gRef;
					edgeOut += gradient(yOut, x, y);
					float low = yRef[i], high = yRef[i];
					for (int dy = -1; dy <= 1; dy++) {
						for (int dx = -1; dx <= 1; dx++) {
							const float v = yRef[i + (size_t)dy * W + dx];
							low = std::min(low, v);
							high = std::max(high, v);
						}
					}
					const double over = std::max({ 0.0, (double)yOut[i] - high, (double)low - yOut[i] });
					overshoot += over;
					edges++;
				} else if (gRef < flatThreshold) {
					// Local detail: the pixel against the mean of its neighbours.
					double meanOut = 0, meanRef = 0;
					for (int dy = -1; dy <= 1; dy++) {
						for (int dx = -1; dx <= 1; dx++) {
							meanOut += yOut[i + (size_t)dy * W + dx];
							meanRef += yRef[i + (size_t)dy * W + dx];
						}
					}
					flatOut += std::abs(yOut[i] - meanOut / 9);
					flatRef += std::abs(yRef[i] - meanRef / 9);
					flats++;
				}
			}
		}

		m.sharp = edgeRef > 0 ? edgeOut / edgeRef : 0;
		m.halo = edges ? overshoot / edges : 0;
		m.grain = flatRef > 0 ? flatOut / flatRef : 0;
	}

	// Blocking: the steps that sit on the compression grid against the steps
	// everywhere else. The reference has none, so 1.00 means the upscaler left
	// nothing of them and anything above says the blocks are still visible.
	if (gridPeriod > 1) {
		double onGrid = 0, offGrid = 0;
		long long onCount = 0, offCount = 0;
		for (int y = 0; y < H; y++) {
			for (int x = 1; x < W; x++) {
				const double step = std::abs(yOut[(size_t)y * W + x] - yOut[(size_t)y * W + x - 1]);
				if (x % gridPeriod == 0) {
					onGrid += step;
					onCount++;
				} else {
					offGrid += step;
					offCount++;
				}
			}
		}
		m.block = (onCount && offCount && offGrid > 0) ? (onGrid / onCount) / (offGrid / offCount) : 0;
	}

	return m;
}

// ------------------------------------------------------------- resize passes --

// The renderer's upscalers, compiled from the same sources with the same
// defines, driven with the constants of TextureResizeShader.
class CUpscalePasses
{
public:
	struct Method {
		const char* name;
		int  index;        // into m_ps
		bool onePass;
		bool easu = false; // AMD FSR 1 EASU: its own constants
	};

	bool Init(ID3D11Device* dev, std::string& error);

	int MethodCount() const { return (int)m_methods.size(); }
	const Method& GetMethod(int i) const { return m_methods[i]; }

	// Hamming, the renderer's default downscaler: what follows a model that
	// overshoots the size. Not one of the compared methods.
	const Method& Downscaler() const { return m_downscaler; }

	// Two separable passes, or one for the single-pass shaders, exactly as
	// ResizeShaderPass does: the intermediate is destination width by source height.
	bool Resize(ID3D11Device* dev, ID3D11DeviceContext* ctx, const Method& method,
	            ID3D11ShaderResourceView* src, UINT srcW, UINT srcH,
	            ID3D11RenderTargetView* dst, UINT dstW, UINT dstH);

	// One pixel shader over a target the size of its source, with its own
	// constant block: a pass that does not resize, which is what a sharpener is.
	void Apply(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, ID3D11ShaderResourceView* src,
	           ID3D11RenderTargetView* dst, UINT w, UINT h, ID3D11Buffer* constants)
	{
		Draw(ctx, ps, src, dst, w, h, w, h, 1.0f, 1.0f, constants);
	}

private:
	bool Compile(ID3D11Device* dev, const wchar_t* file, const D3D_SHADER_MACRO* defines, CComPtr<ID3D11PixelShader>& ps, std::string& error);
	bool LoadCso(ID3D11Device* dev, const wchar_t* file, CComPtr<ID3D11PixelShader>& ps);
	// ownConstants: the pass's block is already filled (EASU's), leave it alone.
	void Draw(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, ID3D11ShaderResourceView* src,
	          ID3D11RenderTargetView* dst, UINT srcW, UINT srcH, UINT dstW, UINT dstH, float scaleX, float scaleY,
	          ID3D11Buffer* ownConstants = nullptr);

	CComPtr<ID3D11VertexShader> m_vs;
	CComPtr<ID3D11InputLayout>  m_layout;
	CComPtr<ID3D11SamplerState> m_point;
	CComPtr<ID3D11Buffer>       m_vb;
	CComPtr<ID3D11Buffer>       m_cb;
	CComPtr<ID3D11Buffer>       m_cbEasu;
	std::vector<CComPtr<ID3D11PixelShader>> m_ps;   // pairs: X then Y, or one for single-pass
	std::vector<Method> m_methods;
	Method m_downscaler = { "Hamming", -1, false };
	Target m_intermediate;
	UINT m_interW = 0, m_interH = 0;
};

bool CUpscalePasses::Compile(ID3D11Device* dev, const wchar_t* file, const D3D_SHADER_MACRO* defines,
                             CComPtr<ID3D11PixelShader>& ps, std::string& error)
{
	CComPtr<ID3DBlob> code, errors;
	// The downscalers include their filter definitions from Shaders\resize.
	const HRESULT hr = D3DCompileFromFile(file, defines, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "ps_4_0",
	                                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
	if (FAILED(hr)) {
		error = errors ? std::string((const char*)errors->GetBufferPointer(), errors->GetBufferSize()) : "shader not found";
		return false;
	}
	return SUCCEEDED(dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps));
}

// Jinc2m ships as a compiled object; the filter embeds that same file.
bool CUpscalePasses::LoadCso(ID3D11Device* dev, const wchar_t* file, CComPtr<ID3D11PixelShader>& ps)
{
	FILE* f = nullptr;
	if (_wfopen_s(&f, file, L"rb") != 0 || !f) {
		return false;
	}
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	std::vector<BYTE> code((size_t)std::max(size, 0L));
	const bool read = size > 0 && fread(code.data(), 1, code.size(), f) == code.size();
	fclose(f);
	return read && SUCCEEDED(dev->CreatePixelShader(code.data(), code.size(), nullptr, &ps));
}

bool CUpscalePasses::Init(ID3D11Device* dev, std::string& error)
{
	const wchar_t* vsFile = L"..\\..\\Shaders\\d3d11\\vs_simple.hlsl";
	CComPtr<ID3DBlob> code, errors;
	HRESULT hr = D3DCompileFromFile(vsFile, nullptr, nullptr, "main", "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
	if (FAILED(hr) || FAILED(dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &m_vs))) {
		error = errors ? std::string((const char*)errors->GetBufferPointer(), errors->GetBufferSize()) : "vs_simple.hlsl";
		return false;
	}
	const D3D11_INPUT_ELEMENT_DESC layout[] = {
		{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}
	};
	if (FAILED(dev->CreateInputLayout(layout, (UINT)std::size(layout), code->GetBufferPointer(), code->GetBufferSize(), &m_layout))) {
		error = "CreateInputLayout failed";
		return false;
	}

	struct Entry { const char* name; const wchar_t* file; const char* method; };
	static const Entry entries[] = {
		{ "Mitchell-Netravali", L"..\\..\\Shaders\\d3d11\\ps_interpolation_spline4.hlsl",  "0" },
		{ "Catmull-Rom",        L"..\\..\\Shaders\\d3d11\\ps_interpolation_spline4.hlsl",  "1" },
		{ "Lanczos2",           L"..\\..\\Shaders\\d3d11\\ps_interpolation_lanczos2.hlsl", nullptr },
		{ "Lanczos3",           L"..\\..\\Shaders\\d3d11\\ps_interpolation_lanczos3.hlsl", nullptr },
	};
	for (const Entry& e : entries) {
		CComPtr<ID3D11PixelShader> x, y;
		for (int axis = 0; axis < 2; axis++) {
			const char* axisText = axis ? "1" : "0";
			D3D_SHADER_MACRO defines[3] = { { "AXIS", axisText }, { nullptr, nullptr }, { nullptr, nullptr } };
			if (e.method) {
				defines[1] = { "METHOD", e.method };
			}
			if (!Compile(dev, e.file, defines, axis ? y : x, error)) {
				error = std::string(e.name) + ": " + error;
				return false;
			}
		}
		m_methods.push_back({ e.name, (int)m_ps.size(), false });
		m_ps.push_back(x);
		m_ps.push_back(y);
	}

	CComPtr<ID3D11PixelShader> jinc;
	if (LoadCso(dev, L"..\\..\\_bin\\shaders\\ps_resize_onepass_jinc2.cso", jinc)) {
		m_methods.push_back({ "Jinc2m", (int)m_ps.size(), true });
		m_ps.push_back(jinc);
	}

	// AMD FSR 1 EASU, when its headers were fetched into upscalers\.
	if (GetFileAttributesW(L"upscalers\\ffx_fsr1.h") != INVALID_FILE_ATTRIBUTES) {
		CComPtr<ID3DBlob> easuCode, easuErrors;
		CComPtr<ID3D11PixelShader> easu;
		const HRESULT easuHr = D3DCompileFromFile(L"upscalers\\ps_fsr_easu.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
			"main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &easuCode, &easuErrors);
		if (SUCCEEDED(easuHr) && SUCCEEDED(dev->CreatePixelShader(easuCode->GetBufferPointer(), easuCode->GetBufferSize(), nullptr, &easu))) {
			Method m = { "AMD FSR 1 EASU", (int)m_ps.size(), true };
			m.easu = true;
			m_methods.push_back(m);
			m_ps.push_back(easu);
		} else {
			printf("  FSR 1 EASU: %s\n", easuErrors ? std::string((const char*)easuErrors->GetBufferPointer(), easuErrors->GetBufferSize()).c_str() : "compile failed");
		}
	}

	{
		CComPtr<ID3D11PixelShader> x, y;
		for (int axis = 0; axis < 2; axis++) {
			const D3D_SHADER_MACRO defines[] = { { "AXIS", axis ? "1" : "0" }, { "FILTER", "2" }, { nullptr, nullptr } };
			if (!Compile(dev, L"..\\..\\Shaders\\d3d11\\ps_convolution.hlsl", defines, axis ? y : x, error)) {
				error = "Hamming: " + error;
				return false;
			}
		}
		m_downscaler.index = (int)m_ps.size();
		m_ps.push_back(x);
		m_ps.push_back(y);
	}

	D3D11_SAMPLER_DESC sd = {};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	dev->CreateSamplerState(&sd, &m_point);

	// The renderer's quad, unrotated and unflipped.
	const Vertex11 quad[4] = {
		{ -1, -1, 0, 0, 1 }, { -1, 1, 0, 0, 0 }, { 1, -1, 0, 1, 1 }, { 1, 1, 0, 1, 0 },
	};
	D3D11_BUFFER_DESC bd = { sizeof(quad), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
	const D3D11_SUBRESOURCE_DATA init = { quad, 0, 0 };
	dev->CreateBuffer(&bd, &init, &m_vb);
	D3D11_BUFFER_DESC cbd = { 32, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
	dev->CreateBuffer(&cbd, nullptr, &m_cb);
	cbd.ByteWidth = 64;
	dev->CreateBuffer(&cbd, nullptr, &m_cbEasu);
	if (!m_point || !m_vb || !m_cb) {
		error = "pipeline state objects";
		return false;
	}
	return true;
}

void CUpscalePasses::Draw(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, ID3D11ShaderResourceView* src,
                          ID3D11RenderTargetView* dst, UINT srcW, UINT srcH, UINT dstW, UINT dstH,
                          float scaleX, float scaleY, ID3D11Buffer* ownConstants)
{
	// PS_CONSTANTS of the resize shaders: the source size, its texel size, and
	// the ratio of source to destination -- TextureResizeShader's block.
	if (!ownConstants) {
		const FLOAT constants[8] = {
			(float)srcW, (float)srcH, 1.0f / srcW, 1.0f / srcH,
			scaleX, scaleY, 0, 0
		};
		D3D11_MAPPED_SUBRESOURCE mr = {};
		if (SUCCEEDED(ctx->Map(m_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr))) {
			memcpy(mr.pData, constants, sizeof(constants));
			ctx->Unmap(m_cb, 0);
		}
	}

	const D3D11_VIEWPORT vp = { 0, 0, (FLOAT)dstW, (FLOAT)dstH, 0, 1 };
	const UINT stride = sizeof(Vertex11), offset = 0;
	ID3D11Buffer* vb = m_vb;
	ID3D11Buffer* cb = ownConstants ? ownConstants : m_cb.p;
	ID3D11SamplerState* samp = m_point;

	ctx->IASetInputLayout(m_layout);
	ctx->OMSetRenderTargets(1, &dst, nullptr);
	ctx->RSSetViewports(1, &vp);
	ctx->OMSetBlendState(nullptr, nullptr, D3D11_DEFAULT_SAMPLE_MASK);
	ctx->VSSetShader(m_vs, nullptr, 0);
	ctx->PSSetShader(ps, nullptr, 0);
	ctx->PSSetShaderResources(0, 1, &src);
	ctx->PSSetSamplers(0, 1, &samp);
	ctx->PSSetConstantBuffers(0, 1, &cb);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
	ctx->Draw(4, 0);

	ID3D11ShaderResourceView* noView = nullptr;
	ctx->PSSetShaderResources(0, 1, &noView);
	ID3D11RenderTargetView* noTarget = nullptr;
	ctx->OMSetRenderTargets(1, &noTarget, nullptr);
}

bool CUpscalePasses::Resize(ID3D11Device* dev, ID3D11DeviceContext* ctx, const Method& method,
                            ID3D11ShaderResourceView* src, UINT srcW, UINT srcH,
                            ID3D11RenderTargetView* dst, UINT dstW, UINT dstH)
{
	if (method.easu) {
		// FsrEasuCon() with the whole input as the viewport.
		const float rx = (float)srcW / dstW, ry = (float)srcH / dstH;
		const FLOAT con[16] = {
			rx, ry, 0.5f * rx - 0.5f, 0.5f * ry - 0.5f,
			1.0f / srcW, 1.0f / srcH, 1.0f / srcW, -1.0f / srcH,
			-1.0f / srcW, 2.0f / srcH, 1.0f / srcW, 2.0f / srcH,
			0.0f, 4.0f / srcH, 0.0f, 0.0f,
		};
		D3D11_MAPPED_SUBRESOURCE mr = {};
		if (FAILED(ctx->Map(m_cbEasu, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr))) {
			return false;
		}
		memcpy(mr.pData, con, sizeof(con));
		ctx->Unmap(m_cbEasu, 0);
		Draw(ctx, m_ps[method.index], src, dst, srcW, srcH, dstW, dstH, rx, ry, m_cbEasu);
		return true;
	}
	if (method.onePass) {
		Draw(ctx, m_ps[method.index], src, dst, srcW, srcH, dstW, dstH, (float)srcW / dstW, (float)srcH / dstH);
		return true;
	}

	if (m_interW != dstW || m_interH != srcH) {
		m_intermediate = Target{};
		if (!MakeTarget(dev, dstW, srcH, false, m_intermediate, DXGI_FORMAT_R16G16B16A16_FLOAT)) {
			return false;
		}
		m_interW = dstW;
		m_interH = srcH;
	}

	Draw(ctx, m_ps[method.index], src, m_intermediate.rtv, srcW, srcH, dstW, srcH, (float)srcW / dstW, 1.0f);
	Draw(ctx, m_ps[method.index + 1], m_intermediate.srv, dst, dstW, srcH, dstW, dstH, 1.0f, (float)srcH / dstH);
	return true;
}

// Puts a picture through one of the resize methods on the GPU: what is left of
// the scale after the neural doubler has done its part.
static bool ResizeOnGpu(ID3D11Device* dev, ID3D11DeviceContext* ctx, CUpscalePasses& passes,
                        const CUpscalePasses::Method& method, const std::vector<float>& src, int sw, int sh,
                        int dw, int dh, std::vector<float>& dst)
{
	Tex2D_t texIn;
	Target texOut;
	CComPtr<ID3D11Texture2D> stage;
	if (FAILED(texIn.CheckCreate(dev, DXGI_FORMAT_R16G16B16A16_FLOAT, sw, sh, Tex2D_DefaultShaderRTarget))
			|| !MakeTarget(dev, dw, dh, false, texOut, DXGI_FORMAT_R16G16B16A16_FLOAT)) {
		return false;
	}

	std::vector<HALF> half(src.size());
	DirectX::PackedVector::XMConvertFloatToHalfStream(half.data(), sizeof(HALF), src.data(), sizeof(float), src.size());
	ctx->UpdateSubresource(texIn.pTexture, 0, nullptr, half.data(), 4 * sizeof(HALF) * sw, 0);

	D3D11_TEXTURE2D_DESC sd = {};
	texOut.tex->GetDesc(&sd);
	sd.Usage = D3D11_USAGE_STAGING;
	sd.BindFlags = 0;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	sd.MiscFlags = 0;
	if (FAILED(dev->CreateTexture2D(&sd, nullptr, &stage))
			|| !passes.Resize(dev, ctx, method, texIn.pShaderResource, sw, sh, texOut.rtv, dw, dh)) {
		return false;
	}
	ctx->CopyResource(stage, texOut.tex);
	return ReadRgbaHalf(ctx, stage, dw, dh, dst);
}

#ifdef HAVE_ONNXRUNTIME

// EfRLFN through ONNX Runtime, for the quality comparison. The picture goes
// through the CPU here. That costs little next to the network itself: a version
// that kept the picture on the GPU, half floats and all, measured no faster.
class COnnxUpscaler
{
public:
	bool Init(const wchar_t* model, std::string& error)
	{
		try {
			m_env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_ERROR, "mpcvr");
			Ort::SessionOptions options;
			// DirectML wants the memory pattern planner off and one stream.
			options.DisableMemPattern();
			options.SetExecutionMode(ORT_SEQUENTIAL);
			Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(options, 0));
			m_session = std::make_unique<Ort::Session>(*m_env, model, options);
		}
		catch (const Ort::Exception& e) {
			error = e.what();
			return false;
		}
		return true;
	}

	bool Run(const std::vector<float>& rgba, int W, int H, std::vector<float>& out, double& ms)
	{
		const size_t plane = (size_t)W * H;
		m_planar.resize(3 * plane);
		for (size_t i = 0; i < plane; i++) {
			for (int c = 0; c < 3; c++) {
				m_planar[c * plane + i] = std::clamp(rgba[4 * i + c], 0.0f, 1.0f);
			}
		}

		const int64_t shape[4] = { 1, 3, H, W };
		const char* inputNames[] = { "input" };
		const char* outputNames[] = { "output" };
		LARGE_INTEGER qpf = {}, c0 = {}, c1 = {};
		QueryPerformanceFrequency(&qpf);
		try {
			auto memory = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU);
			Ort::Value tensor = Ort::Value::CreateTensor<float>(memory, m_planar.data(), m_planar.size(), shape, 4);

			// The first run at a given size builds the kernels; time the second.
			m_session->Run(Ort::RunOptions{ nullptr }, inputNames, &tensor, 1, outputNames, 1);
			QueryPerformanceCounter(&c0);
			auto outputs = m_session->Run(Ort::RunOptions{ nullptr }, inputNames, &tensor, 1, outputNames, 1);
			QueryPerformanceCounter(&c1);

			const auto dims = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
			if (dims.size() != 4 || dims[1] != 3) {
				return false;
			}
			m_width = (int)dims[3];
			m_height = (int)dims[2];
			const size_t outPlane = (size_t)m_width * m_height;
			const float* data = outputs[0].GetTensorData<float>();
			out.resize(4 * outPlane);
			for (size_t i = 0; i < outPlane; i++) {
				for (int c = 0; c < 3; c++) {
					out[4 * i + c] = std::clamp(data[c * outPlane + i], 0.0f, 1.0f);
				}
				out[4 * i + 3] = 1.0f;
			}
		}
		catch (const Ort::Exception&) {
			return false;
		}
		ms = double(c1.QuadPart - c0.QuadPart) * 1000.0 / qpf.QuadPart;
		return true;
	}

	int OutWidth() const { return m_width; }
	int OutHeight() const { return m_height; }

private:
	std::unique_ptr<Ort::Env> m_env;
	std::unique_ptr<Ort::Session> m_session;
	std::vector<float> m_planar;
	int m_width = 0, m_height = 0;
};

#endif // HAVE_ONNXRUNTIME

// ------------------------------------------------------------------ the run --

struct Degradation {
	const char* name;
	float grain;      // luma sigma, 0 for none
	float jpeg;       // quality 0..1, 0 for none
};

// maxRefs limits the run to the first references (--srrefs), noModels leaves out
// EfRLFN, whose seconds per picture otherwise dominate the run (--nomodels).
static int RunUpscale(ID3D11Device* dev, ID3D11DeviceContext* ctx, int maxRefs = 0, bool noModels = false)
{
	Head("Upscaling suite: what each method rebuilds from a reduced picture");

	const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	int rc = 0;
	{
		CComPtr<IWICImagingFactory> factory;
		Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))), "WIC factory");

		// References: whatever was dropped into upscale_refs, else a wallpaper,
		// which is only good enough to prove the suite runs.
		std::vector<std::wstring> files;
		{
			WIN32_FIND_DATAW fd = {};
			for (const wchar_t* pattern : { L"upscale_refs\\*.png", L"upscale_refs\\*.jpg", L"upscale_refs\\*.jpeg" }) {
				HANDLE h = FindFirstFileW(pattern, &fd);
				if (h != INVALID_HANDLE_VALUE) {
					do {
						files.push_back(std::wstring(L"upscale_refs\\") + fd.cFileName);
					} while (FindNextFileW(h, &fd));
					FindClose(h);
				}
			}
			std::sort(files.begin(), files.end());
			if (maxRefs > 0 && files.size() > (size_t)maxRefs) {
				files.resize((size_t)maxRefs);
			}
		}
		const bool bOwnReferences = !files.empty();
		if (!bOwnReferences) {
			files.push_back(L"C:\\Windows\\Web\\Wallpaper\\ThemeB\\img24.jpg");
			printf("  no picture in upscale_refs, falling back to a wallpaper\n");
		}

		CUpscalePasses passes;
		std::string error;
		const bool passesOk = factory && passes.Init(dev, error);
		Check(passesOk, "resize shaders compiled");
		if (!passesOk) {
			printf("  %s\n", error.c_str());
			rc = 1;
		}

		// mpv's user shaders, translated into upscalers\hlsl by mpv_shaders.py. The luma
		// ones run on the luma of the reduced picture and are measured on luma, which is
		// all Measure() compares for the renderer's filters too.
		std::vector<std::unique_ptr<CMpvShader>> lumaShaders;
		if (!rc) {
			std::vector<std::wstring> dirs;
			WIN32_FIND_DATAW fd = {};
			HANDLE h = FindFirstFileW(L"upscalers\\hlsl\\*", &fd);
			if (h != INVALID_HANDLE_VALUE) {
				do {
					if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') {
						dirs.push_back(fd.cFileName);
					}
				} while (FindNextFileW(h, &fd));
				FindClose(h);
			}
			std::sort(dirs.begin(), dirs.end());
			for (const std::wstring& d : dirs) {
				auto shader = std::make_unique<CMpvShader>();
				std::string shaderError;
				if (!shader->Load(dev, L"upscalers\\hlsl\\" + d, shaderError)) {
					printf("  %S: %s\n", d.c_str(), shaderError.c_str());
					g_failures++;
					continue;
				}
				if (!shader->Hooks("LUMA") || shader->Hooks("CHROMA")) {
					continue;   // chroma shaders are --tchroma's
				}
				if (shader->HasPixelOffset()) {
					printf("  mpv shader %s, %d passes, output shifted by %.2f, %.2f pixels (taken back with Catmull-Rom,\n"
						"    as the player's own resize would)\n", shader->Name().c_str(), shader->PassCount(),
						shader->PixelOffsetX(), shader->PixelOffsetY());
					lumaShaders.push_back(std::move(shader));
					continue;
				}
				printf("  mpv shader %s, %d passes\n", shader->Name().c_str(), shader->PassCount());
				lumaShaders.push_back(std::move(shader));
			}
		}

		CComPtr<ID3D11Query> disjoint, tsStart, tsEnd;
		if (!rc) {
			D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
			dev->CreateQuery(&qd, &disjoint);
			qd.Query = D3D11_QUERY_TIMESTAMP;
			dev->CreateQuery(&qd, &tsStart);
			dev->CreateQuery(&qd, &tsEnd);
			Check(disjoint && tsStart && tsEnd, "timestamp queries");
			if (!disjoint || !tsStart || !tsEnd) {
				rc = 1;
			}
		}

		static const Degradation degradations[] = {
			{ "clean",      0.0f,   0.0f  },
			{ "grain",      0.006f, 0.0f  },   // about 1.5/255, a light film grain
			{ "compressed", 0.0f,   0.72f },   // a web release
		};
		static const int factors[] = { 2, 3 };

		if (!rc) {
			g_report = fopen("temporal_results_upscale.txt", "w");
			Out("\nUpscaling on real pictures. Each reference is reduced by an integer factor, degraded the way a\n");
			Out("source is, then brought back to the reference size by each method and compared to the reference.\n\n");
			Out("psnr and ssim: luma, against the reference; psnr-d: psnr on the reference's most detailed quarter,\n");
			Out("where the eye sees the difference. sharp: gradient on the reference's edges, output over\n");
			Out("reference, so 1.00 is the reference's own sharpness. halo: overshoot past the reference's local\n");
			Out("range on those edges, lower is better. grain: fine detail left in flat areas, output over reference.\n");
			Out("block: on the compressed rows only, the steps sitting on the compression grid against the steps\n");
			Out("elsewhere, so 1.00 means the blocks are gone. ms: GPU time of the resize itself.\n");

			// The renderer's filters cover whatever scale a model or a doubler leaves:
			// Catmull-Rom when it falls short, Hamming when it overshoots.
			int catmullRom = -1, jinc = -1;
			for (int mi = 0; mi < passes.MethodCount(); mi++) {
				if (!strcmp(passes.GetMethod(mi).name, "Catmull-Rom")) {
					catmullRom = mi;
				}
				if (!strcmp(passes.GetMethod(mi).name, "Jinc2m")) {
					jinc = mi;
				}
			}
			if (!lumaShaders.empty()) {
				Out("mpv shaders (translated by mpv_shaders.py) run on the reduced picture's luma, as mpv runs them;\n");
				Out("a doubler that falls short of the size is finished by the renderer's Catmull-Rom, timed with it.\n");
			}

#ifdef HAVE_ONNXRUNTIME
			// EfRLFN at x2 and at x4.
			auto loadModel = [&](const wchar_t* model) {
				std::unique_ptr<COnnxUpscaler> result;
				if (noModels) {
					return result;
				}
				if (GetFileAttributesW(model) == INVALID_FILE_ATTRIBUTES) {
					printf("  no %S, skipping it\n", model);
					return result;
				}
				auto candidate = std::make_unique<COnnxUpscaler>();
				std::string onnxError;
				if (candidate->Init(model, onnxError)) {
					result = std::move(candidate);
				} else {
					printf("  %S not available: %s\n", model, onnxError.c_str());
					g_failures++;
				}
				return result;
			};
			std::unique_ptr<COnnxUpscaler> modelX2 = loadModel(L"models\\efrlfn_x2.onnx");
			std::unique_ptr<COnnxUpscaler> modelX4 = loadModel(L"models\\efrlfn_x4.onnx");
			if (modelX2 || modelX4) {
				Out("EfRLFN runs through ONNX Runtime with DirectML, with the picture going through the CPU; its time\n");
				Out("(ms*) is almost all the network's own, and far outside a frame's budget (see --tupscalecost).\n");
			}
#else
			(void)noModels;
#endif

			std::vector<float> lowres, upscaled, work;
			for (const std::wstring& file : files) {
				UpscaleRef ref;
				ref.name = file.substr(file.find_last_of(L'\\') + 1);
				if (!LoadReference(factory, file.c_str(), ref, error)) {
					Out("\n%S: %s\n", ref.name.c_str(), error.c_str());
					g_failures++;
					continue;
				}
				Out("\n%S -- reference %dx%d%s\n", ref.name.c_str(), ref.W, ref.H,
				    ref.bTrue4K ? "" : " (narrower than 3840: ranks the methods, but does not stand for a 4K screen)");
				const POINT detail = FindDetailWindow(ref.rgba, ref.W, ref.H, std::min(960, ref.W), std::min(540, ref.H));

				for (const int factor : factors) {
					if (ref.W % factor || ref.H % factor) {
						continue;
					}
					const int lw = ref.W / factor, lh = ref.H / factor;

					for (const Degradation& deg : degradations) {
						DownscaleBox(ref.rgba, ref.W, ref.H, factor, lowres);
						if (deg.grain > 0) {
							AddGrain(lowres, lw, lh, deg.grain, 0xBEEF1234u + factor);
						}
						if (deg.jpeg > 0 && !JpegRoundTrip(factory, lowres, lw, lh, deg.jpeg, error)) {
							Out("  %s\n", error.c_str());
							g_failures++;
							continue;
						}

						Out("\n  %dx%d -> %dx%d (x%d), %s\n", lw, lh, ref.W, ref.H, factor, deg.name);
						Out("  %-24s %8s %8s %7s %7s %8s %7s %7s %9s\n",
						    "method", "psnr", "psnr-d", "ssim", "sharp", "halo", "grain", "block", "time");

						// Kept for the board: Jinc2m, the best of the current filters, and the models.
						std::vector<float> keptJinc, keptCatmull, keptX2, keptX4;

						// What the reduction alone costs, as a floor: the low-resolution
						// picture blown up by nearest neighbour is not measured, but the
						// reference against itself would read psnr 99.
						Tex2D_t texLow;
						Target texHigh;
						CComPtr<ID3D11Texture2D> stage;
						{
							std::vector<HALF> half(lowres.size());
							DirectX::PackedVector::XMConvertFloatToHalfStream(half.data(), sizeof(HALF), lowres.data(), sizeof(float), lowres.size());
							const bool ok = SUCCEEDED(texLow.CheckCreate(dev, DXGI_FORMAT_R16G16B16A16_FLOAT, lw, lh, Tex2D_DefaultShaderRTarget))
								&& MakeTarget(dev, ref.W, ref.H, false, texHigh, DXGI_FORMAT_R16G16B16A16_FLOAT);
							if (ok) {
								ctx->UpdateSubresource(texLow.pTexture, 0, nullptr, half.data(), 4 * sizeof(HALF) * lw, 0);
								D3D11_TEXTURE2D_DESC sd = {};
								texHigh.tex->GetDesc(&sd);
								sd.Usage = D3D11_USAGE_STAGING;
								sd.BindFlags = 0;
								sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
								sd.MiscFlags = 0;
								dev->CreateTexture2D(&sd, nullptr, &stage);
							}
							if (!ok || !stage) {
								Out("  textures failed\n");
								g_failures++;
								continue;
							}
						}

						for (int mi = 0; mi < passes.MethodCount(); mi++) {
							const CUpscalePasses::Method& method = passes.GetMethod(mi);

							// Two warm-ups, then a batch: one pass alone is shorter than the
							// GPU takes to come back up to speed after the metrics ran on the CPU.
							const int kRuns = 16;
							for (int warm = 0; warm < 2; warm++) {
								passes.Resize(dev, ctx, method, texLow.pShaderResource, lw, lh, texHigh.rtv, ref.W, ref.H);
							}
							ctx->Begin(disjoint);
							ctx->End(tsStart);
							for (int run = 0; run < kRuns; run++) {
								passes.Resize(dev, ctx, method, texLow.pShaderResource, lw, lh, texHigh.rtv, ref.W, ref.H);
							}
							ctx->End(tsEnd);
							ctx->End(disjoint);

							D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
							UINT64 t0 = 0, t1 = 0;
							while (ctx->GetData(disjoint, &dj, sizeof(dj), 0) == S_FALSE) {
								Sleep(0);
							}
							while (ctx->GetData(tsStart, &t0, sizeof(t0), 0) == S_FALSE) {
								Sleep(0);
							}
							while (ctx->GetData(tsEnd, &t1, sizeof(t1), 0) == S_FALSE) {
								Sleep(0);
							}
							const double ms = (!dj.Disjoint && dj.Frequency) ? double(t1 - t0) * 1000.0 / dj.Frequency / kRuns : 0.0;

							ctx->CopyResource(stage, texHigh.tex);
							if (!ReadRgbaHalf(ctx, stage, ref.W, ref.H, upscaled)) {
								Out("  %-24s readback failed\n", method.name);
								g_failures++;
								continue;
							}

							const UpscaleMetrics m = Measure(upscaled, ref.rgba, ref.W, ref.H, deg.jpeg > 0 ? 8 * factor : 0);
							Out("  %-24s %8.3f %8.3f %7.4f %7.3f %8.5f %7.3f %7.3f %6.2f ms",
							    method.name, m.psnr, m.psnrDetail, m.ssim, m.sharp, m.halo, m.grain, m.block, ms);
							if (m.notFinite) {
								// The shape says where to look: a whole row or column is a
								// position problem, a sprinkle is a value problem.
								std::string worstRows, worstColumns;
								for (const auto& [row, count] : m.rows) {
									if (count > 16 && worstRows.size() < 40) {
										worstRows += std::format("y{}x{} ", row, count);
									}
								}
								for (const auto& [column, count] : m.columns) {
									if (count > 16 && worstColumns.size() < 40) {
										worstColumns += std::format("x{}x{} ", column, count);
									}
								}
								Out("   (%lld not a number, first at %d,%d, %zu rows %zu columns: %s%s)",
								    m.notFinite, m.firstX, m.firstY, m.rows.size(), m.columns.size(),
								    worstRows.c_str(), worstColumns.c_str());
								g_failures++;
							}
							Out("\n");

							if (mi == jinc) {
								keptJinc = upscaled;
							}
							if (mi == catmullRom) {
								keptCatmull = upscaled;
							}
						}

						// mpv's shaders on the luma plane. The row reads like the others: the
						// output goes back as grey, whose luma is the plane itself.
						std::map<std::string, std::vector<float>> keptShaders;
						if (!lumaShaders.empty()) {
							std::vector<HALF> yHalf(4 * (size_t)lw * lh);
							const HALF zero = DirectX::PackedVector::XMConvertFloatToHalf(0.0f);
							const HALF one = DirectX::PackedVector::XMConvertFloatToHalf(1.0f);
							for (size_t i = 0; i < (size_t)lw * lh; i++) {
								yHalf[4 * i] = DirectX::PackedVector::XMConvertFloatToHalf(Luma(&lowres[4 * i]));
								yHalf[4 * i + 1] = zero;
								yHalf[4 * i + 2] = zero;
								yHalf[4 * i + 3] = one;
							}
							MpvTexture yPlane;
							if (!MakeMpvTexture(dev, lw, lh, yPlane, yHalf.data(), 4 * sizeof(HALF) * lw)) {
								Out("  luma plane texture failed\n");
								g_failures++;
							}
							for (auto& shader : lumaShaders) {
								if (!yPlane.tex) {
									break;
								}
								std::string runError;
								auto runOnce = [&]() {
									std::map<std::string, MpvTexture> planes;
									planes["LUMA"] = yPlane;
									if (shader->Run(dev, ctx, "LUMA", planes, ref.W, ref.H, runError) < 0) {
										return false;
									}
									const MpvTexture& y = planes["LUMA"];
									if (y.w == (UINT)ref.W && y.h == (UINT)ref.H) {
										ctx->CopyResource(texHigh.tex, y.tex);
										return true;
									}
									if (y.w > (UINT)ref.W || catmullRom < 0) {
										runError = std::format("left the plane at {}x{}", y.w, y.h);
										return false;
									}
									return passes.Resize(dev, ctx, passes.GetMethod(catmullRom), y.srv, y.w, y.h, texHigh.rtv, ref.W, ref.H);
								};
								bool ok = runOnce() && runOnce();   // warm-up
								const int kRuns = 4;
								if (ok) {
									ctx->Begin(disjoint);
									ctx->End(tsStart);
									for (int run = 0; run < kRuns && ok; run++) {
										ok = runOnce();
									}
									ctx->End(tsEnd);
									ctx->End(disjoint);
								}
								if (!ok) {
									Out("  %-24s failed: %s\n", shader->Name().c_str(), runError.c_str());
									g_failures++;
									continue;
								}
								D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
								UINT64 t0 = 0, t1 = 0;
								while (ctx->GetData(disjoint, &dj, sizeof(dj), 0) == S_FALSE) {
									Sleep(0);
								}
								while (ctx->GetData(tsStart, &t0, sizeof(t0), 0) == S_FALSE) {
									Sleep(0);
								}
								while (ctx->GetData(tsEnd, &t1, sizeof(t1), 0) == S_FALSE) {
									Sleep(0);
								}
								const double ms = (!dj.Disjoint && dj.Frequency) ? double(t1 - t0) * 1000.0 / dj.Frequency / kRuns : 0.0;

								ctx->CopyResource(stage, texHigh.tex);
								if (!ReadRgbaHalf(ctx, stage, ref.W, ref.H, upscaled)) {
									Out("  %-24s readback failed\n", shader->Name().c_str());
									g_failures++;
									continue;
								}
								if (shader->HasPixelOffset()) {
									// Where the shader says it left its result, put back. The sign is
									// the one measured: a sweep of shifts peaks at the declared offset.
									ShiftPlane(upscaled, ref.W, ref.H, shader->PixelOffsetX(), shader->PixelOffsetY());
								}
								for (size_t p = 0; p + 3 < upscaled.size(); p += 4) {
									upscaled[p + 1] = upscaled[p + 2] = upscaled[p];
									upscaled[p + 3] = 1.0f;
								}
								const UpscaleMetrics m = Measure(upscaled, ref.rgba, ref.W, ref.H, deg.jpeg > 0 ? 8 * factor : 0);
								Out("  %-24s %8.3f %8.3f %7.4f %7.3f %8.5f %7.3f %7.3f %6.2f ms",
								    shader->Name().c_str(), m.psnr, m.psnrDetail, m.ssim, m.sharp, m.halo, m.grain, m.block, ms);
								{
									// The same output, held to the values the source really
									// carries around each pixel: the ringing a doubler adds
									// against the detail that costs.
									std::vector<float> ringed(upscaled);
									AntiRingPlane(ringed, ref.W, ref.H, lowres, lw, lh, factor, 0.8f);
									for (size_t q = 0; q + 3 < ringed.size(); q += 4) {
										ringed[q + 1] = ringed[q + 2] = ringed[q];
									}
									const UpscaleMetrics mar = Measure(ringed, ref.rgba, ref.W, ref.H, deg.jpeg > 0 ? 8 * factor : 0);
									Out("\n  %-24s %8.3f %8.3f %7.4f %7.3f %8.5f %7.3f %7.3f %6.2f ms",
									    (shader->Name() + " AR").c_str(), mar.psnr, mar.psnrDetail, mar.ssim,
									    mar.sharp, mar.halo, mar.grain, mar.block, ms);
								}
								if (m.notFinite) {
									Out("   (%lld not a number)", m.notFinite);
									g_failures++;
								}
								Out("\n");
								// Back to colour the way a renderer would do it: Catmull-Rom's picture
								// with its luma replaced by the shader's, chroma untouched.
								if (keptCatmull.size() == upscaled.size()) {
									for (size_t p = 0; p + 3 < upscaled.size(); p += 4) {
										const float delta = upscaled[p] - Luma(&keptCatmull[p]);
										for (int k = 0; k < 3; k++) {
											upscaled[p + k] = std::clamp(keptCatmull[p + k] + delta, 0.0f, 1.0f);
										}
									}
								}
								keptShaders[shader->Name()] = upscaled;
							}
						}

#ifdef HAVE_ONNXRUNTIME
						// One model row: the network, then the renderer's filter for the
						// scale it leaves -- Catmull-Rom up, Hamming down.
						auto modelRow = [&](COnnxUpscaler* model, const char* name, std::vector<float>& kept) {
							std::vector<float> raw;
							double ms = 0;
							if (!model->Run(lowres, lw, lh, raw, ms)) {
								Out("  %-24s failed to run\n", name);
								g_failures++;
								return;
							}
							const int mw = model->OutWidth(), mh = model->OutHeight();
							if (mw == ref.W && mh == ref.H) {
								kept.swap(raw);
							} else {
								const CUpscalePasses::Method& filter = (mw > ref.W) ? passes.Downscaler() : passes.GetMethod(catmullRom);
								if (!ResizeOnGpu(dev, ctx, passes, filter, raw, mw, mh, ref.W, ref.H, kept)) {
									Out("  %-24s could not be resized to the reference\n", name);
									g_failures++;
									return;
								}
							}
							const UpscaleMetrics m = Measure(kept, ref.rgba, ref.W, ref.H, deg.jpeg > 0 ? 8 * factor : 0);
							Out("  %-24s %8.3f %8.3f %7.4f %7.3f %8.5f %7.3f %7.3f %6.0f ms*", name,
							    m.psnr, m.psnrDetail, m.ssim, m.sharp, m.halo, m.grain, m.block, ms);
							if (m.notFinite) {
								Out("   (%lld not a number)", m.notFinite);
								g_failures++;
							}
							Out("\n");
						};
						if (modelX2 && catmullRom >= 0) {
							modelRow(modelX2.get(), (factor == 2) ? "EfRLFN x2" : "EfRLFN x2 + Catmull-Rom", keptX2);
						}
						if (modelX4) {
							modelRow(modelX4.get(), "EfRLFN x4 + Hamming", keptX4);
						}
#endif

						// Reference, Jinc2m, EfRLFN x2, EfRLFN x4: one picture per case.
						const std::wstring board = std::format(L"upscale_{}_{}_x{}_board.png", ref.name,
							std::wstring(deg.name, deg.name + strlen(deg.name)), factor);
						SaveBoard(factory, board.c_str(), {
							&ref.rgba,
							keptJinc.empty() ? nullptr : &keptJinc,
							keptX2.empty() ? nullptr : &keptX2,
							keptX4.empty() ? nullptr : &keptX4 }, ref.W, ref.H, detail);

						// Close up on the most detailed window: the reference, the renderer's
						// Jinc2m and Catmull-Rom, and three of the shaders, all in colour.
						if (!keptShaders.empty()) {
							auto kept = [&](const char* name) {
								const auto it = keptShaders.find(name);
								return (it == keptShaders.end()) ? nullptr : &it->second;
							};
							const POINT zoomAt = FindDetailWindow(ref.rgba, ref.W, ref.H, 320, 180);
							const std::wstring zoomBoard = std::format(L"upscale_{}_{}_x{}_zoom.png", ref.name,
								std::wstring(deg.name, deg.name + strlen(deg.name)), factor);
							SaveZoomBoard(factory, zoomBoard.c_str(), {
								&ref.rgba,
								keptJinc.empty() ? nullptr : &keptJinc,
								keptCatmull.empty() ? nullptr : &keptCatmull,
								kept("FSRCNNX_x2_16-0-4-1"),
								kept("ravu-zoom-ar-r3"),
								kept("ArtCNN_C4F16") }, ref.W, ref.H, zoomAt);
						}
					}
				}
			}

			fclose(g_report);
			g_report = nullptr;
			printf("\n  written to temporal_results_upscale.txt, boards saved as upscale_*_board.png\n");
			printf("  (each board: reference top left, Jinc2m top right, EfRLFN x2 bottom left, EfRLFN x4 bottom right)\n");
			if (!bOwnReferences) {
				printf("  put real 4K frames in %s\\upscale_refs to get meaningful numbers\n", "tools\\dlssnr_probe");
			}
		}
	}
	if (SUCCEEDED(coInit)) {
		CoUninitialize();
	}
	return (rc || g_failures) ? 1 : 0;
}

// -------------------------------------------------------------------- cost --

#ifdef HAVE_ONNXRUNTIME

// --tupscalecost: what EfRLFN costs on this GPU at the sizes films come in,
// against the budget of a frame. Content does not change the time; the picture is
// a ramp with some noise so nothing can be skipped.
//
// Measured 2026-09-16 on an RTX 3050 6 GB with DirectML, best of three: 222 ms at
// 960x540, 400 ms at 1280x720, 922 ms at 1920x1080 -- linear in the pixel count,
// every node on the GPU. The network does about 535 000 multiply-adds per input
// pixel (six blocks of three 3x3 convolutions over 52 channels, at the input
// size), 1.1 trillion for a 1080p frame: no runtime brings that into the 41.7 ms
// of a film frame on a mid-range card, so the renderer does not ship it. A
// session with the sizes fixed, which lets DirectML fuse the graph, was no
// faster at 540p and hung the GPU at 1080p; it is left out of this suite.
static int RunUpscaleCost(ID3D11Device* dev, ID3D11DeviceContext* ctx)
{
	(void)dev; (void)ctx;
	Head("EfRLFN cost at film sizes");

	std::string error;
	COnnxUpscaler model;
	if (!model.Init(L"models\\efrlfn_x2.onnx", error)) {
		printf("  models\\efrlfn_x2.onnx: %s\n", error.c_str());
		return 1;
	}

	g_report = fopen("temporal_results_upscalecost.txt", "w");
	Out("\nEfRLFN x2 through ONNX Runtime and DirectML on this GPU, best of three runs after a warm-up, the\n");
	Out("picture copied through the CPU (a few ms of the total at these sizes).\n\n");
	Out("  %-10s %10s %14s\n", "input", "ms", "frames per s");

	struct Size { int w, h; };
	static const Size sizes[] = { { 960, 540 }, { 1280, 720 }, { 1920, 1080 } };
	std::vector<float> picture, out;
	for (const Size& s : sizes) {
		picture.assign(4 * (size_t)s.w * s.h, 1.0f);
		Rng r(1234);
		for (int y = 0; y < s.h; y++) {
			for (int x = 0; x < s.w; x++) {
				float* p = &picture[4 * ((size_t)y * s.w + x)];
				p[0] = std::clamp(0.2f + 0.6f * x / s.w + 0.02f * r.Gauss(), 0.0f, 1.0f);
				p[1] = std::clamp(0.2f + 0.6f * y / s.h + 0.02f * r.Gauss(), 0.0f, 1.0f);
				p[2] = std::clamp(0.5f + 0.02f * r.Gauss(), 0.0f, 1.0f);
			}
		}

		double best = 1e9, ms = 0;
		for (int run = 0; run < 3; run++) {
			if (!model.Run(picture, s.w, s.h, out, ms)) {
				best = -1;
				break;
			}
			best = std::min(best, ms);
		}
		if (best < 0) {
			Out("  %-10s failed to run\n", std::format("{}x{}", s.w, s.h).c_str());
			g_failures++;
			continue;
		}
		Out("  %-10s %10.1f %14.2f\n", std::format("{}x{}", s.w, s.h).c_str(), best, 1000.0 / best);
	}

	Out("\nA film frame at 24 frames per second has 41.7 ms for everything; at 60, 16.7 ms.\n");
	fclose(g_report);
	g_report = nullptr;
	printf("\n  written to temporal_results_upscalecost.txt\n");
	return g_failures ? 1 : 0;
}

#else // !HAVE_ONNXRUNTIME

static int RunUpscaleCost(ID3D11Device*, ID3D11DeviceContext*)
{
	Head("EfRLFN cost at film sizes");
	printf("  built without ONNX Runtime: fetch it into external\\onnxruntime and rebuild\n");
	return 1;
}

#endif // HAVE_ONNXRUNTIME

} // namespace temporal
