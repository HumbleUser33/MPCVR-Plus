// Logos of known alpha and colour, drawn and laid over a picture's planes the way a
// broadcaster lays its own: the truth the logo hiding is measured against. Shared by the
// harness (logo_suite.inl) and playback_test, so both lay the same logos.
//
// A logo is drawn by GDI at four times its size and reduced, which gives its edges the
// partial alpha a real one has, then laid over the picture in Y'CbCr at 4:4:4 and the
// chroma brought back to 4:2:0: so the chroma of the result is not exactly the model's,
// as on a real channel.

#include "Logo/LogoAnalysis.h"
#include "Logo/LogoParams.h"

#pragma comment(lib, "gdi32.lib")

namespace LogoCompose {

// BT.709 limited range, normalized.
inline void ToYCbCr(float r, float g, float b, float out[3])
{
	const float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
	out[0] = (16 + 219 * y) / 255.0f;
	out[1] = (128 + 224 * (b - y) / 1.8556f) / 255.0f;
	out[2] = (128 + 224 * (r - y) / 1.5748f) / 255.0f;
}

struct Layer {
	std::vector<float> a;     // coverage at 4x
	float c[3] = {};          // Y'CbCr
	float opacity = 1;
};

// A layer drawn by GDI: draw() paints white on black into a DC of W4 x H4.
template <class Draw>
static Layer DrawLayer(int W4, int H4, Draw draw)
{
	Layer l;
	BITMAPINFO bi = {};
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = W4;
	bi.bmiHeader.biHeight = -H4;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	void* bits = nullptr;
	HDC dc = CreateCompatibleDC(nullptr);
	HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
	HGDIOBJ old = SelectObject(dc, bmp);
	RECT all = { 0, 0, W4, H4 };
	FillRect(dc, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, RGB(255, 255, 255));
	draw(dc);
	GdiFlush();
	l.a.resize((size_t)W4 * H4);
	const BYTE* p = (const BYTE*)bits;
	for (size_t i = 0; i < l.a.size(); i++) {
		l.a[i] = p[4 * i + 1] / 255.0f;
	}
	SelectObject(dc, old);
	DeleteObject(bmp);
	DeleteDC(dc);
	return l;
}

// angle: in tenths of a degree, 900 to read from the bottom up.
static void TextLayer(HDC dc, const wchar_t* text, int x, int y, int height, int weight, const wchar_t* face, int angle = 0)
{
	HFONT f = CreateFontW(-height, 0, angle, angle, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
		NONANTIALIASED_QUALITY, DEFAULT_PITCH, face);
	HGDIOBJ o = SelectObject(dc, f);
	TextOutW(dc, x, y, text, (int)wcslen(text));
	SelectObject(dc, o);
	DeleteObject(f);
}

// Coverage grown by r pixels (a square): an outline under a text.
static void Grow(std::vector<float>& a, int W, int H, int r)
{
	std::vector<float> t(a.size());
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			float m = 0;
			for (int dx = -r; dx <= r; dx++) {
				const int xx = x + dx;
				if (xx >= 0 && xx < W) {
					m = std::max(m, a[(size_t)y * W + xx]);
				}
			}
			t[(size_t)y * W + x] = m;
		}
	}
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			float m = 0;
			for (int dy = -r; dy <= r; dy++) {
				const int yy = y + dy;
				if (yy >= 0 && yy < H) {
					m = std::max(m, t[(size_t)yy * W + x]);
				}
			}
			a[(size_t)y * W + x] = m;
		}
	}
}

struct Design {
	const char* name;
	Logo::Maps truth;     // alpha and colour of every plane, in its box of the picture
};

// The layers over one another, reduced by four, as alpha and colour at full size in the
// picture at (x, y), and the chroma's as the 4:4:4 laying gives it on average.
static Design Flatten(const char* name, const std::vector<Layer>& layers, int W4, int H4, int x, int y)
{
	const int W = W4 / 4, H = H4 / 4;
	std::vector<float> A((size_t)W4 * H4, 0), P[3];
	for (int k = 0; k < 3; k++) {
		P[k].assign((size_t)W4 * H4, 0);   // premultiplied colour
	}
	for (const Layer& l : layers) {
		for (size_t i = 0; i < A.size(); i++) {
			const float a = l.a[i] * l.opacity;
			for (int k = 0; k < 3; k++) {
				P[k][i] = a * l.c[k] + (1 - a) * P[k][i];
			}
			A[i] = a + (1 - a) * A[i];
		}
	}
	Design d;
	d.name = name;
	Logo::Maps& m = d.truth;
	m.box = { x & ~1, y & ~1, (W + 1) & ~1, (H + 1) & ~1 };
	m.divW = m.divH = 2;
	m.alphaMin = 0.02f;
	m.alpha[0].resize(m.box.w, m.box.h);
	m.laid[0].resize(m.box.w, m.box.h);
	// The truth is laid exactly: no excess, so its inversion is not doubted.
	std::vector<float> pa((size_t)m.box.w * m.box.h, 0), pc[3];
	for (int k = 0; k < 3; k++) {
		pc[k].assign(pa.size(), 0);
	}
	for (int yy = 0; yy < H; yy++) {
		for (int xx = 0; xx < W; xx++) {
			double a = 0, c[3] = {};
			for (int dy = 0; dy < 4; dy++) {
				for (int dx = 0; dx < 4; dx++) {
					const size_t i = (size_t)(4 * yy + dy) * W4 + 4 * xx + dx;
					a += A[i];
					for (int k = 0; k < 3; k++) {
						c[k] += P[k][i];
					}
				}
			}
			const size_t j = (size_t)yy * m.box.w + xx;
			pa[j] = (float)(a / 16);
			for (int k = 0; k < 3; k++) {
				pc[k][j] = (float)(c[k] / 16);
			}
		}
	}
	for (size_t j = 0; j < pa.size(); j++) {
		m.alpha[0].v[j] = pa[j];
		m.laid[0].v[j] = pc[0][j];
	}
	for (int k = 1; k < 3; k++) {
		m.alpha[k].resize(m.box.w / 2, m.box.h / 2);
		m.laid[k].resize(m.box.w / 2, m.box.h / 2);
		for (int cy = 0; cy < m.box.h / 2; cy++) {
			for (int cx = 0; cx < m.box.w / 2; cx++) {
				double a = 0, c = 0;
				for (int dy = 0; dy < 2; dy++) {
					for (int dx = 0; dx < 2; dx++) {
						const size_t j = (size_t)(2 * cy + dy) * m.box.w + 2 * cx + dx;
						a += pa[j];
						c += pc[k][j];
					}
				}
				m.alpha[k].at(cx, cy) = (float)(a / 4);
				m.laid[k].at(cx, cy) = (float)(c / 4);
			}
		}
	}
	return d;
}

// The six logos, for a picture of W x H whose picture area starts at row top.
static std::vector<Design> Designs(int W, int top)
{
	std::vector<Design> out;
	float white[3], black[3], red[3], orange[3], blue[3], grey[3];
	ToYCbCr(1, 1, 1, white);
	ToYCbCr(0, 0, 0, black);
	ToYCbCr(0.90f, 0.22f, 0.21f, red);
	ToYCbCr(0.97f, 0.64f, 0.11f, orange);
	ToYCbCr(0.11f, 0.65f, 0.84f, blue);
	ToYCbCr(0.85f, 0.85f, 0.88f, grey);
	auto set = [](Layer& l, const float c[3], float opacity) {
		for (int k = 0; k < 3; k++) {
			l.c[k] = c[k];
		}
		l.opacity = opacity;
	};
	const int y0 = top + 28;

	{   // A translucent white word with holes in its letters.
		const int W4 = 4 * 200, H4 = 4 * 56;
		Layer t = DrawLayer(W4, H4, [&](HDC dc) { TextLayer(dc, L"TV8 b", 16, 0, 4 * 50, FW_BLACK, L"Arial"); });
		set(t, white, 0.55f);
		out.push_back(Flatten("translucent", { t }, W4, H4, W - 60 - 200, y0));
	}
	{   // Opaque white letters with a dark rim.
		const int W4 = 4 * 170, H4 = 4 * 60;
		Layer t = DrawLayer(W4, H4, [&](HDC dc) { TextLayer(dc, L"W9 a", 24, 8, 4 * 46, FW_BOLD, L"Arial"); });
		Layer rim = t;
		Grow(rim.a, W4, H4, 8);
		set(rim, black, 1.0f);
		set(t, white, 1.0f);
		out.push_back(Flatten("outlined", { rim, t }, W4, H4, 60, y0));
	}
	{   // A badge whose alpha runs from 0.25 to 0.75, its text cut out of it.
		const int W4 = 4 * 140, H4 = 4 * 56;
		Layer badge = DrawLayer(W4, H4, [&](HDC dc) {
			SelectObject(dc, GetStockObject(WHITE_BRUSH));
			SelectObject(dc, GetStockObject(WHITE_PEN));
			RoundRect(dc, 8, 8, W4 - 8, H4 - 8, 80, 80);
			SetTextColor(dc, RGB(0, 0, 0));
			TextLayer(dc, L"news", 60, 24, 4 * 36, FW_BOLD, L"Arial");
		});
		for (int y = 0; y < H4; y++) {
			for (int x = 0; x < W4; x++) {
				badge.a[(size_t)y * W4 + x] *= 0.25f + 0.5f * x / (W4 - 1);
			}
		}
		set(badge, grey, 1.0f);
		out.push_back(Flatten("gradient", { badge }, W4, H4, W - 60 - 140, y0 + 520));
	}
	{   // A small thin "HD".
		const int W4 = 4 * 40, H4 = 4 * 22;
		Layer t = DrawLayer(W4, H4, [&](HDC dc) { TextLayer(dc, L"HD", 4, 4, 4 * 18, FW_NORMAL, L"Arial"); });
		set(t, white, 1.0f);
		out.push_back(Flatten("small", { t }, W4, H4, W - 60 - 40, y0 + 4));
	}
	{   // Opaque white letters, a red dot and two coloured letters: a channel's name.
		const int W4 = 4 * 280, H4 = 4 * 64;
		Layer word = DrawLayer(W4, H4, [&](HDC dc) { TextLayer(dc, L"channel", 8, 0, 4 * 48, FW_SEMIBOLD, L"Segoe UI"); });
		Layer dot = DrawLayer(W4, H4, [&](HDC dc) {
			SelectObject(dc, GetStockObject(WHITE_BRUSH));
			SelectObject(dc, GetStockObject(WHITE_PEN));
			Ellipse(dc, 4 * 182, 4 * 28, 4 * 196, 4 * 42);
		});
		Layer t = DrawLayer(W4, H4, [&](HDC dc) { TextLayer(dc, L"t", 4 * 200, 0, 4 * 48, FW_SEMIBOLD, L"Segoe UI"); });
		Layer v = DrawLayer(W4, H4, [&](HDC dc) { TextLayer(dc, L"v", 4 * 216, 0, 4 * 48, FW_SEMIBOLD, L"Segoe UI"); });
		set(word, white, 1.0f);
		set(dot, red, 1.0f);
		set(t, orange, 1.0f);
		set(v, blue, 1.0f);
		out.push_back(Flatten("channel", { word, dot, t, v }, W4, H4, W - 60 - 280, y0 + 250));
	}
	{   // Translucent white letters read from the bottom up, half over the black bar above
		// the picture and half over the picture, as a channel lays its logo over a wide film.
		const int W4 = 4 * 56, H4 = 4 * 170;
		Layer t = DrawLayer(W4, H4, [&](HDC dc) { TextLayer(dc, L"kino", 16, H4 - 16, 4 * 44, FW_BLACK, L"Arial", 900); });
		set(t, white, 0.4f);
		out.push_back(Flatten("bar", { t }, W4, H4, 300, top - 120));
	}
	return out;
}

// The logo laid over a picture's planes at strength s (s = 0: not on screen). Luma by
// the model; chroma at 4:4:4 over the picture's chroma brought up bilinearly, then back
// to 4:2:0, only the change of the logo's own pixels kept.
static void Lay(const Design& d, float s, Logo::Plane p[3])
{
	if (s <= 0) {
		return;
	}
	const Logo::Maps& m = d.truth;
	const Logo::Rect& b = m.box;
	for (int y = 0; y < b.h; y++) {
		for (int x = 0; x < b.w; x++) {
			const float a = s * m.alpha[0].at(x, y);
			float& v = p[0].at(b.x + x, b.y + y);
			v = (1 - a) * v + s * m.laid[0].at(x, y);
		}
	}
	const Logo::Plane& A = m.alpha[0];
	for (int k = 1; k < 3; k++) {
		Logo::Plane& c = p[k];
		const int cx0 = b.x / 2, cy0 = b.y / 2;
		for (int cy = 0; cy < b.h / 2; cy++) {
			for (int cx = 0; cx < b.w / 2; cx++) {
				// What the logo lays, less what it covers of the picture's chroma brought up.
				double change = 4.0 * s * m.laid[k].at(cx, cy);
				for (int dy = 0; dy < 2; dy++) {
					for (int dx = 0; dx < 2; dx++) {
						const int lx = 2 * cx + dx, ly = 2 * cy + dy;
						// The picture's chroma at this luma pixel, bilinear (centre-sited).
						const float fx = std::clamp((b.x + lx + 0.5f) / 2 - 0.5f, 0.0f, (float)(c.w - 1));
						const float fy = std::clamp((b.y + ly + 0.5f) / 2 - 0.5f, 0.0f, (float)(c.h - 1));
						const int x0 = (int)fx, y0 = (int)fy, x1 = std::min(x0 + 1, c.w - 1), y1 = std::min(y0 + 1, c.h - 1);
						const float ax = fx - x0, ay = fy - y0;
						const float under = (1 - ay) * ((1 - ax) * c.at(x0, y0) + ax * c.at(x1, y0)) + ay * ((1 - ax) * c.at(x0, y1) + ax * c.at(x1, y1));
						change -= s * A.at(lx, ly) * under;
					}
				}
				float& v = c.at(cx0 + cx, cy0 + cy);
				v = std::clamp(v + (float)(change / 4), 0.0f, 1.0f);
			}
		}
	}
}

} // namespace LogoCompose
