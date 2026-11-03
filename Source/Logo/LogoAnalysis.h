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

#pragma once

#include <cstdint>
#include <vector>

// Hiding a channel's logo: finding it, telling it from the picture under it, and taking
// it out again. The renderer and the measurement tools both run this code, so the tools
// measure what ships.
//
// A broadcaster lays its logo over the picture the same way on every picture:
//
//   observed = (1 - alpha) * picture + alpha * colour
//
// with an alpha and a colour of its own for every pixel and every plane. That is the
// model of AviUtl's delogo and its analyser logoscan (MakKi), and what lets a translucent
// logo be taken out exactly rather than blurred over: with alpha and the colour known,
//
//   picture = (observed - alpha * colour) / (1 - alpha)
//
// Where alpha nears 1 there is nothing under the logo to find, and the picture there is
// filled in from what surrounds it.
//
// The codec's ringing fits the same line. Round a white logo it leaves the picture a
// little darker, the more so the darker the picture is against the logo: observed =
// (1 + b) picture - b * colour, an alpha below zero. So alpha is kept down to kAlphaLeast,
// and what the logo adds is kept as one number, alpha times its colour ("laid"), which
// stays steady where alpha is near zero and the colour alone would not.
//
// Everything here works on normalized code values, one plane at a time, at the plane's
// own size: luma, and each chroma plane at its subsampled size. Nothing converts colour.

namespace Logo {

// The least alpha kept: the codec's ringing round a logo, not a logo of its own.
constexpr float kAlphaLeast = -0.5f;

// A plane of normalized code values, row after row.
struct Plane {
	int w = 0, h = 0;
	std::vector<float> v;
	float& at(int x, int y) { return v[(size_t)y * w + x]; }
	float at(int x, int y) const { return v[(size_t)y * w + x]; }
	void resize(int W, int H, float fill = 0) { w = W; h = H; v.assign((size_t)W * H, fill); }
};

// A rectangle, in luma pixels.
struct Rect {
	int x = 0, y = 0, w = 0, h = 0;
	bool empty() const { return w <= 0 || h <= 0; }
};

// ------------------------------------------------------------------- finding --

// The pixels that keep an edge from one picture to the next, as LogoScan finds them: a
// logo stays where it is while the film changes under it. Fed with the luma of pictures
// taken far enough apart to differ, at a working size of at most 1920 pixels across.
class CLocator
{
public:
	struct Options {
		float edge = 0.02f;        // an edge: Sobel slope over this, in full-range code values a pixel
		float share = 0.45f;       // a logo's edge: an edge in at least this share of the pictures
		float merge = 1.0f / 160;  // letters this far apart (a share of the width) make one logo
		float most = 0.25f;        // a logo is at most this share of the width and of the height
		float busy = 0.02f;        // more of the picture than this kept its edges: it hardly changed
		int margin = 3;            // edges this close to the picture's border are not a logo's
	};

	struct Result {
		bool found = false;
		bool still = false;        // the pictures hardly changed: too soon to tell
		Rect box;                  // the logo with a margin round it, in full-size luma pixels
		Rect picture;              // the picture inside any black bars, full-size
		float score = 0;           // the logo's edges, weighted by their share
		int edgePixels = 0;
	};

	// scale: full size / working size (1 or 2); edge: Options::edge, which counting needs.
	void Reset(int W, int H, int scale, float edge);
	// One picture's luma at the working size.
	void Add(const Plane& luma);
	int Pictures() const { return m_n; }
	int Width() const { return m_W; }
	int Height() const { return m_H; }
	int Scale() const { return m_scale; }
	Result Locate(const Options& o) const;

	// The share of pictures with an edge, and the spread of each pixel over time: for the
	// bench's pictures of what was found.
	void Maps(Plane& share, Plane& spread) const;

private:
	int m_W = 0, m_H = 0, m_scale = 1, m_n = 0;
	float m_edge = 0.02f;
	std::vector<float> m_count, m_sum, m_sum2;
};

// ------------------------------------------------------------- disentangling --

// What a picture under the logo is taken to have been, from the ring of pixels round
// the logo's box (and, after a first fit, from the pixels inside it that the logo
// leaves alone).
enum Background : int {
	BackgroundFlat = 0,      // the ring's trimmed mean, as logoscan takes it
	BackgroundPlane,         // a plane fitted to the ring, as LogoScan takes it
	BackgroundHarmonic,      // the ring carried smoothly across the box (Laplace)
};

// One picture's box, every plane at its own size: luma, then Cb and Cr.
struct Crop {
	Plane p[3];
};

// The logo, every plane at its own size, per pixel: alpha, and what it lays over the
// picture, alpha times its colour: observed = (1 - alpha) * picture + laid.
//
// And what tells how far the inversion of a pixel is to be doubted against the fill: the
// error the line has of its own at each luma pixel (excess: what it left of the pixel when
// it was fitted, less what the picture it was fitted against got wrong there), and the
// fill's own error by how far a pixel is from the picture it is filled from (fillVar, from
// 1 pixel on). The line holds well inside a translucent logo and on the picture round it,
// and badly on the edge of an opaque one, where the codec smears logo and picture together
// differently on every picture; there the fill is the better guess. Kept apart, so that
// the doubt follows the margin chosen.
struct Maps {
	Rect box;                // luma pixels; the chroma box is box / (divW, divH)
	int divW = 2, divH = 2;
	Plane alpha[3], laid[3];
	Plane excess;            // luma, a variance, in full-range code values
	std::vector<float> fillVar;
	float noise = 1.5f / 255;
	float alphaMin = 0.04f;  // closer to zero than this, a pixel is not the logo's
	int pictures = 0;        // pictures the fit was made on
	bool valid() const { return !box.empty() && alpha[0].w == box.w; }
};

class CDeblender
{
public:
	struct Options {
		Background background = BackgroundHarmonic;
		int ring = 2;              // luma pixels of the box's border read as the picture
		float noise = 1.5f / 255;  // the codec's noise, in full-range code values
		int passes = 3;            // fit, drop pictures without the logo, fit again...
		bool chromaFromLuma = true;// chroma's alpha from luma's, its colour fitted alone
		float illPosed = 0.004f;   // the picture under a pixel varied less than this (std): alpha unknown
	};

	void Reset(const Rect& box, int divW, int divH);
	void Add(const Crop& c);
	int Pictures() const { return (int)m_crops.size(); }
	const Rect& Box() const { return m_box; }
	bool Fit(const Options& o, Maps& out);
	// How many pictures the last fit left out as showing no logo.
	int Dropped() const { return m_dropped; }

private:
	Rect m_box;
	int m_divW = 2, m_divH = 2;
	std::vector<Crop> m_crops;
	int m_dropped = 0;
};

// The picture under the box's unknown pixels, carried in from the known ones.
void FillHarmonic(const Plane& in, const std::vector<uint8_t>& known, Plane& out, int iterations);
// Push-pull (Gortler et al.): the known pixels averaged down a pyramid, the holes filled
// on the way back up. kernel 0: 2 x 2 boxes and bilinear; 1: [1 3 3 1] and bilinear.
void FillPushPull(const Plane& in, const std::vector<float>& weight, Plane& out, int kernel);

// ------------------------------------------------------------------ removing --

// The margin: luma pixels round the logo worked as well, for the edge the codec smeared.
constexpr int kMarginLeast = 0, kMarginMost = 3, kMarginDefault = 1;

struct RemoveOptions {
	float doubt = 30.0f;       // the doubt measured, times this: w = (1-a)^2 / ((1-a)^2 + s^2 doubt)
	int halo = kMarginDefault; // the margin, luma pixels
	int fill = 1;              // 0 harmonic (reference), 1 push-pull [1 3 3 1], 2 push-pull 2 x 2
	int smooth = 0;            // Jacobi steps after the push-pull
	float texture = 0.5f;      // fine detail mirrored into the fill from across the logo's edge
	float snapLow = 0.15f;     // a strength measured under this is no logo on screen
	float snapHigh = 0.85f;    // over this, the whole logo
	float trust = 0.03f;       // the logo must stand out from the picture by this (RMS) to be measured
};

// The logo taken out of one plane of a picture, at strength s (0: no logo on screen).
// plane: 0 luma, 1 Cb, 2 Cr. The plane is the whole picture's; only the box changes.
void Remove(const Maps& m, int plane, float s, const RemoveOptions& o, Plane& picture);

// How strongly the logo is on screen in this picture, from its luma: the least-squares
// strength that explains the pixels under the logo against the picture filled in round it.
// confidence: how much the logo stood out from what it covered (0: nothing to go by).
float Presence(const Maps& m, const Plane& luma, const RemoveOptions& o, float* confidence);

// The strength kept from one picture to the next: the one measured, snapped to none and to
// all near either end, or the last one where the logo did not stand out enough to tell.
float PresenceStep(float previous, float measured, float confidence, const RemoveOptions& o);

// The pixels of one plane worked with this margin: the logo's, the margin round them, and
// the codec's ringing (alpha below zero).
std::vector<uint8_t> Unknown(const Maps& m, int plane, int margin);
// How far each of them is to be doubted, times RemoveOptions::doubt, as the shader is given it.
Plane DoubtPlane(const Maps& m, int plane, const RemoveOptions& o);
// For each of them, the pixel of the box whose detail is mirrored into it: the known pixel
// mirrored across the nearest edge, or that pixel itself where the mirror falls on the logo
// or outside the box; x = -1 where there is none.
void MirrorPlane(const Maps& m, int plane, int margin, std::vector<int>& qx, std::vector<int>& qy);

} // namespace Logo
