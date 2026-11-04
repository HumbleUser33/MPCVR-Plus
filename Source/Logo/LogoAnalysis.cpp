/*
 * (C) 2026 see Authors.txt
 *
 * This file is part of MPC-BE.
 *
 * MPC-BE is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * MPC-BE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "stdafx.h"
#include "LogoAnalysis.h"
#include "LogoParams.h"

#include <algorithm>
#include <cmath>

namespace Logo {

namespace {

// The square of side 2r+1 round each set pixel, set too. Separable.
void Dilate(std::vector<uint8_t>& m, int W, int H, int r)
{
	if (r <= 0) {
		return;
	}
	std::vector<uint8_t> t(m.size());
	for (int y = 0; y < H; y++) {
		const uint8_t* s = &m[(size_t)y * W];
		uint8_t* d = &t[(size_t)y * W];
		int last = -1000000;
		for (int x = 0; x < W; x++) {
			if (s[x]) {
				last = x;
			}
			d[x] = (x - last <= r);
		}
		last = 1000000;
		for (int x = W - 1; x >= 0; x--) {
			if (s[x]) {
				last = x;
			}
			d[x] |= (last - x <= r);
		}
	}
	for (int x = 0; x < W; x++) {
		int last = -1000000;
		for (int y = 0; y < H; y++) {
			if (t[(size_t)y * W + x]) {
				last = y;
			}
			m[(size_t)y * W + x] = (y - last <= r);
		}
		last = 1000000;
		for (int y = H - 1; y >= 0; y--) {
			if (t[(size_t)y * W + x]) {
				last = y;
			}
			m[(size_t)y * W + x] |= (last - y <= r);
		}
	}
}

// Each pixel's distance (the larger of across and down) from the nearest set one, up to 64.
std::vector<int> Distance(const std::vector<uint8_t>& set, int W, int H)
{
	std::vector<int> d(set.size());
	for (size_t i = 0; i < set.size(); i++) {
		d[i] = set[i] ? 0 : 64;
	}
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			int& v = d[(size_t)y * W + x];
			for (int dy = -1; dy <= 0; dy++) {
				for (int dx = -1; dx <= 1; dx++) {
					if ((dy == 0 && dx >= 0) || x + dx < 0 || x + dx >= W || y + dy < 0) {
						continue;
					}
					v = std::min(v, d[(size_t)(y + dy) * W + x + dx] + 1);
				}
			}
		}
	}
	for (int y = H - 1; y >= 0; y--) {
		for (int x = W - 1; x >= 0; x--) {
			int& v = d[(size_t)y * W + x];
			for (int dy = 0; dy <= 1; dy++) {
				for (int dx = -1; dx <= 1; dx++) {
					if ((dy == 0 && dx <= 0) || x + dx < 0 || x + dx >= W || y + dy >= H) {
						continue;
					}
					v = std::min(v, d[(size_t)(y + dy) * W + x + dx] + 1);
				}
			}
		}
	}
	return d;
}

// A plane fitted to the ring's pixels: value = a + b (x - cx) + c (y - cy).
struct PlaneFit {
	double a = 0, b = 0, c = 0, cx = 0, cy = 0;
	double rms = 0;   // what the ring strays from it
	double operator()(int x, int y) const { return a + b * (x - cx) + c * (y - cy); }
};

PlaneFit FitPlane(const Plane& p, const std::vector<uint8_t>& known)
{
	PlaneFit f;
	double n = 0;
	for (int y = 0; y < p.h; y++) {
		for (int x = 0; x < p.w; x++) {
			if (known[(size_t)y * p.w + x]) {
				f.cx += x;
				f.cy += y;
				n++;
			}
		}
	}
	if (n < 3) {
		return f;
	}
	f.cx /= n;
	f.cy /= n;
	double sxx = 0, sxy = 0, syy = 0, sv = 0, sxv = 0, syv = 0;
	for (int y = 0; y < p.h; y++) {
		for (int x = 0; x < p.w; x++) {
			if (known[(size_t)y * p.w + x]) {
				const double dx = x - f.cx, dy = y - f.cy, v = p.at(x, y);
				sxx += dx * dx; sxy += dx * dy; syy += dy * dy;
				sv += v; sxv += dx * v; syv += dy * v;
			}
		}
	}
	f.a = sv / n;
	const double det = sxx * syy - sxy * sxy;
	if (std::abs(det) > 1e-9) {
		f.b = (sxv * syy - syv * sxy) / det;
		f.c = (syv * sxx - sxv * sxy) / det;
	}
	double e = 0;
	for (int y = 0; y < p.h; y++) {
		for (int x = 0; x < p.w; x++) {
			if (known[(size_t)y * p.w + x]) {
				const double d = p.at(x, y) - f(x, y);
				e += d * d;
			}
		}
	}
	f.rms = std::sqrt(e / n);
	return f;
}

std::vector<uint8_t> RingMask(int w, int h, int ring)
{
	std::vector<uint8_t> m((size_t)w * h, 0);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			m[(size_t)y * w + x] = (x < ring || y < ring || x >= w - ring || y >= h - ring);
		}
	}
	return m;
}

// The picture under the unknown pixels, the chosen way.
void EstimateBackground(const Plane& in, const std::vector<uint8_t>& known, Background method, Plane& out)
{
	out = in;
	if (method == BackgroundHarmonic) {
		FillHarmonic(in, known, out, 60);
		return;
	}
	if (method == BackgroundPlane) {
		const PlaneFit f = FitPlane(in, known);
		for (int y = 0; y < in.h; y++) {
			for (int x = 0; x < in.w; x++) {
				if (!known[(size_t)y * in.w + x]) {
					out.at(x, y) = (float)f(x, y);
				}
			}
		}
		return;
	}
	std::vector<float> ring;
	for (size_t i = 0; i < in.v.size(); i++) {
		if (known[i]) {
			ring.push_back(in.v[i]);
		}
	}
	float mean = 0;
	if (!ring.empty()) {
		std::sort(ring.begin(), ring.end());
		const size_t a = ring.size() / 4, b = std::max(a + 1, ring.size() - ring.size() / 4);
		double s = 0;
		for (size_t i = a; i < b; i++) {
			s += ring[i];
		}
		mean = (float)(s / (b - a));
	}
	for (size_t i = 0; i < in.v.size(); i++) {
		if (!known[i]) {
			out.v[i] = mean;
		}
	}
}

// The chroma box of a luma box.
Rect PlaneBox(const Rect& box, int plane, int divW, int divH)
{
	if (plane == 0) {
		return box;
	}
	return { box.x / divW, box.y / divH, box.w / divW, box.h / divH };
}

// A box's bars in one plane's pixels: a chroma row is the bar's when both luma rows it
// covers are, as the shader takes it.
Bars PlaneBars(const Bars& b, int plane, int divW, int divH)
{
	const int dw = plane ? divW : 1, dh = plane ? divH : 1;
	return { b.top / dh, b.bottom / dh, b.left / dw, b.right / dw };
}

bool InBar(const Bars& e, int w, int h, int x, int y)
{
	return y < e.top || y >= h - e.bottom || x < e.left || x >= w - e.right;
}

std::vector<uint8_t> BarMask(const Bars& e, int w, int h)
{
	std::vector<uint8_t> m((size_t)w * h);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			m[(size_t)y * w + x] = InBar(e, w, h, x, y);
		}
	}
	return m;
}

bool Any(const std::vector<uint8_t>& m)
{
	return std::find(m.begin(), m.end(), 1) != m.end();
}

void LevelBars(const Plane& obs, const std::vector<uint8_t>& unknown, const Bars& e, Plane& fill);

// The picture under the unknown pixels with any bar kept apart: the picture's part
// estimated from the picture's known pixels alone, the bar's taken as the bar's level, row
// by row as the renderer gives it back: the bar's last row before the picture is often a
// little lighter than the rest of it.
void EstimateApart(const Plane& in, const std::vector<uint8_t>& known, const Bars& e,
	Background method, Plane& out)
{
	const std::vector<uint8_t> bar = BarMask(e, in.w, in.h);
	if (!Any(bar)) {
		EstimateBackground(in, known, method, out);
		return;
	}
	std::vector<uint8_t> knownPicture(known.size()), picture(known.size()), unknown(known.size());
	double sum = 0;
	int n = 0;
	for (size_t i = 0; i < known.size(); i++) {
		picture[i] = !bar[i];
		knownPicture[i] = known[i] && !bar[i];
		unknown[i] = !known[i];
		if (known[i] && bar[i]) {
			sum += in.v[i];
			n++;
		}
	}
	if (method == BackgroundHarmonic) {
		out = in;
		FillHarmonic(in, knownPicture, out, 60, &picture);
	} else {
		EstimateBackground(in, knownPicture, method, out);
	}
	// The bar's mean where a row of it has nothing known.
	const float level = n ? (float)(sum / n) : 0.0f;
	for (size_t i = 0; i < known.size(); i++) {
		if (bar[i]) {
			out.v[i] = n ? level : in.v[i];
		}
	}
	LevelBars(in, unknown, e, out);
}

// The picture's first rows (columns) at a bar's edge: the encoder, or the resize the film
// went through, often blends the bar into them, darkening the picture there by as much all
// along the edge. The line reads that as a black logo laid across the whole box, and
// neither the picture nor the logo can be told there. The logo's alpha and what it lays do
// not depend on what is under it: on the rows blended, they are taken from the first row
// clear of the blend. A row is blended when the ring's pixels on it follow less than
// kBlendFollows of the changes of those on the row kEdgeBlend in from the edge, over the
// pictures fitted on. Measured (--tlogo --real): 0.19, 0.61 and 0.95 on the three rows after
// the bar of one TV recording, 0.37 then 1.00 and 1.04 on another's; 0.994 to 1.001 on a
// bar with no blend, through x264 too (--tlogo).
constexpr int kEdgeBlend = 3;
constexpr double kBlendFollows = 0.97;

// How many rows (columns) from each bar's edge are blended, in luma pixels; and how much
// each of the first kEdgeBlend follows, side after side (top, bottom, left, right), -1
// where there is no bar.
Bars Blended(const std::vector<Crop>& crops, const Bars& e, int w, int h, int ring, std::vector<float>& measured)
{
	Bars out;
	// The ring's mean on line `at` of a side, in one picture.
	auto ringMean = [&](const Plane& p, bool row, int at) {
		double sum = 0;
		int n = 0;
		const int across = row ? w : h;
		for (int t = 0; t < across; t++) {
			if (t < ring || t >= across - ring) {
				sum += row ? p.at(t, at) : p.at(at, t);
				n++;
			}
		}
		return n ? sum / n : 0.0;
	};
	// How much of the changes of line `ref` line `at` follows.
	auto follows = [&](bool row, int at, int ref) {
		double sa = 0, sb = 0, sab = 0, sbb = 0;
		const double n = (double)crops.size();
		for (const Crop& c : crops) {
			const double a = ringMean(c.p[0], row, at), b = ringMean(c.p[0], row, ref);
			sa += a;
			sb += b;
			sab += a * b;
			sbb += b * b;
		}
		const double vb = sbb / n - (sb / n) * (sb / n);
		if (vb < 1e-6) {
			return 1.0;   // the picture there never changed: nothing to tell by
		}
		return (sab / n - (sa / n) * (sb / n)) / vb;
	};
	measured.assign(4 * kEdgeBlend, -1.0f);
	int side = 0;
	auto count = [&](bool row, int edge, int step, int limit) {
		float* m = &measured[kEdgeBlend * side];
		const int ref = edge + step * kEdgeBlend;
		if (step > 0 ? ref >= limit : ref < limit) {
			return 0;
		}
		int j = kEdgeBlend;
		for (int k = 0; k < kEdgeBlend; k++) {
			m[k] = (float)follows(row, edge + step * k, ref);
			if (m[k] >= kBlendFollows && j == kEdgeBlend) {
				j = k;
			}
		}
		return j;
	};
	if (crops.empty()) {
		return out;
	}
	if (e.top > 0) {
		out.top = count(true, e.top, 1, h - e.bottom);
	}
	side = 1;
	if (e.bottom > 0) {
		out.bottom = count(true, h - e.bottom - 1, -1, e.top);
	}
	side = 2;
	if (e.left > 0) {
		out.left = count(false, e.left, 1, w - e.right);
	}
	side = 3;
	if (e.right > 0) {
		out.right = count(false, w - e.right - 1, -1, e.left);
	}
	return out;
}

// The blended rows (columns) of a plane, `band` of them at each side, taken from the one past them.
void AcrossEdge(Plane& q, const Bars& e, const Bars& band)
{
	const int w = q.w, h = q.h;
	if (band.top > 0 && e.top + band.top < h - e.bottom) {
		for (int y = e.top; y < e.top + band.top; y++) {
			for (int x = 0; x < w; x++) {
				q.at(x, y) = q.at(x, e.top + band.top);
			}
		}
	}
	if (band.bottom > 0 && h - e.bottom - band.bottom - 1 >= e.top) {
		for (int y = h - e.bottom - band.bottom; y < h - e.bottom; y++) {
			for (int x = 0; x < w; x++) {
				q.at(x, y) = q.at(x, h - e.bottom - band.bottom - 1);
			}
		}
	}
	if (band.left > 0 && e.left + band.left < w - e.right) {
		for (int y = 0; y < h; y++) {
			for (int x = e.left; x < e.left + band.left; x++) {
				q.at(x, y) = q.at(e.left + band.left, y);
			}
		}
	}
	if (band.right > 0 && w - e.right - band.right - 1 >= e.left) {
		for (int y = 0; y < h; y++) {
			for (int x = w - e.right - band.right; x < w - e.right; x++) {
				q.at(x, y) = q.at(w - e.right - band.right - 1, y);
			}
		}
	}
}

} // namespace

// The pixels of one plane the fill does not take from: the logo's and a margin round them,
// filled in where the logo is too opaque to see through; and the codec's ringing, which
// is worked out of them rather than taken as the picture.
std::vector<uint8_t> Unknown(const Maps& m, int plane, int halo)
{
	const Plane& a = m.alpha[plane];
	std::vector<uint8_t> u(a.v.size());
	for (size_t i = 0; i < u.size(); i++) {
		u[i] = a.v[i] >= m.alphaMin;
	}
	const int div = plane ? std::max(m.divW, m.divH) : 1;
	Dilate(u, a.w, a.h, (halo + div - 1) / div);
	for (size_t i = 0; i < u.size(); i++) {
		u[i] |= a.v[i] <= -m.alphaMin;
	}
	return u;
}

namespace {

// The fill reads the picture's known pixels, never a bar's: its holes are filled from the
// picture alone, and a bar's own pixels are given the bar's level (LevelBars).
void Fill(const Plane& in, const std::vector<uint8_t>& unknown, const std::vector<uint8_t>& bar,
	const RemoveOptions& o, Plane& out)
{
	std::vector<uint8_t> known(unknown.size()), picture(unknown.size());
	std::vector<float> weight(unknown.size());
	for (size_t i = 0; i < unknown.size(); i++) {
		known[i] = !unknown[i] && !bar[i];
		picture[i] = !bar[i];
		weight[i] = known[i] ? 1.0f : 0.0f;
	}
	if (o.fill == 0) {
		FillHarmonic(in, known, out, 400, &picture);
		return;
	}
	FillPushPull(in, weight, out, o.fill == 1 ? 1 : 0);
	if (o.smooth > 0) {
		Plane start = out;
		FillHarmonic(start, known, out, -o.smooth);
	}
}

} // namespace

// ----------------------------------------------------------------- CLocator --

void CLocator::Reset(int W, int H, int scale, float edge)
{
	m_edge = edge;
	m_W = W;
	m_H = H;
	m_scale = std::max(1, scale);
	m_n = 0;
	m_count.assign((size_t)W * H, 0);
	m_sum.assign((size_t)W * H, 0);
	m_sum2.assign((size_t)W * H, 0);
}

void CLocator::Add(const Plane& luma)
{
	if (luma.w != m_W || luma.h != m_H) {
		return;
	}
	const int W = m_W, H = m_H;
	for (int y = 0; y < H; y++) {
		const float* r = &luma.v[(size_t)y * W];
		float* c = &m_count[(size_t)y * W];
		float* s = &m_sum[(size_t)y * W];
		float* s2 = &m_sum2[(size_t)y * W];
		for (int x = 0; x < W; x++) {
			s[x] += r[x];
			s2[x] += r[x] * r[x];
		}
		if (y == 0 || y == H - 1) {
			continue;
		}
		const float* u = r - W;
		const float* d = r + W;
		const float t2 = m_edge * m_edge * 64.0f;   // Sobel / 8 against the edge, squared
		for (int x = 1; x < W - 1; x++) {
			const float gx = (u[x + 1] + 2 * r[x + 1] + d[x + 1]) - (u[x - 1] + 2 * r[x - 1] + d[x - 1]);
			const float gy = (d[x - 1] + 2 * d[x] + d[x + 1]) - (u[x - 1] + 2 * u[x] + u[x + 1]);
			c[x] += (gx * gx + gy * gy > t2) ? 1.0f : 0.0f;
		}
	}
	m_n++;
}

void CLocator::Maps(Plane& share, Plane& spread) const
{
	share.resize(m_W, m_H);
	spread.resize(m_W, m_H);
	if (!m_n) {
		return;
	}
	for (size_t i = 0; i < share.v.size(); i++) {
		share.v[i] = m_count[i] / m_n;
		const float mean = m_sum[i] / m_n;
		spread.v[i] = std::sqrt(std::max(0.0f, m_sum2[i] / m_n - mean * mean));
	}
}

CLocator::Result CLocator::Locate(const Options& o) const
{
	Result r;
	const int W = m_W, H = m_H;
	if (m_n < 2 || W < 32 || H < 32) {
		r.still = true;
		return r;
	}

	// Black bars: rows and columns from the border inwards that stayed dark and still over
	// most of their length. A logo laid over a bar leaves the rest of the row dark.
	auto darkStill = [&](size_t i) {
		const float mean = m_sum[i] / m_n;
		const float var = m_sum2[i] / m_n - mean * mean;
		return mean < 0.1f && var < 0.02f * 0.02f;
	};
	auto rowBar = [&](int y) {
		int n = 0;
		for (int x = 0; x < W; x++) {
			n += darkStill((size_t)y * W + x);
		}
		return n >= W / 2;
	};
	auto colBar = [&](int x) {
		int n = 0;
		for (int y = 0; y < H; y++) {
			n += darkStill((size_t)y * W + x);
		}
		return n >= H / 2;
	};
	int top = 0, bottom = H, left = 0, right = W;
	while (top < H / 3 && rowBar(top)) {
		top++;
	}
	while (bottom > H - H / 3 && rowBar(bottom - 1)) {
		bottom--;
	}
	while (left < W / 3 && colBar(left)) {
		left++;
	}
	while (right > W - W / 3 && colBar(right - 1)) {
		right--;
	}
	const int S = m_scale;
	r.picture = { left * S, top * S, (right - left) * S, (bottom - top) * S };

	const int m = o.margin;
	const int x0 = left + m, x1 = right - m, y0 = top + m, y1 = bottom - m;
	if (x1 - x0 < 16 || y1 - y0 < 16) {
		r.still = true;
		return r;
	}
	// The logo's edges are looked for over the bars as well: a channel's logo often sits
	// across a bar's edge, part over the bar and part over the picture. Not along that
	// edge itself, which every picture not dark there keeps, nor on the frame's border.
	auto barEdge = [&](int x, int y) {
		return (top > 0 && std::abs(y - top) <= m) || (bottom < H && std::abs(y - bottom) <= m)
			|| (left > 0 && std::abs(x - left) <= m) || (right < W && std::abs(x - right) <= m);
	};
	const float need = o.share * m_n;
	std::vector<uint8_t> cand((size_t)W * H, 0);
	int edges = 0;
	for (int y = m; y < H - m; y++) {
		for (int x = m; x < W - m; x++) {
			const size_t i = (size_t)y * W + x;
			if (m_count[i] >= need && !barEdge(x, y)) {
				cand[i] = 1;
				edges += x >= x0 && x < x1 && y >= y0 && y < y1;
			}
		}
	}
	// So much of the picture kept its edges that it cannot have changed much: a still
	// scene, or too few pictures yet.
	if (edges > o.busy * (double)(x1 - x0) * (y1 - y0)) {
		r.still = true;
		return r;
	}

	// Letters close together make one logo.
	std::vector<uint8_t> grown = cand;
	Dilate(grown, W, H, std::max(1, (int)std::lround(o.merge * W)));
	std::vector<int> label((size_t)W * H, -1);
	struct Group { int x0, y0, x1, y1, n; double score; };
	std::vector<Group> groups;
	std::vector<int> stack;
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			const size_t i = (size_t)y * W + x;
			if (!grown[i] || label[i] >= 0) {
				continue;
			}
			const int id = (int)groups.size();
			Group g = { W, H, -1, -1, 0, 0 };
			stack.push_back((int)i);
			label[i] = id;
			while (!stack.empty()) {
				const int j = stack.back();
				stack.pop_back();
				const int jx = j % W, jy = j / W;
				if (cand[j]) {
					g.x0 = std::min(g.x0, jx); g.y0 = std::min(g.y0, jy);
					g.x1 = std::max(g.x1, jx); g.y1 = std::max(g.y1, jy);
					g.n++;
					g.score += m_count[j] / m_n;
				}
				for (int dy = -1; dy <= 1; dy++) {
					for (int dx = -1; dx <= 1; dx++) {
						const int nx = jx + dx, ny = jy + dy;
						if (nx < 0 || ny < 0 || nx >= W || ny >= H) {
							continue;
						}
						const size_t k = (size_t)ny * W + nx;
						if (grown[k] && label[k] < 0) {
							label[k] = id;
							stack.push_back((int)k);
						}
					}
				}
			}
			groups.push_back(g);
		}
	}
	const Group* best = nullptr;
	const int minPixels = std::max(12, W / 160);
	for (const Group& g : groups) {
		if (g.n < minPixels) {
			continue;
		}
		const int gw = g.x1 - g.x0 + 1, gh = g.y1 - g.y0 + 1;
		if (gw > o.most * W || gh > o.most * H) {
			continue;
		}
		if (!best || g.score > best->score) {
			best = &g;
		}
	}
	if (!best) {
		return r;
	}

	// The box round it, in full-size pixels, with room for the ring the picture under
	// the logo is read from; even, for the chroma. Inside the frame, over a bar or not.
	const int margin = std::max(10, (W * S) / 192);
	int bx0 = best->x0 * S - margin, by0 = best->y0 * S - margin;
	int bx1 = (best->x1 + 1) * S + margin, by1 = (best->y1 + 1) * S + margin;
	bx0 = std::max(bx0, 0);
	by0 = std::max(by0, 0);
	bx0 &= ~1;
	by0 &= ~1;
	bx1 = (bx1 + 1) & ~1;
	by1 = (by1 + 1) & ~1;
	bx1 = std::min(bx1, W * S);
	by1 = std::min(by1, H * S);
	r.box = { bx0, by0, bx1 - bx0, by1 - by0 };
	r.found = !r.box.empty();
	r.score = (float)best->score;
	r.edgePixels = best->n;
	return r;
}

// --------------------------------------------------------------- CDeblender --

void CDeblender::Reset(const Rect& box, const Rect& picture, int divW, int divH)
{
	m_box = box;
	m_inside = { 0, 0, box.w, box.h };
	if (!picture.empty()) {
		const int x0 = std::clamp(picture.x - box.x, 0, box.w), y0 = std::clamp(picture.y - box.y, 0, box.h);
		const int x1 = std::clamp(picture.x + picture.w - box.x, 0, box.w), y1 = std::clamp(picture.y + picture.h - box.y, 0, box.h);
		m_inside = { x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0) };
	}
	m_divW = std::max(1, divW);
	m_divH = std::max(1, divH);
	m_crops.clear();
	m_dropped = 0;
	m_colour = 0;
}

void CDeblender::Add(const Crop& c)
{
	if (c.p[0].w != m_box.w || c.p[0].h != m_box.h) {
		return;
	}
	m_crops.push_back(c);
}

bool CDeblender::Fit(const Options& o, Maps& out)
{
	const int n = (int)m_crops.size();
	if (n < 4 || m_box.empty()) {
		return false;
	}
	out = Maps();
	out.box = m_box;
	out.divW = m_divW;
	out.divH = m_divH;
	for (int p = 0; p < 3; p++) {
		const Rect b = PlaneBox(m_box, p, m_divW, m_divH);
		out.alpha[p].resize(b.w, b.h, 0);
		out.laid[p].resize(b.w, b.h, 0);
	}
	out.noise = o.noise;
	std::vector<double> residual;   // luma: what the line leaves of each pixel, squared

	std::vector<uint8_t> keep(n, 1);
	std::vector<uint8_t> anchors[3];   // pixels inside the box found free of the logo
	const double noise2 = (double)o.noise * o.noise;
	const int passes = std::max(1, o.passes);
	// The parts of the box over a black bar, which the picture is never estimated from nor
	// into: the bar is its own level.
	const Bars fitBars = { m_inside.y, m_box.h - m_inside.y - m_inside.h, m_inside.x, m_box.w - m_inside.x - m_inside.w };
	Bars edges[3];
	std::vector<uint8_t> bars[3];
	for (int p = 0; p < 3; p++) {
		edges[p] = PlaneBars(fitBars, p, m_divW, m_divH);
		bars[p] = BarMask(edges[p], out.alpha[p].w, out.alpha[p].h);
	}
	// The rows blended with a bar, in each plane's pixels: a chroma row is when either of
	// the luma rows it covers is.
	Bars blend[3];
	blend[0] = m_blend = Blended(m_crops, fitBars, m_box.w, m_box.h, o.ring, m_follows);
	for (int p = 1; p < 3; p++) {
		blend[p] = { (blend[0].top + m_divH - 1) / m_divH, (blend[0].bottom + m_divH - 1) / m_divH,
			(blend[0].left + m_divW - 1) / m_divW, (blend[0].right + m_divW - 1) / m_divW };
	}

	for (int pass = 0; pass < passes; pass++) {
		for (int p = 0; p < 3; p++) {
			const Plane& shape = out.alpha[p];
			const int w = shape.w, h = shape.h;
			const size_t N = (size_t)w * h;
			const int ring = p ? std::max(1, o.ring / std::max(m_divW, m_divH)) : o.ring;
			const std::vector<uint8_t>& bar = bars[p];
			std::vector<uint8_t> known = RingMask(w, h, ring);
			if (pass > 0 && o.background == BackgroundHarmonic && anchors[p].size() == N) {
				for (size_t i = 0; i < N; i++) {
					known[i] |= anchors[p][i];
				}
			}
			// How far the picture strays from a plane, read on the ring's picture pixels.
			std::vector<uint8_t> ringOnly = RingMask(w, h, ring);
			{
				std::vector<uint8_t> ringPicture = ringOnly;
				for (size_t i = 0; i < N; i++) {
					ringPicture[i] &= !bar[i];
				}
				if (Any(ringPicture)) {
					ringOnly.swap(ringPicture);
				}
			}
			const bool chromaAlone = p > 0 && o.chromaFromLuma;

			std::vector<double> sw(N, 0), sx(N, 0), sy(N, 0), sxx(N, 0), sxy(N, 0), syy(N, 0);
			Plane bg;
			for (int t = 0; t < n; t++) {
				if (!keep[t]) {
					continue;
				}
				const Plane& obs = m_crops[t].p[p];
				const PlaneFit f = FitPlane(obs, ringOnly);
				const double wt = 1.0 / (noise2 + f.rms * f.rms);
				EstimateApart(obs, known, edges[p], o.background, bg);
				for (size_t i = 0; i < N; i++) {
					const double x = bg.v[i], y = obs.v[i];
					sw[i] += wt; sx[i] += wt * x; sy[i] += wt * y;
					sxx[i] += wt * x * x; sxy[i] += wt * x * y; syy[i] += wt * y * y;
				}
			}

			Plane& A = out.alpha[p];
			Plane& L = out.laid[p];
			for (size_t i = 0; i < N; i++) {
				if (sw[i] <= 0) {
					A.v[i] = 0;
					L.v[i] = 0;
					continue;
				}
				const double mx = sx[i] / sw[i], my = sy[i] / sw[i];
				const double vxx = std::max(0.0, sxx[i] / sw[i] - mx * mx);
				const double vxy = sxy[i] / sw[i] - mx * my;
				if (chromaAlone) {
					// alpha from the luma under it; the colour that fits best with it.
					const int cx = (int)(i % w), cy = (int)(i / w);
					const Plane& la = out.alpha[0];
					double a = 0;
					int k = 0;
					for (int dy = 0; dy < m_divH; dy++) {
						for (int dx = 0; dx < m_divW; dx++) {
							const int lx = cx * m_divW + dx, ly = cy * m_divH + dy;
							if (lx < la.w && ly < la.h) {
								a += la.at(lx, ly);
								k++;
							}
						}
					}
					a = k ? a / k : 0;
					A.v[i] = (float)a;
					L.v[i] = (float)(my - (1 - a) * mx);
					continue;
				}
				if (std::sqrt(vxx) < o.illPosed) {
					// The picture under it hardly changed: nothing tells the logo from it.
					// Something stays there that the picture round it does not explain.
					const bool over = std::abs(my - mx) > 3.0 * o.noise;
					A.v[i] = over ? 1.0f : 0.0f;
					L.v[i] = over ? (float)my : 0.0f;
					continue;
				}
				const double a = std::clamp(1.0 - vxy / vxx, (double)kAlphaLeast, 1.0);
				A.v[i] = (float)a;
				L.v[i] = (float)(my - (1 - a) * mx);
			}

			AcrossEdge(A, edges[p], blend[p]);
			AcrossEdge(L, edges[p], blend[p]);

			if (p == 0 && pass == passes - 1) {
				residual.assign(N, 0);
				for (size_t i = 0; i < N; i++) {
					if (sw[i] <= 0) {
						continue;
					}
					const double a = 1.0 - A.v[i], b = L.v[i];
					const double r = syy[i] - 2 * a * sxy[i] - 2 * b * sy[i] + a * a * sxx[i] + 2 * a * b * sx[i] + b * b * sw[i];
					residual[i] = std::max(0.0, r / sw[i]);
				}
				// The blended rows as sure as the row their logo was taken from.
				Plane r;
				r.resize(w, h);
				for (size_t i = 0; i < N; i++) {
					r.v[i] = (float)residual[i];
				}
				AcrossEdge(r, edges[0], blend[0]);
				for (size_t i = 0; i < N; i++) {
					residual[i] = r.v[i];
				}
			}
			if (p == 0) {
				// What alpha reads where there is surely no logo: the band just inside the
				// ring, which the locator's margin keeps clear; over the picture.
				std::vector<float> band;
				for (int y = 0; y < h; y++) {
					for (int x = 0; x < w; x++) {
						const int d = std::min(std::min(x, y), std::min(w - 1 - x, h - 1 - y));
						if (d >= ring && d < ring + 4 && !bar[(size_t)y * w + x]) {
							band.push_back(std::abs(A.at(x, y)));
						}
					}
				}
				if (!band.empty()) {
					std::nth_element(band.begin(), band.begin() + band.size() * 9 / 10, band.end());
					out.alphaMin = std::clamp(band[band.size() * 9 / 10] * 2.0f, 0.02f, 0.15f);
				}
				// The logo's colour where it is over the picture, if it has one: most are
				// a single colour, white as a rule.
				{
					const Rect picture = { m_box.x + m_inside.x, m_box.y + m_inside.y, m_inside.w, m_inside.h };
					float c[3] = {};
					m_colour = !m_inside.empty() && LogoColours(out, picture, c) && c[2] - c[0] < o.oneColour ? c[1] : 0.0f;
				}
				// Over a bar the picture under the logo never changed, and the line cannot
				// tell alpha from colour there: what shows is the logo laid over the bar's
				// level. With the logo's colour, alpha is what takes the bar to what shows;
				// without it the logo is taken as opaque there, and filled. On this film
				// either gives the bar back; on another, where this part of the logo is over
				// the picture, the first inverts it where the second can only fill.
				for (size_t i = 0; i < N; i++) {
					if (!bar[i] || sw[i] <= 0) {
						continue;
					}
					const double mx = sx[i] / sw[i], my = sy[i] / sw[i], d = my - mx;
					if (std::abs(d) <= o.barLogo) {
						A.v[i] = 0;
						L.v[i] = (float)d;
						continue;
					}
					double a = 1;
					if (m_colour > 0 && std::abs(m_colour - mx) > 0.1) {
						a = std::clamp(d / (m_colour - mx), (double)kAlphaLeast, 1.0);
						// It shows over the bar: no less than what counts as the logo's.
						a = a < 0 ? std::min(a, -(double)out.alphaMin) : std::max(a, (double)out.alphaMin);
					}
					A.v[i] = (float)a;
					L.v[i] = (float)(my - (1 - a) * mx);
				}
				// Not the pixels next to the logo: the codec smears the logo into them,
				// and the fit is to measure by how much, not take them as the picture.
				std::vector<uint8_t> beside(N);
				for (size_t i = 0; i < N; i++) {
					beside[i] = A.v[i] >= out.alphaMin;
				}
				Dilate(beside, w, h, 2);
				anchors[0].assign(N, 0);
				for (size_t i = 0; i < N; i++) {
					anchors[0][i] = std::abs(A.v[i]) < out.alphaMin * 0.5f && !beside[i];
				}
			}
		}
		// Chroma anchors from luma's.
		for (int p = 1; p < 3; p++) {
			const Plane& a = out.alpha[p];
			std::vector<uint8_t> beside(a.v.size());
			for (size_t i = 0; i < a.v.size(); i++) {
				beside[i] = a.v[i] >= out.alphaMin;
			}
			Dilate(beside, a.w, a.h, 1);
			anchors[p].assign(a.v.size(), 0);
			for (size_t i = 0; i < a.v.size(); i++) {
				anchors[p][i] = std::abs(a.v[i]) < out.alphaMin * 0.5f && !beside[i];
			}
		}

		if (pass + 1 < passes) {
			// Pictures the logo was not on (advertising, a fade): what the fit expects is
			// further from them than the picture round the logo carried in.
			const Plane& A = out.alpha[0];
			const Plane& L = out.laid[0];
			float strongest = 0;
			for (float a : A.v) {
				strongest = std::max(strongest, a);
			}
			const float strong = std::max(out.alphaMin * 2, strongest * 0.5f);
			std::vector<uint8_t> known = RingMask(A.w, A.h, o.ring);
			for (size_t i = 0; i < known.size(); i++) {
				known[i] |= anchors[0][i];
			}
			m_dropped = 0;
			Plane bg;
			for (int t = 0; t < n; t++) {
				const Plane& obs = m_crops[t].p[0];
				EstimateApart(obs, known, edges[0], BackgroundHarmonic, bg);
				double e1 = 0, e0 = 0;
				for (size_t i = 0; i < A.v.size(); i++) {
					if (A.v[i] < strong) {
						continue;
					}
					const double fit = (1 - A.v[i]) * bg.v[i] + L.v[i];
					e1 += (obs.v[i] - fit) * (obs.v[i] - fit);
					e0 += (obs.v[i] - bg.v[i]) * (obs.v[i] - bg.v[i]);
				}
				keep[t] = e1 < e0;
				m_dropped += !keep[t];
			}
			if (m_dropped > n - 4) {
				std::fill(keep.begin(), keep.end(), 1);
				m_dropped = 0;
			}
		}
	}
	out.pictures = n - m_dropped;

	// The fill's error by how far a pixel is from the picture it is filled from: the
	// pixels round the logo left out, filled in, and their own values compared, as the
	// renderer fills -- from the picture, never from a bar.
	{
		const Plane& A = out.alpha[0];
		const int w = A.w, h = A.h;
		const std::vector<uint8_t>& bar = bars[0];
		const std::vector<uint8_t> unknown = Unknown(out, 0, kMarginMost);
		std::vector<uint8_t> grown = unknown;
		constexpr int kFar = 6;
		Dilate(grown, w, h, kFar);
		std::vector<uint8_t> known(grown.size());
		std::vector<float> weight(grown.size());
		for (size_t i = 0; i < grown.size(); i++) {
			known[i] = !grown[i] && !bar[i];
			weight[i] = known[i] ? 1.0f : 0.0f;
		}
		const std::vector<int> away = Distance(known, w, h);
		double err[kFar + 1] = {}, cnt[kFar + 1] = {};
		Plane fill;
		for (int t = 0; t < n; t += 2) {
			if (!keep[t]) {
				continue;
			}
			const Plane& obs = m_crops[t].p[0];
			FillPushPull(obs, weight, fill, 1);
			for (size_t i = 0; i < grown.size(); i++) {
				if (grown[i] && !unknown[i] && !bar[i]) {
					const int d = std::clamp(away[i], 1, kFar);
					const double e = fill.v[i] - obs.v[i];
					err[d] += e * e;
					cnt[d]++;
				}
			}
		}
		double fillVar[kFar + 1] = {};
		const double floor = (double)o.noise * o.noise;
		out.fillVar.assign(kFar + 1, (float)floor);
		for (int d = 1; d <= kFar; d++) {
			fillVar[d] = std::max(floor, cnt[d] ? err[d] / cnt[d] : (d > 1 ? fillVar[d - 1] : floor));
			fillVar[d] = std::max(fillVar[d], fillVar[d - 1]);   // no nearer than a nearer pixel
			out.fillVar[d] = (float)fillVar[d];
		}
		// What the line left, less what the picture it was fitted against got wrong: that
		// picture was carried in from the ring and the pixels found free of the logo, and
		// is as far wrong as a fill from as far away.
		out.excess.resize(w, h, 0);
		if (residual.size() == out.excess.v.size()) {
			std::vector<uint8_t> fitKnown = RingMask(w, h, o.ring);
			if (o.background == BackgroundHarmonic && anchors[0].size() == fitKnown.size()) {
				for (size_t i = 0; i < fitKnown.size(); i++) {
					fitKnown[i] |= anchors[0][i];
				}
			}
			for (size_t i = 0; i < fitKnown.size(); i++) {
				fitKnown[i] &= !bar[i];
			}
			const std::vector<int> fitDepth = Distance(fitKnown, w, h);
			for (size_t i = 0; i < residual.size(); i++) {
				if (bar[i]) {
					// Over a bar alpha was told from the colour, not by the line: nothing
					// to doubt it by.
					out.excess.v[i] = (float)(floor * 0.25);
					continue;
				}
				const double slope = 1.0 - A.v[i];
				const double bgVar = fitDepth[i] == 0 ? 0.0 : fillVar[std::clamp(fitDepth[i], 1, kFar)];
				out.excess.v[i] = (float)std::max(floor * 0.25, residual[i] - slope * slope * bgVar);
			}
		}
	}
	return true;
}

bool LogoColours(const Maps& m, const Rect& picture, float out[3])
{
	struct Sample { float c, w; };
	std::vector<Sample> s;
	const Plane& A = m.alpha[0];
	const Plane& L = m.laid[0];
	for (int y = 0; y < A.h; y++) {
		for (int x = 0; x < A.w; x++) {
			const int X = m.box.x + x, Y = m.box.y + y;
			const bool over = !picture.empty()
				&& (X < picture.x || Y < picture.y || X >= picture.x + picture.w || Y >= picture.y + picture.h);
			const float a = A.at(x, y);
			if (!over && a >= 0.2f) {
				s.push_back({ L.at(x, y) / a, a * a });
			}
		}
	}
	if (s.size() < 20) {
		return false;
	}
	std::sort(s.begin(), s.end(), [](const Sample& a, const Sample& b) { return a.c < b.c; });
	double total = 0;
	for (const Sample& e : s) {
		total += e.w;
	}
	const double q[3] = { 0.1, 0.5, 0.9 };
	double acc = 0;
	size_t i = 0;
	for (int k = 0; k < 3; k++) {
		while (i + 1 < s.size() && acc + s[i].w < q[k] * total) {
			acc += s[i].w;
			i++;
		}
		out[k] = s[i].c;
	}
	return true;
}

// --------------------------------------------------------------------- fills --

void FillPushPull(const Plane& in, const std::vector<float>& weight, Plane& out, int kernel)
{
	struct Level { int w, h; std::vector<float> v, wt; };
	std::vector<Level> lv;
	lv.push_back({ in.w, in.h, in.v, weight });
	for (size_t i = 0; i < lv[0].v.size(); i++) {
		lv[0].v[i] = weight[i] > 0 ? in.v[i] : 0.0f;
	}
	static const float k4[4] = { 0.25f, 0.75f, 0.75f, 0.25f };
	while (lv.back().w > 1 || lv.back().h > 1) {
		const Level& f = lv.back();
		Level c;
		c.w = (f.w + 1) / 2;
		c.h = (f.h + 1) / 2;
		c.v.assign((size_t)c.w * c.h, 0);
		c.wt.assign((size_t)c.w * c.h, 0);
		for (int y = 0; y < c.h; y++) {
			for (int x = 0; x < c.w; x++) {
				double sv = 0, sw = 0;
				if (kernel == 0) {
					for (int dy = 0; dy < 2; dy++) {
						for (int dx = 0; dx < 2; dx++) {
							const int fx = 2 * x + dx, fy = 2 * y + dy;
							if (fx < f.w && fy < f.h) {
								const size_t i = (size_t)fy * f.w + fx;
								sv += f.wt[i] * f.v[i];
								sw += f.wt[i];
							}
						}
					}
				} else {
					for (int dy = 0; dy < 4; dy++) {
						for (int dx = 0; dx < 4; dx++) {
							const int fx = 2 * x - 1 + dx, fy = 2 * y - 1 + dy;
							if (fx < 0 || fy < 0 || fx >= f.w || fy >= f.h) {
								continue;
							}
							const size_t i = (size_t)fy * f.w + fx;
							const double k = (double)k4[dx] * k4[dy];
							sv += k * f.wt[i] * f.v[i];
							sw += k * f.wt[i];
						}
					}
				}
				const size_t j = (size_t)y * c.w + x;
				c.v[j] = sw > 0 ? (float)(sv / sw) : 0.0f;
				c.wt[j] = (float)std::min(1.0, sw);
			}
		}
		lv.push_back(std::move(c));
	}
	for (int l = (int)lv.size() - 2; l >= 0; l--) {
		Level& f = lv[l];
		const Level& c = lv[l + 1];
		for (int y = 0; y < f.h; y++) {
			const float cyf = std::clamp((y + 0.5f) / 2 - 0.5f, 0.0f, (float)(c.h - 1));
			const int cy0 = (int)cyf, cy1 = std::min(cy0 + 1, c.h - 1);
			const float fy = cyf - cy0;
			for (int x = 0; x < f.w; x++) {
				const float cxf = std::clamp((x + 0.5f) / 2 - 0.5f, 0.0f, (float)(c.w - 1));
				const int cx0 = (int)cxf, cx1 = std::min(cx0 + 1, c.w - 1);
				const float fx = cxf - cx0;
				const float up = (1 - fy) * ((1 - fx) * c.v[(size_t)cy0 * c.w + cx0] + fx * c.v[(size_t)cy0 * c.w + cx1])
					+ fy * ((1 - fx) * c.v[(size_t)cy1 * c.w + cx0] + fx * c.v[(size_t)cy1 * c.w + cx1]);
				const size_t i = (size_t)y * f.w + x;
				const float w = std::min(1.0f, f.wt[i]);
				f.v[i] = w * f.v[i] + (1 - w) * up;
			}
		}
	}
	out.resize(in.w, in.h);
	for (size_t i = 0; i < out.v.size(); i++) {
		out.v[i] = weight[i] >= 1.0f ? in.v[i] : lv[0].v[i];
	}
}

void FillHarmonic(const Plane& in, const std::vector<uint8_t>& known, Plane& out, int iterations,
	const std::vector<uint8_t>* domain)
{
	// iterations < 0: start from out as it is and take that many plain Jacobi steps
	// (the renderer's smoothing after its push-pull); otherwise start from a push-pull
	// and go on with over-relaxation.
	const bool jacobi = iterations < 0;
	auto inside = [&](size_t i) { return !domain || (*domain)[i]; };
	if (!jacobi) {
		std::vector<float> weight(known.size());
		for (size_t i = 0; i < known.size(); i++) {
			weight[i] = known[i] && inside(i) ? 1.0f : 0.0f;
		}
		FillPushPull(in, weight, out, 1);
	}
	const int W = in.w, H = in.h;
	const int steps = jacobi ? -iterations : iterations;
	std::vector<float> next;
	for (int it = 0; it < steps; it++) {
		if (jacobi) {
			next = out.v;
		}
		for (int y = 0; y < H; y++) {
			for (int x = 0; x < W; x++) {
				const size_t i = (size_t)y * W + x;
				if (known[i] || !inside(i)) {
					continue;
				}
				float s = 0;
				int k = 0;
				if (x > 0 && inside(i - 1)) { s += out.v[i - 1]; k++; }
				if (x < W - 1 && inside(i + 1)) { s += out.v[i + 1]; k++; }
				if (y > 0 && inside(i - W)) { s += out.v[i - W]; k++; }
				if (y < H - 1 && inside(i + W)) { s += out.v[i + W]; k++; }
				if (!k) {
					continue;
				}
				if (jacobi) {
					next[i] = s / k;
				} else {
					out.v[i] += 1.8f * (s / k - out.v[i]);
				}
			}
		}
		if (jacobi) {
			out.v.swap(next);
		}
	}
	for (size_t i = 0; i < known.size(); i++) {
		if (known[i]) {
			out.v[i] = in.v[i];
		}
	}
}

// ------------------------------------------------------------------ removing --

namespace {

// For every pixel, the nearest known one (eight ways), as an index; -1 where none is.
std::vector<int> NearestKnown(const std::vector<uint8_t>& known, int W, int H)
{
	std::vector<int> from(known.size(), -1);
	std::vector<int> queue;
	queue.reserve(known.size());
	for (size_t i = 0; i < known.size(); i++) {
		if (known[i]) {
			from[i] = (int)i;
			queue.push_back((int)i);
		}
	}
	for (size_t q = 0; q < queue.size(); q++) {
		const int i = queue[q], x = i % W, y = i / W;
		for (int dy = -1; dy <= 1; dy++) {
			for (int dx = -1; dx <= 1; dx++) {
				const int nx = x + dx, ny = y + dy;
				if (nx < 0 || ny < 0 || nx >= W || ny >= H) {
					continue;
				}
				const int j = ny * W + nx;
				if (from[j] < 0) {
					from[j] = from[i];
					queue.push_back(j);
				}
			}
		}
	}
	return from;
}

// The fine detail of the picture round the logo carried into the fill: at each unknown
// pixel, what the known pixel mirrored across the nearest edge has over the mean of the
// known pixels in the 5 x 5 round it. The fill keeps the picture's slow changes; this lends
// it the grain and the texture next to it, so that the logo's shape does not show as a
// smooth patch.
void AddTexture(const Maps& m, int plane, const Plane& obs, const std::vector<uint8_t>& unknown,
	const std::vector<uint8_t>& bar, const RemoveOptions& o, Plane& fill)
{
	const int W = obs.w, H = obs.h;
	std::vector<int> qx, qy;
	MirrorPlane(m, plane, o.halo, qx, qy);
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			const size_t i = (size_t)y * W + x;
			if (!unknown[i] || qx[i] < 0 || bar[i] || bar[(size_t)qy[i] * W + qx[i]]) {
				continue;
			}
			const int cx = qx[i], cy = qy[i];
			double sum = 0;
			int k = 0;
			for (int dy = -2; dy <= 2; dy++) {
				for (int dx = -2; dx <= 2; dx++) {
					const int nx = cx + dx, ny = cy + dy;
					if (nx >= 0 && ny >= 0 && nx < W && ny < H && !unknown[(size_t)ny * W + nx] && !bar[(size_t)ny * W + nx]) {
						sum += obs.at(nx, ny);
						k++;
					}
				}
			}
			const float low = k ? (float)(sum / k) : obs.at(cx, cy);
			fill.v[i] += o.texture * (obs.at(cx, cy) - low);
		}
	}
}

// The bar's pixels given the bar's level: each row's of a bar above or below the logo,
// each column's of one beside it, the mean of its pixels round the logo. Where a row has
// none, what the fill made of it stays.
void LevelBars(const Plane& obs, const std::vector<uint8_t>& unknown, const Bars& e, Plane& fill)
{
	const int W = obs.w, H = obs.h;
	auto level = [&](bool row, int k, float& out) {
		double s = 0;
		int n = 0;
		const int len = row ? W : H;
		for (int t = 0; t < len; t++) {
			const int x = row ? t : k, y = row ? k : t;
			if (!unknown[(size_t)y * W + x]) {
				s += obs.at(x, y);
				n++;
			}
		}
		if (n) {
			out = (float)(s / n);
		}
		return n > 0;
	};
	std::vector<float> rows(H, 0), cols(W, 0);
	std::vector<uint8_t> rowHas(H, 0), colHas(W, 0);
	for (int y = 0; y < H; y++) {
		if (y < e.top || y >= H - e.bottom) {
			rowHas[y] = level(true, y, rows[y]);
		}
	}
	for (int x = 0; x < W; x++) {
		if (x < e.left || x >= W - e.right) {
			colHas[x] = level(false, x, cols[x]);
		}
	}
	for (int y = 0; y < H; y++) {
		const bool inRow = y < e.top || y >= H - e.bottom;
		for (int x = 0; x < W; x++) {
			if (inRow) {
				if (rowHas[y]) {
					fill.at(x, y) = rows[y];
				}
			} else if ((x < e.left || x >= W - e.right) && colHas[x]) {
				fill.at(x, y) = cols[x];
			}
		}
	}
}

// Sums about the bar's usual level, which keeps a float's precision in the shader's.
constexpr float kBarOffset = 16.0f / 255;

} // namespace

Bars FindBars(const Maps& m, const Plane& luma, const RemoveOptions& o)
{
	Bars b;
	if (!m.valid()) {
		return b;
	}
	const Rect& r = m.box;
	if (r.x < 0 || r.y < 0 || r.x + r.w > luma.w || r.y + r.h > luma.h) {
		return b;
	}
	const int w = r.w, h = r.h;
	const std::vector<uint8_t> unknown = Unknown(m, 0, o.halo);
	// A row (or a column) of the box whose pixels round the logo are dark and flat.
	auto flat = [&](bool row, int k) {
		double s = 0, s2 = 0;
		int n = 0;
		const int len = row ? w : h;
		for (int t = 0; t < len; t++) {
			const int x = row ? t : k, y = row ? k : t;
			if (unknown[(size_t)y * w + x]) {
				continue;
			}
			const double d = luma.at(r.x + x, r.y + y) - kBarOffset;
			s += d;
			s2 += d * d;
			n++;
		}
		if (n < 4) {
			return false;
		}
		const double mean = s / n;
		return mean + kBarOffset < o.barDark && s2 / n - mean * mean < (double)o.barFlat * o.barFlat;
	};
	// From each edge of the box to the first row that is not.
	int first = h, last = -1;
	for (int y = 0; y < h; y++) {
		if (!flat(true, y)) {
			first = std::min(first, y);
			last = y;
		}
	}
	b.top = first;
	b.bottom = h - 1 - last;
	first = w;
	last = -1;
	for (int x = 0; x < w; x++) {
		if (!flat(false, x)) {
			first = std::min(first, x);
			last = x;
		}
	}
	b.left = first;
	b.right = w - 1 - last;
	return b;
}

void MirrorPlane(const Maps& m, int plane, int margin, std::vector<int>& qx, std::vector<int>& qy)
{
	const Plane& A = m.alpha[plane];
	const int W = A.w, H = A.h;
	const std::vector<uint8_t> unknown = Unknown(m, plane, margin);
	std::vector<uint8_t> known(unknown.size());
	for (size_t i = 0; i < known.size(); i++) {
		known[i] = !unknown[i];
	}
	const std::vector<int> from = NearestKnown(known, W, H);
	qx.assign(unknown.size(), -1);
	qy.assign(unknown.size(), -1);
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			const size_t i = (size_t)y * W + x;
			if (!unknown[i] || from[i] < 0) {
				continue;
			}
			const int kx = from[i] % W, ky = from[i] / W;
			int mx = 2 * kx - x, my = 2 * ky - y;
			if (mx < 0 || my < 0 || mx >= W || my >= H || !known[(size_t)my * W + mx]) {
				mx = kx;
				my = ky;
			}
			qx[i] = mx;
			qy[i] = my;
		}
	}
}

namespace {

// The doubt of the luma, times o.doubt, before it is rounded for the shader.
Plane LumaDoubt(const Maps& m, const RemoveOptions& o)
{
	const Plane& A = m.alpha[0];
	const int w = A.w, h = A.h;
	Plane D;
	D.resize(w, h, 0);
	if (m.excess.v.size() != A.v.size() || m.fillVar.size() < 2) {
		return D;
	}
	const std::vector<uint8_t> unknown = Unknown(m, 0, o.halo);
	std::vector<uint8_t> source(unknown.size());
	for (size_t i = 0; i < unknown.size(); i++) {
		source[i] = !unknown[i];
	}
	// Each pixel's distance from the picture it is filled from.
	const std::vector<int> depth = Distance(source, w, h);
	const int farthest = (int)m.fillVar.size() - 1;
	for (size_t i = 0; i < D.v.size(); i++) {
		if (!unknown[i]) {
			continue;
		}
		// The line's own error against the fill's from this far.
		const double f = m.fillVar[std::clamp(depth[i], 1, farthest)];
		D.v[i] = (float)(o.doubt * m.excess.v[i] / f);
	}
	return D;
}

} // namespace

// The luma's doubt; the chroma's, the mean of the luma's under each of its pixels.
Plane UnroundedDoubt(const Maps& m, int plane, const RemoveOptions& o)
{
	const Plane L = LumaDoubt(m, o);
	if (plane == 0) {
		return L;
	}
	const Plane& A = m.alpha[plane];
	Plane C;
	C.resize(A.w, A.h, 0);
	for (int cy = 0; cy < C.h; cy++) {
		for (int cx = 0; cx < C.w; cx++) {
			double s = 0;
			int k = 0;
			for (int dy = 0; dy < m.divH; dy++) {
				for (int dx = 0; dx < m.divW; dx++) {
					const int lx = cx * m.divW + dx, ly = cy * m.divH + dy;
					if (lx < L.w && ly < L.h) {
						s += L.at(lx, ly);
						k++;
					}
				}
			}
			C.at(cx, cy) = k ? (float)(s / k) : 0.0f;
		}
	}
	return C;
}

void Remove(const Maps& m, int plane, float s, const RemoveOptions& o, const Bars& bars, Plane& picture)
{
	if (!m.valid()) {
		return;
	}
	const Rect b = PlaneBox(m.box, plane, m.divW, m.divH);
	if (b.x < 0 || b.y < 0 || b.x + b.w > picture.w || b.y + b.h > picture.h) {
		return;
	}
	Plane obs;
	obs.resize(b.w, b.h);
	for (int y = 0; y < b.h; y++) {
		for (int x = 0; x < b.w; x++) {
			obs.at(x, y) = picture.at(b.x + x, b.y + y);
		}
	}
	const std::vector<uint8_t> unknown = Unknown(m, plane, o.halo);
	const Bars e = PlaneBars(bars, plane, m.divW, m.divH);
	const std::vector<uint8_t> bar = BarMask(e, b.w, b.h);
	Plane fill;
	Fill(obs, unknown, bar, o, fill);
	if (o.texture > 0) {
		AddTexture(m, plane, obs, unknown, bar, o, fill);
	}
	LevelBars(obs, unknown, e, fill);
	const Plane doubt = DoubtPlane(m, plane, o);
	const Plane& A = m.alpha[plane];
	const Plane& L = m.laid[plane];
	for (int y = 0; y < b.h; y++) {
		for (int x = 0; x < b.w; x++) {
			const size_t i = (size_t)y * b.w + x;
			if (!unknown[i]) {
				continue;
			}
			// Over a bar the fill is the bar's own level (LevelBars), and the inversion is
			// trusted as anywhere: over a real bar it gives the bar back, over a picture's
			// dark flat rows taken for one, the picture.
			// Every pixel worked is inverted with what the fit found there, the margin's
			// too: the faint edge the codec smeared is the logo's, under alphaMin or not.
			const float a = std::clamp(s * A.v[i], kAlphaLeast, 1.0f);
			const float laid = s * L.v[i];
			const float inv = a < 0.999f ? (obs.v[i] - laid) / (1 - a) : fill.v[i];
			const float d = (1 - a) * (1 - a);
			const float w = d / (d + s * s * doubt.v[i] + 1e-12f);
			picture.at(b.x + x, b.y + y) = std::clamp(w * inv + (1 - w) * fill.v[i], 0.0f, 1.0f);
		}
	}
}

float Presence(const Maps& m, const Plane& luma, const RemoveOptions& o, float* confidence)
{
	if (confidence) {
		*confidence = 0;
	}
	if (!m.valid()) {
		return 0;
	}
	const Rect& b = m.box;
	if (b.x < 0 || b.y < 0 || b.x + b.w > luma.w || b.y + b.h > luma.h) {
		return 0;
	}
	Plane obs;
	obs.resize(b.w, b.h);
	for (int y = 0; y < b.h; y++) {
		for (int x = 0; x < b.w; x++) {
			obs.at(x, y) = luma.at(b.x + x, b.y + y);
		}
	}
	const Bars bars = FindBars(m, luma, o);
	const std::vector<uint8_t> unknown = Unknown(m, 0, o.halo);
	const std::vector<uint8_t> bar = BarMask(bars, b.w, b.h);
	Plane fill;
	Fill(obs, unknown, bar, o, fill);
	LevelBars(obs, unknown, bars, fill);
	const Plane& A = m.alpha[0];
	const Plane& L = m.laid[0];
	// The logo at full strength would change a pixel by laid - alpha * picture.
	double num = 0, den = 0;
	int k = 0;
	for (size_t i = 0; i < A.v.size(); i++) {
		if (A.v[i] < m.alphaMin) {
			continue;
		}
		const double c = L.v[i] - A.v[i] * fill.v[i];
		num += c * (obs.v[i] - fill.v[i]);
		den += c * c;
		k++;
	}
	if (!k || den <= 0) {
		return 0;
	}
	if (confidence) {
		*confidence = (float)std::sqrt(den / k);
	}
	return (float)std::clamp(num / den, 0.0, 1.0);
}

// ------------------------------------------------------------ for the shader --

namespace {

// alpha and laid, from kAlphaLeast to 1, as 16-bit fractions of 1.
uint16_t Word(float v)
{
	return (uint16_t)std::lround(std::clamp((v - kAlphaLeast) / (1 - kAlphaLeast), 0.0f, 1.0f) * 65535.0f);
}

float Unword(uint16_t w)
{
	return kAlphaLeast + w / 65535.0f * (1 - kAlphaLeast);
}

} // namespace

// Whether a pixel is filled, and its doubt, in one 16-bit channel: 0 for a pixel left as
// it is, from a half up for one filled, the doubt d as 1/2 + d / (2 (1 + d)).
uint16_t FillWord(bool unknown, float doubt)
{
	if (!unknown) {
		return 0;
	}
	const float e = std::max(doubt, 0.0f) / (1 + std::max(doubt, 0.0f));
	return (uint16_t)std::lround(std::clamp(0.5f + 0.5f * e, 0.5f, 1.0f) * 65535.0f);
}

float DoubtOfWord(uint16_t w)
{
	const float e = std::min(2 * (w / 65535.0f) - 1, 0.999999f);
	return e / (1 - e);
}

Plane DoubtPlane(const Maps& m, int plane, const RemoveOptions& o)
{
	Plane d = UnroundedDoubt(m, plane, o);
	for (float& v : d.v) {
		v = DoubtOfWord(FillWord(true, v));
	}
	return d;
}

void ShaderMaps(const Maps& m, const RemoveOptions& o, std::vector<uint16_t>& luma, std::vector<uint16_t>& chroma,
	std::vector<uint16_t>& mirrorLuma, std::vector<uint16_t>& mirrorChroma)
{
	luma.clear();
	chroma.clear();
	mirrorLuma.clear();
	mirrorChroma.clear();
	if (!m.valid()) {
		return;
	}
	auto mirror = [&](int plane, std::vector<uint16_t>& out) {
		std::vector<int> qx, qy;
		MirrorPlane(m, plane, o.halo, qx, qy);
		out.resize(2 * qx.size());
		for (size_t i = 0; i < qx.size(); i++) {
			out[2 * i] = qx[i] < 0 ? 0xFFFF : (uint16_t)qx[i];
			out[2 * i + 1] = qy[i] < 0 ? 0xFFFF : (uint16_t)qy[i];
		}
	};
	const std::vector<uint8_t> uy = Unknown(m, 0, o.halo);
	const Plane dy = UnroundedDoubt(m, 0, o);
	luma.resize(4 * m.alpha[0].v.size());
	for (size_t i = 0; i < m.alpha[0].v.size(); i++) {
		luma[4 * i + 0] = Word(m.alpha[0].v[i]);
		luma[4 * i + 1] = Word(m.laid[0].v[i]);
		luma[4 * i + 2] = FillWord(uy[i] != 0, dy.v[i]);
		luma[4 * i + 3] = 0;
	}
	const std::vector<uint8_t> uc = Unknown(m, 1, o.halo);
	const Plane dc = UnroundedDoubt(m, 1, o);
	chroma.resize(4 * m.alpha[1].v.size());
	for (size_t i = 0; i < m.alpha[1].v.size(); i++) {
		chroma[4 * i + 0] = Word(m.alpha[1].v[i]);
		chroma[4 * i + 1] = Word(m.laid[1].v[i]);
		chroma[4 * i + 2] = Word(m.laid[2].v[i]);
		chroma[4 * i + 3] = FillWord(uc[i] != 0, dc.v[i]);
	}
	mirror(0, mirrorLuma);
	mirror(1, mirrorChroma);
}

void RoundMaps(Maps& m)
{
	for (int p = 0; p < 3; p++) {
		for (Plane* q : { &m.alpha[p], &m.laid[p] }) {
			for (float& v : q->v) {
				v = Unword(Word(v));
			}
		}
	}
	// One alpha for both chroma planes, as the shader has it.
	m.alpha[2] = m.alpha[1];
}

float PresenceStep(float previous, float measured, float confidence, const RemoveOptions& o)
{
	if (confidence < o.trust) {
		return previous;
	}
	return measured < o.snapLow ? 0.0f : measured > o.snapHigh ? 1.0f : measured;
}

} // namespace Logo
