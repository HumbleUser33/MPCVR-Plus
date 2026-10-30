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

// CDX11VideoProcessor: the statistics, laid out as the pipeline with each step's GPU time.
// All of it is this fork's, and kept out of DX11VideoProcessor.cpp so that a merge of
// upstream's file meets as little of ours as possible (MERGING-UPSTREAM.md).

#include "stdafx.h"
#include "Helper.h"
#include "Times.h"
#include "resource.h"
#include "VideoRenderer.h"
#include "DX11VideoProcessor.h"
#include "DX11VideoProcessorShared.h"
#include "Shaders.h"

// The statistics follow a picture: what comes in, each step it goes through in the
// order Process() takes them, with what each step costs on its own line, and then how
// it reaches the screen. The costs used to sit in lines of their own at the bottom,
// grouped by where their code came from -- DLSS SR's under "DLSS", FSRCNNX's under
// "Prescale" -- and a step was described in one place and timed in another.
std::wstring CDX11VideoProcessor::GetStatsText()
{
	// A label, then the text, then the time in a column of its own, right-aligned to
	// the end of the rules that head the blocks. Wide enough for the usual steps to keep
	// to one line ("D3D11 VP to R10G10B10A2_UNORM, converts chroma, resizes"), and well
	// under kStatsMaxColumns.
	constexpr size_t kLabel = 14;
	constexpr size_t kTimeEnd = 80;
	constexpr size_t kTime = 6;

	std::wstring str;
	str.reserve(1600);
	str.assign(m_strStatsHeader);
	str.append(m_strStatsDispInfo);
	str += std::format(L"\nGraph. Adapter: {}", m_strAdapterDescription);

	const auto Rule = [&](const std::wstring& title, const wchar_t* right) {
		std::wstring line = L"-- " + title + L' ';
		const size_t tail = right ? wcslen(right) + 1 : 0;
		line.append(line.size() + tail + 2 <= kTimeEnd ? kTimeEnd - line.size() - tail : 2, L'-');
		if (right) {
			line += L' ';
			line += right;
		}
		str += L'\n';
		str += line;
	};

	// The pipeline's lines. ms < 0: no time to show. A time is counted in the total
	// unless it is the total.
	//
	// The text gets the room left before the time column, broken between words, and
	// what does not fit goes on the lines under it, under the text: a step that has a lot
	// to say -- "(No Super Resolution when chroma by shaders)" -- once pushed its time out
	// of the column and the box. Lines of its own the text brings ("\n ...", the HDR
	// metadata) follow as they are.
	int step = 0;
	double total = 0;
	bool bTimed = false;
	const auto Line = [&](const std::wstring& label, const std::wstring& text, double ms, bool bCount) {
		std::wstring line = label;
		line.resize(std::max(line.size(), kLabel), L' ');
		line += L": ";
		const size_t indent = line.size();
		const size_t room = kTimeEnd - kTime - 1 - indent;
		const size_t own = text.find(L'\n');
		const std::wstring first = text.substr(0, own);
		std::wstring more;   // the lines the text could not keep on the first
		size_t pos = 0;
		while (pos < first.size()) {
			size_t end = first.size();
			if (end - pos > room) {
				// The last space that leaves the line within the room, or a hard cut
				// for a word longer than the room itself.
				const size_t space = first.rfind(L' ', pos + room);
				end = (space != std::wstring::npos && space > pos) ? space : pos + room;
			}
			std::wstring piece = first.substr(pos, end - pos);
			if (!pos) {
				line += piece;
			} else {
				more += L'\n';
				more.append(indent, L' ');
				more += piece;
			}
			pos = end;
			while (pos < first.size() && first[pos] == L' ') {
				pos++;
			}
		}
		if (ms >= 0) {
			line.append(kTimeEnd - kTime - line.size(), L' ');
			line += std::format(L"{:6.1f}", ms);
			if (bCount) {
				total += ms;
				bTimed = true;
			}
		}
		str += L'\n';
		str += line;
		str += more;
		if (own != std::wstring::npos) {
			str.append(text, own);
		}
	};
	const auto Step = [&](const wchar_t* name, const std::wstring& text, double ms) {
		Line(std::format(L"{} {}", ++step, name), text, ms, true);
	};
	const auto SubStep = [&](const wchar_t* name, const std::wstring& text, double ms) {
		Line(std::format(L"    {}", name), text, ms, true);
	};
	const auto Ms = [&](CGpuStageTimes::Stage stage) { return m_DlssStageTimes.MeanMs(stage); };
	// Two stages shown as one figure; negative when neither ran lately.
	const auto Sum = [](double a, double b) { return (a < 0 && b < 0) ? -1.0 : std::max(a, 0.0) + std::max(b, 0.0); };

	// -- What comes in.
	Rule(L"Source", nullptr);
	str.append(m_strStatsInputFmt);
	if (m_Dovi.bValid && m_Dovi.bHasMMR) {
		str.append(L", MMR");
	}
	const wchar_t frametype = (m_SampleFormat != D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE) ? 'i' : 'p';
	str += std::format(L"\nFrame rate    : {:7.3f}{} in, {:7.3f} drawn",
		m_pFilter->m_FrameStats.GetAverageFps(), frametype, m_pFilter->m_DrawStats.GetAverageFps());

	str.append(m_OsdPlus.Text());

	// -- Each step, in the order the picture goes through them.
	Rule(std::format(L"Pipeline, in {}", DXGIFormatToString(m_InternalTexFmt)), L"GPU ms");
	const bool bSubsampled = m_srcParams.Subsampling == 420 || m_srcParams.Subsampling == 422;

	// The chroma rebuilt by the shaders and handed to the processor in 4:4:4, as the
	// sample is copied.
	if (m_D3D11VP.IsReady() && m_bChromaReplacedVP && bSubsampled) {
		Step(L"Chroma", std::format(L"{} -> 4:4:4 {}, for the processor",
			m_srcParams.Subsampling == 420 ? L"4:2:0" : L"4:2:2", ChromaScalingName()), Ms(CGpuStageTimes::Prepass));
	}

	// The conversion: the processor converts the picture itself, its chroma included
	// unless the step above took that, and resizes it in the same call when it is the one
	// resizing -- its time is then the resizing's too, and the Scaling step has none of its
	// own. Otherwise the shaders convert, with the Chroma upsampling method, a
	// prescaler's time included.
	const UINT dstW = m_videoRect.Width();
	const UINT dstH = m_videoRect.Height();
	const bool bResized = m_srcRectWidth != dstW || m_srcRectHeight != dstH;
	// The processor resizes on its first turn, or on its second after DLSS 5 NR.
	const bool bVPResizesHere = m_D3D11VP.IsReady() && VPResizesFirst() && !m_bVPScalingUseShaders && !m_bVPScaled;
	if (m_D3D11VP.IsReady()) {
		std::wstring text = std::format(L"D3D11 VP to {}", DXGIFormatToString(m_D3D11OutputFmt));
		if (bSubsampled && !m_bChromaReplacedVP) {
			// Asked for and not done: interlaced pictures have come, and this driver
			// deinterlaces NV12 alone (ChromaToShaders); or a format it does not take.
			text += !m_bVPReplaceChroma ? L", converts chroma"
				: (m_bInterlaced && m_bInterlacedSeen) ? L", converts chroma (interlaced pictures)"
				: L", converts chroma (not replaced in this format)";
		}
		if (bVPResizesHere && bResized) {
			text += L", resizes";
		}
		if (m_bVPPreEnlarged) {
			text += std::format(L", to {}x{} for RTX Video HDR", m_TexConvertOutput.desc.Width, m_TexConvertOutput.desc.Height);
		}
		Step(L"Processor", text, Ms(CGpuStageTimes::VP));
	} else if (m_PSConvColorData.bEnable) {
		std::wstring text = L"shaders";
		if (bSubsampled) {
			text += L", chroma ";
			text += ChromaScalingName();
		}
		Step(L"Convert", text, Ms(CGpuStageTimes::ShaderConvert));
	}

	// DLSS 5 NR where it runs: at the source's size before the resizing, or after it.
	// A session that is not running says why, where it would have run.
	const auto DlssNRSteps = [&](bool bAfterUpscale) {
		if (!m_bDlssNR) {
			return;
		}
		if (!m_bDlssNRActive) {
			if (!bAfterUpscale) {
				Step(L"DLSS 5 NR", m_DlssNR.GetStatusLine(), -1);
			}
			return;
		}
		if (bAfterUpscale != m_bDlssNRAfterUpscale) {
			return;
		}
		static const wchar_t* const styles[] = { L"default", L"natural", L"cinematic" };
		const wchar_t* style = (m_DlssParams.iStyle >= 0 && m_DlssParams.iStyle < (int)std::size(styles))
			? styles[m_DlssParams.iStyle] : L"?";
		// Timed on the Direct3D 12 device it runs on, by timestamps on its own queue.
		Step(L"DLSS 5 NR", std::format(L"{}/P{} i{:.2f} t{:.2f} s{:.2f} k{:.2f}{}{}",
			style, m_DlssParams.iPreset, m_DlssParams.fIntensity, m_DlssParams.fLocalTone,
			m_DlssParams.fLocalStructure, m_DlssParams.fSkinStructure,
			m_DlssParams.bUseAutoMask ? L" auto" : L"", bAfterUpscale ? L", after upscaling" : L""),
			m_DlssNRTimes.Mean());
		if (m_DlssCompose.IsCreated()) {
			// The reduction and the composition at a working size below the picture's.
			SubStep(L"working", std::format(L"{} % ({}x{}), reduced and carried back", m_iDlssNRWorkingScale,
				m_DlssCompose.WorkWidth(), m_DlssCompose.WorkHeight()),
				Sum(Ms(CGpuStageTimes::NRReduce), Ms(CGpuStageTimes::NRCompose)));
		}
		if (m_iDlssNRStabilizer > 0) {
			SubStep(L"stabilizer", std::format(L"{}, {}{}", m_iDlssNRStabilizer,
				m_DlssStabilizer.IsCreated() ? m_DlssStabilizer.GetStatusLine() : std::wstring(L"not started"),
				m_DlssStabilizer.GetMotionVectors() ? L", vectors to DLSS" : L""),
				Sum(Ms(CGpuStageTimes::NRMotion), Ms(CGpuStageTimes::NRStabilize)));
		}
	};
	DlssNRSteps(false);

	// The resizing, whoever does it, with all it cost: the resize shader, and before it
	// the processor's second turn, DLSS SR or an mpv prescaler.
	{
		// From the size the shaders receive: the source's, or what the processor left
		// where it enlarged a small picture for RTX Video HDR (the Processor step says so).
		const UINT fromW = m_bVPPreEnlarged ? m_TexConvertOutput.desc.Width : m_srcRectWidth;
		const UINT fromH = m_bVPPreEnlarged ? m_TexConvertOutput.desc.Height : m_srcRectHeight;
		std::wstring text = m_iRotation
			? std::format(L"{}x{} r{}°> {}x{}", fromW, fromH, m_iRotation, dstW, dstH)
			: std::format(L"{}x{} -> {}x{}", fromW, fromH, dstW, dstH);
		// DLSS SR names itself in the shader names, DLAA included, which runs at no change of size.
		const auto IsDlssSR = [](const wchar_t* name) { return name && !wcsncmp(name, L"DLSS SR", 7); };
		const bool bDlssSR = m_bDlssSRActive && m_DlssSR.IsFeatureReady()
			&& (IsDlssSR(m_strShaderX) || IsDlssSR(m_strShaderY));
		double extra = -1;
		if (bResized || bDlssSR) {
			// With DLSS Super Resolution on, the video processor never scales, and the
			// resize after DLSS reads as no scaling at all.
			const bool bSecondTurn = m_bVPScaled;
			if (bVPResizesHere || bSecondTurn) {
				text.append(L" D3D11");
				if (m_srcRectWidth < dstW && m_srcRectHeight < dstH) {
					// The driver is asked for it exactly as upstream asks, and answers
					// that it will; on a 4:4:4 picture it then leaves the picture alone,
					// so the line says which of the two happened rather than starring
					// work nobody did.
					const bool bChromaKept = bSecondTurn ? m_VPScaleKey.bKeepChroma : m_bChromaReplacedVP;
					if (bChromaKept && m_iVPSuperRes != SUPERRES_Disable) {
						text.append(L" (No Super Resolution when chroma by shaders)");
					} else if (bSecondTurn ? m_bVPScaleSuperRes : m_bVPUseSuperRes) {
						text.append(L" SuperResolution*");
					}
				}
				// On its first turn the processor resizes in the call that converts, and
				// the Processor step's time is the resizing's too. It writes the picture
				// at its size into a texture of its own, and the resize shader then only
				// copies it to its place in the window -- a film with black bars, or one
				// in a window -- which is the time this step shows: 0.5 ms for a 4K
				// picture, read as Super Resolution's until the line said so.
				if (bSecondTurn) {
					extra = Ms(CGpuStageTimes::VPScale);
				} else if (Ms(CGpuStageTimes::Resize) >= 0) {
					text.append(L", placed by the shaders");
				}
			} else if (bDlssSR) {
				// Its quality mode and preset, DLAA among them: the last word of its own line.
				const std::wstring sr = m_DlssSR.GetStatsLine();
				const size_t space = sr.rfind(L' ');
				text += L" DLSS SR ";
				text.append(space == std::wstring::npos ? sr : sr.substr(space + 1));
				extra = Ms(CGpuStageTimes::SR);
			} else {
				text += L' ';
				if (m_strShaderX) {
					text.append(m_strShaderX);
					if (m_strShaderY && m_strShaderY != m_strShaderX) {
						text += L'/';
						text.append(m_strShaderY);
					}
				} else if (m_strShaderY) {
					text.append(m_strShaderY);
				}
				if (m_bMpvLumaRan) {
					extra = Ms(CGpuStageTimes::MpvLuma);
				}
			}
		}
		Step(L"Scaling", text, Sum(extra, Ms(CGpuStageTimes::Resize)));
		if (bDlssSR && !m_strDlssSRMotion.empty()) {
			SubStep(L"motion", m_strDlssSRMotion, Ms(CGpuStageTimes::SRMotion));
		} else if (m_bDlssSR && !bDlssSR) {
			// Ticked and not enlarging: why, or that nothing needed enlarging.
			SubStep(L"DLSS SR", m_DlssSR.GetStatsLine(), -1);
		}
	}

	DlssNRSteps(true);

	// Everything after the resize, at the size the picture is shown at.
	{
		std::wstring text;
		const auto Add = [&](const std::wstring& what) {
			if (!text.empty()) {
				text += L", ";
			}
			text += what;
		};
		if (m_strCorrection) {
			Add(m_strCorrection);
		}
		if (m_pPSHDR10ToneMapping) {
			Add(L"tone mapping");
		}
		if (m_pPSSharpen) {
			Add(std::format(L"{} {}", m_iSharpen == SHARPEN_UnsharpClamp ? L"Unsharp + Clamp" : L"Adaptive-Sharpen",
				m_iSharpenLevel));
		}
		if (m_pPostScaleShaders.size()) {
			Add(std::format(L"shaders[{}]", m_pPostScaleShaders.size()));
		}
		if (m_bDitherUsed) {
			Add(L"dither");
		}
		if (!text.empty()) {
			Step(L"Finish", text, Ms(CGpuStageTimes::Finish));
		}
	}

	// What the steps cost together, against the time a picture lasts: the first thing
	// to look at when pictures come late.
	const double frameMs = m_pFilter->m_FrameStats.GetAverageFrameDuration() / 10000.0 / (m_bDoubleFrames ? 2 : 1);
	if (bTimed) {
		Line(L"  Total", frameMs > 0 ? std::format(L"for a frame of {:.1f} ms", frameMs) : std::wstring(), total, false);
	}

	// What becomes of HDR is not a pass of its own, so it has no number and no time and
	// comes after the total: passthrough is nothing to compute, RTX Video HDR runs inside
	// the processor and is in its time, and a tone mapping or a conversion to SDR is in
	// Finish.
	if (!m_strStatsHDR.empty()) {
		// The star says the driver took RTX Video HDR on; it then leaves out, without a
		// word, a picture the processor writes in fewer than 640x360 pixels
		// (kRtxHdrMinPixels). Where that is what happens, the line says so.
		std::wstring hdr = m_strStatsHDR;
		if (m_bVPUseRTXVideoHDR && m_D3D11VP.IsReady()) {
			const UINT64 written = VPResizesFirst()
				? (UINT64)m_videoRect.Width() * m_videoRect.Height()
				: (UINT64)m_TexConvertOutput.desc.Width * m_TexConvertOutput.desc.Height;
			const wchar_t* const rtx = L"RTX Video HDR*";
			const size_t at = hdr.find(rtx);
			if (written < kRtxHdrMinPixels && at != std::wstring::npos) {
				hdr.insert(at + wcslen(rtx), L", left out by the driver under 640x360");
			}
		}
		Line(L"  HDR", hdr, -1, false);
	}
	// Upstream's Measured method: the brightest of the last picture measured, and the peak
	// the curve was given, smoothed over the window. Read back a picture or two late, never
	// waited for; its cost is in Finish.
	if ((m_bSdrMeasureActive || m_bHdrLocalMeasureActive) && m_fHdrMeasuredPeakNits > 0.0f) {
		// the SDR shader keeps the peak above the display's own, so this is what it used
		const float usedNits = m_bSdrMeasureActive
			? std::clamp(m_fHdrSmoothedPeakNits, static_cast<float>(m_iSDRDisplayNits) + 1.0f, 10000.0f)
			: std::min(m_fHdrSmoothedPeakNits, 10000.0f);
		Line(L"  HDR measured", std::format(L"frame peak {:.0f}, used {:.0f} nits", m_fHdrMeasuredPeakNits, usedNits), -1, false);
	}

	// -- How the picture reaches the screen, in the order it happens.
	Rule(L"Timing", nullptr);

	// Every picture is scheduled against this clock, so when the pictures stop
	// arriving on time the first thing worth knowing is whose clock it is.
	if (m_pFilter->m_szClockName[0]) {
		str += std::format(L"\nClock         : {}{}", m_pFilter->m_szClockName,
			m_pFilter->m_bClockStalled ? L", stopped -- pictures paced by the stream" : L"");
	}

	// The figures below change with every picture, so each keeps a width of its own:
	// one that went from one digit to two moved everything after it on its line, and the
	// line could not be read while the film played.

	// How early a sample is asked for, how long a picture then takes from its sample to
	// the GPU being done with it, and how many were not done at their present time.
	if (m_bScheduled || m_bDlssNRActive || m_bDlssSRActive || m_MpvLuma.IsLoaded()) {
		if (!m_bDlssRenderAhead) {
			str.append(L"\nRender ahead  : off");
		} else if (m_RenderAhead.MeanLatencyMs() >= 0) {
			str += std::format(L"\nRender ahead  : asked {:3.0f} ms early, ready in {:5.1f} (max {:5.1f}), {} not in time",
				m_RenderAhead.GetAhead() / 10000.0, m_RenderAhead.MeanLatencyMs(), m_RenderAhead.MaxLatencyMs(),
				m_RenderAhead.LateCount());
		}
	}

	// The last picture on the render thread: copying the sample, drawing it (DLSS 5 NR's
	// work issued, not waited for), waiting for the moment to present it, and Present itself.
	{
		const double ticksPerMs = GetPreciseTicksPerSecond() / 1000.0;
		const uint64_t wait = std::min(m_presentWaitTicks, m_RenderStats.presentticks);
		str += std::format(L"\nCPU (ms)      : copy {:5.1f}, draw {:5.1f}, wait {:5.1f}, Present {:5.1f}",
			m_RenderStats.copyticks / ticksPerMs, m_RenderStats.paintticks / ticksPerMs,
			wait / ticksPerMs, (m_RenderStats.presentticks - wait) / ticksPerMs);
	}

	str.append(m_strStatsPresent);   // the swap chain, and what times each Present

	str += std::format(L"\nSync offset   : {:+6.1f} ms", m_RenderStats.syncoffset / 10000.0);
	if (m_bScheduled) {
		// The Sync offset above is then the refresh chosen against the picture's time.
		// This line is what DXGI reported of a picture already shown, which is the
		// proof of it, and what the schedule had to do to keep the step.
		if (m_PresentSchedule.ShownKnown()) {
			str += std::format(L"\nOn screen     : {:+6.1f} ms", m_PresentSchedule.ShownOffsetMs());
		} else {
			str += L"\nOn screen     :      ? ms";
		}
		str += std::format(L", scheduled: repeated {}, dropped {}, shown late {}",
			m_PresentSchedule.RepeatedCount(), m_PresentSchedule.DroppedCount(), m_PresentSchedule.LateCount());
	} else
	if (m_bDlssRenderAhead && m_UsedSwapEffect == DXGI_SWAP_EFFECT_DISCARD && m_pFilter->m_filterState == State_Running) {
		// Worth saying, since the remedy is one setting away: the Discard swap effect
		// keeps no account of the refreshes, and without it no refresh can be chosen.
		str += L"\nOn screen     : not scheduled (Swap effect: Discard)";
	} else
	if (m_bOnScreenKnown) {
		str += std::format(L"\nOn screen     : {:+6.1f} ms, queue {}", m_rtOnScreenOffset / 10000.0, m_uPresentQueue);
	}

#if SYNC_OFFSET_EX
	{
		const auto [so_min, so_max] = m_Syncs.MinMax();
		const auto [sod_min, sod_max] = m_SyncDevs.MinMax();
		str += std::format(L", range[{:+3.0f};{:+3.0f}], max change{:+3.0f}/{:+3.0f}",
			so_min / 10000.0f,
			so_max / 10000.0f,
			sod_min / 10000.0f,
			sod_max / 10000.0f);
	}
#endif
#if TEST_TICKS
	str += std::format(L"\n1:{:6.3f}, 2:{:6.3f}, 3:{:6.3f}, 4:{:6.3f}, 5:{:6.3f}, 6:{:6.3f} ms",
		m_RenderStats.t1 * 1000 / GetPreciseTicksPerSecond(),
		m_RenderStats.t2 * 1000 / GetPreciseTicksPerSecond(),
		m_RenderStats.t3 * 1000 / GetPreciseTicksPerSecond(),
		m_RenderStats.t4 * 1000 / GetPreciseTicksPerSecond(),
		m_RenderStats.t5 * 1000 / GetPreciseTicksPerSecond(),
		m_RenderStats.t6 * 1000 / GetPreciseTicksPerSecond());
#endif
	// The Sync offset and On screen lines are read against the time the picture is
	// due. With a picture delay that is not its stamp any more, and where the picture
	// really is against the sound is the sum: the delay asked for, and the refresh it got.
	if (m_rtVideoDelay) {
		const double delayMs = m_rtVideoDelay / 10000.0;
		const double offsetMs = (m_bScheduled && m_PresentSchedule.ShownKnown()) ? m_PresentSchedule.ShownOffsetMs()
			: m_bOnScreenKnown ? m_rtOnScreenOffset / 10000.0 : m_RenderStats.syncoffset / 10000.0;
		str += std::format(L"\nVideo delay   : {:+.0f} ms asked for, the picture {:+6.1f} ms from its own time", delayMs, delayMs + offsetMs);
	}

	// The samples that came in, and the two kinds left out undrawn: by the renderer for
	// coming too late, and the second field of a deinterlaced one.
	str += std::format(L"\nFrames        : {:5} received, skipped {} late + {} fields, failed {}",
		m_pFilter->m_FrameStats.GetFrames(), m_pFilter->m_DrawStats.m_dropped, m_RenderStats.dropped2, m_RenderStats.failed);
	// A picture whose turn was taken away after it had been given one. Without the
	// way out in WaitForRenderTime this is where the picture stopped for good, so
	// the count is worth showing: it says the freeze was really there and was really
	// caught.
	if (m_pFilter->m_nRenderWaitsBroken) {
		str += std::format(L", turns lost: {}", m_pFilter->m_nRenderWaitsBroken);
	}
	if (m_pFilter->m_nRenderWaitsLate) {
		// The details of the last one on a line of their own, under the text: on the same
		// line they took it out of the box.
		str += std::format(L", turns late: {}\n{:16}(turn {:+} ms, clock {:+} ms, base {:+} ms)",
			m_pFilter->m_nRenderWaitsLate, L"", m_pFilter->m_msLastTurnAhead,
			m_pFilter->m_msClockSinceLastTurn, m_pFilter->m_msBaseSinceLastTurn);
	}

	return str;
}

// A line over the picture for a couple of seconds, under the statistics when they
// are up: which preset a key has just recalled, since nothing else would say.
void CDX11VideoProcessor::DrawNotice(ID3D11Texture2D* pRenderTarget)
{
	if (m_strNotice.empty() || m_windowRect.IsRectEmpty()) {
		return;
	}
	if (GetTickCount64() > m_tickNoticeUntil) {
		m_strNotice.clear();
		return;
	}
	// Half as large again as the statistics, and never less than a thirty-sixth of
	// the window's height: the statistics are sized for a desk, and this is read from
	// a sofa. S_FALSE means the face is already there at that size.
	const SIZE rtSize = m_windowRect.Size();
	const UINT fontH = (UINT)std::max(m_StatsFontH * 3 / 2, (int)rtSize.cy / 36) & ~1u;
	SIZE text = {};
	if (FAILED(m_FontNotice.CreateFontBitmap(L"Consolas", fontH, 0)) || FAILED(m_FontNotice.GetTextExtent(m_strNotice.c_str(), &text))) {
		return;
	}
	ID3D11RenderTargetView* pRenderTargetView = nullptr;
	if (S_OK == m_pDevice->CreateRenderTargetView(pRenderTarget, nullptr, &pRenderTargetView)) {
		const int pad = (int)fontH / 3;
		const int x = m_StatsRect.left;
		const int y = m_bShowStats ? m_StatsRect.bottom + 8 : m_StatsRect.top;
		const RECT box = { x, y, x + text.cx + 2 * pad, y + text.cy + 2 * pad };
		m_NoticeBackground.Set(box, rtSize, D3DCOLOR_ARGB(170, 0, 0, 0));
		m_NoticeBackground.Draw(pRenderTargetView, rtSize);
		m_FontNotice.Draw2DText(pRenderTargetView, rtSize, (float)(x + pad), (float)(y + pad), D3DCOLOR_XRGB(255, 255, 255), m_strNotice.c_str());
		pRenderTargetView->Release();
	}
}

// The video processor works on an engine of its own, and timestamps written on the 3D
// queue around it saw none of its work: with Super Resolution and RTX Video HDR on a
// 1080p film in a 4K window, its step read 0.0 ms while the picture took 21.6 to be
// ready. Two things put the processor between them. One texel of what it wrote, copied
// on the 3D queue, makes that queue wait for the processor before the timestamp that
// ends the step. And the timestamp that starts it is sent to the GPU before the
// processor is asked for anything (BeginVPForTiming): left in the same submission, it
// was held back with the rest of it until the processor was done, and the step still
// read 0.0. With both it reads 17.4 ms, of 20.8 for the whole picture. Only while the
// statistics are timed; the next pass reads that output anyway and waits for it all the
// same.
void CDX11VideoProcessor::WaitForVPForTiming(ID3D11Texture2D* pOutput)
{
	if (!m_DlssStageTimes.IsRecording() || !pOutput) {
		return;
	}
	D3D11_TEXTURE2D_DESC desc = {};
	pOutput->GetDesc(&desc);
	D3D11_TEXTURE2D_DESC probe = {};
	if (m_pVPTimingProbe) {
		m_pVPTimingProbe->GetDesc(&probe);
	}
	if (!m_pVPTimingProbe || probe.Format != desc.Format) {
		m_pVPTimingProbe.Release();
		probe = { 1, 1, 1, 1, desc.Format, { 1, 0 }, D3D11_USAGE_DEFAULT, 0, 0, 0 };
		if (FAILED(m_pDevice->CreateTexture2D(&probe, nullptr, &m_pVPTimingProbe))) {
			return;
		}
	}
	const D3D11_BOX texel = { 0, 0, 0, 1, 1, 1 };
	m_pDeviceContext->CopySubresourceRegion(m_pVPTimingProbe, 0, 0, 0, 0, pOutput, 0, &texel);
}

// The timestamp that starts the processor's step, sent to the GPU before the processor
// is asked for anything.
void CDX11VideoProcessor::BeginVPForTiming(CGpuStageTimes::Stage stage)
{
	m_DlssStageTimes.Begin(m_pDeviceContext, stage);
	if (m_DlssStageTimes.IsRecording()) {
		m_pDeviceContext->Flush();
	}
}
