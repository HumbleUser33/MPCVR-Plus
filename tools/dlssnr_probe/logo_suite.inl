// Hiding a channel's logo: how well the renderer finds it, tells it from the picture under
// it, and takes it out (Source/Logo/LogoAnalysis.cpp, which this compiles).
//
// The truth: pictures of a film, taken far enough apart to differ, made 1920 x 1080 with
// black bars as a channel shows a wide film, and six logos of known alpha and colour laid
// over them (logo_compose.inl):
//   translucent  white letters at 55 %, holes in them
//   outlined     opaque white letters on a dark rim
//   gradient     a badge from 25 % to 75 %, its text cut out
//   small        a thin "HD", 18 pixels high
//   channel      a channel's name: white letters, a red dot, two coloured letters
//   bar          translucent white letters read upwards, half over the black bar above the
//                picture: the bar's part can only be told by the logo's colour
// The renderer's analysis runs as it does on a film, on the pictures in turn: it looks for
// the logo on the first, fits it on the next, and the pictures after those are the test.
// Among the pictures it fits on and among the test ones are stretches without the logo,
// with fades either side, as advertising leaves.
//
//   found        the box holds every pixel of the logo (alpha over 2 %): its share
//   box/logo     the box's area against the logo's own box
//   IoU          the pixels taken for the logo's against its own
//   alpha        the mean error of alpha over the box; colour: of the colour where the
//                logo is at least 30 % opaque, in 8-bit levels
//   psnr         the luma restored where the logo was (its pixels and two round them),
//                against the picture without it; opaque, translucent: the logo's parts
//                at least 90 % opaque and the others
//   ghost        what is left of the logo's outline: the error's slope across it against
//                the logo's own (0: gone, 1: untouched)
//   texture      the restored opaque parts' fine detail against the picture's (1: as much)
//
// --tlogo runs what ships and checks it; --tlogo --study the comparisons it was chosen on;
// --tlogo --file <film> the analysis on a real film, with pictures of what it found and
// did (logo_*.png).

namespace temporal {
namespace logo {

using Logo::Plane;

struct Frame {
	Plane p[3];
};

// Pictures of a film through ffmpeg: 8-bit 4:2:0, one every `every`, from `start`
// seconds; a film narrower than 16:9 put between black bars in 1920 x 1080.
class CFrames
{
public:
	bool Open(const std::wstring& film, double start, int every)
	{
		Close();
		int W0 = 0, H0 = 0;
		if (FILE* probe = OpenPipe(std::format(L"ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0 \"{}\"", film))) {
			char line[256] = {};
			if (fgets(line, sizeof(line), probe)) {
				sscanf_s(line, "%d,%d", &W0, &H0);
			}
			_pclose(probe);
		}
		if (W0 < 64 || H0 < 64) {
			return false;
		}
		m_W0 = W0 & ~1;
		m_H0 = H0 & ~1;
		m_W = std::max(m_W0, 1920);
		m_H = std::max(m_H0, 1080);
		m_top = ((m_H - m_H0) / 2) & ~1;
		m_left = ((m_W - m_W0) / 2) & ~1;
		m_pipe = OpenPipe(std::format(L"ffmpeg -v error -ss {:.3f} -i \"{}\" -an -sn -vf \"select='not(mod(n\\,{}))',crop={}:{}:0:0\" -fps_mode passthrough -f rawvideo -pix_fmt yuv420p - 2>NUL",
			start, film, every, m_W0, m_H0));
		m_raw.resize((size_t)m_W0 * m_H0 * 3 / 2);
		return m_pipe != nullptr;
	}
	void Close()
	{
		if (m_pipe) {
			_pclose(m_pipe);
			m_pipe = nullptr;
		}
	}
	~CFrames() { Close(); }
	int Width() const { return m_W; }
	int Height() const { return m_H; }
	int Top() const { return m_top; }

	bool Next(Frame& f)
	{
		if (!m_pipe || fread(m_raw.data(), m_raw.size(), 1, m_pipe) != 1) {
			return false;
		}
		f.p[0].resize(m_W, m_H, 16 / 255.0f);
		f.p[1].resize(m_W / 2, m_H / 2, 128 / 255.0f);
		f.p[2].resize(m_W / 2, m_H / 2, 128 / 255.0f);
		const BYTE* y = m_raw.data();
		const BYTE* u = y + (size_t)m_W0 * m_H0;
		const BYTE* v = u + (size_t)(m_W0 / 2) * (m_H0 / 2);
		for (int r = 0; r < m_H0; r++) {
			for (int c = 0; c < m_W0; c++) {
				f.p[0].at(m_left + c, m_top + r) = y[(size_t)r * m_W0 + c] / 255.0f;
			}
		}
		for (int r = 0; r < m_H0 / 2; r++) {
			for (int c = 0; c < m_W0 / 2; c++) {
				f.p[1].at(m_left / 2 + c, m_top / 2 + r) = u[(size_t)r * (m_W0 / 2) + c] / 255.0f;
				f.p[2].at(m_left / 2 + c, m_top / 2 + r) = v[(size_t)r * (m_W0 / 2) + c] / 255.0f;
			}
		}
		return true;
	}

private:
	FILE* m_pipe = nullptr;
	int m_W0 = 0, m_H0 = 0, m_W = 0, m_H = 0, m_top = 0, m_left = 0;
	std::vector<BYTE> m_raw;
};

// The logo taken out of a whole picture, its bars told on the luma first, as the renderer
// does; and out of the luma alone.
static void RemoveFrame(const Logo::Maps& m, float s, const Logo::RemoveOptions& o, Plane p[3])
{
	const Logo::Bars bars = Logo::FindBars(m, p[0], o);
	for (int k = 0; k < 3; k++) {
		Logo::Remove(m, k, s, o, bars, p[k]);
	}
}

static void RemoveLuma(const Logo::Maps& m, float s, const Logo::RemoveOptions& o, Plane& luma)
{
	Logo::Remove(m, 0, s, o, Logo::FindBars(m, luma, o), luma);
}

// The logo's colour (luma, 8-bit levels) where it is over the picture, as the fit reads it
// to tell alpha over a bar (Logo::LogoColours, CDeblender::Options::oneColour): its 10th,
// 50th and 90th percentiles, each pixel counted by its alpha squared.
static void PrintColours(const Logo::Maps& m, const Logo::Rect& picture)
{
	float c[3] = {};
	if (!Logo::LogoColours(m, picture, c)) {
		Out("  %-34s too few pixels\n", "colour over the picture");
		return;
	}
	Out("  %-34s %6.1f %6.1f %6.1f  spread %.1f\n", "colour over the picture, 10/50/90 %", c[0] * 255, c[1] * 255, c[2] * 255,
		(c[2] - c[0]) * 255);
}

// How much the picture's first rows at a bar's edge follow the picture past them: under 0.97,
// blended with the bar (CDeblender::Blend).
static void PrintFollows(const Logo::CDeblender& fit)
{
	const std::vector<float>& f = fit.BlendFollows();
	static const char* sides[4] = { "top", "bottom", "left", "right" };
	for (size_t s = 0; s < 4 && 3 * s + 2 < f.size(); s++) {
		if (f[3 * s] >= 0) {
			Out("  %-34s %6.3f %6.3f %6.3f\n", std::format("rows at the {} bar follow", sides[s]).c_str(), f[3 * s], f[3 * s + 1], f[3 * s + 2]);
		}
	}
}

// Rounded to 8 bits, as the decoder gives it.
static void Round8(Frame& f)
{
	for (Plane& p : f.p) {
		for (float& v : p.v) {
			v = std::round(std::clamp(v, 0.0f, 1.0f) * 255.0f) / 255.0f;
		}
	}
}

static Logo::Crop CropOf(const Frame& f, const Logo::Rect& box)
{
	Logo::Crop c;
	for (int k = 0; k < 3; k++) {
		const Logo::Rect b = k ? Logo::Rect{ box.x / 2, box.y / 2, box.w / 2, box.h / 2 } : box;
		c.p[k].resize(b.w, b.h);
		for (int y = 0; y < b.h; y++) {
			for (int x = 0; x < b.w; x++) {
				c.p[k].at(x, y) = f.p[k].at(b.x + x, b.y + y);
			}
		}
	}
	return c;
}

// Alpha of a plane at a picture's pixel: 0 outside the box.
static float AlphaAt(const Logo::Maps& m, int plane, int x, int y)
{
	const int d = plane ? 2 : 1;
	const int bx = m.box.x / d, by = m.box.y / d;
	const Plane& a = m.alpha[plane];
	x -= bx;
	y -= by;
	if (x < 0 || y < 0 || x >= a.w || y >= a.h) {
		return 0;
	}
	return a.at(x, y);
}

// The logo's colour at a picture's pixel: what it lays over the picture, over its alpha.
static float ColourAt(const Logo::Maps& m, int plane, int x, int y)
{
	const int d = plane ? 2 : 1;
	const Plane& l = m.laid[plane];
	x -= m.box.x / d;
	y -= m.box.y / d;
	if (x < 0 || y < 0 || x >= l.w || y >= l.h) {
		return 0;
	}
	const float a = m.alpha[plane].at(x, y);
	return a > 0.05f ? l.at(x, y) / a : 0.0f;
}

struct FitScore {
	double found = 0;      // share of the logo's pixels inside the box
	double boxRatio = 0;
	double iou = 0, alpha = 0, colour = 0;
};

static FitScore ScoreFit(const Logo::Maps& est, const Logo::Maps& truth, const Logo::Rect& box)
{
	FitScore s;
	const Logo::Rect& t = truth.box;
	int tx0 = t.x + t.w, ty0 = t.y + t.h, tx1 = t.x, ty1 = t.y;
	int logo = 0, inside = 0;
	for (int y = 0; y < t.h; y++) {
		for (int x = 0; x < t.w; x++) {
			if (truth.alpha[0].at(x, y) > 0.02f) {
				const int X = t.x + x, Y = t.y + y;
				tx0 = std::min(tx0, X); ty0 = std::min(ty0, Y);
				tx1 = std::max(tx1, X + 1); ty1 = std::max(ty1, Y + 1);
				logo++;
				inside += X >= box.x && Y >= box.y && X < box.x + box.w && Y < box.y + box.h;
			}
		}
	}
	s.found = logo ? (double)inside / logo : 0;
	s.boxRatio = (tx1 > tx0 && ty1 > ty0) ? (double)box.w * box.h / ((double)(tx1 - tx0) * (ty1 - ty0)) : 0;
	if (!est.valid()) {
		return s;
	}
	const int x0 = std::min(t.x, est.box.x), y0 = std::min(t.y, est.box.y);
	const int x1 = std::max(t.x + t.w, est.box.x + est.box.w), y1 = std::max(t.y + t.h, est.box.y + est.box.h);
	int both = 0, either = 0, n = 0, nc = 0;
	double ea = 0, ec = 0;
	for (int y = y0; y < y1; y++) {
		for (int x = x0; x < x1; x++) {
			const float at = AlphaAt(truth, 0, x, y), ae = AlphaAt(est, 0, x, y);
			const bool mt = at > 0.02f, me = ae >= est.alphaMin;
			both += mt && me;
			either += mt || me;
			ea += std::abs(at - ae);
			n++;
			if (at >= 0.3f) {
				ec += std::abs(ColourAt(truth, 0, x, y) - ColourAt(est, 0, x, y)) * 255.0;
				nc++;
			}
		}
	}
	s.iou = either ? (double)both / either : 0;
	s.alpha = n ? ea / n : 0;
	s.colour = nc ? ec / nc : 0;
	return s;
}

struct RemoveScore {
	double psnr = 0, opaque = 0, translucent = 0, chroma = 0, ghost = 0, texture = 0;
	int n = 0;
	void Add(const RemoveScore& o)
	{
		psnr += o.psnr; opaque += o.opaque; translucent += o.translucent; chroma += o.chroma;
		ghost += o.ghost; texture += o.texture; n++;
	}
	RemoveScore Mean() const
	{
		RemoveScore m = *this;
		if (n) {
			m.psnr /= n; m.opaque /= n; m.translucent /= n; m.chroma /= n; m.ghost /= n; m.texture /= n;
		}
		return m;
	}
};

static double PsnrOf(double se, int n) { return n ? (se > 0 ? 10.0 * std::log10(n / se) : 99.0) : 0; }

// The restored picture against the truth, where the logo was.
static RemoveScore ScoreRemoval(const Frame& out, const Frame& seen, const Frame& clean, const Logo::Maps& truth)
{
	RemoveScore s;
	const Logo::Rect& b = truth.box;
	const Plane& A = truth.alpha[0];
	auto alpha = [&](int x, int y) { return (x < 0 || y < 0 || x >= A.w || y >= A.h) ? 0.0f : A.at(x, y); };
	double se = 0, seO = 0, seT = 0;
	int n = 0, nO = 0, nT = 0;
	double gOut = 0, gSeen = 0, hfOut = 0, hfClean = 0;
	for (int y = -2; y < b.h + 2; y++) {
		for (int x = -2; x < b.w + 2; x++) {
			bool nearLogo = false;
			for (int dy = -2; dy <= 2 && !nearLogo; dy++) {
				for (int dx = -2; dx <= 2 && !nearLogo; dx++) {
					nearLogo = alpha(x + dx, y + dy) > 0.02f;
				}
			}
			if (!nearLogo) {
				continue;
			}
			const int X = b.x + x, Y = b.y + y;
			const double d = out.p[0].at(X, Y) - clean.p[0].at(X, Y);
			se += d * d;
			n++;
			const float a = alpha(x, y);
			if (a >= 0.9f) {
				seO += d * d;
				nO++;
				const auto lap = [&](const Plane& p) {
					return std::abs(4 * p.at(X, Y) - p.at(X - 1, Y) - p.at(X + 1, Y) - p.at(X, Y - 1) - p.at(X, Y + 1));
				};
				hfOut += lap(out.p[0]);
				hfClean += lap(clean.p[0]);
			} else if (a > 0.05f) {
				seT += d * d;
				nT++;
			}
			// The logo's outline: where its alpha changes fast.
			const float gax = alpha(x + 1, y) - alpha(x - 1, y), gay = alpha(x, y + 1) - alpha(x, y - 1);
			const float ga = std::sqrt(gax * gax + gay * gay);
			if (ga > 0.2f) {
				const auto slope = [&](const Plane& p, const Plane& q) {
					const float ex = (p.at(X + 1, Y) - q.at(X + 1, Y)) - (p.at(X - 1, Y) - q.at(X - 1, Y));
					const float ey = (p.at(X, Y + 1) - q.at(X, Y + 1)) - (p.at(X, Y - 1) - q.at(X, Y - 1));
					return std::abs(ex * gax + ey * gay) / ga;
				};
				gOut += slope(out.p[0], clean.p[0]);
				gSeen += slope(seen.p[0], clean.p[0]);
			}
		}
	}
	s.psnr = PsnrOf(se, n);
	s.opaque = nO ? PsnrOf(seO, nO) : 0;
	s.translucent = nT ? PsnrOf(seT, nT) : 0;
	s.ghost = gSeen > 0 ? gOut / gSeen : 0;
	s.texture = hfClean > 0 ? hfOut / hfClean : 0;
	double sc = 0;
	int nc = 0;
	for (int k = 1; k < 3; k++) {
		const Plane& C = truth.alpha[k];
		for (int y = 0; y < C.h; y++) {
			for (int x = 0; x < C.w; x++) {
				if (C.at(x, y) > 0.02f) {
					const int X = b.x / 2 + x, Y = b.y / 2 + y;
					const double d = out.p[k].at(X, Y) - clean.p[k].at(X, Y);
					sc += d * d;
					nc++;
				}
			}
		}
	}
	s.chroma = PsnrOf(sc, nc);
	return s;
}

// The strength the logo is laid at on test picture i of n: two stretches without it, the
// first with fades either side.
static float TestStrength(int i, int n)
{
	const int a = n / 3, b = a + 6;
	if (i == a - 2) return 0.66f;
	if (i == a - 1) return 0.33f;
	if (i >= a && i < b) return 0.0f;
	if (i == b) return 0.33f;
	if (i == b + 1) return 0.66f;
	if (i >= n - 3) return 0.0f;
	return 1.0f;
}

// The pictures the fit is made on: a stretch without the logo in the middle.
static float FitStrength(int i, int n)
{
	const int a = n / 2, b = a + n / 8;
	return (i >= a && i < b) ? 0.0f : 1.0f;
}

static void PrintFit(const char* label, const FitScore& f, int dropped)
{
	Out("  %-34s %6.3f %6.2f %6.3f %7.4f %7.2f %4d\n", label, f.found, f.boxRatio, f.iou, f.alpha, f.colour, dropped);
}

static void PrintRemove(const char* label, const RemoveScore& r)
{
	Out("  %-34s %6.2f %6.2f %6.2f %6.2f %6.3f %6.3f\n", label, r.psnr, r.opaque, r.translucent, r.chroma, r.ghost, r.texture);
}

// The pictures the analysis sees, in turn, and for the test ones the truth: the logo laid
// over the film's pictures here, or laid over every picture of the film and the whole
// passed through x264 at a channel's rate (1080p25 at 3 Mbit/s), which leaves round the
// logo the ringing a real channel's has.
class CSource
{
public:
	bool Open(const std::wstring& film, double start, int every, const LogoCompose::Design& d,
		int nLocate, int nFit, int nTest, bool x264)
	{
		m_design = &d;
		m_nLocate = nLocate;
		m_nFit = nFit;
		m_nTest = nTest;
		m_x264 = x264;
		m_i = 0;
		m_truth.clear();
		if (!x264) {
			return m_frames.Open(film, start, every);
		}
		CFrames all;
		if (!all.Open(film, start, 1)) {
			return false;
		}
		const int W = all.Width(), H = all.Height();
		const wchar_t* file = L"logo_x264.mp4";
		DeleteFileW(file);
		FILE* enc = _wpopen(std::format(L"\"ffmpeg -v error -y -f rawvideo -pix_fmt yuv420p -s {}x{} -r 25 -i - -c:v libx264 -preset fast -b:v 3M -maxrate 4M -bufsize 6M -g 50 \"{}\" 2>NUL\"",
			W, H, file).c_str(), L"wb");
		if (!enc) {
			return false;
		}
		std::vector<BYTE> raw((size_t)W * H * 3 / 2);
		Frame f;
		const int total = (nLocate + nFit + nTest) * every;
		for (int n = 0; n < total && all.Next(f); n++) {
			const int i = n / every;
			if (i >= nLocate + nFit && n % every == 0) {
				Frame t = f;
				Round8(t);
				m_truth.push_back(std::move(t));
			}
			LogoCompose::Lay(d, Strength(i), f.p);
			size_t o = 0;
			for (const Plane& p : f.p) {
				for (float v : p.v) {
					raw[o++] = (BYTE)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f);
				}
			}
			fwrite(raw.data(), raw.size(), 1, enc);
		}
		all.Close();
		_pclose(enc);
		return m_frames.Open(file, 0, every);
	}

	float Strength(int i) const
	{
		return i < m_nLocate ? 1.0f
			: i < m_nLocate + m_nFit ? FitStrength(i - m_nLocate, m_nFit)
			: TestStrength(i - m_nLocate - m_nFit, m_nTest);
	}

	// The next picture as the renderer would get it; for a test picture, the truth too.
	bool Next(Frame& seen, Frame* clean)
	{
		if (!m_frames.Next(seen)) {
			return false;
		}
		const int i = m_i++;
		const int test = i - m_nLocate - m_nFit;
		if (m_x264) {
			if (clean) {
				if (test < 0 || test >= (int)m_truth.size()) {
					return false;
				}
				*clean = m_truth[test];
			}
			return true;
		}
		Round8(seen);
		if (clean) {
			*clean = seen;
		}
		LogoCompose::Lay(*m_design, Strength(i), seen.p);
		Round8(seen);
		return true;
	}

	void Close() { m_frames.Close(); }

private:
	CFrames m_frames;
	const LogoCompose::Design* m_design = nullptr;
	int m_nLocate = 0, m_nFit = 0, m_nTest = 0, m_i = 0;
	bool m_x264 = false;
	std::vector<Frame> m_truth;
};

// The renderer's two shaders, compiled from Shaders/d3d11 as the renderer's build does:
// the plane copied, the logo taken out of its box, on 16-bit planes.
class CLogoGpu
{
public:
	bool Init(ID3D11Device* dev, ID3D11DeviceContext* ctx, std::string& error)
	{
		m_dev = dev;
		m_ctx = ctx;
		for (int ch = 1; ch <= 2; ch++) {
			if (!Compile(L"..\\..\\Shaders\\d3d11\\cs_logo_pack.hlsl", ch, 2, &m_pack[ch - 1], error, "MODE")
				|| !Compile(L"..\\..\\Shaders\\d3d11\\cs_logo_remove.hlsl", ch, 0, &m_fill[ch - 1], error, "PASS")
				|| !Compile(L"..\\..\\Shaders\\d3d11\\cs_logo_remove.hlsl", ch, 1, &m_remove[ch - 1], error, "PASS")) {
				return false;
			}
		}
		D3D11_BUFFER_DESC d = {};
		d.Usage = D3D11_USAGE_DYNAMIC;
		d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		d.ByteWidth = sizeof(Logo::PackConstants);
		if (FAILED(dev->CreateBuffer(&d, nullptr, &m_cbPack))) {
			return false;
		}
		d.ByteWidth = sizeof(Logo::RemoveConstants);
		if (FAILED(dev->CreateBuffer(&d, nullptr, &m_cbRemove))) {
			return false;
		}
		D3D11_BUFFER_DESC s = {};
		s.ByteWidth = Logo::kStateBytes;
		s.Usage = D3D11_USAGE_DEFAULT;
		s.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		s.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		const float one[Logo::kStateBytes / 4] = { 1 };
		D3D11_SUBRESOURCE_DATA sd = { one };
		if (FAILED(dev->CreateBuffer(&s, &sd, &m_state))) {
			return false;
		}
		D3D11_UNORDERED_ACCESS_VIEW_DESC u = {};
		u.Format = DXGI_FORMAT_R32_TYPELESS;
		u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		u.Buffer.NumElements = Logo::kStateBytes / 4;
		u.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		return SUCCEEDED(dev->CreateUnorderedAccessView(m_state, &u, &m_stateUav));
	}

	// The planes of a picture and the logo's maps, as the renderer would hold them.
	bool Prepare(const Frame& f, const Logo::Maps& m, const Logo::RemoveOptions& o)
	{
		m_maps = m;
		m_opt = o;
		for (int k = 0; k < 2; k++) {
			const int w = k ? f.p[1].w : f.p[0].w, h = k ? f.p[1].h : f.p[0].h;
			const DXGI_FORMAT fmt = k ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R16_UNORM;
			m_src[k].Release();
			m_dst[k].Release();
			m_stage[k].Release();
			m_dstUav[k].Release();
			if (FAILED(m_src[k].Create(m_dev, fmt, w, h, Tex2D_DefaultShader)) || FAILED(m_dst[k].Create(m_dev, fmt, w, h, Tex2D_DefaultShaderRTargetUAV))
				|| FAILED(m_dev->CreateUnorderedAccessView(m_dst[k].pTexture, nullptr, &m_dstUav[k]))) {
				return false;
			}
			D3D11_TEXTURE2D_DESC sd = {};
			m_dst[k].pTexture->GetDesc(&sd);
			sd.Usage = D3D11_USAGE_STAGING;
			sd.BindFlags = 0;
			sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			sd.MiscFlags = 0;
			if (FAILED(m_dev->CreateTexture2D(&sd, nullptr, &m_stage[k]))) {
				return false;
			}
		}
		std::vector<uint16_t> luma, chroma, mirror[2];
		Logo::ShaderMaps(m, o, luma, chroma, mirror[0], mirror[1]);
		for (int k = 0; k < 2; k++) {
			const int w = k ? m.alpha[1].w : m.alpha[0].w, h = k ? m.alpha[1].h : m.alpha[0].h;
			m_map[k].Release();
			m_mirror[k].Release();
			if (FAILED(m_map[k].Create(m_dev, DXGI_FORMAT_R16G16B16A16_UNORM, w, h, Tex2D_DefaultShader))
				|| FAILED(m_mirror[k].Create(m_dev, DXGI_FORMAT_R16G16_UINT, w, h, Tex2D_DefaultShader))) {
				return false;
			}
			m_ctx->UpdateSubresource(m_map[k].pTexture, 0, nullptr, (k ? chroma : luma).data(), 8 * w, 0);
			m_ctx->UpdateSubresource(m_mirror[k].pTexture, 0, nullptr, mirror[k].data(), 4 * w, 0);
			D3D11_BUFFER_DESC b = {};
			b.ByteWidth = 16 * Logo::PyramidBufferEntries(w, h);
			b.Usage = D3D11_USAGE_DEFAULT;
			b.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
			b.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			b.StructureByteStride = 16;
			m_pyr[k].Release();
			m_pyrUav[k].Release();
			if (FAILED(m_dev->CreateBuffer(&b, nullptr, &m_pyr[k])) || FAILED(m_dev->CreateUnorderedAccessView(m_pyr[k], nullptr, &m_pyrUav[k]))) {
				return false;
			}
		}
		Upload(f);
		return true;
	}

	void Upload(const Frame& f)
	{
		std::vector<uint16_t> w(f.p[0].v.size());
		for (size_t i = 0; i < w.size(); i++) {
			w[i] = (uint16_t)std::lround(std::clamp(f.p[0].v[i], 0.0f, 1.0f) * 65535.0f);
		}
		m_ctx->UpdateSubresource(m_src[0].pTexture, 0, nullptr, w.data(), 2 * f.p[0].w, 0);
		w.resize(2 * f.p[1].v.size());
		for (size_t i = 0; i < f.p[1].v.size(); i++) {
			w[2 * i] = (uint16_t)std::lround(std::clamp(f.p[1].v[i], 0.0f, 1.0f) * 65535.0f);
			w[2 * i + 1] = (uint16_t)std::lround(std::clamp(f.p[2].v[i], 0.0f, 1.0f) * 65535.0f);
		}
		m_ctx->UpdateSubresource(m_src[1].pTexture, 0, nullptr, w.data(), 4 * f.p[1].w, 0);
	}

	// The strength the luma pass left in the state, read back.
	float Strength()
	{
		D3D11_BUFFER_DESC d = {};
		d.ByteWidth = Logo::kStateBytes;
		d.Usage = D3D11_USAGE_STAGING;
		d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		CComPtr<ID3D11Buffer> stage;
		float v = -1;
		if (SUCCEEDED(m_dev->CreateBuffer(&d, nullptr, &stage))) {
			m_ctx->CopyResource(stage, m_state);
			D3D11_MAPPED_SUBRESOURCE mr = {};
			if (SUCCEEDED(m_ctx->Map(stage, 0, D3D11_MAP_READ, 0, &mr))) {
				v = *(const float*)mr.pData;
				m_ctx->Unmap(stage, 0);
			}
		}
		return v;
	}

	void SetStrength(float s)
	{
		const float v[Logo::kStateBytes / 4] = { s };
		m_ctx->UpdateSubresource(m_state, 0, nullptr, v, 0, 0);
	}

	// The renderer's passes for one picture: each plane copied, then the logo taken out of
	// luma (which measures its strength) and of chroma.
	// parts: 1 the planes copied, 2 the fill, 4 the write; all by default.
	void Dispatch(bool update, int parts = 7)
	{
		for (int k = 0; k < 2 && (parts & 1); k++) {
			D3D11_TEXTURE2D_DESC d = {};
			m_src[k].pTexture->GetDesc(&d);
			Logo::PackConstants pc = {};
			pc.width = d.Width;
			pc.height = d.Height;
			pc.scale = 1;
			SetConstants(m_cbPack, &pc, sizeof(pc));
			ID3D11ShaderResourceView* srv[2] = { m_src[k].pShaderResource, nullptr };
			ID3D11UnorderedAccessView* uav[3] = { m_dstUav[k], nullptr, nullptr };
			m_ctx->CSSetShader(m_pack[k], nullptr, 0);
			m_ctx->CSSetShaderResources(0, 1, srv);
			m_ctx->CSSetUnorderedAccessViews(0, 1, uav, nullptr);
			m_ctx->CSSetConstantBuffers(0, 1, &m_cbPack.p);
			m_ctx->Dispatch((d.Width + 15) / 16, (d.Height + 15) / 16, 1);
			ID3D11UnorderedAccessView* none[3] = {};
			ID3D11ShaderResourceView* noSrv[2] = {};
			m_ctx->CSSetUnorderedAccessViews(0, 3, none, nullptr);
			m_ctx->CSSetShaderResources(0, 2, noSrv);
		}
		for (int k = 0; k < 2; k++) {
			const Logo::RemoveConstants rc = Logo::MakeRemoveConstants(m_maps, k, m_opt, update);
			SetConstants(m_cbRemove, &rc, sizeof(rc));
			ID3D11ShaderResourceView* srv[3] = { m_src[k].pShaderResource, m_map[k].pShaderResource, m_mirror[k].pShaderResource };
			ID3D11UnorderedAccessView* uav[3] = { m_dstUav[k], m_pyrUav[k], m_stateUav };
			m_ctx->CSSetShaderResources(0, 3, srv);
			m_ctx->CSSetUnorderedAccessViews(0, 3, uav, nullptr);
			m_ctx->CSSetConstantBuffers(0, 1, &m_cbRemove.p);
			if (parts & 2) {
				m_ctx->CSSetShader(m_fill[k], nullptr, 0);
				m_ctx->Dispatch(1, 1, 1);
			}
			if (parts & 4) {
				m_ctx->CSSetShader(m_remove[k], nullptr, 0);
				m_ctx->Dispatch((rc.boxW + 15) / 16, (rc.boxH + 15) / 16, 1);
			}
			ID3D11UnorderedAccessView* none[3] = {};
			ID3D11ShaderResourceView* noSrv[3] = {};
			m_ctx->CSSetUnorderedAccessViews(0, 3, none, nullptr);
			m_ctx->CSSetShaderResources(0, 3, noSrv);
		}
		m_ctx->CSSetShader(nullptr, nullptr, 0);
	}

	void Read(Frame& out)
	{
		for (int k = 0; k < 2; k++) {
			m_ctx->CopyResource(m_stage[k], m_dst[k].pTexture);
			D3D11_MAPPED_SUBRESOURCE mr = {};
			if (FAILED(m_ctx->Map(m_stage[k], 0, D3D11_MAP_READ, 0, &mr))) {
				continue;
			}
			const Plane& shape = out.p[k ? 1 : 0];
			for (int y = 0; y < shape.h; y++) {
				const uint16_t* row = (const uint16_t*)((const BYTE*)mr.pData + (size_t)mr.RowPitch * y);
				for (int x = 0; x < shape.w; x++) {
					if (k) {
						out.p[1].at(x, y) = row[2 * x] / 65535.0f;
						out.p[2].at(x, y) = row[2 * x + 1] / 65535.0f;
					} else {
						out.p[0].at(x, y) = row[x] / 65535.0f;
					}
				}
			}
			m_ctx->Unmap(m_stage[k], 0);
		}
	}

private:
	bool Compile(const wchar_t* file, int channels, int mode, ID3D11ComputeShader** cs, std::string& error, const char* name)
	{
		char ch[8], md[8];
		sprintf_s(ch, "%d", channels);
		sprintf_s(md, "%d", mode);
		const D3D_SHADER_MACRO defines[] = { { "CHANNELS", ch }, { name, md }, { nullptr, nullptr } };
		CComPtr<ID3DBlob> code, errors;
		const HRESULT hr = D3DCompileFromFile(file, defines, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
		if (FAILED(hr)) {
			error = errors ? std::string((const char*)errors->GetBufferPointer(), errors->GetBufferSize()) : std::format("cannot compile (0x{:08X})", (unsigned)hr);
			return false;
		}
		return SUCCEEDED(m_dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, cs));
	}

	void SetConstants(ID3D11Buffer* b, const void* p, size_t n)
	{
		D3D11_MAPPED_SUBRESOURCE mr = {};
		if (SUCCEEDED(m_ctx->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr))) {
			memcpy(mr.pData, p, n);
			m_ctx->Unmap(b, 0);
		}
	}

	ID3D11Device* m_dev = nullptr;
	ID3D11DeviceContext* m_ctx = nullptr;
	CComPtr<ID3D11ComputeShader> m_pack[2], m_fill[2], m_remove[2];
	CComPtr<ID3D11Buffer> m_cbPack, m_cbRemove, m_state, m_pyr[2];
	CComPtr<ID3D11UnorderedAccessView> m_stateUav, m_pyrUav[2], m_dstUav[2];
	CComPtr<ID3D11Texture2D> m_stage[2];
	Tex2D_t m_src[2], m_dst[2], m_map[2], m_mirror[2];
	Logo::Maps m_maps;
	Logo::RemoveOptions m_opt;
};

// The same picture and logo twice the size: what a 2160p film's would be.
static void Double(const Frame& f, const Logo::Maps& m, Frame& f2, Logo::Maps& m2)
{
	for (int k = 0; k < 3; k++) {
		f2.p[k].resize(2 * f.p[k].w, 2 * f.p[k].h);
		for (int y = 0; y < f2.p[k].h; y++) {
			for (int x = 0; x < f2.p[k].w; x++) {
				f2.p[k].at(x, y) = f.p[k].at(x / 2, y / 2);
			}
		}
	}
	m2 = m;
	m2.box = { 2 * m.box.x, 2 * m.box.y, 2 * m.box.w, 2 * m.box.h };
	for (int k = 0; k < 3; k++) {
		for (Plane* q : { &m2.alpha[k], &m2.laid[k], k ? nullptr : &m2.excess }) {
			if (!q || q->v.empty()) {
				continue;
			}
			const Plane src = *q;
			q->resize(2 * src.w, 2 * src.h);
			for (int y = 0; y < q->h; y++) {
				for (int x = 0; x < q->w; x++) {
					q->at(x, y) = src.at(x / 2, y / 2);
				}
			}
		}
	}
}

// The shaders against the reference in double precision, on test pictures with the logo
// and without; and what they cost.
static void PortCheck(ID3D11Device* dev, ID3D11DeviceContext* ctx, const std::vector<Frame>& seen,
	const std::vector<float>& strength, Logo::Maps maps, const Logo::RemoveOptions& o)
{
	CLogoGpu gpu;
	std::string error;
	if (!gpu.Init(dev, ctx, error)) {
		Out("  %s\n", error.c_str());
		Check(false, "the logo's shaders compiled");
		return;
	}
	Logo::RoundMaps(maps);
	Out("  %-34s %8s %8s %8s\n", "shaders against the reference", "on", "luma", "chroma");
	double worst = 0;
	float prev = 1;
	gpu.SetStrength(1);
	for (size_t i = 0; i < seen.size(); i += 3) {
		if (!gpu.Prepare(seen[i], maps, o)) {
			Check(false, "the logo's textures");
			return;
		}
		gpu.SetStrength(prev);
		gpu.Dispatch(true);
		Frame g = seen[i];
		gpu.Read(g);
		float conf = 0;
		const float measured = Logo::Presence(maps, seen[i].p[0], o, &conf);
		const float s = Logo::PresenceStep(prev, measured, conf, o);
		prev = s;
		Frame c = seen[i];
		RemoveFrame(maps, s, o, c.p);
		double dl = 0, dc = 0;
		for (int k = 0; k < 3; k++) {
			for (size_t j = 0; j < c.p[k].v.size(); j++) {
				const double d = std::abs(c.p[k].v[j] - g.p[k].v[j]) * 65535.0;
				(k ? dc : dl) = std::max(k ? dc : dl, d);
			}
		}
		Out("  %-34s %8.2f %8.1f %8.1f\n", std::format("picture {} (laid {:.2f})", i, strength[i]).c_str(), s, dl, dc);
		if (std::max(dl, dc) > 64) {
			// Where they part, and what the maps say there.
			const float gs = gpu.Strength();
			int wk = 0;
			size_t wj = 0;
			double wd = 0;
			for (int k = 0; k < 3; k++) {
				for (size_t j = 0; j < c.p[k].v.size(); j++) {
					const double d = std::abs(c.p[k].v[j] - g.p[k].v[j]);
					if (d > wd) {
						wd = d;
						wk = k;
						wj = j;
					}
				}
			}
			const int X = (int)(wj % c.p[wk].w), Y = (int)(wj / c.p[wk].w);
			const int d = wk ? 2 : 1;
			const int bx = X - maps.box.x / d, by = Y - maps.box.y / d;
			const Plane& A = maps.alpha[wk];
			const bool inBox = bx >= 0 && by >= 0 && bx < A.w && by < A.h;
			Out("    gpu strength %.3f; plane %d at (%d, %d), box (%d, %d): seen %.4f, reference %.4f, shader %.4f",
				gs, wk, X, Y, bx, by, seen[i].p[wk].v[wj], c.p[wk].v[wj], g.p[wk].v[wj]);
			if (inBox) {
				Out(", alpha %.4f, laid %.4f, doubt %.4g", A.at(bx, by), maps.laid[wk].at(bx, by), Logo::DoubtPlane(maps, wk, o).at(bx, by));
			}
			Out("\n");
		}
		worst = std::max(worst, std::max(dl, dc));
	}
	// A few steps of 16 bits: the shader's float sums in another order than the reference's.
	Check(worst <= 64, "the shaders give the reference's picture (16-bit steps)");

	// What it costs a picture: the two planes copied and the logo taken out.
	Frame f2;
	Logo::Maps m2;
	Double(seen[0], maps, f2, m2);
	const Logo::Rect& b = maps.box;
	for (int big = 0; big < 2; big++) {
		if (!gpu.Prepare(big ? f2 : seen[0], big ? m2 : maps, o)) {
			continue;
		}
		const double ms = GpuMs(dev, ctx, 64, [&]() { gpu.Dispatch(false); });
		Out("  %-34s %8.3f ms\n", std::format("cost, {}p, a {} x {} box", big ? 2160 : 1080, big ? 2 * b.w : b.w, big ? 2 * b.h : b.h).c_str(), ms);
		const double copy = GpuMs(dev, ctx, 64, [&]() { gpu.Dispatch(false, 1); });
		const double fill = GpuMs(dev, ctx, 64, [&]() { gpu.Dispatch(false, 2); });
		const double write = GpuMs(dev, ctx, 64, [&]() { gpu.Dispatch(false, 4); });
		Out("  %-34s %8.3f %8.3f %8.3f ms\n", "  planes copied, fill, write", copy, fill, write);
	}
}

// A box of the picture in RGB, enlarged `zoom` times, put into a canvas at (dx, dy).
static void Blit(std::vector<float>& canvas, int CW, const Frame& f, const Logo::Rect& r, int zoom, int dx, int dy)
{
	for (int y = 0; y < r.h * zoom; y++) {
		for (int x = 0; x < r.w * zoom; x++) {
			const int X = r.x + x / zoom, Y = r.y + y / zoom;
			const float Yv = (f.p[0].at(X, Y) * 255 - 16) / 219;
			const float Cb = (f.p[1].at(X / 2, Y / 2) * 255 - 128) / 224;
			const float Cr = (f.p[2].at(X / 2, Y / 2) * 255 - 128) / 224;
			float* o = &canvas[4 * ((size_t)(dy + y) * CW + dx + x)];
			o[0] = Yv + 1.5748f * Cr;
			o[1] = Yv - 0.1873f * Cb - 0.4681f * Cr;
			o[2] = Yv + 1.8556f * Cb;
			o[3] = 1;
		}
	}
}

// The analysis on a real film: no truth, so pictures of what it found and did, and what
// is left of the outline it found.
static int RunReal(const wchar_t* film)
{
	if (!film || !*film) {
		Check(false, "--tlogo --real needs --film <film>");
		return 1;
	}
	const std::wstring path(film);
	Out("  %S\n", path.substr(path.find_last_of(L"\\/") + 1).c_str());
	const double start = 120;
	const int every = 10, nLocate = 90, nFit = 240, nTest = 10, testEvery = 40;
	CFrames fr;
	if (!fr.Open(path, start, every)) {
		Check(false, "the film through ffmpeg");
		return 1;
	}
	const int W = fr.Width(), H = fr.Height();
	Logo::CLocator::Options lo;
	Logo::CDeblender::Options fo;
	Logo::RemoveOptions ro;

	Logo::CLocator loc;
	loc.Reset(W, H, 1, lo.edge);
	Logo::CLocator::Result found;
	Frame f;
	Out("  %-34s %s\n", "pictures looked at", "box");
	for (int i = 0; i < nLocate && fr.Next(f); i++) {
		loc.Add(f.p[0]);
		for (int c : { 10, 20, 30, 45, 60, 90 }) {
			if (c == i + 1) {
				const Logo::CLocator::Result r = loc.Locate(lo);
				Out("  %-34s %s\n", std::format("{}", c).c_str(), r.found
					? std::format("{} x {} at ({}, {}), {} edge pixels, picture {}..{}", r.box.w, r.box.h, r.box.x, r.box.y, r.edgePixels, r.picture.y, r.picture.y + r.picture.h).c_str()
					: r.still ? "still" : "none");
				// At 60, or later if the film stood still until then.
				if (c == 60 || (c > 60 && !found.found)) {
					found = r;
				}
			}
		}
	}
	Check(found.found, "the logo found");
	if (!found.found) {
		return 1;
	}
	Logo::CDeblender fit;
	fit.Reset(found.box, found.picture, 2, 2);
	Logo::Maps maps, half;
	Out("  %-34s %6s %6s %8s %6s\n", "pictures fitted on", "out", "a min", "logo px", "mean a");
	for (int i = 0; i < nFit && fr.Next(f); i++) {
		fit.Add(CropOf(f, found.box));
		for (int c : { 30, 60, 120, 240 }) {
			if (c == i + 1) {
				Logo::Maps m;
				fit.Fit(fo, m);
				int px = 0;
				double sa = 0;
				for (float a : m.alpha[0].v) {
					if (a >= m.alphaMin) {
						px++;
						sa += a;
					}
				}
				Out("  %-34s %6d %6.3f %8d %6.3f\n", std::format("{}", c).c_str(), fit.Dropped(), m.alphaMin, px, px ? sa / px : 0.0);
				PrintColours(m, found.picture);
				if (c == 120) {
					half = m;
				}
				if (c == nFit) {
					maps = m;
				}
			}
		}
	}
	if (!maps.valid()) {
		Check(false, "the logo fitted");
		return 1;
	}
	PrintColours(maps, found.picture);
	PrintFollows(fit);
	Out("  %-34s top %d, bottom %d, left %d, right %d\n", "rows blended with a bar", fit.Blend().top, fit.Blend().bottom,
		fit.Blend().left, fit.Blend().right);
	{
		double d = 0;
		for (size_t i = 0; i < maps.alpha[0].v.size(); i++) {
			d += std::abs(maps.alpha[0].v[i] - half.alpha[0].v[i]);
		}
		Out("  %-34s %7.4f\n", "alpha, 120 against 240 pictures", d / maps.alpha[0].v.size());
	}

	// Alpha and what is laid, by distance from the logo's opaque parts: a shadow, a rim, or
	// the codec's ringing show there.
	{
		const Plane& A = maps.alpha[0];
		const Plane& L = maps.laid[0];
		std::vector<uint8_t> core(A.v.size());
		for (size_t i = 0; i < core.size(); i++) {
			core[i] = A.v[i] >= 0.5f;
		}
		Out("  %-34s %7s %7s %7s %6s %6s\n", "distance from alpha 0.5", "alpha", "laid", "colour", "under", "over");
		for (int d = 0; d <= 8; d++) {
			double sa = 0, sl = 0;
			int n = 0, under = 0, over = 0;
			for (int y = 0; y < A.h; y++) {
				for (int x = 0; x < A.w; x++) {
					int best = 99;
					for (int dy = -d; dy <= d && best > 0; dy++) {
						for (int dx = -d; dx <= d; dx++) {
							const int xx = x + dx, yy = y + dy;
							if (xx >= 0 && yy >= 0 && xx < A.w && yy < A.h && core[(size_t)yy * A.w + xx]) {
								best = std::min(best, std::max(std::abs(dx), std::abs(dy)));
							}
						}
					}
					if (best != d) {
						continue;
					}
					sa += A.at(x, y);
					sl += L.at(x, y);
					n++;
					under += A.at(x, y) <= -maps.alphaMin;
					over += A.at(x, y) >= maps.alphaMin;
				}
			}
			if (n) {
				Out("  %-34s %7.3f %7.3f %7.1f %6d %6d\n", std::format("{}", d).c_str(), sa / n, sl / n,
					std::abs(sa) > 1e-6 ? sl / sa * 255 : 0.0, under, over);
			}
		}
	}

	// Test pictures from further on, and what is left of the outline.
	std::vector<Frame> seen, out;
	for (int i = 0; (int)seen.size() < nTest && fr.Next(f); i++) {
		if (i % testEvery == 0) {
			seen.push_back(f);
		}
	}
	struct Left { double outline = 0, texture = 0; };
	// The outline the fit found, across which the logo's edge would show; and the fine
	// detail where the logo was opaque against that round it.
	auto measure = [&](const Frame& before, const Frame& after) {
		const Logo::Rect& b = maps.box;
		const Plane& A = maps.alpha[0];
		double gOut = 0, gSeen = 0, hfOut = 0, hfRound = 0;
		int nIn = 0, nRound = 0;
		for (int y = 1; y < b.h - 1; y++) {
			for (int x = 1; x < b.w - 1; x++) {
				const int X = b.x + x, Y = b.y + y;
				const float gax = A.at(x + 1, y) - A.at(x - 1, y), gay = A.at(x, y + 1) - A.at(x, y - 1);
				const float ga = std::sqrt(gax * gax + gay * gay);
				auto across = [&](const Plane& p) {
					return std::abs((p.at(X + 1, Y) - p.at(X - 1, Y)) * gax + (p.at(X, Y + 1) - p.at(X, Y - 1)) * gay) / ga;
				};
				if (ga > 0.2f) {
					gOut += across(after.p[0]);
					gSeen += across(before.p[0]);
				}
				const float lap = std::abs(4 * after.p[0].at(X, Y) - after.p[0].at(X - 1, Y) - after.p[0].at(X + 1, Y)
					- after.p[0].at(X, Y - 1) - after.p[0].at(X, Y + 1));
				if (A.at(x, y) >= 0.9f) {
					hfOut += lap;
					nIn++;
				} else if (A.at(x, y) < maps.alphaMin && x > 4 && y > 4 && x < b.w - 5 && y < b.h - 5) {
					hfRound += lap;
					nRound++;
				}
			}
		}
		Left l;
		l.outline = gSeen > 0 ? gOut / gSeen : 0.0;
		l.texture = (nIn && nRound && hfRound > 0) ? (hfOut / nIn) / (hfRound / nRound) : 0.0;
		return l;
	};
	auto removeAll = [&](const Logo::RemoveOptions& o, std::vector<Frame>* keep, bool print) {
		Left sum;
		for (size_t i = 0; i < seen.size(); i++) {
			Frame after = seen[i];
			const float s = Logo::Presence(maps, seen[i].p[0], o, nullptr);
			RemoveFrame(maps, s, o, after.p);
			const Left l = measure(seen[i], after);
			if (print) {
				Out("  %-34s %6.2f %8.3f %8.3f\n", std::format("{}", i).c_str(), s, l.outline, l.texture);
			}
			sum.outline += l.outline / seen.size();
			sum.texture += l.texture / seen.size();
			if (keep) {
				keep->push_back(std::move(after));
			}
		}
		return sum;
	};
	// The fill's own error on this film: the logo's opaque shape moved where there is no
	// logo, filled in there as it would be, and set against the pixels it covered.
	{
		Logo::Maps probe = maps;
		const int shift = maps.box.w + 64;
		probe.excess = Plane();   // nothing doubted: the fill alone
		probe.box.x = maps.box.x - shift >= 0 ? maps.box.x - shift : maps.box.x + shift;
		for (int k = 0; k < 3; k++) {
			for (size_t i = 0; i < probe.alpha[k].v.size(); i++) {
				const bool core = maps.alpha[k].v[i] >= 0.5f;
				probe.alpha[k].v[i] = core ? 1.0f : 0.0f;
				probe.laid[k].v[i] = 0;
			}
		}
		for (float texture : { 0.0f, 0.5f, 1.0f }) {
		Logo::RemoveOptions po = ro;
		po.texture = texture;
		double bias = 0, se = 0, hfOut = 0, hfIn = 0;
		int n = 0;
		for (const Frame& f : seen) {
			Frame o = f;
			RemoveLuma(probe, 1.0f, po, o.p[0]);
			for (int y = 1; y < probe.box.h - 1; y++) {
				for (int x = 1; x < probe.box.w - 1; x++) {
					if (probe.alpha[0].at(x, y) < 1) {
						continue;
					}
					const int X = probe.box.x + x, Y = probe.box.y + y;
					const double d = (o.p[0].at(X, Y) - f.p[0].at(X, Y)) * 255;
					bias += d;
					se += d * d;
					n++;
					auto lap = [&](const Plane& p) {
						return std::abs(4 * p.at(X, Y) - p.at(X - 1, Y) - p.at(X + 1, Y) - p.at(X, Y - 1) - p.at(X, Y + 1));
					};
					hfOut += lap(o.p[0]);
					hfIn += lap(f.p[0]);
				}
			}
		}
		Out("  %-34s %7.2f %7.2f %7.3f  (8-bit levels; texture: filled against real)\n",
			std::format("fill where no logo, texture {:.1f}", texture).c_str(),
			n ? bias / n : 0.0, n ? std::sqrt(se / n) : 0.0, hfIn > 0 ? hfOut / hfIn : 0.0);
		}
		// And by the logo, where what the fill reads from is the logo's surroundings: the
		// mean of what the opaque parts became against the mean round them.
		double inside = 0, round = 0;
		int ni = 0, nr = 0;
		std::vector<uint8_t> core(maps.alpha[0].v.size());
		for (size_t i = 0; i < core.size(); i++) {
			core[i] = maps.alpha[0].v[i] >= 0.9f;
		}
		for (const Frame& f : seen) {
			Frame o = f;
			const float s = Logo::Presence(maps, f.p[0], ro, nullptr);
			RemoveLuma(maps, s, ro, o.p[0]);
			for (int y = 0; y < maps.box.h; y++) {
				for (int x = 0; x < maps.box.w; x++) {
					const float a = maps.alpha[0].at(x, y);
					const int X = maps.box.x + x, Y = maps.box.y + y;
					if (core[(size_t)y * maps.box.w + x]) {
						inside += o.p[0].at(X, Y) * 255;
						ni++;
					} else if (std::abs(a) < maps.alphaMin && x >= 4 && y >= 4 && x < maps.box.w - 4 && y < maps.box.h - 4) {
						round += o.p[0].at(X, Y) * 255;
						nr++;
					}
				}
			}
		}
		Out("  %-34s %7.2f\n", "by the logo: inside less round it", (ni && nr) ? inside / ni - round / nr : 0.0);
		// The chroma the same way: what the opaque parts became against the picture round
		// them, Cb and Cr, and how far from grey each is.
		{
			const Plane& Ac = maps.alpha[1];
			const int cx0 = maps.box.x / 2, cy0 = maps.box.y / 2;
			double in[2] = {}, rd[2] = {}, satIn = 0, satRd = 0, seenIn[2] = {};
			int nIn = 0, nRd = 0;
			for (const Frame& f : seen) {
				Frame o = f;
				const float s = Logo::Presence(maps, f.p[0], ro, nullptr);
				const Logo::Bars bars = Logo::FindBars(maps, o.p[0], ro);
				for (int k = 1; k < 3; k++) {
					Logo::Remove(maps, k, s, ro, bars, o.p[k]);
				}
				for (int y = 2; y < Ac.h - 2; y++) {
					for (int x = 2; x < Ac.w - 2; x++) {
						const float a = Ac.at(x, y);
						const float cb = o.p[1].at(cx0 + x, cy0 + y) * 255 - 128, cr = o.p[2].at(cx0 + x, cy0 + y) * 255 - 128;
						if (a >= 0.9f) {
							in[0] += cb; in[1] += cr; satIn += std::sqrt(cb * cb + cr * cr); nIn++;
							seenIn[0] += f.p[1].at(cx0 + x, cy0 + y) * 255 - 128;
							seenIn[1] += f.p[2].at(cx0 + x, cy0 + y) * 255 - 128;
						} else if (std::abs(a) < maps.alphaMin) {
							rd[0] += cb; rd[1] += cr; satRd += std::sqrt(cb * cb + cr * cr); nRd++;
						}
					}
				}
			}
			if (nIn && nRd) {
				Out("  %-34s Cb %6.2f / %6.2f  Cr %6.2f / %6.2f  colour %5.2f / %5.2f  (seen Cb %.2f Cr %.2f)\n",
					"chroma inside / round it", in[0] / nIn, rd[0] / nRd, in[1] / nIn, rd[1] / nRd, satIn / nIn, satRd / nRd,
					seenIn[0] / nIn, seenIn[1] / nIn);
			}
		}
	}

	Out("  %-34s %6s %8s %8s\n", "test picture", "on", "outline", "texture");
	removeAll(ro, &out, true);
	Out("  %-34s %6s %8s %8s\n", "mean over the test pictures", "", "outline", "texture");
	for (int h = Logo::kMarginLeast; h <= Logo::kMarginMost; h++) {
		Logo::RemoveOptions o = ro;
		o.halo = h;
		const Left l = removeAll(o, nullptr, false);
		Out("  %-34s %6s %8.3f %8.3f\n", std::format("margin {}", h).c_str(), "", l.outline, l.texture);
	}
	for (float t : { 0.0f, 0.5f, 1.0f }) {
		Logo::RemoveOptions o = ro;
		o.texture = t;
		const Left l = removeAll(o, nullptr, false);
		Out("  %-34s %6s %8.3f %8.3f\n", std::format("detail {:.1f}", t).c_str(), "", l.outline, l.texture);
	}
	// What each margin leaves round the letters: the luma restored by its distance from
	// them, less that 6 to 8 pixels away, in 8-bit levels (0: no rim, darker or lighter).
	{
		const Plane& A = maps.alpha[0];
		std::vector<uint8_t> core(A.v.size());
		for (size_t i = 0; i < core.size(); i++) {
			core[i] = A.v[i] >= 0.5f;
		}
		std::vector<int> dist(core.size(), 99);
		for (int y = 0; y < A.h; y++) {
			for (int x = 0; x < A.w; x++) {
				for (int dy = -8; dy <= 8; dy++) {
					for (int dx = -8; dx <= 8; dx++) {
						const int xx = x + dx, yy = y + dy;
						if (xx >= 0 && yy >= 0 && xx < A.w && yy < A.h && core[(size_t)yy * A.w + xx]) {
							int& d = dist[(size_t)y * A.w + x];
							d = std::min(d, std::max(std::abs(dx), std::abs(dy)));
						}
					}
				}
			}
		}
		Out("  %-34s %6s %6s %6s %6s %6s %6s\n", "rim by distance (levels)", "in", "1", "2", "3", "4", "5");
		for (int mgn = -1; mgn <= Logo::kMarginMost; mgn++) {
			double sum[7] = {}, ref = 0;
			int cnt[7] = {}, nref = 0;
			for (const Frame& f : seen) {
				Frame t = f;
				if (mgn >= 0) {
					Logo::RemoveOptions mo = ro;
					mo.halo = mgn;
					const float s = Logo::Presence(maps, f.p[0], mo, nullptr);
					RemoveLuma(maps, s, mo, t.p[0]);
				}
				double fr = 0;
				int fn = 0;
				double fs[7] = {};
				int fc[7] = {};
				for (int y = 0; y < A.h; y++) {
					for (int x = 0; x < A.w; x++) {
						const int d = dist[(size_t)y * A.w + x];
						const float v = t.p[0].at(maps.box.x + x, maps.box.y + y) * 255;
						if (d >= 6 && d <= 8) {
							fr += v;
							fn++;
						} else if (d <= 5) {
							fs[d] += v;
							fc[d]++;
						}
					}
				}
				if (!fn) {
					continue;
				}
				for (int d = 0; d <= 5; d++) {
					if (fc[d]) {
						sum[d] += fs[d] / fc[d] - fr / fn;
						cnt[d]++;
					}
				}
			}
			std::string line = std::format("  {:<34}", mgn < 0 ? std::string("seen, logo on") : std::format("margin {}", mgn));
			for (int d = 0; d <= 5; d++) {
				line += std::format(" {:6.2f}", cnt[d] ? sum[d] / cnt[d] : 0.0);
			}
			Out("%s\n", line.c_str());
		}
	}
	for (float k : { 3.0f, 10.0f, 30.0f, 100.0f }) {
		Logo::RemoveOptions o = ro;
		o.doubt = k;
		const Left l = removeAll(o, nullptr, false);
		Out("  %-34s %6s %8.3f %8.3f\n", std::format("doubt x {:.1f}", k).c_str(), "", l.outline, l.texture);
	}
	for (int fill : { 0, 2 }) {
		Logo::RemoveOptions o = ro;
		o.fill = fill;
		const Left l = removeAll(o, nullptr, false);
		Out("  %-34s %6s %8.3f %8.3f\n", fill ? "fill: push-pull 2x2" : "fill: harmonic", "", l.outline, l.texture);
	}
	fr.Close();

	// Pictures: the box and its alpha; each test picture before and after.
	CComPtr<IWICImagingFactory> factory;
	if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
		const Logo::Rect& b = maps.box;
		const Logo::Rect around = { std::max(0, b.x - 16) & ~1, std::max(0, b.y - 16) & ~1,
			std::min(W - (std::max(0, b.x - 16) & ~1), b.w + 32), std::min(H - (std::max(0, b.y - 16) & ~1), b.h + 32) };
		const int zoom = 2;
		const int tw = around.w * zoom, th = around.h * zoom;
		{
			const int CW = tw * 2 + 8, CH = th;
			std::vector<float> canvas(4 * (size_t)CW * CH, 0);
			Blit(canvas, CW, seen[0], around, zoom, 0, 0);
			for (int y = 0; y < b.h * zoom; y++) {
				for (int x = 0; x < b.w * zoom; x++) {
					const float a = maps.alpha[0].at(x / zoom, y / zoom);
					float* o = &canvas[4 * ((size_t)(y + (b.y - around.y) * zoom) * CW + tw + 8 + x + (b.x - around.x) * zoom)];
					o[0] = o[1] = o[2] = a;
					o[3] = 1;
				}
			}
			SavePng(factory, L"logo_found.png", canvas, CW, Rect{ 0, 0, CW, CH }, 1.0f);
		}
		{
			const int rows = (int)seen.size();
			const int CW = tw * 2 + 8, CH = rows * (th + 8);
			std::vector<float> canvas(4 * (size_t)CW * CH, 0);
			for (int r = 0; r < rows; r++) {
				Blit(canvas, CW, seen[r], around, zoom, 0, r * (th + 8));
				Blit(canvas, CW, out[r], around, zoom, tw + 8, r * (th + 8));
			}
			SavePng(factory, L"logo_pairs.png", canvas, CW, Rect{ 0, 0, CW, CH }, 1.0f);
		}
		{
			// Before, then each margin, on the darker test pictures, where a rim shows most.
			std::vector<size_t> rowsPicked;
			for (size_t r = 0; r < seen.size() && rowsPicked.size() < 4; r++) {
				double mean = 0;
				for (int y = 0; y < b.h; y++) {
					for (int x = 0; x < b.w; x++) {
						mean += seen[r].p[0].at(b.x + x, b.y + y);
					}
				}
				if (mean / ((double)b.w * b.h) < 0.35) {
					rowsPicked.push_back(r);
				}
			}
			const int z = 3, mw = around.w * z, mh = around.h * z;
			const int cols = 1 + Logo::kMarginMost - Logo::kMarginLeast + 1;
			const int CW = cols * (mw + 8), CH = (int)rowsPicked.size() * (mh + 8);
			if (CH > 0) {
				std::vector<float> canvas(4 * (size_t)CW * CH, 0);
				for (size_t r = 0; r < rowsPicked.size(); r++) {
					const Frame& f = seen[rowsPicked[r]];
					Blit(canvas, CW, f, around, z, 0, (int)r * (mh + 8));
					for (int mgn = Logo::kMarginLeast; mgn <= Logo::kMarginMost; mgn++) {
						Logo::RemoveOptions mo = ro;
						mo.halo = mgn;
						Frame t = f;
						const float s = Logo::Presence(maps, f.p[0], mo, nullptr);
						RemoveFrame(maps, s, mo, t.p);
						Blit(canvas, CW, t, around, z, (1 + mgn - Logo::kMarginLeast) * (mw + 8), (int)r * (mh + 8));
					}
				}
				SavePng(factory, L"logo_margins.png", canvas, CW, Rect{ 0, 0, CW, CH }, 1.0f);
			}
		}
		{
			// Before, smooth fill, fill with the texture mirrored in.
			Logo::RemoveOptions to = ro;
			to.texture = 0.5f;
			const int rows = (int)seen.size();
			const int CW = tw * 3 + 16, CH = rows * (th + 8);
			std::vector<float> canvas(4 * (size_t)CW * CH, 0);
			for (int r = 0; r < rows; r++) {
				Frame t = seen[r];
				const float s = Logo::Presence(maps, seen[r].p[0], to, nullptr);
				RemoveFrame(maps, s, to, t.p);
				Blit(canvas, CW, seen[r], around, zoom, 0, r * (th + 8));
				Blit(canvas, CW, out[r], around, zoom, tw + 8, r * (th + 8));
				Blit(canvas, CW, t, around, zoom, 2 * tw + 16, r * (th + 8));
			}
			SavePng(factory, L"logo_texture.png", canvas, CW, Rect{ 0, 0, CW, CH }, 1.0f);
		}
		Out("  pictures: logo_found.png (the box and its alpha), logo_pairs.png (before, after)\n");
	}
	return g_failures ? 1 : 0;
}

static int Run(ID3D11Device* dev, ID3D11DeviceContext* ctx, int mode, const wchar_t* film)
{
	Head(mode ? "Hiding a channel's logo: the comparison it was chosen on" : "Hiding a channel's logo");

	if (mode == 2) {
		return RunReal(film);
	}
	const bool study = mode == 1;
	const std::wstring path = film && *film ? film : L"upscale_refs\\Fight Club (1999).mkv";
	const double start = 600;
	const int every = 10;
	const int nLocate = study ? 90 : 60, nFit = study ? 240 : 120, nTest = 24;
	const int checkLocate[] = { 10, 20, 30, 45, 60, 90 };
	const int checkFit[] = { 15, 30, 60, 120, 240 };

	CFrames frames;
	if (!frames.Open(path, start, every)) {
		Check(false, "the film through ffmpeg");
		return 1;
	}
	const int W = frames.Width(), H = frames.Height();
	frames.Close();
	const std::vector<LogoCompose::Design> designs = LogoCompose::Designs(W, frames.Top());

	Logo::CLocator::Options lo;
	Logo::CDeblender::Options fo;
	Logo::RemoveOptions ro;

	for (int x264 = 0; x264 < 2; x264++)
	for (const LogoCompose::Design& d : designs) {
		// Through x264 for every logo in the study; for the battery, three.
		if (x264 && !study && strcmp(d.name, "translucent") && strcmp(d.name, "channel") && strcmp(d.name, "bar")) {
			continue;
		}
		// MPCVR_LOGO_ONLY=<name>[,x264]: one logo, one way, while a change is looked at.
		if (const char* only = getenv("MPCVR_LOGO_ONLY")) {
			const std::string o(only);
			if (o.substr(0, o.find(',')) != d.name || (o.find(",x264") != std::string::npos) != (x264 != 0)) {
				continue;
			}
		}
		const std::string tag = std::format("{}{}", d.name, x264 ? " through x264" : "");
		Out("\n  %s, %dx%d logo box\n", tag.c_str(), d.truth.box.w, d.truth.box.h);
		CSource fr;
		if (!fr.Open(path, start, every, d, nLocate, nFit, nTest, x264 != 0)) {
			Check(false, "the film through ffmpeg");
			return 1;
		}

		// Finding it.
		Logo::CLocator loc;
		loc.Reset(W, H, 1, lo.edge);
		Logo::CLocator::Result found;
		Frame f;
		Out("  %-34s %6s %6s\n", "pictures looked at", "found", "box");
		for (int i = 0; i < nLocate && fr.Next(f, nullptr); i++) {
			loc.Add(f.p[0]);
			for (int c : checkLocate) {
				if (c == i + 1 && (study || c == nLocate)) {
					const Logo::CLocator::Result r = loc.Locate(lo);
					const FitScore fs = ScoreFit(Logo::Maps(), d.truth, r.box);
					Out("  %-34s %6.3f %6.2f%s\n", std::format("{}", c).c_str(), r.found ? fs.found : 0.0, r.found ? fs.boxRatio : 0.0,
						r.still ? "  (still)" : r.found ? "" : "  (none)");
					if (c == nLocate) {
						found = r;
					}
				}
			}
		}
		Check(found.found, std::format("{}: the logo found", tag).c_str());
		const Logo::Rect box = found.found ? found.box : Logo::Rect{ d.truth.box.x - 12, d.truth.box.y - 12, d.truth.box.w + 24, d.truth.box.h + 24 };
		if (found.found) {
			const FitScore fs = ScoreFit(Logo::Maps(), d.truth, box);
			Check(fs.found >= 0.999, std::format("{}: the box holds the whole logo", tag).c_str());
		}

		// Fitting it, on the pictures after.
		Logo::CDeblender fit;
		fit.Reset(box, found.picture, 2, 2);
		Logo::Maps maps;
		Out("  %-34s %6s %6s %6s %7s %7s %4s\n", "pictures fitted on", "found", "box", "IoU", "alpha", "colour", "out");
		for (int i = 0; i < nFit && fr.Next(f, nullptr); i++) {
			fit.Add(CropOf(f, box));
			for (int c : checkFit) {
				if (c == i + 1 && (study || c == nFit)) {
					Logo::Maps m;
					fit.Fit(fo, m);
					PrintFit(std::format("{}", c).c_str(), ScoreFit(m, d.truth, box), fit.Dropped());
					if (c == nFit) {
						maps = m;
					}
				}
			}
		}
		PrintColours(maps, found.picture);
		PrintFollows(fit);
		Out("  %-34s top %d, bottom %d, left %d, right %d\n", "rows blended with a bar", fit.Blend().top, fit.Blend().bottom,
			fit.Blend().left, fit.Blend().right);
		if (study) {
			// The last: a logo across a bar taken as opaque over it, its colour not used.
			struct Variant { const char* name; Logo::Background bg; int passes; bool chroma; bool colour = true; };
			const Variant variants[] = {
				{ "ring mean, 1 pass", Logo::BackgroundFlat, 1, true },
				{ "ring plane, 1 pass", Logo::BackgroundPlane, 1, true },
				{ "harmonic, 1 pass", Logo::BackgroundHarmonic, 1, true },
				{ "harmonic, 2 passes", Logo::BackgroundHarmonic, 2, true },
				{ "harmonic, 3 passes", Logo::BackgroundHarmonic, 3, true },
				{ "harmonic, 3, chroma alpha own", Logo::BackgroundHarmonic, 3, false },
				{ "harmonic, 3, opaque over a bar", Logo::BackgroundHarmonic, 3, true, false },
			};
			Out("  %-34s %6s %6s %6s %7s %7s %4s\n", "background, at 240", "found", "box", "IoU", "alpha", "colour", "out");
			for (const Variant& v : variants) {
				Logo::CDeblender::Options o = fo;
				o.background = v.bg;
				o.passes = v.passes;
				o.chromaFromLuma = v.chroma;
				if (!v.colour) {
					o.oneColour = 0;
				}
				Logo::Maps m;
				fit.Fit(o, m);
				PrintFit(v.name, ScoreFit(m, d.truth, box), fit.Dropped());
			}
		}

		// Taking it out of the pictures after those.
		std::vector<Frame> clean, seen;
		std::vector<float> strength;
		Frame t;
		for (int i = 0; i < nTest && fr.Next(f, &t); i++) {
			clean.push_back(t);
			seen.push_back(f);
			strength.push_back(TestStrength(i, nTest));
		}
		fr.Close();
		if (clean.empty() || !maps.valid()) {
			Check(false, std::format("{}: pictures to test on", tag).c_str());
			continue;
		}

		// The presence measured on each test picture.
		{
			std::string line = "  presence (laid / measured)       ";
			double worstAbsent = 0, worstPresent = 0;
			for (size_t i = 0; i < seen.size(); i++) {
				float conf = 0;
				const float s = Logo::Presence(maps, seen[i].p[0], ro, &conf);
				line += std::format(" {:.2f}/{:.2f}", strength[i], s);
				if (strength[i] == 0) {
					worstAbsent = std::max(worstAbsent, (double)s);
				} else if (strength[i] == 1) {
					worstPresent = std::max(worstPresent, 1.0 - s);
				}
			}
			Out("%s\n", line.c_str());
			Out("  %-34s %6.3f %6.3f\n", "presence: most when absent, least", worstAbsent, 1 - worstPresent);
		}

		auto removeAll = [&](const Logo::Maps& m, const Logo::RemoveOptions& o, bool trueStrength) {
			RemoveScore sum, absent;
			for (size_t i = 0; i < seen.size(); i++) {
				Frame out = seen[i];
				float s = strength[i];
				if (!trueStrength) {
					s = Logo::Presence(m, seen[i].p[0], o, nullptr);
				}
				RemoveFrame(m, s, o, out.p);
				const RemoveScore r = ScoreRemoval(out, seen[i], clean[i], d.truth);
				if (strength[i] > 0) {
					sum.Add(r);
				} else {
					absent.Add(r);
				}
			}
			return std::make_pair(sum.Mean(), absent.Mean());
		};

		Out("  %-34s %6s %6s %6s %6s %6s %6s\n", "taken out", "psnr", "opaque", "transl", "chroma", "ghost", "texture");
		{
			RemoveScore untouched;
			for (size_t i = 0; i < seen.size(); i++) {
				if (strength[i] > 0) {
					untouched.Add(ScoreRemoval(seen[i], seen[i], clean[i], d.truth));
				}
			}
			PrintRemove("left on screen", untouched.Mean());
		}
		const auto shipped = removeAll(maps, ro, false);
		PrintRemove("as it ships", shipped.first);
		PrintRemove("as it ships, pictures without it", shipped.second);
		Check(shipped.first.psnr > 0, std::format("{}: the logo taken out", tag).c_str());
		PortCheck(dev, ctx, seen, strength, maps, ro);
		if (study) {
			PrintRemove("its fit, the true strength", removeAll(maps, ro, true).first);
			PrintRemove("the true logo, the true strength", removeAll(d.truth, ro, true).first);
			struct Fill { const char* name; int fill, smooth; };
			const Fill fills[] = {
				{ "harmonic (reference)", 0, 0 },
				{ "push-pull 2x2", 2, 0 },
				{ "push-pull [1 3 3 1]", 1, 0 },
				{ "push-pull [1 3 3 1] + 8 Jacobi", 1, 8 },
				{ "push-pull [1 3 3 1] + 32 Jacobi", 1, 32 },
			};
			for (const Fill& fl : fills) {
				Logo::RemoveOptions o = ro;
				o.fill = fl.fill;
				o.smooth = fl.smooth;
				PrintRemove(std::format("fill: {}", fl.name).c_str(), removeAll(maps, o, true).first);
			}
			for (float k : { 3.0f, 10.0f, 30.0f, 100.0f }) {
				Logo::RemoveOptions o = ro;
				o.doubt = k;
				PrintRemove(std::format("doubt x {:.1f}", k).c_str(), removeAll(maps, o, true).first);
			}
			for (float t : { 0.0f, 0.5f, 1.0f }) {
				Logo::RemoveOptions o = ro;
				o.texture = t;
				PrintRemove(std::format("detail {:.1f}", t).c_str(), removeAll(maps, o, true).first);
			}
			for (int h = Logo::kMarginLeast; h <= Logo::kMarginMost; h++) {
				Logo::RemoveOptions o = ro;
				o.halo = h;
				PrintRemove(std::format("margin {}", h).c_str(), removeAll(maps, o, true).first);
			}
			// A bar: how dark its rows are taken to be (0: no bar), with the strength as
			// measured, as it ships; then on the pictures without the logo.
			for (float dark : { 0.0f, 20.0f, 25.5f }) {
				Logo::RemoveOptions o = ro;
				o.barDark = dark / 255;
				const auto r = removeAll(maps, o, false);
				PrintRemove(std::format("bar under {:.1f} levels", dark).c_str(), r.first);
				PrintRemove(std::format("bar under {:.1f}, without it", dark).c_str(), r.second);
			}
		}
	}
	return g_failures ? 1 : 0;
}

} // namespace logo

static int RunLogo(ID3D11Device* dev, ID3D11DeviceContext* ctx, int mode, const wchar_t* film)
{
	g_report = _wfopen(mode == 2 ? L"temporal_results_logo_real.txt" : mode == 1 ? L"temporal_results_logo_study.txt" : L"temporal_results_logo.txt", L"w");
	const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	const int rc = logo::Run(dev, ctx, mode, film);
	if (SUCCEEDED(coInit)) {
		CoUninitialize();
	}
	if (g_report) {
		fclose(g_report);
		g_report = nullptr;
	}
	return rc;
}

} // namespace temporal
