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

#include <string>
#include <vector>
#include "LogoAnalysis.h"

// The logos found so far, one file each, so that a channel analysed once is hidden on
// every film of it after: %LOCALAPPDATA%\MPC-BE Filters\MPC Video Renderer\logos\, beside
// the folder NGX keeps its own files in. A file's last write is when its logo was last
// used, and the least recently used go first when there are too many.

namespace Logo {

struct Stored {
	std::wstring path;
	int videoW = 0, videoH = 0;
	std::wstring film;     // the film it was found on
	Maps maps;
};

constexpr int kMostStored = 32;

// A logo kept, as its file's header says: what the settings page lists.
struct Listed {
	std::wstring path;
	int videoW = 0, videoH = 0;
	Rect box;
};

// The folder, made if it is not there; empty if it cannot be.
std::wstring LibraryFolder();
// Every logo kept for a picture of this size (all of them for 0 x 0), the most recently used first.
std::vector<Stored> KeptLogos(int videoW, int videoH);
// The same from their headers alone, without reading the logos.
std::vector<Listed> ListKept(int videoW, int videoH);
// A logo by what it is, as the page and the statistics name it: "298 x 72 px top right".
std::wstring Describe(const Rect& box, int videoW, int videoH);
// A new logo kept; the oldest dropped past kMostStored. Returns the file, or empty.
std::wstring SaveToLibrary(const Maps& m, int videoW, int videoH, const std::wstring& film);
// Marks a logo used now.
void TouchStored(const std::wstring& path);
// Every logo forgotten. Returns how many were.
int ForgetLibrary();

// One file, for the tools.
bool SaveLogo(const std::wstring& path, const Maps& m, int videoW, int videoH, const std::wstring& film);
bool LoadLogo(const std::wstring& path, Stored& out);

} // namespace Logo
