// --teffect: what DLSSNR.ControlMask does to the network's effect, on a real picture.
//
// Included by harness.cpp after detect_suite.inl. With no mask and with uniform mask
// values it measures how far the output moves from the input -- the effect itself --,
// how far it lands from plain DLSS, and how much it changes from frame to frame on a
// still picture with light per-frame grain. It also writes centre crops of the input,
// of some outputs and of their amplified differences (effect_*.png).
//
// Written after the first user test of the stabilizer in MPC-BE (2026-09-14): any
// Stabilizer value made the DLSS 5 effect disappear. The synthetic suites measured
// flicker on value-noise textures the network barely changes, so they could not have
// seen it. --timage <path> picks the picture; a Windows wallpaper by default.

#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")

namespace temporal {

// A picture from disk, centre-cropped to the target aspect ratio and scaled, as RGBA
// float with the file's own encoding (the renderer hands the network gamma-encoded
// RGB as well).
static bool LoadPicture(IWICImagingFactory* factory, const wchar_t* path, int W, int H,
                        std::vector<float>& rgba, std::string& error)
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

	UINT cw = sw, ch = sh;
	if ((double)sw * H > (double)sh * W) {
		cw = (UINT)((double)sh * W / H);
	} else {
		ch = (UINT)((double)sw * H / W);
	}
	const WICRect crop = { (INT)((sw - cw) / 2), (INT)((sh - ch) / 2), (INT)cw, (INT)ch };

	CComPtr<IWICBitmapClipper> clipper;
	CComPtr<IWICBitmapScaler> scaler;
	CComPtr<IWICFormatConverter> converter;
	std::vector<BYTE> bytes(4 * (size_t)W * H);
	hr = factory->CreateBitmapClipper(&clipper);
	if (SUCCEEDED(hr)) {
		hr = clipper->Initialize(frame, &crop);
	}
	if (SUCCEEDED(hr)) {
		hr = factory->CreateBitmapScaler(&scaler);
	}
	if (SUCCEEDED(hr)) {
		hr = scaler->Initialize(clipper, (UINT)W, (UINT)H, WICBitmapInterpolationModeFant);
	}
	if (SUCCEEDED(hr)) {
		hr = factory->CreateFormatConverter(&converter);
	}
	if (SUCCEEDED(hr)) {
		hr = converter->Initialize(scaler, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
	}
	if (SUCCEEDED(hr)) {
		hr = converter->CopyPixels(nullptr, 4 * (UINT)W, (UINT)bytes.size(), bytes.data());
	}
	if (FAILED(hr)) {
		error = std::format("cannot crop and scale the picture (0x{:08X})", (unsigned)hr);
		return false;
	}

	rgba.resize(bytes.size());
	for (size_t i = 0; i < bytes.size(); i++) {
		rgba[i] = (i % 4 == 3) ? 1.0f : bytes[i] / 255.0f;
	}
	return true;
}

// A rectangle of an RGBA float picture, times gain, as an 8-bit PNG.
static bool SavePng(IWICImagingFactory* factory, const wchar_t* path, const std::vector<float>& rgba, int W,
                    const Rect& r, float gain)
{
	const int cw = r.x1 - r.x0, ch = r.y1 - r.y0;
	std::vector<BYTE> bgra(4 * (size_t)cw * ch);
	for (int y = 0; y < ch; y++) {
		for (int x = 0; x < cw; x++) {
			const float* p = &rgba[4 * ((size_t)(r.y0 + y) * W + r.x0 + x)];
			BYTE* o = &bgra[4 * ((size_t)y * cw + x)];
			o[0] = (BYTE)std::lround(std::clamp(p[2] * gain, 0.0f, 1.0f) * 255);
			o[1] = (BYTE)std::lround(std::clamp(p[1] * gain, 0.0f, 1.0f) * 255);
			o[2] = (BYTE)std::lround(std::clamp(p[0] * gain, 0.0f, 1.0f) * 255);
			o[3] = 255;
		}
	}

	CComPtr<IWICStream> stream;
	CComPtr<IWICBitmapEncoder> encoder;
	CComPtr<IWICBitmapFrameEncode> frame;
	CComPtr<IPropertyBag2> props;
	WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
	HRESULT hr = factory->CreateStream(&stream);
	if (SUCCEEDED(hr)) {
		hr = stream->InitializeFromFilename(path, GENERIC_WRITE);
	}
	if (SUCCEEDED(hr)) {
		hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
	}
	if (SUCCEEDED(hr)) {
		hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
	}
	if (SUCCEEDED(hr)) {
		hr = encoder->CreateNewFrame(&frame, &props);
	}
	if (SUCCEEDED(hr)) {
		hr = frame->Initialize(props);
	}
	if (SUCCEEDED(hr)) {
		hr = frame->SetSize((UINT)cw, (UINT)ch);
	}
	if (SUCCEEDED(hr)) {
		hr = frame->SetPixelFormat(&format);
	}
	if (SUCCEEDED(hr) && format != GUID_WICPixelFormat32bppBGRA) {
		hr = E_FAIL;
	}
	if (SUCCEEDED(hr)) {
		hr = frame->WritePixels((UINT)ch, 4 * (UINT)cw, (UINT)bgra.size(), bgra.data());
	}
	if (SUCCEEDED(hr)) {
		hr = frame->Commit();
	}
	if (SUCCEEDED(hr)) {
		hr = encoder->Commit();
	}
	return SUCCEEDED(hr);
}

static bool ReadRgbaHalf(ID3D11DeviceContext* ctx, ID3D11Texture2D* stage, int W, int H, std::vector<float>& out)
{
	D3D11_MAPPED_SUBRESOURCE mr = {};
	if (FAILED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &mr))) {
		return false;
	}
	out.resize(4 * (size_t)W * H);
	for (int y = 0; y < H; y++) {
		DirectX::PackedVector::XMConvertHalfToFloatStream(&out[4 * (size_t)y * W], sizeof(float),
			(const HALF*)((const BYTE*)mr.pData + (size_t)mr.RowPitch * y), sizeof(HALF), 4 * (size_t)W);
	}
	ctx->Unmap(stage, 0);
	// Direct3D does not destroy an object when it is released, only at the next Flush.
	// The suites make their textures afresh for every picture and read each one back
	// through here, and on an RTX 4060 nothing else was flushing them: the sharpening
	// suite reached 24 GB of memory and 7 GB of the card's, and died of bad_alloc in its
	// third method. With this it peaks at 4.3 GB and 370 MB.
	ctx->Flush();
	return true;
}

struct EffectConfig {
	const char* name;
	bool noHistory;
	bool autoMask;
	int  mask;          // -1: no ControlMask bound; 0..255: that value everywhere
	bool lateBind;      // bound after CreateFeature, the order the renderer used
	const wchar_t* png; // centre crop and amplified difference saved under this name, or nullptr
};

static int RunEffect(ID3D11Device* dev, ID3D11DeviceContext* ctx, CDlssNR& dlss, const wchar_t* imagePath, bool strong)
{
	const int W = 1920, H = 1080;
	const int frames = 40, measureFrom = 24;
	const float grain = 1.2f / 255;
	const float diffGain = 8.0f;

	static const EffectConfig cfgs[] = {
		{ "plain",                    false, true,  -1,  false, L"plain"   },
		{ "plain, auto mask off",     false, false, -1,  false, nullptr    },
		{ "no history",               true,  true,  -1,  false, nullptr    },
		{ "mask 1.00",                false, true,  255, false, L"mask100" },
		{ "mask 0.75",                false, true,  191, false, nullptr    },
		{ "mask 0.50",                false, true,  128, false, nullptr    },
		{ "mask 0.25",                false, true,  64,  false, L"mask025" },
		{ "mask 0.00",                false, true,  0,   false, L"mask000" },
		{ "mask 1.00, auto mask off", false, false, 255, false, nullptr    },
		{ "mask 0.00, auto mask off", false, false, 0,   false, nullptr    },
		{ "mask 1.00, no history",    true,  true,  255, false, nullptr    },
		{ "mask 0.25, late bind",     false, true,  64,  true,  nullptr    },
	};

	Head("Temporal suite: ControlMask and the network's effect");
	const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	int rc = 0;
	{
		CComPtr<IWICImagingFactory> factory;
		Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))), "WIC factory");
		std::vector<float> picture;
		std::string error;
		const bool pictureOk = factory && LoadPicture(factory, imagePath, W, H, picture, error);
		Check(pictureOk, "picture decoded, cropped and scaled to 1920x1080");
		if (!pictureOk) {
			printf("  %S: %s\n", imagePath, error.c_str());
			rc = 1;
		}

		Tex2D_t texIn, texOut, texMask;
		CComPtr<ID3D11Texture2D> stage;
		if (!rc) {
			Check(MakeSharedPair(dev, texIn, texOut, W, H), "colour pair 1920x1080 RGBA16F");
			Check(SUCCEEDED(texMask.CheckCreate(dev, DXGI_FORMAT_R8_UNORM, W, H, Tex2D_DefaultShaderRTargetUAVShared)), "ControlMask R8");
			D3D11_TEXTURE2D_DESC sd = texOut.desc;
			sd.Usage = D3D11_USAGE_STAGING;
			sd.BindFlags = 0;
			sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			sd.MiscFlags = 0;
			Check(texOut.pTexture && SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &stage)), "colour readback staging");
			if (!texIn.pTexture || !texOut.pTexture || !texMask.pTexture || !stage) {
				rc = 1;
			}
		}

		if (!rc) {
			CDlssNR::Params params;
			params.iStyle = 0;
			params.iPreset = 0;
			params.fIntensity = 1.50f;
			params.fLocalTone = strong ? 1.0f : 0.30f;
			params.fLocalStructure = strong ? 1.0f : 0.50f;
			params.fSkinStructure = 0.90f;

			g_report = fopen("temporal_results_effect.txt", "w");
			Out("\nControlMask and the network's effect -- %S cropped to %dx%d, %d frames per run, last %d measured,\n",
			    imagePath, W, H, frames, frames - measureFrom);
			Out("%s strengths. The same picture every frame, with fresh %.1f/255 grain.\n",
			    strong ? "strong (tone 1.00, structure 1.00)" : "default (tone 0.30, structure 0.50)", grain * 255);
			Out("effect = mean |out - in|, on luma and on RGB averaged over channels: the network's own change to the\n");
			Out("picture. of plain = the RGB effect as a share of plain DLSS. vs plain = mean RGB |out - plain| on the last\n");
			Out("frame. flicker = mean |Y(t) - Y(t-1)| of the output.\n\n");
			Out("  %-26s %9s %9s %8s %9s %9s %6s\n", "config", "effect Y", "eff RGB", "of plain", "vs plain", "flicker Y", "failed");

			std::vector<float> rgba(4 * (size_t)W * H), out, plainLast, luma, prevLuma, diff;
			std::vector<HALF> half(rgba.size());
			std::vector<uint8_t> maskBytes((size_t)W * H);
			const Rect crop = { W / 4, H / 4, W / 4 + W / 2, H / 4 + H / 2 };
			double plainEffect = 0;
			double inputFlicker = 0;
			bool inputSaved = false;

			for (size_t ci = 0; ci < std::size(cfgs); ci++) {
				const EffectConfig& c = cfgs[ci];
				dlss.ReleaseFeature();

				CDlssNR::Guides g;
				if (c.mask >= 0) {
					std::fill(maskBytes.begin(), maskBytes.end(), (uint8_t)c.mask);
					ctx->UpdateSubresource(texMask.pTexture, 0, nullptr, maskBytes.data(), W, 0);
					if (!c.lateBind) {
						g.pControlMask = texMask.pTexture;
					}
				}
				bool ok = dlss.SetGuides(g) && dlss.CreateFeature(texIn.pTexture, texOut.pTexture, W, H, params);
				if (ok && c.lateBind) {
					g.pControlMask = texMask.pTexture;
					ok = dlss.SetGuides(g);
				}
				if (!ok) {
					Out("  %-26s could not set up: %S\n", c.name, dlss.GetStatusLine().c_str());
					g_failures++;
					continue;
				}
				dlss.RequestReset();
				params.bNoHistory = c.noHistory;
				params.bUseAutoMask = c.autoMask;

				double effY = 0, effRGB = 0, flicker = 0, inFlicker = 0;
				int count = 0, flickerCount = 0, failed = 0;
				bool haveLast = false;
				prevLuma.clear();
				std::vector<float> prevIn;

				for (int t = 0; t < frames; t++) {
					Rng r(0x5EEDu + (uint32_t)t * 7919u);
					for (size_t i = 0; i < picture.size(); i += 4) {
						const float n = r.Gauss() * grain;
						for (int k = 0; k < 3; k++) {
							rgba[i + k] = std::round(std::clamp(picture[i + k] + n, 0.0f, 1.0f) * 255) / 255;
						}
						rgba[i + 3] = 1.0f;
					}
					DirectX::PackedVector::XMConvertFloatToHalfStream(half.data(), sizeof(HALF), rgba.data(), sizeof(float), rgba.size());
					ctx->UpdateSubresource(texIn.pTexture, 0, nullptr, half.data(), 4 * sizeof(HALF) * W, 0);

					if (!dlss.Evaluate(params)) {
						if (++failed <= 2) {
							Out("  %-26s frame %d: Evaluate failed -- %S\n", c.name, t, dlss.GetStatusLine().c_str());
						}
						continue;
					}
					if (t < measureFrom) {
						continue;
					}
					ctx->CopyResource(stage, texOut.pTexture);
					if (!ReadRgbaHalf(ctx, stage, W, H, out)) {
						failed++;
						continue;
					}

					luma.resize((size_t)W * H);
					std::vector<float> inLuma(luma.size());
					double sy = 0, srgb = 0;
					for (size_t p = 0, i = 0; p < luma.size(); p++, i += 4) {
						const float yo = 0.2126f * out[i] + 0.7152f * out[i + 1] + 0.0722f * out[i + 2];
						const float yi = 0.2126f * rgba[i] + 0.7152f * rgba[i + 1] + 0.0722f * rgba[i + 2];
						luma[p] = yo;
						inLuma[p] = yi;
						sy += std::abs(yo - yi);
						srgb += (std::abs(out[i] - rgba[i]) + std::abs(out[i + 1] - rgba[i + 1]) + std::abs(out[i + 2] - rgba[i + 2])) / 3;
					}
					effY += sy / luma.size();
					effRGB += srgb / luma.size();
					if (!prevLuma.empty()) {
						double sf = 0, si = 0;
						for (size_t p = 0; p < luma.size(); p++) {
							sf += std::abs(luma[p] - prevLuma[p]);
							si += std::abs(inLuma[p] - prevIn[p]);
						}
						flicker += sf / luma.size();
						inFlicker += si / luma.size();
						flickerCount++;
					}
					prevLuma.swap(luma);
					prevIn.swap(inLuma);
					count++;
					haveLast = true;
				}

				if (!count || !haveLast) {
					Out("  %-26s no usable frames (%d failed)\n", c.name, failed);
					g_failures++;
					continue;
				}
				const double e = effRGB / count;
				if (ci == 0) {
					plainLast = out;
					plainEffect = e;
					inputFlicker = flickerCount ? inFlicker / flickerCount : 0;
				}
				double vsPlain = 0;
				if (!plainLast.empty()) {
					for (size_t i = 0; i < out.size(); i += 4) {
						vsPlain += (std::abs(out[i] - plainLast[i]) + std::abs(out[i + 1] - plainLast[i + 1]) + std::abs(out[i + 2] - plainLast[i + 2])) / 3;
					}
					vsPlain /= out.size() / 4;
				}
				Out("  %-26s %9.5f %9.5f %7.0f%% %9.5f %9.5f %6d\n",
				    c.name, effY / count, e, plainEffect > 0 ? 100.0 * e / plainEffect : 0.0,
				    vsPlain, flickerCount ? flicker / flickerCount : 0.0, failed);

				if (!inputSaved) {
					inputSaved = SavePng(factory, L"effect_input.png", rgba, W, crop, 1.0f);
				}
				if (c.png) {
					diff.resize(out.size());
					for (size_t i = 0; i < out.size(); i++) {
						diff[i] = std::abs(out[i] - rgba[i]);
					}
					const bool saved = SavePng(factory, std::format(L"effect_{}.png", c.png).c_str(), out, W, crop, 1.0f)
						&& SavePng(factory, std::format(L"effect_{}_diff.png", c.png).c_str(), diff, W, crop, diffGain);
					if (!saved) {
						Out("  (could not write the %S pictures)\n", c.png);
					}
				}
			}
			Out("  %-26s %9s %9s %8s %9s %9.5f\n", "(input)", "-", "-", "-", "-", inputFlicker);
			Out("\nPictures: the centre %dx%d of the input and of plain, mask 1.00, 0.25 and 0.00 outputs, with their\n",
			    crop.x1 - crop.x0, crop.y1 - crop.y0);
			Out("differences from the input amplified %.0fx (effect_*_diff.png).\n", diffGain);

			dlss.SetGuides(CDlssNR::Guides{});
			params.bUseAutoMask = true;
			fclose(g_report);
			g_report = nullptr;
			printf("\n  written to temporal_results_effect.txt\n");
		}
	}
	if (SUCCEEDED(coInit)) {
		CoUninitialize();
	}
	return (rc || g_failures) ? 1 : 0;
}

} // namespace temporal
