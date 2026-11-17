// --ttonemap: the measured peak the HDR tone mapping follows, against libplacebo's.
//
// Tone mapping by the measured peak (Measured peak, the Measured method of the
// conversion to SDR) is only as good as the peak it is given: too low and the
// highlights above it are cut flat, too high and the whole picture is pressed down for
// nothing. What decides it is how the measurement follows the film -- above all at a
// cut, where the right peak changes at once.
//
// So the pictures are a film made for the purpose: four scenes cut together, each a
// reference picture made HDR with a known peak (diffuse white at 100 nits, the
// brightest parts expanded to the scene's peak), and a light grain that changes every
// picture so that nothing is perfectly still. Each picture goes through the filter's
// own measurement, cs_hdr_hist.hlsl and cs_hdr_resolve.hlsl compiled from its source
// with the constants the filter gives them, and through libplacebo's, written again
// here from pl_shader_detect_peak and update_peak_buf (v7.387, LGPL-2.1+): the
// histogram's peak, then a low-pass of 20 pictures that a change in the mean
// brightness opens wide (its scene change hysteresis, 1 to 3 hundredths of PQ). Both
// are then judged against the peak of the picture itself, the one a measurement that
// knew the future would use, through the BT.2390 curve the filter offers, to a display
// of 750 nits:
//
//   peak err  how far the peak used is from the picture's own, on average
//   settle    pictures after the cut until the peak used is within 10% of the scene's
//   clipped   of the highlights (above 200 nits), the share brighter than the peak used,
//             cut flat by the curve, per cent
//   off       how far the curve puts the highlights from where the picture's own peak
//             puts them, in nits on the display, on average
//
// The filter's default restarts its window at no cut ("madVR never restarts it"), and
// the registry's SdrPeakSceneCuts makes it restart; both are run.

#include "../../Source/IVideoRenderer.h"   // the registry's window and floor

namespace temporal {

namespace {

struct TmScene {
	int ref;          // which reference picture
	double peak;      // nits its brightest parts are expanded to
	int frames;
};

const TmScene kTmScenes[] = {
	{ 0, 1000.0, 48 },
	{ 1, 4000.0, 72 },
	{ 2,  300.0, 72 },
	{ 0, 1000.0, 48 },
};

constexpr int kTmW = 1920, kTmH = 800;   // halved from the 4K references, scope ones included
constexpr double kTmDisplay = 750.0;
constexpr double kTmFps = 24.0;

double PqFromNits(double nits)
{
	const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
	const double y = std::pow(std::max(nits, 0.0) / 10000.0, m1);
	return std::pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

double NitsFromPq(double pq)
{
	const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
	const double e = std::pow(std::clamp(pq, 0.0, 1.0), 1.0 / m2);
	return 10000.0 * std::pow(std::max(e - c1, 0.0) / (c2 - c3 * e), 1.0 / m1);
}

// The SDR picture made HDR, its light first divided by its own peak (LinearPeak): below
// half of that it stays diffuse, up to 100 nits, and what is above climbs to the scene's peak.
double Expand(double linear, double peak)
{
	if (linear <= 0.5) {
		return 200.0 * linear;
	}
	return 100.0 + (peak - 100.0) * std::pow((linear - 0.5) / 0.5, 2.4);
}

// BT.2390's curve on a luminance, from a source peak to the display's, as the filter's
// BT2390Tonemap draws it (in PQ, Hermite spline above the knee), with what lies above
// the source peak held at the display's: the shader extrapolates the spline there, and
// the display cuts it.
double Bt2390(double nits, double sourcePeak, double displayPeak)
{
	if (displayPeak >= sourcePeak) {
		return nits;
	}
	const double maxPq = PqFromNits(sourcePeak), target = PqFromNits(displayPeak);
	const double e1 = PqFromNits(nits);
	const double ks = std::max(0.0, 1.5 * target - 0.5 * maxPq);
	double e2 = e1;
	if (e1 > ks) {
		const double t = std::min((e1 - ks) / std::max(1e-6, maxPq - ks), 1.0);
		const double t2 = t * t, t3 = t2 * t;
		e2 = (2 * t3 - 3 * t2 + 1) * ks + (t3 - 2 * t2 + t) * (maxPq - ks) + (-2 * t3 + 3 * t2) * target;
	}
	return NitsFromPq(e2);
}

uint32_t TmHash(uint32_t x)
{
	x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
	return x;
}

// One picture of the film: RGBA in nits, and what is true of it.
struct TmFrame {
	std::vector<float> nits;   // RGBA
	std::vector<uint16_t> pq;  // the same in PQ, as half floats: what the filter's textures hold
	double peak9999 = 0;       // the peak 1 pixel in 10 000 may pass, as the filter's
	double peak100 = 0;
	double avgPq = 0;          // the mean of PQ luminance over the pixels above 1% PQ
	double activeShare = 0;    // and the share of pixels that are
};

// The light 1 pixel in 10 000 passes in a linear picture: what is made the scene's peak,
// so that the peak each scene is said to have is the one its pictures really have.
double LinearPeak(const std::vector<float>& linear)
{
	const size_t n = (size_t)kTmW * kTmH;
	std::vector<float> m(n);
	for (size_t i = 0; i < n; i++) {
		m[i] = std::max({ linear[i * 4], linear[i * 4 + 1], linear[i * 4 + 2] });
	}
	const size_t k = n - 1 - n / 10000;
	std::nth_element(m.begin(), m.begin() + k, m.end());
	return std::max((double)m[k], 1e-3);
}

void MakeTmFrame(const std::vector<float>& linear, double linearPeak, double peak, int frame, TmFrame& f)
{
	const size_t n = (size_t)kTmW * kTmH;
	f.nits.resize(n * 4);
	f.pq.resize(n * 4);
	const uint16_t one = DirectX::PackedVector::XMConvertFloatToHalf(1.0f);
	std::vector<float> maxRgb(n);
	double sumPq = 0;
	size_t active = 0;
	for (size_t i = 0; i < n; i++) {
		// A grain of 1%, new each picture.
		const double g = 1.0 + 0.01 * ((TmHash((uint32_t)(i * 2654435761u) ^ (uint32_t)(frame * 40503u)) & 0xffff) / 32767.5 - 1.0);
		double m = 0;
		for (int c = 0; c < 3; c++) {
			const double v = Expand(std::min(linear[i * 4 + c] / linearPeak, 1.0), peak) * g;
			f.nits[i * 4 + c] = (float)v;
			f.pq[i * 4 + c] = DirectX::PackedVector::XMConvertFloatToHalf((float)PqFromNits(v));
			m = std::max(m, v);
		}
		f.nits[i * 4 + 3] = 1.0f;
		f.pq[i * 4 + 3] = one;
		maxRgb[i] = (float)m;
		const double y = 0.2627 * f.nits[i * 4] + 0.6780 * f.nits[i * 4 + 1] + 0.0593 * f.nits[i * 4 + 2];
		const double pq = PqFromNits(y);
		if (pq > 0.01) {
			sumPq += pq;
			active++;
		}
	}
	f.activeShare = (double)active / n;
	f.avgPq = active ? sumPq / active : 0.0;
	std::vector<float> sorted(maxRgb);
	const size_t k = n - 1 - n / 10000;
	std::nth_element(sorted.begin(), sorted.begin() + k, sorted.end());
	f.peak9999 = sorted[k];
	f.peak100 = *std::max_element(maxRgb.begin(), maxRgb.end());
}

// libplacebo's tracking: the frame's peak (percentile of max RGB, or its maximum), a
// low-pass of `period` pictures, opened by a change of the mean.
struct PlaceboTracker {
	double period = 20.0, low = 1.0, high = 3.0, percentile = 100.0;
	double maxPq = 0, avgPq = 0;
	bool started = false;

	double Next(const TmFrame& f)
	{
		const double framePeak = percentile >= 100.0 ? f.peak100 : f.peak9999;
		const double maxNow = PqFromNits(framePeak), avgNow = f.avgPq;
		if (!started) {
			started = true;
			maxPq = maxNow;
			avgPq = avgNow;
			return framePeak;
		}
		const double coeff = 1.0 - std::exp(-1.0 / period);
		maxPq += coeff * (maxNow - maxPq);
		avgPq += coeff * (avgNow - avgPq);
		const double tLow = low * 1e-2, tHigh = high * 1e-2;
		const double delta = f.activeShare * std::abs(avgNow - avgPq);
		const double x = std::clamp((delta - tLow) / (tHigh - tLow), 0.0, 1.0);
		const double mix = x * x * (3.0 - 2.0 * x);
		maxPq += mix * (maxNow - maxPq);
		avgPq += mix * (avgNow - avgPq);
		return NitsFromPq(maxPq);
	}
};

// The filter's measurement, as its own shaders run it.
struct FilterTracker {
	CComPtr<ID3D11ComputeShader> hist, resolve;
	CComPtr<ID3D11Buffer> histBuf, stateBuf, measureCb, resolveCb, stage;
	CComPtr<ID3D11UnorderedAccessView> histUav, stateUav;
	CComPtr<ID3D11Texture2D> tex;
	CComPtr<ID3D11ShaderResourceView> srv;

	bool Init(ID3D11Device* dev, std::string& error)
	{
		const auto compile = [&](const wchar_t* file, CComPtr<ID3D11ComputeShader>& cs) {
			CComPtr<ID3DBlob> code, errors;
			if (FAILED(D3DCompileFromFile(file, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_0",
					D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
				error = errors ? std::string((const char*)errors->GetBufferPointer(), errors->GetBufferSize()) : "compile";
				return false;
			}
			return SUCCEEDED(dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs));
		};
		if (!compile(L"..\\..\\Shaders\\d3d11\\cs_hdr_hist.hlsl", hist) || !compile(L"..\\..\\Shaders\\d3d11\\cs_hdr_resolve.hlsl", resolve)) {
			return false;
		}
		D3D11_BUFFER_DESC bd = {};
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bd.ByteWidth = 1024 * 4;
		bd.StructureByteStride = 4;
		std::vector<uint32_t> zeros(1024 * 4, 0);   // also the state's 517 float4, and more
		const D3D11_SUBRESOURCE_DATA init = { zeros.data(), 0, 0 };
		if (FAILED(dev->CreateBuffer(&bd, &init, &histBuf))) {
			return false;
		}
		bd.ByteWidth = (5 + 512) * 16;
		bd.StructureByteStride = 16;
		std::vector<float> stateZeros((5 + 512) * 4, 0.0f);
		const D3D11_SUBRESOURCE_DATA stateInit = { stateZeros.data(), 0, 0 };
		if (FAILED(dev->CreateBuffer(&bd, &stateInit, &stateBuf))) {
			return false;
		}
		D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
		ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		ud.Buffer.NumElements = 1024;
		if (FAILED(dev->CreateUnorderedAccessView(histBuf, &ud, &histUav))) {
			return false;
		}
		ud.Buffer.NumElements = 5 + 512;
		if (FAILED(dev->CreateUnorderedAccessView(stateBuf, &ud, &stateUav))) {
			return false;
		}
		D3D11_BUFFER_DESC cd = {};
		cd.Usage = D3D11_USAGE_DEFAULT;
		cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cd.ByteWidth = 16;
		if (FAILED(dev->CreateBuffer(&cd, nullptr, &measureCb))) {
			return false;
		}
		cd.ByteWidth = 32;
		if (FAILED(dev->CreateBuffer(&cd, nullptr, &resolveCb))) {
			return false;
		}
		D3D11_BUFFER_DESC sd = {};
		sd.Usage = D3D11_USAGE_STAGING;
		sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		sd.ByteWidth = 64;
		if (FAILED(dev->CreateBuffer(&sd, nullptr, &stage))) {
			return false;
		}
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = kTmW;
		td.Height = kTmH;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		return SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &tex)) && SUCCEEDED(dev->CreateShaderResourceView(tex, nullptr, &srv));
	}

	// The filter's constants (MeasureHdrPeak): a picture of 1/24 s, a release of 1 s,
	// one pixel in 10 000 allowed past the peak, the window and floor of the registry's
	// defaults, and a scene cut at a change of 0.10 in mean PQ when cuts restart it.
	double Next(ID3D11DeviceContext* ctx, const TmFrame& f, bool sceneCuts)
	{
		ctx->UpdateSubresource(tex, 0, nullptr, f.pq.data(), kTmW * 4 * sizeof(uint16_t), 0);
		const UINT measure[4] = { 0, 0, (UINT)kTmW, (UINT)kTmH };
		const float resolveConstants[8] = { (float)(1.0 / kTmFps), 1.0f, sceneCuts ? 0.10f : 2.0f, 1.0e-4f,
			SDR_PEAK_WINDOW_DEF / 1000.0f, (float)SDR_PEAK_FLOOR_DEF, 0, 0 };
		ctx->UpdateSubresource(measureCb, 0, nullptr, measure, 0, 0);
		ctx->UpdateSubresource(resolveCb, 0, nullptr, resolveConstants, 0, 0);
		ID3D11ShaderResourceView* views[1] = { srv };
		ID3D11UnorderedAccessView* uavs[2] = { histUav, stateUav };
		ctx->CSSetShader(hist, nullptr, 0);
		ctx->CSSetShaderResources(0, 1, views);
		ctx->CSSetConstantBuffers(0, 1, &measureCb.p);
		ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		ctx->Dispatch((kTmW + 31) / 32, (kTmH + 31) / 32, 1);
		views[0] = nullptr;
		ctx->CSSetShaderResources(0, 1, views);
		ctx->CSSetShader(resolve, nullptr, 0);
		ctx->CSSetConstantBuffers(0, 1, &resolveCb.p);
		ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
		ctx->Dispatch(1, 1, 1);
		ID3D11UnorderedAccessView* none[2] = {};
		ctx->CSSetUnorderedAccessViews(0, 2, none, nullptr);
		ctx->CSSetShader(nullptr, nullptr, 0);
		const D3D11_BOX box = { 0, 0, 0, 64, 1, 1 };
		ctx->CopySubresourceRegion(stage, 0, 0, 0, 0, stateBuf, 0, &box);
		D3D11_MAPPED_SUBRESOURCE mr = {};
		if (FAILED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &mr))) {
			return 0;
		}
		const double used = ((const float*)mr.pData)[6];   // state[1].z, the peak the curve is given
		ctx->Unmap(stage, 0);
		return used;
	}
};

struct TmRow {
	const char* name;
	std::vector<double> used;
};

} // namespace

int RunToneMap(ID3D11Device* dev, ID3D11DeviceContext* ctx)
{
	Head("The measured peak HDR tone mapping follows");
	const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

	// The first three references in the folder the upscaling suite reads.
	std::vector<std::wstring> files;
	{
		WIN32_FIND_DATAW fd = {};
		HANDLE h = FindFirstFileW(L"upscale_refs\\*.png", &fd);
		if (h != INVALID_HANDLE_VALUE) {
			do {
				files.push_back(fd.cFileName);
			} while (FindNextFileW(h, &fd));
			FindClose(h);
		}
		std::sort(files.begin(), files.end());
	}
	CComPtr<IWICImagingFactory> factory;
	CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
	std::vector<std::vector<float>> pictures;
	std::vector<double> peaks;   // each one's own peak, linear
	for (size_t i = 0; i < files.size() && pictures.size() < 3 && factory; i++) {
		UpscaleRef ref;
		std::string error;
		if (!LoadReference(factory, (L"upscale_refs\\" + files[i]).c_str(), ref, error) || ref.W < 2 * kTmW || ref.H < 2 * kTmH) {
			continue;
		}
		// Halved, from the middle, and made linear (BT.1886, gamma 2.4).
		std::vector<float> lin((size_t)kTmW * kTmH * 4);
		const int ox = (ref.W - 2 * kTmW) / 2, oy = (ref.H - 2 * kTmH) / 2;
		for (int y = 0; y < kTmH; y++) {
			for (int x = 0; x < kTmW; x++) {
				for (int c = 0; c < 4; c++) {
					double s = 0;
					for (int dy = 0; dy < 2; dy++) {
						for (int dx = 0; dx < 2; dx++) {
							s += std::pow(std::clamp((double)ref.rgba[(((size_t)(oy + 2 * y + dy)) * ref.W + ox + 2 * x + dx) * 4 + c], 0.0, 1.0), 2.4);
						}
					}
					lin[((size_t)y * kTmW + x) * 4 + c] = (float)(s / 4);
				}
			}
		}
		printf("  reference %zu: %S\n", pictures.size() + 1, files[i].c_str());
		peaks.push_back(LinearPeak(lin));
		pictures.push_back(std::move(lin));
	}
	if (pictures.size() < 3) {
		printf("  three 4K references are needed in upscale_refs\n");
		g_failures++;
		return 1;
	}

	FilterTracker gpu;
	std::string error;
	if (!gpu.Init(dev, error)) {
		printf("  the filter's measurement shaders: %s\n", error.c_str());
		g_failures++;
		return 1;
	}

	// The film, picture by picture, with every tracker's peak.
	std::vector<TmRow> rows = {
		{ "the filter, its default (no cut restarts the window)", {} },
		{ "the filter, SdrPeakSceneCuts on", {} },
		{ "libplacebo, its default (the maximum)", {} },
		{ "libplacebo, high quality (99.995%)", {} },
	};
	std::vector<double> truth;      // each picture's own peak, as the filter counts it
	std::vector<int> sceneOf;
	std::vector<double> clipFor[4], keptFor[4];
	PlaceboTracker pl, plHq;
	plHq.percentile = 99.995;
	FilterTracker gpuCuts;
	if (!gpuCuts.Init(dev, error)) {
		printf("  the filter's measurement shaders: %s\n", error.c_str());
		g_failures++;
		return 1;
	}
	printf("  four scenes cut together, peaks of %.0f, %.0f, %.0f and %.0f nits, %d pictures a second;\n"
		"  a display of %.0f nits, BT.2390\n", kTmScenes[0].peak, kTmScenes[1].peak, kTmScenes[2].peak, kTmScenes[3].peak,
		(int)kTmFps, kTmDisplay);

	int frame = 0;
	for (int s = 0; s < (int)std::size(kTmScenes); s++) {
		for (int k = 0; k < kTmScenes[s].frames; k++, frame++) {
			TmFrame f;
			MakeTmFrame(pictures[kTmScenes[s].ref], peaks[kTmScenes[s].ref], kTmScenes[s].peak, frame, f);
			truth.push_back(f.peak9999);
			sceneOf.push_back(s);
			rows[0].used.push_back(gpu.Next(ctx, f, false));
			rows[1].used.push_back(gpuCuts.Next(ctx, f, true));
			rows[2].used.push_back(pl.Next(f));
			rows[3].used.push_back(plHq.Next(f));

			// The curve with each peak, against the curve with the picture's own, on the
			// highlights alone (above 200 nits), since the curve leaves the rest where it
			// is: those cut flat, and how far from where the picture's own peak puts them.
			for (size_t r = 0; r < rows.size(); r++) {
				const double used = std::max(rows[r].used.back(), 1.0);
				double clipped = 0, off = 0;
				size_t n = 0;
				for (size_t i = 0; i < f.nits.size() / 4; i++) {
					const double y = 0.2627 * f.nits[i * 4] + 0.6780 * f.nits[i * 4 + 1] + 0.0593 * f.nits[i * 4 + 2];
					if (y <= 200.0) {
						continue;
					}
					n++;
					clipped += y > used && used > kTmDisplay ? 1 : 0;
					off += std::abs(Bt2390(std::min(y, used), used, kTmDisplay)
						- Bt2390(std::min(y, f.peak9999), f.peak9999, kTmDisplay));
				}
				clipFor[r].push_back(n ? 100.0 * clipped / n : 0.0);
				keptFor[r].push_back(n ? off / n : 0.0);
			}
		}
	}

	printf("\n  %-52s", "scene");
	for (int s = 0; s < (int)std::size(kTmScenes); s++) {
		printf("   %d: %4.0f nits", s + 1, kTmScenes[s].peak);
	}
	printf("\n");
	for (size_t r = 0; r < rows.size(); r++) {
		struct { const char* what; int kind; } lines[] = { { "peak err %", 0 }, { "settle", 1 }, { "clipped %", 2 }, { "off, nits", 3 } };
		printf("  %s\n", rows[r].name);
		for (const auto& line : lines) {
			printf("    %-50s", line.what);
			for (int s = 0; s < (int)std::size(kTmScenes); s++) {
				double sum = 0, sceneTruth = 0;
				int n = 0, first = -1;
				for (size_t i = 0; i < truth.size(); i++) {
					if (sceneOf[i] == s) {
						sceneTruth += truth[i];
						n++;
						if (first < 0) {
							first = (int)i;
						}
					}
				}
				sceneTruth /= n;
				if (line.kind == 1) {
					int settle = -1;
					for (int i = first; i < first + n; i++) {
						bool within = true;
						for (int j = i; j < first + n; j++) {
							within = within && std::abs(rows[r].used[j] - sceneTruth) <= 0.10 * sceneTruth;
						}
						if (within) {
							settle = i - first;
							break;
						}
					}
					if (settle < 0) {
						printf("   %12s", "never");
					} else {
						printf("   %12d", settle);
					}
					continue;
				}
				for (int i = first; i < first + n; i++) {
					sum += line.kind == 0 ? 100.0 * std::abs(rows[r].used[i] - truth[i]) / truth[i]
						: line.kind == 2 ? clipFor[r][i] : keptFor[r][i];
				}
				printf("   %12.1f", sum / n);
			}
			printf("\n");
		}
	}
	printf("\n  settle: pictures after the cut until the peak used stays within 10%% of the scene's own; the\n"
		"  filter's floor of %d nits keeps its peak above a scene that is darker than that, and the curve\n"
		"  does nothing below the display's %.0f nits anyway.\n", SDR_PEAK_FLOOR_DEF, kTmDisplay);
	// The factory goes before COM does: released after CoUninitialize, it crashed the
	// harness on its way out (0xC0000005 in the battery, every number already printed).
	factory.Release();
	if (SUCCEEDED(coInit)) {
		CoUninitialize();
	}
	return g_failures ? 1 : 0;
}

} // namespace temporal
