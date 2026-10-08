// --tdownport: the downscaling shaders that ship, against the kernels they claim.
//
// Each of them is one filter() function in Shaders/resize/convolution_filters.hlsl,
// compiled by compile_shaders.cmd into ps_downscaler_<name>_<axis>.cso, and the
// filter embeds those exact objects as resources. A wrong coefficient in one of
// those polynomials compiles perfectly, looks like a picture, and is invisible to
// any eye and to any comparison against another resizer. The only thing that catches
// it is the arithmetic, which is what this does.
//
// The shipped object is loaded rather than the source rebuilt here, run through the
// renderer's own constant block and its point sampler, and compared against the same
// convolution worked out on the CPU in double precision -- the same clamping at the
// edges that the sampler does, and the same rounding to half between the two passes
// that the intermediate target does, so what is left is the kernel and nothing else.

namespace temporal {

namespace {

struct PortKernel {
	const char* name;
	double support;
	double (*k)(double);
	// Where the kernel changes branch. A tap landing on one of these is a tie: the
	// shader decides it in float, from an interpolated coordinate, and this decides
	// it in double from an arithmetic one. Box is the plain case -- its interval is
	// half-open at 0.5, so the tap counts whole or not at all, and a last-bit
	// disagreement moves the answer by a whole texel. Nothing about the kernel is in
	// question there, so those pixels are set aside and counted rather than judged.
	double edges[4];
};

constexpr double kPi = 3.14159265358979323846;

double PortSinc(double x)
{
	if (x == 0.0) {
		return 1.0;
	}
	x *= kPi;
	return sin(x) / x;
}

double PortBox(double x)      { return (x >= -0.5 && x < 0.5) ? 1.0 : 0.0; }
double PortBilinear(double x) { x = fabs(x); return x < 1.0 ? 1.0 - x : 0.0; }

double PortHamming(double x)
{
	x = fabs(x);
	if (x == 0.0) {
		return 1.0;
	}
	if (x >= 1.0) {
		return 0.0;
	}
	return PortSinc(x) * (0.54 + 0.46 * cos(kPi * x));
}

double PortBicubic(double x, double a)
{
	x = fabs(x);
	if (x < 1.0) {
		return ((a + 2.0) * x - (a + 3.0)) * x * x + 1.0;
	}
	if (x < 2.0) {
		return (((x - 5.0) * x + 8.0) * x - 4.0) * a;
	}
	return 0.0;
}

double PortBicubic05(double x) { return PortBicubic(x, -0.5); }
double PortBicubic15(double x) { return PortBicubic(x, -1.5); }

double PortLanczos(double x)
{
	return (x >= -3.0 && x < 3.0) ? PortSinc(x) * PortSinc(x / 3.0) : 0.0;
}

double PortSpline36(double x)
{
	x = fabs(x);
	if (x < 1.0) {
		return ((13.0 / 11.0 * x - 453.0 / 209.0) * x - 3.0 / 209.0) * x + 1.0;
	}
	if (x < 2.0) {
		x -= 1.0;
		return ((-6.0 / 11.0 * x + 270.0 / 209.0) * x - 156.0 / 209.0) * x;
	}
	if (x < 3.0) {
		x -= 2.0;
		return ((1.0 / 11.0 * x - 45.0 / 209.0) * x + 26.0 / 209.0) * x;
	}
	return 0.0;
}

const PortKernel kPortKernels[] = {
	{ "Box",           0.5, PortBox,       { 0.5 } },
	{ "Bilinear",      1.0, PortBilinear,  { 1.0 } },
	{ "Hamming",       1.0, PortHamming,   { 0.0, 1.0 } },
	{ "Bicubic",       2.0, PortBicubic05, { 1.0, 2.0 } },
	{ "Bicubic sharp", 2.0, PortBicubic15, { 1.0, 2.0 } },
	{ "Lanczos",       3.0, PortLanczos,   { 0.0, 3.0 } },
	{ "Spline36",      3.0, PortSpline36,  { 1.0, 2.0, 3.0 } },
};

const PortKernel* KernelNamed(const char* name)
{
	for (const auto& k : kPortKernels) {
		if (!strcmp(k.name, name)) {
			return &k;
		}
	}
	return nullptr;
}

float ToHalfAndBack(float v)
{
	return DirectX::PackedVector::XMConvertHalfToFloat(DirectX::PackedVector::XMConvertFloatToHalf(v));
}

// ps_convolution.hlsl, one axis, in double. low and high are worked out in float
// because the shader works them out in float, and a boundary that falls either side
// of an integer there must fall the same side here or a tap appears out of nowhere.
void PortConvolve(const std::vector<float>& src, int sw, int sh,
                  std::vector<float>& dst, int dw, int dh, const PortKernel& kern, int axis,
                  std::vector<bool>* pOnEdge = nullptr, std::vector<float>* pCond = nullptr)
{
	const int outW = axis == 0 ? dw : dw;
	const int outH = axis == 0 ? sh : dh;
	dst.assign((size_t)outW * outH * 4, 0.0f);
	if (pOnEdge) {
		pOnEdge->assign((size_t)outW * outH, false);
	}
	if (pCond) {
		pCond->assign((size_t)outW * outH, 1.0f);
	}

	const int inAlong = axis == 0 ? sw : sh;
	const int outAlong = axis == 0 ? dw : dh;
	const float scale = (float)inAlong / outAlong;
	const float support = (float)kern.support * scale;
	const float ss = 1.0f / scale;

	for (int y = 0; y < outH; y++) {
		for (int x = 0; x < outW; x++) {
			const int at = axis == 0 ? x : y;
			const float tex = (at + 0.5f) / outAlong;
			const float pos = tex * inAlong + 0.5f;
			const int low = (int)floorf(pos - support);
			const int high = (int)ceilf(pos + support);

			double ww = 0.0, absw = 0.0, acc[4] = { 0, 0, 0, 0 };
			bool onEdge = false;
			for (int n = low; n < high; n++) {
				const float arg = (n - pos + 0.5f) * ss;
				// near is a macro in windef.h, of all things.
				const double tieWindow = std::max(1e-5, (double)pos * 1e-6);
				for (const double e : kern.edges) {
					if (e != 0.0 && fabs(fabs((double)arg) - e) < tieWindow) {
						onEdge = true;
					}
				}
				const double w = kern.k(arg);
				ww += w;
				absw += fabs(w);
				const int c = std::clamp(n, 0, inAlong - 1);
				const size_t at4 = axis == 0
					? ((size_t)y * sw + c) * 4
					: ((size_t)c * sw + x) * 4;
				for (int i = 0; i < 4; i++) {
					acc[i] += w * src[at4 + i];
				}
			}
			const size_t out4 = ((size_t)y * outW + x) * 4;
			for (int i = 0; i < 4; i++) {
				dst[out4 + i] = ww != 0.0 ? (float)(acc[i] / ww) : 0.0f;
			}
			if (pOnEdge && onEdge) {
				(*pOnEdge)[(size_t)y * outW + x] = true;
			}
			if (pCond) {
				(*pCond)[(size_t)y * outW + x] = ww != 0.0 ? (float)(absw / fabs(ww)) : 1.0f;
			}
		}
	}
	// What the intermediate and the destination are: half floats.
	for (float& v : dst) {
		v = ToHalfAndBack(v);
	}
}

// A picture whose tails matter: hard edges at every phase, a ramp, and noise that
// does not repeat. A flat picture would pass with any kernel at all.
void PortPicture(std::vector<float>& rgba, int w, int h)
{
	rgba.assign((size_t)w * h * 4, 0.0f);
	uint32_t seed = 0x9E3779B9u;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			seed = seed * 1664525u + 1013904223u;
			const float noise = ((seed >> 8) & 0xFFFF) / 65535.0f;
			const float ramp = (float)x / w;
			const float edge = ((x / 7 + y / 5) & 1) ? 0.92f : 0.06f;
			const float v[4] = {
				0.55f * edge + 0.45f * noise,
				0.70f * ramp + 0.30f * edge,
				0.50f * noise + 0.50f * (1.0f - ramp),
				1.0f,
			};
			for (int i = 0; i < 4; i++) {
				rgba[((size_t)y * w + x) * 4 + i] = ToHalfAndBack(v[i]);
			}
		}
	}
}

// Half's step around 1.0, which is the scale the picture is on: ten bits of
// mantissa. Measuring each value against its own magnitude instead would ask a
// result that landed near zero to be right to a thousandth of nothing, and say
// nothing at all about the kernel.
constexpr double kHalfStep = 1.0 / 1024.0;
// What a pass is allowed to be out by, per unit of cancellation in its own sum.
// Lanczos and the two bicubics give their taps both signs, so the weights cancel
// and what is left is known to fewer bits than the terms that made it -- the
// textbook bound on a weighted sum scales with sum|w| over |sum w|, and so does
// this. Three steps of half at no cancellation at all, and the same three for every
// factor the sum cancels by. A coefficient typed wrong moves a weight by percent
// and the picture with it, which stays orders above this at any conditioning.
constexpr double kAllowed = 3.0 * kHalfStep;
// And what the check is actually for. A coefficient typed wrong moves its weight by
// percent, and the picture with it: tens of steps of half, not a handful. Anything
// between the two bars is noted and not failed -- it is not rounding as this models
// it, and it is nowhere near a wrong number either, and saying so is more use than
// widening the first bar until it stops showing.
constexpr double kWrong = 30.0 * kHalfStep;

} // namespace

static int RunDownscalePort(ID3D11Device* dev, ID3D11DeviceContext* ctx)
{
	printf("\n=== The downscaling shaders against their kernels (--tdownport) ===\n");
	printf("The compiled objects the filter embeds, run on the GPU through the\n"
	       "renderer's own constants, against the same convolution in double on the CPU.\n\n");

	CUpscalePasses passes;
	std::string error;
	if (!passes.Init(dev, error)) {
		printf("  the resize passes could not be set up: %s\n", error.c_str());
		return 1;
	}
	if (passes.ShippedCount() == 0) {
		printf("  no compiled downscaler found in _bin\\shaders: build the shaders first\n");
		return 1;
	}

	struct Case { int sw, sh, dw, dh; const char* what; };
	static const Case cases[] = {
		{ 320, 180, 211, 119, "1.52x, a ratio that is not whole" },
		{ 320, 180, 161,  91, "1.99x, just off the ratio where every tap is a tie" },
		{ 320, 180, 288, 162, "1.11x, barely reduced" },
	};

	int failures = 0;
	for (const auto& c : cases) {
		printf("  %dx%d -> %dx%d, %s\n", c.sw, c.sh, c.dw, c.dh, c.what);

		std::vector<float> picture;
		PortPicture(picture, c.sw, c.sh);

		Target texIn, texOut;
		CComPtr<ID3D11Texture2D> stage;
		if (!MakeTarget(dev, c.sw, c.sh, false, texIn, DXGI_FORMAT_R16G16B16A16_FLOAT)
				|| !MakeTarget(dev, c.dw, c.dh, false, texOut, DXGI_FORMAT_R16G16B16A16_FLOAT)) {
			printf("    textures failed\n");
			return 1;
		}
		std::vector<HALF> half(picture.size());
		DirectX::PackedVector::XMConvertFloatToHalfStream(half.data(), sizeof(HALF), picture.data(), sizeof(float), picture.size());
		ctx->UpdateSubresource(texIn.tex, 0, nullptr, half.data(), 4 * sizeof(HALF) * c.sw, 0);

		D3D11_TEXTURE2D_DESC sd = {};
		texOut.tex->GetDesc(&sd);
		sd.Usage = D3D11_USAGE_STAGING;
		sd.BindFlags = 0;
		sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		sd.MiscFlags = 0;
		if (FAILED(dev->CreateTexture2D(&sd, nullptr, &stage))) {
			printf("    staging failed\n");
			return 1;
		}

		for (int i = 0; i < passes.ShippedCount(); i++) {
			const CUpscalePasses::Method& m = passes.Shipped(i);
			const PortKernel* kern = KernelNamed(m.name);
			if (!kern) {
				printf("    %-14s no kernel of that name here\n", m.name);
				failures++;
				continue;
			}

			if (!passes.Resize(dev, ctx, m, texIn.srv, c.sw, c.sh, texOut.rtv, c.dw, c.dh)) {
				printf("    %-14s the resize failed\n", m.name);
				failures++;
				continue;
			}
			ctx->CopyResource(stage, texOut.tex);
			std::vector<float> gpu;
			if (!ReadRgbaHalf(ctx, stage, c.dw, c.dh, gpu)) {
				printf("    %-14s could not be read back\n", m.name);
				failures++;
				continue;
			}

			// The same two passes, in the same order, with the same half in between.
			std::vector<float> inter, cpu;
			std::vector<bool> edgeX, edgeY;
			std::vector<float> condX, condY;
			PortConvolve(picture, c.sw, c.sh, inter, c.dw, c.dh, *kern, 0, &edgeX, &condX);
			PortConvolve(inter, c.dw, c.sh, cpu, c.dw, c.dh, *kern, 1, &edgeY, &condY);

			double worst = 0.0;
			size_t apart = 0, wrong = 0, ties = 0, judged = 0;
			for (size_t pix = 0; pix < (size_t)c.dw * c.dh; pix++) {
				// A pixel is a tie if either pass put a tap on a branch point: the X
				// pass fed it, the Y pass finished it.
				const size_t col = pix % c.dw;
				const bool tie = edgeY[pix] || edgeX[col];
				if (tie) {
					ties++;
					continue;
				}
				judged++;
				// How much the two sums that made this pixel cancelled.
				const double cond = std::max((double)condX[col], (double)condY[pix]);
				for (int i = 0; i < 4; i++) {
					const double d = fabs((double)gpu[pix * 4 + i] - (double)cpu[pix * 4 + i]);
					worst = std::max(worst, d / cond);
					if (d > kWrong * cond) {
						wrong++;
					} else if (d > kAllowed * cond) {
						apart++;
					}
				}
			}
			if (apart && getenv("MPCVR_PORT_WHY")) {
				// Where the first disagreement sits, and what the taps were: a tap
				// count that differs says a boundary fell the other way, which is a
				// tie and not a wrong coefficient.
				for (size_t p = 0, shown = 0; p < cpu.size() && shown < 2; p++) {
					if (fabs((double)gpu[p] - (double)cpu[p]) <= kAllowed) {
						continue;
					}
					const int px = (int)((p / 4) % c.dw), py = (int)((p / 4) / c.dw);
					const float scaleY = (float)c.sh / c.dh;
					const float supportY = (float)kern->support * scaleY;
					const float posY = ((py + 0.5f) / c.dh) * c.sh + 0.5f;
					printf("      at %d,%d ch %d: gpu %.6f cpu %.6f | y pos %.7f support %.7f"
					       " taps [%d,%d) args",
						px, py, (int)(p % 4), gpu[p], cpu[p], posY, supportY,
						(int)floorf(posY - supportY), (int)ceilf(posY + supportY));
					for (int n = (int)floorf(posY - supportY); n < (int)ceilf(posY + supportY); n++) {
						printf(" %.6f", (n - posY + 0.5f) * (1.0f / scaleY));
					}
					printf("\n");
					shown++;
				}
			}
			const bool ok = wrong == 0 && judged > 0;
			failures += ok ? 0 : 1;
			printf("    %-14s worst %.2f half steps for the cancelling it did, %zu of %zu past %.0f",
				m.name, worst / kHalfStep, apart + wrong, judged * 4, kAllowed / kHalfStep);
			if (ties) {
				printf(", %zu pixel(s) set aside on a tie", ties);
			}
			printf("\n  [%s] %s is the kernel it says it is", ok ? "ok  " : "FAIL", m.name);
			if (apart) {
				printf(" (%zu value(s) further out than rounding accounts for, none near a wrong coefficient)", apart);
			}
			printf("\n");
		}
	}

	printf("\n=== Result ===\n  %d check(s) failed\n", failures);
	return failures ? 1 : 0;
}

} // namespace temporal
