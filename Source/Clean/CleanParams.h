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

// Artifact removal on the decoded planes: what the five levels of each filter mean,
// and the constant block the two compute shaders read (Shaders/d3d11/cs_deblock.hlsl,
// cs_deband.hlsl). The renderer and the measurement tools both take them from here,
// so the tools measure the tables that ship.
//
// Both filters work on one plane at a time, at its own size, on the values the codec
// coded: luma, or the two chroma components of an interleaved plane together.
// Thresholds are given in 8-bit code levels of the plane's full range and the shaders
// divide them by 255, so a level means the same on a 10-bit source as on an 8-bit one.

namespace Clean {

// The constant buffer of both shaders, register b0. 48 bytes.
struct Constants {
	uint32_t width, height;   // the plane, in its own pixels
	uint32_t seed;            // deband: the picture's, so that a redraw keeps its grain
	uint32_t iterations;      // deband
	float threshold;          // deblock: the quantizer it undoes (ffmpeg's qp); deband: in 1/1000 of the range
	float radius;             // deband: in the plane's own pixels
	float grain;              // deband: in 1/1000 of the range
	float quantStep;          // 0: written at full precision; else the step the result is dithered to
	float neutral[2];         // deband: where no grain is added, per component (black for Y, mid for Cb/Cr)
	uint32_t mode;            // deblock: 0 hard, 1 soft, 2 medium thresholding
	uint32_t pad;
};
static_assert(sizeof(Constants) == 48, "the shaders' cbuffer is 48 bytes");

enum DeblockMode : uint32_t { DeblockHard = 0, DeblockSoft = 1, DeblockMedium = 2 };

struct DeblockLevel {
	float qp;                 // the quantizer it assumes the codec used
	uint32_t mode;
};

struct DebandLevel {
	uint32_t iterations;
	float threshold;          // 1/1000 of the range, as libplacebo
	float radius;             // pixels of the plane
	float grain;              // 1/1000 of the range
};

constexpr int kLevels = 5;

// The deblocking the renderer ships: cs_deblock.hlsl's MODE (0 SPP, 1 PP7) and, for
// SPP, its SHIFTS. PP7: on 1080p 4:2:0 on an RTX 4060 it costs 0.28 ms where SPP over
// 8 grids costs 0.88 and over 4 grids 0.46, and it keeps about two thirds of SPP-8's
// gain (MPEG-2 intra at q8: 44.45 dB untouched, 45.56 PP7, 46.02 SPP-8; --tclean --study).
constexpr int kDeblockMode = 1;
constexpr int kDeblockShifts = 8;

// Level k is the quantizer that does best on the k-th rung of a ladder of damage, every
// 8 x 8 block of each plane coded as an MPEG-2 intra block at quantiser_scale_code 2,
// 4, 8, 12 and 16 (--tclean --ladder, 16 pictures). The medium thresholds, FFmpeg's
// default for pp7: soft ones reach one to two tenths of a decibel more at their best and
// leave more of the blocks there (q8: 1.33 times the steps elsewhere across the grid,
// against 1.28; q16: 1.56 against 1.40), and at equal blocking the two are within 0.1 dB.
constexpr DeblockLevel kDeblock[kLevels] = {
	{ 1.5f, DeblockMedium },
	{ 2.5f, DeblockMedium },
	{ 4.0f, DeblockMedium },
	{ 6.0f, DeblockMedium },
	{ 8.0f, DeblockMedium },
};

// Five points along the banding the smoothing leaves against the detail it takes, on 88
// pictures through x264, x265 and SVT-AV1 (clean_refs.py; CAMBI, libvmaf's banding
// score, 10.0 and 11.5 untouched at 10 bits): 0.56, 0.33, 0.10, 0.05 and 0.02. Levels 1
// to 3 cost nothing measurable, 4 and 5 up to 0.3 dB. No grain: the smoothing alone takes
// out 93 to 99.8 % of the banding, and grain, which takes CAMBI to nothing, costs detail
// and makes still areas move from one picture to the next by half as much again
// (--tclean --study, section 5).
constexpr DebandLevel kDeband[kLevels] = {
	{ 1, 2.0f, 16.0f, 0.0f },
	{ 1, 4.0f, 16.0f, 0.0f },
	{ 2, 4.0f, 16.0f, 0.0f },
	{ 2, 6.0f, 20.0f, 0.0f },
	{ 3, 8.0f, 24.0f, 0.0f },
};

// The constants for one plane. level is 1..kLevels.
inline Constants DeblockConstants(int level, uint32_t width, uint32_t height, float quantStep)
{
	const DeblockLevel& l = kDeblock[(level < 1 ? 1 : level > kLevels ? kLevels : level) - 1];
	Constants c = {};
	c.width = width;
	c.height = height;
	c.threshold = l.qp;
	c.mode = l.mode;
	c.quantStep = quantStep;
	return c;
}

// neutral is where the grain fades out in the plane's normalized range: luma's black
// (16/255 limited, 0 full), or the chroma's middle code (128/255, 512/1023). libplacebo
// keeps its grain off true black the same way.
inline Constants DebandConstants(int level, uint32_t width, uint32_t height, float neutral,
	uint32_t seed, float quantStep)
{
	const DebandLevel& l = kDeband[(level < 1 ? 1 : level > kLevels ? kLevels : level) - 1];
	Constants c = {};
	c.width = width;
	c.height = height;
	c.seed = seed;
	c.iterations = l.iterations;
	c.threshold = l.threshold;
	c.radius = l.radius;
	c.grain = l.grain;
	c.quantStep = quantStep;
	c.neutral[0] = c.neutral[1] = neutral;
	return c;
}

} // namespace Clean
