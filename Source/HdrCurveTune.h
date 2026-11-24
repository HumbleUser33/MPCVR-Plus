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
#include <format>
#include <string>
#include <vector>
#include "HdrCurve.h"

// The HDR curve tuned to the film at hand: a few of its pictures, as the curve would receive
// them, and the values that bring them towards what real HDR masters look like.
//
// RTX Video HDR makes an HDR picture out of an SDR one by guessing, and its guess is the same
// for every film: one comes out with its shadows sunk, the next dim all through. A master
// graded for HDR is the reference the guess is held to. What is compared is what the film's
// content does not decide on its own:
//   - how far a picture's shadows sit under its key (its log-average light): ln(key / p10),
//     the depth. A dark scene graded well has its shadows as far under its own key as a
//     bright one; a guess that sinks them puts them further;
//   - the film's key over its pictures, the light its scenes are shown at, which a film's
//     content does move: so the mid grey is only moved when the film is far from the masters,
//     and part of the way;
//   - its peak against the display's, which needs no master: what the display cannot show is
//     rolled off under its peak, as BT.2390 has it, and nothing is limited that already fits.
// The targets are those of masters measured with the same tool (hdr_tune_targets.py).
//
// Each picture comes as two histograms of 1024 bins over the PQ signal, made on the GPU by
// Shaders/d3d11/cs_hdr_tune_hist.hlsl from the picture the curve's step reads: one of the
// light (BT.2020 luminance), one of the brightest channel. PQ bins are close to even steps of
// what the eye sees, about 2.6 % of the light near 0.1 nit and 1.4 % near 10.
//
// Every value is solved through HdrCurve::ShaderConstants() and HdrCurve::Luminance(), the
// curve as it ships, so what is solved for is what will be drawn. Pure functions, for the
// filter and for dlssnr_harness --thdrtune.

namespace HdrCurveTune {

constexpr int kBins = 1024; // HIST_BINS in cs_hdr_tune_hist.hlsl

// PQ, SMPTE ST 2084, in nits.
constexpr double kPqM1 = 2610.0 / 16384.0;
constexpr double kPqM2 = 2523.0 / 4096.0 * 128.0;
constexpr double kPqC1 = 3424.0 / 4096.0;
constexpr double kPqC2 = 2413.0 / 4096.0 * 32.0;
constexpr double kPqC3 = 2392.0 / 4096.0 * 32.0;

inline double PqToNits(double e)
{
	e = std::clamp(e, 0.0, 1.0);
	const double p = std::pow(e, 1.0 / kPqM2);
	return 10000.0 * std::pow(std::max(p - kPqC1, 0.0) / (kPqC2 - kPqC3 * p), 1.0 / kPqM1);
}

inline double NitsToPq(double nits)
{
	const double y = std::pow(std::clamp(nits / 10000.0, 0.0, 1.0), kPqM1);
	return std::pow((kPqC1 + kPqC2 * y) / (1.0 + kPqC3 * y), kPqM2);
}

// The PQ value at a bin's centre: the shader puts a value in the nearest bin, so that bin i
// holds i / 1023 give or take half a bin.
inline double BinPq(double bin)
{
	return std::clamp(bin / (kBins - 1), 0.0, 1.0);
}

// What the tune aims at, and where it leaves a film alone.
struct Targets {
	double depth;      // ln(key / p10): how far a picture's shadows sit under its key
	double depthBand;  // a film whose depth is within this of the target is not lifted
	double key;        // nits: the film's key, the median over its pictures
	double keyBand;    // ln: the mid grey left alone while the film's key is within this of it
};

// Six HDR10 and Dolby Vision masters of 30 to 40 s each (tools\TestBattery, 01 to 06), sixteen
// pictures each, passthrough and no tone mapping, measured on 24 Nov 2026 on an RTX 4060 by
// playback_test --hdrtunescan and read by hdr_tune_targets.py: all 96 pictures as one film.
// The masters one at a time: depth 1.91 to 2.66, and 3.62 for the darkest; key 0.89 to 6.84
// nits. The bands are half their interquartile range: short pieces of a scene or three, they
// are further apart than whole films would be.
//              depth  band   key   band
inline constexpr Targets kMasters = { 2.36, 0.46, 2.35, 0.71 };

// Under this a pixel is black: letterbox bars, fades, the black of an OLED. Left out of every
// percentile, or a film with bars would look darker than the same film cropped.
constexpr double kBlackNits = 0.01;
// The share of a picture's pixels above its peak: specks of light are not what a roll-off is
// for, and the display clips them as it would anyway.
constexpr double kPeakShare = 5e-4;
// A picture with less than this share lit says nothing of the film.
constexpr double kMinLitShare = 0.02;

// How far the film is taken towards the masters: three quarters of the way. All the way would
// make every film the same; the film's own look is kept a little.
constexpr double kPull = 0.75;
// The power the mid grey becomes, held here: lower is a flatter picture (washed out), higher
// a denser one.
constexpr double kGammaMin = 0.90;
constexpr double kGammaMax = 1.10;
// The shadows are raised, never pressed down, and by one EV at most. The lift fades out at
// twice the film's key: the shadows move, its mid tones hardly.
constexpr int kLiftLimit = 100;
constexpr int kRangeLow = 2;
constexpr int kRangeHigh = 40;
// The key moved by half an EV at most, and the contrast of the pictures (their 90th over their
// 10th percentile, in log) kept to 85 % of the film's: light given to a colour raises its
// colourfulness too, and a flatter picture reads as washed out.
constexpr double kContrastKept = 0.85;
constexpr double kKeyMoveLimit = 0.5 * 0.69314718055994531; // half an EV, in ln
// Where the highlights land under the cap: close, so that they keep most of their light, and
// not on it, so that a gradient keeps its top.
constexpr double kPeakLandLow = 0.95;
constexpr double kPeakLandHigh = 0.99;

// For the report only: a picture three times darker or brighter than the masters' key.
enum Kind : int { Black = 0, Dark, Normal, Bright };

inline const wchar_t* KindName(int kind)
{
	switch (kind) {
	case Dark:   return L"dark";
	case Normal: return L"normal";
	case Bright: return L"bright";
	}
	return L"black";
}

// One picture, from its two histograms.
struct Stats {
	double pixels = 0;
	double black = 1;    // share of the pixels under kBlackNits
	double p05 = 0, p10 = 0, p25 = 0, p50 = 0, p75 = 0, p90 = 0, p99 = 0; // nits, of the lit pixels
	double key = 0;      // log-average light of the lit pixels, nits
	double peak = 0;     // brightest channel, all but kPeakShare of the pixels, nits
	int kind = Black;
	bool Lit() const { return kind != Black && p10 > 0 && key > 0; }
};

// The share q of the pixels from bin `from` up, read where it falls inside its bin: a bin's
// pixels taken as spread evenly over its PQ width.
inline double Percentile(const unsigned* hist, int from, double count, double q)
{
	const double want = std::clamp(q, 0.0, 1.0) * count;
	double seen = 0;
	for (int i = from; i < kBins; i++) {
		const double n = hist[i];
		if (n > 0 && seen + n >= want) {
			const double inside = (want - seen) / n;
			return PqToNits(BinPq(i - 0.5 + inside));
		}
		seen += n;
	}
	return PqToNits(1.0);
}

inline Stats Measure(const unsigned* light, const unsigned* top, const Targets& t = kMasters)
{
	Stats s;
	// The first bin whose centre is lit.
	int lit = 0;
	while (lit < kBins && PqToNits(BinPq(lit)) < kBlackNits) {
		lit++;
	}
	double all = 0, dark = 0, logSum = 0;
	for (int i = 0; i < kBins; i++) {
		all += light[i];
		if (i < lit) {
			dark += light[i];
		} else if (light[i]) {
			logSum += light[i] * std::log(PqToNits(BinPq(i)));
		}
	}
	s.pixels = all;
	if (all <= 0) {
		return s;
	}
	const double count = all - dark;
	s.black = dark / all;
	s.peak = Percentile(top, 0, all, 1.0 - kPeakShare);
	if (count < kMinLitShare * all) {
		return s;
	}
	s.p05 = Percentile(light, lit, count, 0.05);
	s.p10 = Percentile(light, lit, count, 0.10);
	s.p25 = Percentile(light, lit, count, 0.25);
	s.p50 = Percentile(light, lit, count, 0.50);
	s.p75 = Percentile(light, lit, count, 0.75);
	s.p90 = Percentile(light, lit, count, 0.90);
	s.p99 = Percentile(light, lit, count, 0.99);
	s.key = std::exp(logSum / count);
	s.kind = s.key < t.key / 3 ? Dark : s.key > t.key * 3 ? Bright : Normal;
	return s;
}

inline double Median(std::vector<double> v)
{
	if (v.empty()) {
		return 0;
	}
	std::sort(v.begin(), v.end());
	const size_t n = v.size();
	return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

inline double Light(double nits, const HdrCurve::Values& v)
{
	return HdrCurve::Luminance(nits, HdrCurve::ShaderConstants(v));
}

// The film's depth, key and contrast through a curve: the median over its lit pictures, each
// picture's key and percentiles taken through it. Through the neutral curve, the film's own.
inline double DepthThrough(const std::vector<Stats>& pictures, const HdrCurve::Values& v)
{
	std::vector<double> d;
	for (const Stats& s : pictures) {
		if (s.Lit()) {
			d.push_back(std::log(Light(s.key, v) / Light(s.p10, v)));
		}
	}
	return Median(d);
}

inline double KeyThrough(const std::vector<Stats>& pictures, const HdrCurve::Values& v)
{
	std::vector<double> k;
	for (const Stats& s : pictures) {
		if (s.Lit()) {
			k.push_back(Light(s.key, v));
		}
	}
	return Median(k);
}

inline double ContrastThrough(const std::vector<Stats>& pictures, const HdrCurve::Values& v)
{
	std::vector<double> c;
	for (const Stats& s : pictures) {
		if (s.Lit()) {
			c.push_back(std::log(Light(s.p90, v) / Light(s.p10, v)));
		}
	}
	return Median(c);
}

// What the film's pictures say, together.
struct Film {
	int used = 0, lit = 0, dark = 0, normal = 0, bright = 0, black = 0;
	double depth = 0;    // D: median ln(key / p10) of the lit pictures
	double key = 0;      // K: median key of the lit pictures, nits
	double contrast = 0; // C: median ln(p90 / p10) of the lit pictures
	double peak = 0;     // P: the second brightest picture's peak
};

inline Film Summarise(const std::vector<Stats>& pictures)
{
	Film f;
	std::vector<double> peaks;
	for (const Stats& s : pictures) {
		if (s.pixels <= 0) {
			continue;
		}
		f.used++;
		peaks.push_back(s.peak);
		f.black += s.kind == Black;
		f.dark += s.kind == Dark;
		f.normal += s.kind == Normal;
		f.bright += s.kind == Bright;
		f.lit += s.Lit();
	}
	const HdrCurve::Values neutral;
	f.depth = DepthThrough(pictures, neutral);
	f.key = KeyThrough(pictures, neutral);
	f.contrast = ContrastThrough(pictures, neutral);
	// The second brightest: one picture with a flare in it does not set the cap of a film.
	std::sort(peaks.begin(), peaks.end());
	f.peak = peaks.empty() ? 0 : peaks.size() == 1 ? peaks.back() : peaks[peaks.size() - 2];
	return f;
}

// The roll-off and the cap. Nothing when the film's light fits the display: limiting what
// already fits would only dim it. Otherwise the cap is the display's peak and the roll-off
// starts at BT.2390's knee for this film on this display (its EETF's KS = 1.5 maxLum - 0.5,
// in PQ, relative to the film's peak): 4000 nits on an 800-nit display start at about 360.
inline void HighlightKnee(double filmPeak, int displayNits, HdrCurve::Values& v)
{
	v.startNits = 0;
	v.capNits = 0;
	v.rollOffPercent = HdrCurve::kRollDef;
	if (displayNits <= 0 || filmPeak <= displayNits * 1.05) {
		return;
	}
	const double pqFilm = NitsToPq(filmPeak);
	const double ks = std::max(1.5 * NitsToPq(displayNits) / pqFilm - 0.5, 0.0);
	const double start = PqToNits(ks * pqFilm);
	v.capNits = std::clamp(displayNits, HdrCurve::kCapMin, HdrCurve::kCapMax);
	v.startNits = std::clamp((int)std::lround(start), HdrCurve::kStartMin, std::max(HdrCurve::kStartMin, v.capNits - 1));
}

// The mid grey that takes the film's key K three quarters of the way to the target, as a
// power held to [kGammaMin, kGammaMax] and to half an EV of change at K itself. The curve's
// power pivots on the cap, or 1000 nits: its mid grey is the light 203 nits becomes, so the
// power is solved and the mid grey made from it.
inline int SolveMidGrey(double key, double target, double anchor)
{
	if (key <= 0 || target <= 0 || key >= anchor * 0.9) {
		return HdrCurve::kMidNeutral;
	}
	const double aim = key * std::pow(target / key, kPull);
	// K becomes anchor (K / anchor)^g: its change, in ln, is (g - 1) ln(K / anchor).
	const double reach = kKeyMoveLimit / std::abs(std::log(key / anchor));
	const double gamma = std::clamp(std::log(aim / anchor) / std::log(key / anchor),
		std::max(kGammaMin, 1.0 - reach), std::min(kGammaMax, 1.0 + reach));
	const int grey = (int)std::lround(anchor * std::pow(HdrCurve::kMidNeutral / anchor, gamma));
	return std::clamp(grey, HdrCurve::kMidMin, HdrCurve::kMidMax);
}

// The least lift, in 1/100 EV, that brings the film's depth to `aim`, the curve's other values
// as they are. One EV at most; none when it is there already.
inline int SolveLift(const std::vector<Stats>& pictures, HdrCurve::Values v, double aim)
{
	v.liftEv100 = 0;
	if (DepthThrough(pictures, v) <= aim) {
		return 0;
	}
	int lo = 0, hi = kLiftLimit;
	v.liftEv100 = hi;
	if (DepthThrough(pictures, v) > aim) {
		return hi;
	}
	while (hi - lo > 1) {
		v.liftEv100 = (lo + hi) / 2;
		(DepthThrough(pictures, v) <= aim ? hi : lo) = v.liftEv100;
	}
	return hi;
}

// The roll-off's per cent that lands the film's peak just under the cap. A higher per cent
// rolls off less, so the peak comes out brighter.
inline int SolveRollOff(HdrCurve::Values v, double filmPeak)
{
	if (!v.startNits || !v.capNits) {
		return HdrCurve::kRollDef;
	}
	const double low = kPeakLandLow * v.capNits, high = kPeakLandHigh * v.capNits;
	const double aim = 0.5 * (low + high);
	int lo = HdrCurve::kRollMin, hi = HdrCurve::kRollMax;
	v.rollOffPercent = hi;
	if (Light(filmPeak, v) < low) {
		return hi;
	}
	v.rollOffPercent = lo;
	if (Light(filmPeak, v) > high) {
		return lo;
	}
	while (hi - lo > 1) {
		v.rollOffPercent = (lo + hi) / 2;
		(Light(filmPeak, v) >= aim ? hi : lo) = v.rollOffPercent;
	}
	return hi;
}

struct Tuned {
	HdrCurve::Values values;
	Film film;
	double depthAfter = 0, keyAfter = 0, contrastAfter = 0, peakAfter = 0;
	bool guarded = false; // the guard took some of it back
	bool enough = false;  // the pictures said enough to tune on
};

// The least lit pictures to tune on.
constexpr int kMinPictures = 6;

inline Tuned Tune(const std::vector<Stats>& pictures, int displayNits, const Targets& t = kMasters)
{
	Tuned r;
	r.film = Summarise(pictures);
	const Film& f = r.film;
	HdrCurve::Values& v = r.values;
	v = HdrCurve::Values{};
	r.enough = f.lit >= kMinPictures;
	if (!r.enough) {
		return r;
	}

	// 1. The highlights, first: the cap is what the mid grey's power pivots on.
	HighlightKnee(f.peak, displayNits, v);
	const double anchor = v.capNits ? v.capNits : HdrCurve::kPresetPeakNits;

	// 2. The mid grey, where the film's key is far from the masters'.
	if (std::abs(std::log(f.key / t.key)) > t.keyBand) {
		v.midGreyNits = SolveMidGrey(f.key, t.key, anchor);
	}

	// 3. The lift, after the mid grey: a power under 1 raises the shadows more than the key,
	// and the depth is measured through it.
	const double depth = DepthThrough(pictures, v);
	if (depth > t.depth + t.depthBand) {
		v.rangeNits = std::clamp((int)std::lround(2.0 * KeyThrough(pictures, v)), kRangeLow, kRangeHigh);
		v.liftEv100 = SolveLift(pictures, v, t.depth + (depth - t.depth) * (1.0 - kPull));
	}

	// 4. The guard on the pictures' contrast: the lift taken back first, then the mid grey, a
	// step at a time.
	const auto Washed = [&]() {
		return f.contrast > 0 && ContrastThrough(pictures, v) < kContrastKept * f.contrast;
	};
	while (Washed() && v.liftEv100 > 0) {
		v.liftEv100 = std::max(0, v.liftEv100 - 10);
		r.guarded = true;
	}
	while (Washed() && v.midGreyNits != HdrCurve::kMidNeutral) {
		v.midGreyNits += v.midGreyNits > HdrCurve::kMidNeutral ? -std::min(5, v.midGreyNits - HdrCurve::kMidNeutral)
		                                                       : std::min(5, HdrCurve::kMidNeutral - v.midGreyNits);
		r.guarded = true;
	}
	if (!v.liftEv100) {
		v.rangeNits = HdrCurve::kRangeDef;
	}

	// 5. The roll-off, on the light the steps above leave the peak with.
	v.rollOffPercent = SolveRollOff(v, f.peak);
	v = HdrCurve::Clamped(v);

	r.depthAfter = DepthThrough(pictures, v);
	r.keyAfter = KeyThrough(pictures, v);
	r.contrastAfter = ContrastThrough(pictures, v);
	r.peakAfter = f.peak > 0 ? Light(f.peak, v) : 0;
	return r;
}

// For the notice and the statistics.
inline std::wstring Describe(const HdrCurve::Values& v)
{
	std::wstring s = v.liftEv100 ? std::format(L"lift {:+.2f} EV to {} nits", v.liftEv100 / 100.0, v.rangeNits) : L"no lift";
	s += std::format(L", mid grey {}", v.midGreyNits);
	s += v.startNits ? std::format(L", roll-off from {} ({}%)", v.startNits, v.rollOffPercent) : L", no roll-off";
	s += v.capNits ? std::format(L", cap {}", v.capNits) : L", no cap";
	return s;
}

} // namespace HdrCurveTune
