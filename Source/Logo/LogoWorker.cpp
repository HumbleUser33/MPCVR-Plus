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
#include <algorithm>
#include <format>
#include "LogoWorker.h"
#include "LogoLibrary.h"
#include "LogoParams.h"

namespace Logo {

namespace {

// Pictures waiting for the worker past this are left out: it is behind, and the next
// ones will do as well.
constexpr size_t kMostQueued = 8;

bool Same(const Rect& a, const Rect& b)
{
	return std::abs(a.x - b.x) <= 4 && std::abs(a.y - b.y) <= 4 && std::abs(a.w - b.w) <= 8 && std::abs(a.h - b.h) <= 8;
}

const wchar_t* Where(const Rect& box, int W, int H)
{
	const bool left = box.x + box.w / 2 < W / 3, right = box.x + box.w / 2 > 2 * W / 3;
	const bool top = box.y + box.h / 2 < H / 2;
	return top ? (left ? L"top left" : right ? L"top right" : L"at the top")
	           : (left ? L"bottom left" : right ? L"bottom right" : L"at the bottom");
}

} // namespace

void CWorker::Launch(int W, int H, int divW, int divH, Phase phase)
{
	m_W = W;
	m_H = H;
	m_divW = divW;
	m_divH = divH;
	m_scale = W > 1920 ? 2 : 1;
	m_box = Rect();
	m_looked = m_fitted = 0;
	m_haveReady = m_readyFinal = false;
	m_stored.clear();
	m_knownFrom.clear();
	m_luma.clear();
	m_crops.clear();
	m_quit = false;
	m_phase = phase;
}

void CWorker::Start(int W, int H, int divW, int divH, const std::wstring& film)
{
	Stop();
	std::lock_guard<std::mutex> lock(m_lock);
	m_film = film;
	m_kept.clear();
	Launch(W, H, divW, divH, Phase::Locating);
	m_thread = std::thread(&CWorker::Run, this);
}

void CWorker::Recognise(int W, int H, int divW, int divH, std::vector<Stored>&& kept)
{
	Stop();
	std::lock_guard<std::mutex> lock(m_lock);
	Launch(W, H, divW, divH, Phase::Recognising);
	m_kept = std::move(kept);
	// The box round every logo kept, two pixels wider each way, inside the picture.
	int x0 = W, y0 = H, x1 = 0, y1 = 0;
	for (const Stored& s : m_kept) {
		x0 = std::min(x0, s.maps.box.x - 2);
		y0 = std::min(y0, s.maps.box.y - 2);
		x1 = std::max(x1, s.maps.box.x + s.maps.box.w + 2);
		y1 = std::max(y1, s.maps.box.y + s.maps.box.h + 2);
	}
	x0 = std::max(0, x0) & ~1;
	y0 = std::max(0, y0) & ~1;
	x1 = std::min(W, (x1 + 1) & ~1);
	y1 = std::min(H, (y1 + 1) & ~1);
	m_box = { x0, y0, x1 - x0, y1 - y0 };
	if (m_kept.empty() || m_box.empty()) {
		m_phase = Phase::Unknown;
		return;
	}
	m_thread = std::thread(&CWorker::RunRecognise, this);
}

void CWorker::Stop()
{
	{
		std::lock_guard<std::mutex> lock(m_lock);
		m_quit = true;
	}
	m_wake.notify_all();
	if (m_thread.joinable()) {
		m_thread.join();
	}
	if (m_phase == Phase::Recognising || m_phase == Phase::Locating || m_phase == Phase::Fitting) {
		m_phase = Phase::Idle;
	}
}

bool CWorker::WantsLuma(int* scale) const
{
	if (m_phase != Phase::Locating) {
		return false;
	}
	std::lock_guard<std::mutex> lock(m_lock);
	if (scale) {
		*scale = m_scale;
	}
	return m_luma.size() < kMostQueued;
}

bool CWorker::WantsBox(Rect* box) const
{
	if (m_phase != Phase::Fitting && m_phase != Phase::Recognising) {
		return false;
	}
	std::lock_guard<std::mutex> lock(m_lock);
	if (box) {
		*box = m_box;
	}
	return m_crops.size() < kMostQueued;
}

void CWorker::PushLuma(Plane&& luma)
{
	{
		std::lock_guard<std::mutex> lock(m_lock);
		if (m_luma.size() >= kMostQueued) {
			return;
		}
		m_luma.push_back(std::move(luma));
	}
	m_wake.notify_one();
}

void CWorker::PushBox(Crop&& crop)
{
	{
		std::lock_guard<std::mutex> lock(m_lock);
		if (m_crops.size() >= kMostQueued) {
			return;
		}
		m_crops.push_back(std::move(crop));
	}
	m_wake.notify_one();
}

bool CWorker::TakeMaps(Maps& out, bool* final)
{
	std::lock_guard<std::mutex> lock(m_lock);
	if (!m_haveReady) {
		return false;
	}
	out = m_ready;
	if (final) {
		*final = m_readyFinal;
	}
	m_haveReady = false;
	return true;
}

std::wstring CWorker::StoredPath() const
{
	std::lock_guard<std::mutex> lock(m_lock);
	return m_stored;
}

std::wstring CWorker::Status() const
{
	std::lock_guard<std::mutex> lock(m_lock);
	const int total = kLocateFirst + kFitPictures;
	switch (m_phase.load()) {
	case Phase::Recognising:
		return L"looking for a logo kept";
	case Phase::Unknown:
		return L"no logo kept for this film";
	case Phase::Locating:
		return std::format(L"looking for the logo, {} %", std::min(99, 100 * std::min(m_looked, kLocateFirst) / total));
	case Phase::Fitting:
		return std::format(L"{} x {} px {} found, {} %", m_box.w, m_box.h, Where(m_box, m_W, m_H),
			std::min(99, 100 * (kLocateFirst + m_fitted) / total));
	case Phase::Done:
		if (!m_knownFrom.empty()) {
			return std::format(L"{} x {} px {}, kept from {}", m_box.w, m_box.h, Where(m_box, m_W, m_H), m_knownFrom);
		}
		return std::format(L"{} x {} px {} found", m_box.w, m_box.h, Where(m_box, m_W, m_H));
	case Phase::NotFound:
		return L"no logo found";
	default:
		return {};
	}
}

void CWorker::RunRecognise()
{
	const RemoveOptions ro;
	struct Try { const Stored* kept; int dx, dy; double sum = 0; int sure = 0; };
	std::vector<Try> tries;
	Rect area;
	{
		std::lock_guard<std::mutex> lock(m_lock);
		area = m_box;
		for (const Stored& s : m_kept) {
			for (int dy = -2; dy <= 2; dy++) {
				for (int dx = -2; dx <= 2; dx++) {
					tries.push_back({ &s, dx, dy });
				}
			}
		}
	}
	for (int seen = 0; seen < kRecogniseMost; ) {
		Crop crop;
		{
			std::unique_lock<std::mutex> lock(m_lock);
			m_wake.wait(lock, [&] { return m_quit || !m_crops.empty(); });
			if (m_quit) {
				return;
			}
			crop = std::move(m_crops.front());
			m_crops.pop_front();
		}
		if (crop.p[0].w != area.w || crop.p[0].h != area.h) {
			continue;
		}
		seen++;
		for (Try& t : tries) {
			Maps m = t.kept->maps;
			m.box.x += t.dx - area.x;
			m.box.y += t.dy - area.y;
			float sure = 0;
			const float s = Presence(m, crop.p[0], ro, &sure);
			if (sure >= ro.trust) {
				t.sum += s;
				t.sure++;
			}
		}
		// A logo is taken when it was on screen on enough of the pictures it could be
		// told on; the best of those that were.
		const Try* best = nullptr;
		for (const Try& t : tries) {
			if (t.sure >= kRecogniseSure && t.sum / t.sure >= 0.7 && (!best || t.sum / t.sure > best->sum / best->sure)) {
				best = &t;
			}
		}
		if (best && seen >= kRecogniseSure) {
			Maps m = best->kept->maps;
			m.box.x += best->dx;
			m.box.y += best->dy;
			TouchStored(best->kept->path);
			std::lock_guard<std::mutex> lock(m_lock);
			m_box = m.box;
			m_knownFrom = best->kept->film.empty() ? L"another film" : best->kept->film;
			m_stored = best->kept->path;
			m_ready = std::move(m);
			m_haveReady = true;
			m_readyFinal = true;
			m_phase = Phase::Done;
			return;
		}
	}
	m_phase = Phase::Unknown;
}

void CWorker::Run()
{
	const CLocator::Options lo;
	const CDeblender::Options fo;
	CLocator loc;
	CDeblender fit;
	Rect last;
	{
		std::lock_guard<std::mutex> lock(m_lock);
		loc.Reset(m_W / m_scale, m_H / m_scale, m_scale, lo.edge);
	}
	for (;;) {
		Plane luma;
		Crop crop;
		bool haveLuma = false, haveCrop = false;
		{
			std::unique_lock<std::mutex> lock(m_lock);
			m_wake.wait(lock, [&] { return m_quit || !m_luma.empty() || !m_crops.empty(); });
			if (m_quit) {
				return;
			}
			if (!m_luma.empty()) {
				luma = std::move(m_luma.front());
				m_luma.pop_front();
				haveLuma = true;
			} else {
				crop = std::move(m_crops.front());
				m_crops.pop_front();
				haveCrop = true;
			}
		}

		if (haveLuma && m_phase == Phase::Locating) {
			loc.Add(luma);
			const int looked = loc.Pictures();
			{
				std::lock_guard<std::mutex> lock(m_lock);
				m_looked = looked;
			}
			// Looked for from the twentieth picture, every tenth: taken when two in a row
			// agree, so that one lucky stretch of film does not decide.
			if (looked >= kLocateFirst && looked % 10 == 0) {
				const CLocator::Result r = loc.Locate(lo);
				if (r.found && !last.empty() && Same(r.box, last)) {
					std::lock_guard<std::mutex> lock(m_lock);
					m_box = r.box;
					m_crops.clear();
					fit.Reset(r.box, r.picture, m_divW, m_divH);
					m_phase = Phase::Fitting;
				}
				last = r.found ? r.box : Rect();
				if (m_phase == Phase::Locating && looked >= kLocateMost) {
					m_phase = Phase::NotFound;
					return;
				}
			}
		}

		if (haveCrop && m_phase == Phase::Fitting) {
			fit.Add(crop);
			const int fitted = fit.Pictures();
			{
				std::lock_guard<std::mutex> lock(m_lock);
				m_fitted = fitted;
			}
			const bool final = fitted >= kFitPictures;
			if (fitted == kFitFirst || fitted == 2 * kFitFirst || fitted == kFitPictures / 2 || final) {
				Maps m;
				if (fit.Fit(fo, m)) {
					// Kept from the middle of the fit on, the final one over it.
					std::wstring stored;
					if (fitted >= kFitPictures / 2) {
						std::wstring path;
						{
							std::lock_guard<std::mutex> lock(m_lock);
							path = m_stored;
						}
						if (path.empty()) {
							stored = SaveToLibrary(m, m_W, m_H, m_film);
						} else if (SaveLogo(path, m, m_W, m_H, m_film)) {
							stored = path;
						}
					}
					std::lock_guard<std::mutex> lock(m_lock);
					m_ready = std::move(m);
					m_haveReady = true;
					m_readyFinal = final;
					if (!stored.empty()) {
						m_stored = stored;
					}
				}
			}
			if (final) {
				m_phase = Phase::Done;
				return;
			}
		}
	}
}

} // namespace Logo
