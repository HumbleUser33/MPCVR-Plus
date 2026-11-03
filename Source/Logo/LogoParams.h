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

#include <algorithm>
#include <cstdint>
#include <vector>
#include "LogoAnalysis.h"

// What the two compute shaders of the logo's hiding read (Shaders/d3d11/cs_logo_pack.hlsl,
// cs_logo_remove.hlsl), and how the analysis's maps are handed to them. The renderer and
// the measurement tools both take them from here.

namespace Logo {

// cs_logo_pack.hlsl, register b0. 32 bytes.
struct PackConstants {
	uint32_t offsetX, offsetY;
	uint32_t width, height;
	uint32_t scale;
	uint32_t pad[3];
};
static_assert(sizeof(PackConstants) == 32, "cs_logo_pack.hlsl's constant buffer");

// cs_logo_remove.hlsl, register b0. 304 bytes.
constexpr int kPyramidLevels = 16;   // the box's own included
struct RemoveConstants {
	uint32_t boxX, boxY, boxW, boxH;
	uint32_t levels, jacobi, update;
	float detail;   // RemoveOptions::texture
	float alphaMin, snapLow, snapHigh, trust;
	uint32_t level[kPyramidLevels][4];   // width, height, first entry, unused
};
static_assert(sizeof(RemoveConstants) == 304, "cs_logo_remove.hlsl's constant buffer");

// The pictures the analysis looks at and fits on: one every this many seconds, so that
// they differ. The logo is looked for from the kLocateFirst-th picture on, given up after
// kLocateMost; the first fit is made on kFitFirst pictures, the last on kFitPictures.
constexpr double kSampleSeconds = 0.4;
constexpr int kLocateFirst = 20;
constexpr int kLocateMost = 300;
constexpr int kFitFirst = 30;
constexpr int kFitPictures = 240;
// A logo kept is recognised when it is on screen on kRecogniseSure pictures where it
// stands out enough to tell; looked for on kRecogniseMost at most.
constexpr int kRecogniseSure = 4;
constexpr int kRecogniseMost = 20;

// The levels of the push-pull below a box, down to 1 x 1.
inline uint32_t PyramidLevels(int w, int h)
{
	uint32_t l = 0;
	while (w > 1 || h > 1) {
		w = (w + 1) / 2;
		h = (h + 1) / 2;
		l++;
	}
	return l;
}

// Entries of the pyramid buffer: two box-sized layers, then the levels.
inline uint32_t PyramidEntries(int w, int h)
{
	uint32_t n = 2u * w * h;
	while (w > 1 || h > 1) {
		w = (w + 1) / 2;
		h = (h + 1) / 2;
		n += (uint32_t)w * h;
	}
	return n;
}

inline RemoveConstants MakeRemoveConstants(const Maps& m, int plane, const RemoveOptions& o, bool update)
{
	RemoveConstants c = {};
	const int dw = plane ? m.divW : 1, dh = plane ? m.divH : 1;
	c.boxX = m.box.x / dw;
	c.boxY = m.box.y / dh;
	c.boxW = m.box.w / dw;
	c.boxH = m.box.h / dh;
	c.levels = std::min<uint32_t>(PyramidLevels(c.boxW, c.boxH), kPyramidLevels - 1);
	uint32_t w = c.boxW, h = c.boxH, first = 0;
	for (uint32_t l = 0; l <= c.levels; l++) {
		c.level[l][0] = w;
		c.level[l][1] = h;
		c.level[l][2] = first;
		first += l == 0 ? 2 * w * h : w * h;
		w = (w + 1) / 2;
		h = (h + 1) / 2;
	}
	c.jacobi = o.smooth;
	c.update = update ? 1 : 0;
	c.detail = o.texture;
	c.alphaMin = m.alphaMin;
	c.snapLow = o.snapLow;
	c.snapHigh = o.snapHigh;
	c.trust = o.trust;
	return c;
}

// The maps as the shader reads them, RGBA 16-bit: luma (alpha, laid, fill, 0), chroma
// (alpha, Cb laid, Cr laid, fill). Alpha and laid run from kAlphaLeast to 1; fill is 0 for
// a pixel left as it is, and from a half up for one filled, carrying its doubt (the logo's
// pixels and the halo round them: what the fill does not take from).
void ShaderMaps(const Maps& m, const RemoveOptions& o, std::vector<uint16_t>& luma, std::vector<uint16_t>& chroma,
	std::vector<uint16_t>& mirrorLuma, std::vector<uint16_t>& mirrorChroma);
// The mirror of each pixel worked, RG 16-bit: the box's pixel whose detail it takes,
// 0xFFFF where there is none (Logo::MirrorPlane).

// The maps rounded to what the shader is given, so that a reference in double precision
// works on the same numbers.
void RoundMaps(Maps& m);

} // namespace Logo
