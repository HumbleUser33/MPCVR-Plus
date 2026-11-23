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
#include <cmath>

// The HDR curve: one curve on the light of each pixel, in nits, for every picture that
// leaves in HDR -- HDR10, HDR10+, Dolby Vision, and what RTX Video HDR makes of an SDR
// one. It raises the shadows without touching the highlights, moves the mid grey with the
// peak held where it is, rolls the highlights off and caps them. A single exposure cannot
// do that: it scales the shadows and the highlights alike, and a dark scene lifted enough
// to be seen blows out the bright one after it. The curve gives the three channels one
// gain, worked out on the light, so a colour keeps its hue.
//
// The curve, its presets and the way the strength and the display's peak resolve them are
// Veyra's (github.com/Likely7/Veyra-NRVideo, GPL-3.0, commit c663312:
// include/veyra/engine/HdrOutputTuning.h and shaders/PresentBlit.hlsl), which applies them
// as here, on absolute light just before the PQ encoding. Veyra works in scRGB BT.709 and
// the filter in PQ BT.2020; the light of a pixel is the same either way.
//
// Pure functions, so that the filter, the settings page and the measurement tools
// (dlssnr_harness --thdrcurve) compute the same numbers. Shaders/d3d11/ps_hdr_curve.hlsl
// is the GPU side of Apply().

namespace HdrCurve {

// The presets, as iHdrCurvePreset stores them. Custom applies the values as typed.
// Appended only: the setting saves the number.
enum : int {
	Custom = 0,
	DarkLift,
	HighlightGuard,
	DarkAndBright,
	BrightRoom,
	DarkRoom,
	Punch,
	PresetCount
};

// The values' ranges, as Veyra has them. 203 nits is BT.2408's reference white, where the
// mid grey changes nothing; a roll-off's start or a cap of 0 is none.
constexpr int kLiftMin      = -200; // 1/100 EV
constexpr int kLiftMax      = 200;
constexpr int kRangeMin     = 1;    // nits
constexpr int kRangeMax     = 100;
constexpr int kRangeDef     = 10;
constexpr int kMidMin       = 100;  // nits
constexpr int kMidMax       = 500;
constexpr int kMidNeutral   = 203;
constexpr int kStartMin     = 100;  // nits, or 0
constexpr int kStartMax     = 2000;
constexpr int kRollMin      = 10;   // per cent; 100 closes the roll-off at the peak
constexpr int kRollMax      = 200;
constexpr int kRollDef      = 100;
constexpr int kCapMin       = 100;  // nits, or 0
constexpr int kCapMax       = 4000;
constexpr int kStrengthMin  = 0;    // per cent
constexpr int kStrengthMax  = 200;
constexpr int kStrengthDef  = 100;
// The display the presets are written for, and the peak the mid grey's curve holds when
// nothing caps the picture.
constexpr int kPresetPeakNits = 1000;

struct Values {
	int liftEv100      = 0;
	int rangeNits      = kRangeDef;
	int midGreyNits    = kMidNeutral;
	int startNits      = 0;
	int rollOffPercent = kRollDef;
	int capNits        = 0;
};

inline bool operator==(const Values& a, const Values& b)
{
	return a.liftEv100 == b.liftEv100 && a.rangeNits == b.rangeNits && a.midGreyNits == b.midGreyNits
		&& a.startNits == b.startNits && a.rollOffPercent == b.rollOffPercent && a.capNits == b.capNits;
}

// The values a Settings_t holds (IVideoRenderer.h), and back: a template, so that this
// header needs nothing of the filter's.
template <class Settings>
Values ValuesOf(const Settings& s)
{
	Values v;
	v.liftEv100      = s.iHdrCurveShadowLift;
	v.rangeNits      = s.iHdrCurveShadowRange;
	v.midGreyNits    = s.iHdrCurveMidGrey;
	v.startNits      = s.iHdrCurveHighlightStart;
	v.rollOffPercent = s.iHdrCurveRollOff;
	v.capNits        = s.iHdrCurvePeakCap;
	return v;
}

template <class Settings>
void StoreValues(Settings& s, const Values& v)
{
	s.iHdrCurveShadowLift     = v.liftEv100;
	s.iHdrCurveShadowRange    = v.rangeNits;
	s.iHdrCurveMidGrey        = v.midGreyNits;
	s.iHdrCurveHighlightStart = v.startNits;
	s.iHdrCurveRollOff        = v.rollOffPercent;
	s.iHdrCurvePeakCap        = v.capNits;
}

// As the settings page and the statistics name them.
inline const wchar_t* PresetName(int preset)
{
	switch (preset) {
	case DarkLift:       return L"Dark lift";
	case HighlightGuard: return L"Highlight guard";
	case DarkAndBright:  return L"Dark and bright";
	case BrightRoom:     return L"Bright room";
	case DarkRoom:       return L"Dark room";
	case Punch:          return L"Punch";
	}
	return L"Custom";
}

// Veyra's presets, for a 1000-nit display.
inline Values PresetValues(int preset)
{
	switch (preset) {
	//                             lift range  mid start roll   cap
	case DarkLift:       return {   50,   35, 225,    0, 100,    0 };
	case HighlightGuard: return {    0,   10, 203,  450, 120, 1000 };
	case DarkAndBright:  return {   50,   35, 220,  450, 120, 1000 };
	case BrightRoom:     return {   30,   30, 250,    0, 100,    0 };
	case DarkRoom:       return {   40,   35, 185,  600, 130,  800 };
	case Punch:          return {    0,   10, 175,  450, 110, 1200 };
	}
	return {};
}

inline int ClampOrZero(int v, int lo, int hi)
{
	return v <= 0 ? 0 : std::clamp(v, lo, hi);
}

// Values held to their ranges, whatever the registry held.
inline Values Clamped(Values v)
{
	v.liftEv100      = std::clamp(v.liftEv100, kLiftMin, kLiftMax);
	v.rangeNits      = std::clamp(v.rangeNits, kRangeMin, kRangeMax);
	v.midGreyNits    = std::clamp(v.midGreyNits, kMidMin, kMidMax);
	v.startNits      = ClampOrZero(v.startNits, kStartMin, kStartMax);
	v.rollOffPercent = std::clamp(v.rollOffPercent, kRollMin, kRollMax);
	v.capNits        = ClampOrZero(v.capNits, kCapMin, kCapMax);
	return v;
}

// A preset's highlight numbers follow the display: written for 1000 nits, they are scaled
// by its peak, so a 600-nit set starts rolling off sooner and caps lower, and a 2000-nit
// one keeps the light it can show. The shadow and mid-grey numbers describe the film and
// the room, not the display, and stay. The values typed by hand are not scaled: they say
// what they mean.
inline Values ForDisplay(Values v, int displayNits)
{
	if (displayNits <= 0) {
		return v;
	}
	const double scale = (double)displayNits / kPresetPeakNits;
	const auto scaled = [scale](int n, int lo, int hi) {
		return n ? std::clamp((int)std::lround(n * scale), lo, hi) : 0;
	};
	v.startNits = scaled(v.startNits, kStartMin, kStartMax);
	v.capNits   = scaled(v.capNits, kCapMin, kCapMax);
	return v;
}

// The values a preset, or Custom, stands for on this display, before the strength.
inline Values BaseValues(int preset, const Values& typed, int displayNits)
{
	return (preset > Custom && preset < PresetCount) ? ForDisplay(PresetValues(preset), displayNits) : Clamped(typed);
}

// What is applied: the strength takes each value towards no change, 0 % leaving the
// picture alone and 200 % doing twice as much. The roll-off's start moves the other way, a
// lower start being the stronger effect; the cap protects and does not follow it.
inline Values Applied(int preset, const Values& typed, int displayNits, int strengthPercent)
{
	Values v = BaseValues(preset, typed, displayNits);
	const double k = std::clamp(strengthPercent, kStrengthMin, kStrengthMax) / 100.0;
	if (k <= 0.001) {
		return {};
	}
	if (strengthPercent == 100) {
		return v;
	}
	const auto blend = [k](int base, int neutral) {
		return (int)std::lround(neutral + (base - neutral) * k);
	};
	v.liftEv100      = std::clamp(blend(v.liftEv100, 0), kLiftMin, kLiftMax);
	v.rangeNits      = std::clamp((int)std::lround(v.rangeNits * k), kRangeMin, kRangeMax);
	v.midGreyNits    = std::clamp(blend(v.midGreyNits, kMidNeutral), kMidMin, kMidMax);
	v.rollOffPercent = std::clamp(blend(v.rollOffPercent, kRollDef), kRollMin, kRollMax);
	if (v.startNits) {
		v.startNits = std::clamp((int)std::lround(v.startNits / k), kStartMin, kStartMax);
	}
	return v;
}

// ps_hdr_curve.hlsl's constant buffer.
struct Constants {
	float liftGain;  // 2^EV - 1: what the black end is multiplied by, less one; 0 lifts nothing
	float rangeNits; // where the lift has faded out
	float midGamma;  // the power that moves the mid grey; 1 leaves it
	float peakNits;  // what that power holds fixed: the cap, or 1000 nits
	float startNits; // where the roll-off starts; 0 is none
	float rollNits;  // how far above the start the highlights can go
	float capNits;   // the ceiling; 0 is none
	float padding;
};
static_assert(sizeof(Constants) == 32, "ps_hdr_curve.hlsl reads two float4");

inline Constants ShaderConstants(const Values& v)
{
	Constants c = {};
	c.midGamma = 1.0f;
	const double peak = v.capNits > 0 ? v.capNits : kPresetPeakNits;
	c.peakNits = (float)peak;
	if (v.liftEv100 != 0 && v.rangeNits > 0) {
		c.liftGain  = (float)(std::exp2(v.liftEv100 / 100.0) - 1.0);
		c.rangeNits = (float)v.rangeNits;
	}
	if (v.midGreyNits > 0 && v.midGreyNits != kMidNeutral && peak > v.midGreyNits + 1.0) {
		c.midGamma = (float)(std::log(v.midGreyNits / peak) / std::log(kMidNeutral / peak));
	}
	if (v.startNits > 0) {
		// The roll-off closes at the ceiling: with 100 % the highlights approach the cap,
		// or 1000 nits, and never pass it.
		c.startNits = (float)v.startNits;
		c.rollNits  = (float)(std::max(1.0, peak - v.startNits) * v.rollOffPercent / 100.0);
	}
	c.capNits = (float)v.capNits;
	return c;
}

// Nothing to do: the step is then not drawn at all.
inline bool Neutral(const Constants& c)
{
	return c.liftGain == 0.0f && c.midGamma == 1.0f && c.startNits == 0.0f && c.capNits == 0.0f;
}

// The curve on a pixel's light, in nits.
inline double Luminance(double nits, const Constants& c)
{
	if (c.liftGain != 0.0f && c.rangeNits > 0.0f) {
		const double w = std::clamp(1.0 - nits / c.rangeNits, 0.0, 1.0);
		nits *= 1.0 + w * w * c.liftGain;
	}
	if (c.midGamma != 1.0f) {
		nits = c.peakNits * std::pow(std::max(nits / c.peakNits, 0.0), (double)c.midGamma);
	}
	if (c.startNits > 0.0f && c.rollNits > 0.0f && nits > c.startNits) {
		const double d = nits - c.startNits;
		nits = c.startNits + d / (1.0 + d / c.rollNits);
	}
	if (c.capNits > 0.0f) {
		nits = std::min(nits, (double)c.capNits);
	}
	return nits;
}

// BT.2020's luminance weights.
constexpr double kLumR = 0.2627, kLumG = 0.6780, kLumB = 0.0593;

// The curve on a pixel, BT.2020 in nits: one gain for the three channels, from the light,
// and then no channel above the cap.
inline void Apply(double rgb[3], const Constants& c)
{
	const double y = kLumR * rgb[0] + kLumG * rgb[1] + kLumB * rgb[2];
	if (y <= 1e-6) {
		return;
	}
	const double gain = Luminance(y, c) / y;
	for (int i = 0; i < 3; i++) {
		rgb[i] *= gain;
	}
	if (c.capNits > 0.0f) {
		const double top = std::max({ rgb[0], rgb[1], rgb[2] });
		if (top > c.capNits) {
			for (int i = 0; i < 3; i++) {
				rgb[i] *= c.capNits / top;
			}
		}
	}
}

// The peak the HDR10 metadata says, with a cap: the picture now goes no higher, and a
// display told the film's own peak would compress the highlights a second time. 0, unknown,
// becomes the cap.
inline unsigned SentPeak(unsigned peakNits, const Constants& c)
{
	if (c.capNits <= 0.0f) {
		return peakNits;
	}
	const unsigned cap = (unsigned)c.capNits;
	return peakNits ? std::min(peakNits, cap) : cap;
}

} // namespace HdrCurve
