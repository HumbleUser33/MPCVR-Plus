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
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "LogoLibrary.h"

#pragma comment(lib, "shell32.lib")

namespace Logo {

namespace {

// The file: this header, then alpha and laid of luma, Cb and Cr, row after row, as
// 16-bit fractions of the range from kAlphaLeast to 1; then the noise, the fill's error by
// distance (its count first) and the luma's excess, as floats.
#pragma pack(push, 4)
struct Header {
	char magic[8];           // "MPCVRLG"
	uint32_t version;        // 1
	int32_t videoW, videoH, divW, divH;
	int32_t x, y, w, h;      // the box, in luma pixels
	float alphaMin;
	int32_t pictures;
	wchar_t film[128];
};
#pragma pack(pop)

const char kMagic[8] = { 'M', 'P', 'C', 'V', 'R', 'L', 'G', 0 };

bool WriteAll(HANDLE f, const void* p, DWORD n)
{
	DWORD done = 0;
	return WriteFile(f, p, n, &done, nullptr) && done == n;
}

bool ReadAll(HANDLE f, void* p, DWORD n)
{
	DWORD done = 0;
	return ReadFile(f, p, n, &done, nullptr) && done == n;
}

struct Kept {
	std::wstring path;
	FILETIME used;
};

std::vector<Kept> Files()
{
	std::vector<Kept> out;
	const std::wstring dir = LibraryFolder();
	if (dir.empty()) {
		return out;
	}
	WIN32_FIND_DATAW fd = {};
	HANDLE h = FindFirstFileW((dir + L"*.logo").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) {
		return out;
	}
	do {
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
			out.push_back({ dir + fd.cFileName, fd.ftLastWriteTime });
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	// The most recently used first.
	std::sort(out.begin(), out.end(), [](const Kept& a, const Kept& b) { return CompareFileTime(&a.used, &b.used) > 0; });
	return out;
}

} // namespace

std::wstring LibraryFolder()
{
	wchar_t base[MAX_PATH] = {};
	std::wstring dir;
	if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, base))) {
		dir = std::wstring(base) + L"\\MPC-BE Filters\\MPC Video Renderer\\logos\\";
	} else {
		return {};
	}
	const int r = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
	if (r != ERROR_SUCCESS && r != ERROR_ALREADY_EXISTS && r != ERROR_FILE_EXISTS) {
		return {};
	}
	return dir;
}

bool SaveLogo(const std::wstring& path, const Maps& m, int videoW, int videoH, const std::wstring& film)
{
	if (!m.valid()) {
		return false;
	}
	Header h = {};
	memcpy(h.magic, kMagic, sizeof(h.magic));
	h.version = 1;
	h.videoW = videoW;
	h.videoH = videoH;
	h.divW = m.divW;
	h.divH = m.divH;
	h.x = m.box.x;
	h.y = m.box.y;
	h.w = m.box.w;
	h.h = m.box.h;
	h.alphaMin = m.alphaMin;
	h.pictures = m.pictures;
	wcsncpy_s(h.film, film.c_str(), _TRUNCATE);
	const std::wstring part = path + L".part";
	HANDLE f = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) {
		return false;
	}
	bool ok = WriteAll(f, &h, sizeof(h));
	std::vector<uint16_t> words;
	for (int p = 0; p < 3 && ok; p++) {
		for (const Plane* q : { &m.alpha[p], &m.laid[p] }) {
			words.resize(q->v.size());
			for (size_t i = 0; i < words.size(); i++) {
				words[i] = (uint16_t)std::lround(std::clamp((q->v[i] - kAlphaLeast) / (1 - kAlphaLeast), 0.0f, 1.0f) * 65535.0f);
			}
			ok = ok && WriteAll(f, words.data(), (DWORD)(words.size() * 2));
		}
	}
	const uint32_t steps = (uint32_t)m.fillVar.size();
	ok = ok && WriteAll(f, &m.noise, sizeof(float)) && WriteAll(f, &steps, sizeof(steps))
		&& (!steps || WriteAll(f, m.fillVar.data(), (DWORD)(steps * sizeof(float))));
	const std::vector<float> excess(m.excess.v.size() == m.alpha[0].v.size() ? m.excess.v : std::vector<float>(m.alpha[0].v.size(), 0.0f));
	ok = ok && WriteAll(f, excess.data(), (DWORD)(excess.size() * sizeof(float)));
	CloseHandle(f);
	// Written whole, or not at all: a reader never finds half a logo.
	if (!ok || !MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
		DeleteFileW(part.c_str());
		return false;
	}
	return true;
}

bool LoadLogo(const std::wstring& path, Stored& out)
{
	HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) {
		return false;
	}
	Header h = {};
	bool ok = ReadAll(f, &h, sizeof(h)) && !memcmp(h.magic, kMagic, sizeof(kMagic)) && h.version == 1
		&& h.divW >= 1 && h.divW <= 2 && h.divH >= 1 && h.divH <= 2
		&& h.w > 0 && h.h > 0 && h.w <= 4096 && h.h <= 4096 && h.w % h.divW == 0 && h.h % h.divH == 0;
	Maps m;
	if (ok) {
		m.box = { h.x, h.y, h.w, h.h };
		m.divW = h.divW;
		m.divH = h.divH;
		m.alphaMin = h.alphaMin;
		m.pictures = h.pictures;
		std::vector<uint16_t> words;
		for (int p = 0; p < 3 && ok; p++) {
			const int w = p ? h.w / h.divW : h.w, hh = p ? h.h / h.divH : h.h;
			for (Plane* q : { &m.alpha[p], &m.laid[p] }) {
				q->resize(w, hh);
				words.resize(q->v.size());
				ok = ok && ReadAll(f, words.data(), (DWORD)(words.size() * 2));
				for (size_t i = 0; ok && i < words.size(); i++) {
					q->v[i] = kAlphaLeast + words[i] / 65535.0f * (1 - kAlphaLeast);
				}
			}
		}
		uint32_t steps = 0;
		ok = ok && ReadAll(f, &m.noise, sizeof(float)) && ReadAll(f, &steps, sizeof(steps)) && steps <= 64;
		if (ok) {
			m.fillVar.resize(steps);
			ok = !steps || ReadAll(f, m.fillVar.data(), (DWORD)(steps * sizeof(float)));
		}
		if (ok) {
			m.excess.resize(m.alpha[0].w, m.alpha[0].h);
			ok = ReadAll(f, m.excess.v.data(), (DWORD)(m.excess.v.size() * sizeof(float)));
		}
	}
	CloseHandle(f);
	if (!ok) {
		return false;
	}
	h.film[_countof(h.film) - 1] = 0;
	out.path = path;
	out.videoW = h.videoW;
	out.videoH = h.videoH;
	out.film = h.film;
	out.maps = std::move(m);
	return true;
}

std::vector<Stored> KeptLogos(int videoW, int videoH)
{
	std::vector<Stored> out;
	for (const Kept& k : Files()) {
		Stored s;
		if (LoadLogo(k.path, s) && ((!videoW && !videoH) || (s.videoW == videoW && s.videoH == videoH))) {
			out.push_back(std::move(s));
		}
	}
	return out;
}

std::wstring SaveToLibrary(const Maps& m, int videoW, int videoH, const std::wstring& film)
{
	const std::wstring dir = LibraryFolder();
	if (dir.empty()) {
		return {};
	}
	SYSTEMTIME t = {};
	GetLocalTime(&t);
	std::wstring path;
	for (int n = 0; n < 100; n++) {
		wchar_t name[96];
		swprintf_s(name, L"%dx%d %04u%02u%02u-%02u%02u%02u%s.logo", videoW, videoH, t.wYear, t.wMonth, t.wDay,
			t.wHour, t.wMinute, t.wSecond, n ? (L"-" + std::to_wstring(n)).c_str() : L"");
		if (GetFileAttributesW((dir + name).c_str()) == INVALID_FILE_ATTRIBUTES) {
			path = dir + name;
			break;
		}
	}
	if (path.empty() || !SaveLogo(path, m, videoW, videoH, film)) {
		return {};
	}
	const std::vector<Kept> all = Files();
	for (size_t i = kMostStored; i < all.size(); i++) {
		DeleteFileW(all[i].path.c_str());
	}
	return path;
}

void TouchStored(const std::wstring& path)
{
	HANDLE f = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f != INVALID_HANDLE_VALUE) {
		FILETIME now = {};
		GetSystemTimeAsFileTime(&now);
		SetFileTime(f, nullptr, nullptr, &now);
		CloseHandle(f);
	}
}

int ForgetLibrary()
{
	int n = 0;
	for (const Kept& k : Files()) {
		n += DeleteFileW(k.path.c_str()) ? 1 : 0;
	}
	return n;
}

} // namespace Logo
