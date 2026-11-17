// --tdither: the dither before output, the shader that ships against its arithmetic, and
// what it is worth.
//
// The filter dithers in one place, ps_final_pass.hlsl (and the same ending in the
// sharpening passes that take it on): floor(value * Q + threshold) / Q, the threshold
// read from a 32x32 matrix, Q 255 for an 8-bit swap chain and 1023 for a 10-bit one. It
// runs only where the picture holds more than the swap chain can: an 8-bit swap chain
// behind deeper textures, or a 10-bit one behind 16-bit float textures. Behind 10-bit
// integer textures every step writes ten bits and rounds, and nothing dithers.
//
// So the questions are the arithmetic -- the compiled objects the filter embeds
// (_bin\shaders\ps_final_pass*.cso), run with the filter's own matrix and samplers,
// against the same sum on the CPU -- and then, on smooth ramps the swap chain cannot
// hold, what the dither buys over rounding, what ten-bit textures cost against half
// floats on the way to a 10-bit output, and what a pattern that moves from one picture
// to the next would add. Banding is error at the scale of a band, not of a pixel: it is
// read as the error averaged over 16x16 blocks, where a dither's noise has gone and a
// step has not. --study also writes every picture for CAMBI (clean_refs.py --cambi
// --listing dither_out\cambi.txt).

namespace temporal {

namespace {

constexpr int kDitherSize = 32;
constexpr int kDitherW = 1920;
constexpr int kDitherH = 1080;
constexpr int kDitherBlock = 16;

struct DitherRamp {
	const char* name;   // for the report
	const char* id;     // for the file names
	double from;        // the encoded value at the left edge
	int codes;          // how many steps of the output it climbs across the width
	int Q;              // 255 or 1023: the output's depth
};

// The ramps climb a few output steps across the whole width, the worst case there is:
// bands 160 pixels wide. The two PQ ones sit where 10-bit PQ is
// coarsest for the eye (the shadows, 0.25 is about 5 nits) and where half floats are
// coarsest for the dither (0.70, about 620 nits: a half float carries one bit more
// than ten there, and no more).
const DitherRamp kRamps[] = {
	{ "SDR, to 8 bits",            "sdr8",  0.30, 12, 255 },
	{ "PQ shadows, to 10 bits",    "pq10lo", 0.25, 12, 1023 },
	{ "PQ highlights, to 10 bits", "pq10hi", 0.70, 12, 1023 },
};

// A pass in between, the same for both chains: every value moved a little, as a resize
// or a tone curve does, so that what was a code no longer is.
constexpr double kPassGain = 0.9375;
constexpr double kPassOffset = 0.03;

float DitherHalf(float v)
{
	return DirectX::PackedVector::XMConvertHalfToFloat(DirectX::PackedVector::XMConvertFloatToHalf(v));
}

struct DitherScore {
	double bias = 0;     // mean signed error, in output steps
	double rms = 0;      // per pixel
	double blockRms = 0; // over 16x16 block means: the banding
	double blockMax = 0; // the worst block
	int levels = 0;      // distinct output values
};

DitherScore ScoreDither(const std::vector<double>& codes, const std::vector<double>& truth)
{
	DitherScore s;
	double sum = 0, sq = 0;
	std::vector<char> seen(4096, 0);
	for (size_t i = 0; i < codes.size(); i++) {
		const double e = codes[i] - truth[i];
		sum += e;
		sq += e * e;
		const int c = (int)std::lround(codes[i]);
		if (c >= 0 && c < (int)seen.size() && std::abs(codes[i] - c) < 1e-9 && !seen[c]) {
			seen[c] = 1;
			s.levels++;
		}
	}
	s.bias = sum / codes.size();
	s.rms = std::sqrt(sq / codes.size());
	double bsq = 0;
	int blocks = 0;
	for (int by = 0; by + kDitherBlock <= kDitherH; by += kDitherBlock) {
		for (int bx = 0; bx + kDitherBlock <= kDitherW; bx += kDitherBlock) {
			double be = 0;
			for (int y = by; y < by + kDitherBlock; y++) {
				for (int x = bx; x < bx + kDitherBlock; x++) {
					const size_t i = (size_t)y * kDitherW + x;
					be += codes[i] - truth[i];
				}
			}
			be /= kDitherBlock * kDitherBlock;
			bsq += be * be;
			s.blockMax = std::max(s.blockMax, std::abs(be));
			blocks++;
		}
	}
	s.blockRms = std::sqrt(bsq / blocks);
	return s;
}

bool LoadDitherMatrix(std::vector<uint16_t>& halfs, std::vector<float>& values)
{
	FILE* f = nullptr;
	if (_wfopen_s(&f, L"..\\..\\Source\\res\\dither32x32float16.bin", L"rb") || !f) {
		return false;
	}
	halfs.resize(kDitherSize * kDitherSize);
	const size_t n = fread(halfs.data(), sizeof(uint16_t), halfs.size(), f);
	fclose(f);
	if (n != halfs.size()) {
		return false;
	}
	values.resize(halfs.size());
	for (size_t i = 0; i < halfs.size(); i++) {
		values[i] = DirectX::PackedVector::XMConvertHalfToFloat(halfs[i]);
	}
	return true;
}

bool LoadPixelShaderObject(ID3D11Device* dev, const wchar_t* file, CComPtr<ID3D11PixelShader>& ps)
{
	FILE* f = nullptr;
	if (_wfopen_s(&f, file, L"rb") || !f) {
		return false;
	}
	std::vector<BYTE> code;
	BYTE buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		code.insert(code.end(), buf, buf + n);
	}
	fclose(f);
	return !code.empty() && SUCCEEDED(dev->CreatePixelShader(code.data(), code.size(), nullptr, &ps));
}

// The final pass as the filter runs it, on a half-float picture, into the swap chain's
// format; the codes read back.
bool FinalPassOnGpu(ID3D11Device* dev, ID3D11DeviceContext* ctx, CUpscalePasses& passes, ID3D11PixelShader* ps,
                    ID3D11ShaderResourceView* matrix, ID3D11SamplerState* matrixSampler,
                    const std::vector<uint16_t>& picture, int Q, std::vector<int>& codes)
{
	D3D11_TEXTURE2D_DESC d = {};
	d.Width = kDitherW;
	d.Height = kDitherH;
	d.MipLevels = 1;
	d.ArraySize = 1;
	d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_DEFAULT;
	d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	const D3D11_SUBRESOURCE_DATA init = { picture.data(), kDitherW * 4 * sizeof(uint16_t), 0 };
	CComPtr<ID3D11Texture2D> tex;
	CComPtr<ID3D11ShaderResourceView> srv;
	if (FAILED(dev->CreateTexture2D(&d, &init, &tex)) || FAILED(dev->CreateShaderResourceView(tex, nullptr, &srv))) {
		return false;
	}
	const DXGI_FORMAT outFormat = Q == 1023 ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
	Target out;
	if (!MakeTarget(dev, kDitherW, kDitherH, false, out, outFormat)) {
		return false;
	}
	D3D11_BUFFER_DESC bd = {};
	bd.ByteWidth = 16;
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	// The filter's own block: the matrix's scale over the picture it reads.
	const float constants[4] = { (float)kDitherW / kDitherSize, (float)kDitherH / kDitherSize, 0, 0 };
	const D3D11_SUBRESOURCE_DATA cbInit = { constants, 0, 0 };
	CComPtr<ID3D11Buffer> cb;
	if (FAILED(dev->CreateBuffer(&bd, &cbInit, &cb))) {
		return false;
	}

	ctx->PSSetShaderResources(1, 1, &matrix);
	ctx->PSSetSamplers(1, 1, &matrixSampler);
	passes.Apply(ctx, ps, srv, out.rtv, kDitherW, kDitherH, cb);
	ID3D11ShaderResourceView* none = nullptr;
	ctx->PSSetShaderResources(1, 1, &none);

	D3D11_TEXTURE2D_DESC sd = {};
	out.tex->GetDesc(&sd);
	sd.Usage = D3D11_USAGE_STAGING;
	sd.BindFlags = 0;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	sd.MiscFlags = 0;
	CComPtr<ID3D11Texture2D> stage;
	if (FAILED(dev->CreateTexture2D(&sd, nullptr, &stage))) {
		return false;
	}
	ctx->CopyResource(stage, out.tex);
	D3D11_MAPPED_SUBRESOURCE mr = {};
	if (FAILED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &mr))) {
		return false;
	}
	codes.resize((size_t)kDitherW * kDitherH);
	for (int y = 0; y < kDitherH; y++) {
		const BYTE* row = (const BYTE*)mr.pData + (size_t)y * mr.RowPitch;
		for (int x = 0; x < kDitherW; x++) {
			const uint32_t px = ((const uint32_t*)row)[x];
			codes[(size_t)y * kDitherW + x] = Q == 1023 ? (int)(px & 0x3ff) : (int)(px & 0xff);
		}
	}
	ctx->Unmap(stage, 0);
	return true;
}

// The shader's sum, in the float it is done in. The matrix moves by (ox, oy) for a
// pattern that changes from one picture to the next.
int DitherCode(float v, float threshold, int Q)
{
	const float t = v * (float)Q + threshold;
	return std::clamp((int)std::floor(t), 0, Q);
}

void WriteDitherYuv(const std::wstring& path, const std::vector<double>& codes, int bits)
{
	FILE* f = nullptr;
	if (_wfopen_s(&f, path.c_str(), L"wb") || !f) {
		return;
	}
	const int mid = bits == 10 ? 512 : 128;
	const size_t lumaCount = (size_t)kDitherW * kDitherH, chromaCount = lumaCount / 2;
	if (bits == 10) {
		std::vector<uint16_t> plane(lumaCount);
		for (size_t i = 0; i < lumaCount; i++) {
			plane[i] = (uint16_t)std::clamp((int)std::lround(codes[i]), 0, 1023);
		}
		fwrite(plane.data(), 2, plane.size(), f);
		std::vector<uint16_t> chroma(chromaCount, (uint16_t)mid);
		fwrite(chroma.data(), 2, chroma.size(), f);
	} else {
		std::vector<uint8_t> plane(lumaCount);
		for (size_t i = 0; i < lumaCount; i++) {
			plane[i] = (uint8_t)std::clamp((int)std::lround(codes[i]), 0, 255);
		}
		fwrite(plane.data(), 1, plane.size(), f);
		std::vector<uint8_t> chroma(chromaCount, (uint8_t)mid);
		fwrite(chroma.data(), 1, chroma.size(), f);
	}
	fclose(f);
}

} // namespace

int RunDither(ID3D11Device* dev, ID3D11DeviceContext* ctx, bool study)
{
	Head("The dither before output");

	std::vector<uint16_t> matrixHalfs;
	std::vector<float> matrix;
	if (!LoadDitherMatrix(matrixHalfs, matrix)) {
		printf("  the filter's matrix, Source\\res\\dither32x32float16.bin, cannot be read\n");
		g_failures++;
		return 1;
	}
	CUpscalePasses passes;
	std::string error;
	if (!passes.Init(dev, error)) {
		printf("  passes: %s\n", error.c_str());
		g_failures++;
		return 1;
	}
	CComPtr<ID3D11PixelShader> ps8, ps10;
	if (!LoadPixelShaderObject(dev, L"..\\..\\_bin\\shaders\\ps_final_pass.cso", ps8)
			|| !LoadPixelShaderObject(dev, L"..\\..\\_bin\\shaders\\ps_final_pass_10.cso", ps10)) {
		printf("  _bin\\shaders\\ps_final_pass*.cso cannot be loaded: build the filter first\n");
		g_failures++;
		return 1;
	}

	// The matrix as the filter makes it: the same half in all four channels, point
	// sampled and wrapped.
	CComPtr<ID3D11Texture2D> matrixTex;
	CComPtr<ID3D11ShaderResourceView> matrixSrv;
	CComPtr<ID3D11SamplerState> matrixSampler;
	{
		std::vector<uint16_t> rgba(kDitherSize * kDitherSize * 4);
		for (size_t i = 0; i < matrixHalfs.size(); i++) {
			for (int k = 0; k < 4; k++) {
				rgba[i * 4 + k] = matrixHalfs[i];
			}
		}
		D3D11_TEXTURE2D_DESC d = {};
		d.Width = d.Height = kDitherSize;
		d.MipLevels = 1;
		d.ArraySize = 1;
		d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		const D3D11_SUBRESOURCE_DATA init = { rgba.data(), kDitherSize * 4 * sizeof(uint16_t), 0 };
		D3D11_SAMPLER_DESC sd = {};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		sd.AddressU = sd.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
		sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		if (FAILED(dev->CreateTexture2D(&d, &init, &matrixTex))
				|| FAILED(dev->CreateShaderResourceView(matrixTex, nullptr, &matrixSrv))
				|| FAILED(dev->CreateSamplerState(&sd, &matrixSampler))) {
			printf("  the matrix's texture cannot be made\n");
			g_failures++;
			return 1;
		}
	}

	printf("  %dx%d ramps climbing %d output steps across the width; errors in output steps. Banding\n",
		kDitherW, kDitherH, kRamps[0].codes);
	printf("  is the error of %dx%d block means, where a dither's noise averages out and a step does not.\n",
		kDitherBlock, kDitherBlock);
	printf("  The pass in between: v * %.4f + %.2f, the same for both chains.\n", kPassGain, kPassOffset);

	std::wstring outDir;
	FILE* listing = nullptr;
	if (study) {
		outDir = L"dither_out";
		CreateDirectoryW(outDir.c_str(), nullptr);
		_wfopen_s(&listing, (outDir + L"\\cambi.txt").c_str(), L"w");
	}

	for (const DitherRamp& ramp : kRamps) {
		const int Q = ramp.Q;
		const size_t count = (size_t)kDitherW * kDitherH;
		std::vector<uint16_t> picture(count * 4);
		std::vector<float> asHalf(count);
		std::vector<double> truth(count);
		for (int y = 0; y < kDitherH; y++) {
			for (int x = 0; x < kDitherW; x++) {
				const double v = ramp.from + (x + 0.5) / kDitherW * ramp.codes / Q;
				const size_t i = (size_t)y * kDitherW + x;
				const uint16_t h = DirectX::PackedVector::XMConvertFloatToHalf((float)v);
				for (int k = 0; k < 4; k++) {
					picture[i * 4 + k] = h;
				}
				asHalf[i] = DirectX::PackedVector::XMConvertHalfToFloat(h);
				truth[i] = v * Q;
			}
		}

		// 1. The shader that ships, against the CPU's sum.
		std::vector<int> gpu;
		if (!FinalPassOnGpu(dev, ctx, passes, Q == 1023 ? ps10 : ps8, matrixSrv, matrixSampler, picture, Q, gpu)) {
			printf("\n  %s: the final pass could not be run\n", ramp.name);
			g_failures++;
			continue;
		}
		size_t mismatches = 0;
		std::vector<double> dithered(count), rounded(count), halfRounded(count);
		for (int y = 0; y < kDitherH; y++) {
			for (int x = 0; x < kDitherW; x++) {
				const size_t i = (size_t)y * kDitherW + x;
				const float threshold = matrix[(y % kDitherSize) * kDitherSize + (x % kDitherSize)];
				const int c = DitherCode(asHalf[i], threshold, Q);
				mismatches += c != gpu[i];
				dithered[i] = gpu[i];
				rounded[i] = std::clamp(std::nearbyint(truth[i]), 0.0, (double)Q);
				halfRounded[i] = std::clamp(std::nearbyint((double)asHalf[i] * Q), 0.0, (double)Q);
			}
		}
		printf("\n  %s, from %.2f\n", ramp.name, ramp.from);
		printf("    the final pass that ships (%s) against the same sum on the CPU: %zu of %zu pixels differ\n",
			Q == 1023 ? "ps_final_pass_10.cso" : "ps_final_pass.cso", mismatches, count);
		Check(mismatches * 10000 <= count, "the final pass is the sum it claims");

		// 2. What it is worth.
		struct Row { const char* name; const char* id; std::vector<double> codes; std::vector<double> truth; };
		std::vector<Row> rows;
		rows.push_back({ "rounded, the ramp itself", "round", rounded, truth });
		rows.push_back({ "half floats, rounded", "halfround", halfRounded, truth });
		rows.push_back({ "half floats, the dither", "dither", dithered, truth });

		// The two chains to a 10-bit output, through one pass that moves every value:
		// ten-bit textures round at every write and nothing dithers; half floats round
		// to a half at every write and the final pass dithers.
		if (Q == 1023) {
			std::vector<double> chainTruth(count), chainInt(count), chainHalf(count);
			for (int y = 0; y < kDitherH; y++) {
				for (int x = 0; x < kDitherW; x++) {
					const size_t i = (size_t)y * kDitherW + x;
					chainTruth[i] = (truth[i] / Q * kPassGain + kPassOffset) * Q;
					const double c1 = std::nearbyint((double)asHalf[i] * Q);
					chainInt[i] = std::clamp(std::nearbyint((c1 / Q * kPassGain + kPassOffset) * Q), 0.0, (double)Q);
					const float h2 = DitherHalf((float)((double)asHalf[i] * kPassGain + kPassOffset));
					const float threshold = matrix[(y % kDitherSize) * kDitherSize + (x % kDitherSize)];
					chainHalf[i] = DitherCode(h2, threshold, Q);
				}
			}
			rows.push_back({ "a pass in 10-bit textures, no dither", "chain10", chainInt, chainTruth });
			rows.push_back({ "a pass in half floats, the dither", "chainhalf", chainHalf, chainTruth });
		}

		printf("    %-38s %8s %7s %9s %9s %7s\n", "", "bias", "rms", "banding", "worst", "levels");
		std::vector<DitherScore> scores;
		for (const Row& r : rows) {
			const DitherScore s = ScoreDither(r.codes, r.truth);
			scores.push_back(s);
			printf("    %-38s %+8.4f %7.4f %9.4f %9.4f %7d\n", r.name, s.bias, s.rms, s.blockRms, s.blockMax, s.levels);
			if (listing) {
				const std::wstring file = outDir + L"\\" + std::wstring(ramp.id, ramp.id + strlen(ramp.id)) + L"_"
					+ std::wstring(r.id, r.id + strlen(r.id)) + L".yuv";
				WriteDitherYuv(file, r.codes, Q == 1023 ? 10 : 8);
				fwprintf(listing, L"dither.%S\t%S\t%s\t%d\t%d\t%d\n", ramp.id, r.name, file.c_str(), kDitherW, kDitherH,
					Q == 1023 ? 10 : 8);
			}
		}
		Check(std::abs(scores[2].bias) < 0.01, "the dither adds no offset");
		Check(scores[2].blockRms < scores[1].blockRms, "the dither leaves less banding than rounding");

		// 3. A pattern that moves: what eight pictures in a row average to, pixel by
		// pixel, which is what the eye sees of a still ramp at 24 to 60 pictures a
		// second. The pattern that ships is the same in all eight.
		std::vector<double> still(count), moving(count, 0.0);
		for (int frame = 0; frame < 8; frame++) {
			const int ox = (frame * 11) % kDitherSize, oy = (frame * 19) % kDitherSize;
			for (int y = 0; y < kDitherH; y++) {
				for (int x = 0; x < kDitherW; x++) {
					const size_t i = (size_t)y * kDitherW + x;
					const float threshold = matrix[((y + oy) % kDitherSize) * kDitherSize + ((x + ox) % kDitherSize)];
					moving[i] += DitherCode(asHalf[i], threshold, Q) / 8.0;
				}
			}
		}
		for (size_t i = 0; i < count; i++) {
			still[i] = dithered[i];
		}
		const DitherScore sStill = ScoreDither(still, truth), sMoving = ScoreDither(moving, truth);
		printf("    eight pictures averaged, pixel by pixel: rms %.4f with the pattern that ships, %.4f with one\n"
			"    that moves each picture\n", sStill.rms, sMoving.rms);
	}
	if (listing) {
		fclose(listing);
		printf("\n  pictures and dither_out\\cambi.txt written: python clean_refs.py --cambi --listing dither_out\\cambi.txt\n");
	}
	return g_failures ? 1 : 0;
}

} // namespace temporal
