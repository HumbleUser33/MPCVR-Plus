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

#include "../IVideoRenderer.h"

// Which of the lists the hardware video processor and the shaders share still have
// something to do, and whether the driver's Super Resolution can do anything.
//
// These five decide each other, and they are the ones a user notices when they are
// wrong. They live here rather than inside the page, and they are pure, so the bench
// can put the same question to them that the page puts -- for settings and for a
// state of the renderer that no film to hand would produce.
//
// bWin8 and bWin10 are IsWindows8OrGreater() and IsWindows10OrGreater(), passed in
// rather than asked here for the same reason.
//
// They are decided from the settings and from nothing else. The renderer can say
// what it is doing with the picture at hand, and the page used to listen, which read
// well and worked badly: a 4K film on a 4K screen leaves the processor nothing to
// resize, and a Dolby Vision one keeps it out of the chain altogether, so the page
// moved under the user as the film changed. Worse, it put Request Super Resolution
// out of reach -- with the processor out of the chain it greyed whatever the chroma
// pre-pass box said, and that box is the one it is supposed to answer. These are
// settings for every film there will ever be, so the film playing now does not get
// a vote -- with one exception, and it is a deliberate one. Dolby Vision is not a
// matter of degree: the picture carries Dolby Vision's own colour and its own
// reshaping, which a fixed-function processor cannot convert, so the renderer hands
// the whole chain to the shaders and the processor is not in it at all. Everything
// inside the processor can then do nothing, and everything the shaders do is theirs
// again -- which is a different page, not a shade of the same one. bDoViNow says so,
// and the page tells the user why in a line under the box.

struct Greying {
	bool bVPResizingBox;     // "Use the video processor for resizing"
	bool bChromaList;        // Chroma upsampling, and its label
	bool bUpscalingList;     // Upscaling, and its label
	bool bDownscalingList;   // Downscaling, and its label
	bool bSuperRes;          // Request Super Resolution, and its label
};

inline Greying GreyingFor(const Settings_t& s, bool bWin8, bool bWin10, bool bDoViNow)
{
	// What the video processor takes, the shaders never see. It converts the formats
	// ticked in Source, chroma upsampling included, and resizes as well when "Use the
	// video processor for resizing" is on -- except while DLSS 5 NR or DLSS Super
	// Resolution runs, which keeps it at the source size and hands the resizing back
	// to the shaders. DLSS Super Resolution enlarges in its place, so the Upscaling
	// method then only stands in where it cannot run.
	// "Replace the video processor's chroma upsampling" gives the chroma of a
	// progressive 4:2:0/4:2:2 picture back to the shaders, and only the chroma: the
	// processor is handed a 4:4:4 picture and goes on converting and resizing it.
	const bool bAllVPFormats = s.VPFmts.bNV12 && s.VPFmts.bP01x
		&& s.VPFmts.bYUY2 && s.VPFmts.bOther;
	const bool bVPAvailable = !(s.bUseD3D11 && !bWin8);   // no D3D11 VP on Windows 7
	const bool bDlssPass = s.bUseD3D11 && (s.bDlssNR || s.bDlssSR);
	const bool bChromaToShaders = s.bUseD3D11 && s.bVPReplaceChroma;
	const bool bVPTakesPicture = bAllVPFormats && bVPAvailable && !bDoViNow;
	const bool bVPConverts = bVPTakesPicture && !bChromaToShaders;
	const bool bVPResizes = bVPTakesPicture && s.bVPScaling && !bDlssPass;

	Greying g = {};
	g.bVPResizingBox = !bDoViNow;
	g.bChromaList = !bVPConverts;
	g.bDownscalingList = !bVPResizes;
	g.bUpscalingList = !bVPResizes && !(s.bUseD3D11 && s.bDlssSR);

	// RTX Video Super Resolution lives inside the processor and only does something
	// while the processor is the one enlarging the picture -- which is what "Use the
	// video processor for resizing" gives it, and what a DLSS pass takes away. It
	// also needs a subsampled picture: the driver leaves a 4:4:4 one untouched, so it
	// can do nothing while the chroma replacement is really handing one over.
#ifdef _WIN64
	g.bSuperRes = s.bUseD3D11 && bWin10 && bVPResizes && !bChromaToShaders;
#else
	g.bSuperRes = false;   // the extension is x64 only
#endif
	return g;
}
