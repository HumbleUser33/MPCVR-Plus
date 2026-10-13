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
#include <shlwapi.h>

// What DLSS 5 NR and DLSS Super Resolution both need of the machine before either
// has loaded anything: the folder this filter is in, and the driver's NGX runtime.

namespace NgxRuntime {

inline std::wstring DirOf(const std::wstring& path)
{
	const size_t sep = path.find_last_of(L"\\/");
	return (sep == std::wstring::npos) ? std::wstring(L".") : path.substr(0, sep);
}

// The folder of the module this code is in, the filter's own.
inline std::wstring ModuleDir()
{
	wchar_t buf[MAX_PATH] = {};
	HMODULE hMod = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                   (LPCWSTR)&ModuleDir, &hMod);
	GetModuleFileNameW(hMod, buf, (DWORD)std::size(buf));
	return DirOf(buf);
}

// The driver's NGX runtime, _nvngx.dll, loaded: the one the installed driver names in
// the registry. Only when it names none, the newest the driver store holds -- a store
// keeps the packages of drivers that were replaced, and the first it lists is the
// oldest: with three of them on one machine, DLSS 5 NR, which used to take the first,
// ran on the runtime of a driver two versions back while DLSS Super Resolution, which
// asked the registry, ran on the installed one. The two ask here now.
//
// Null when there is none. The caller frees what it is given; pPath, when asked for,
// says where it came from.
inline HMODULE Load(std::wstring* pPath = nullptr)
{
	std::wstring path;
	HMODULE hRuntime = nullptr;

	HKEY hKey = nullptr;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
		wchar_t dir[MAX_PATH] = {};
		DWORD size = sizeof(dir) - sizeof(wchar_t);
		DWORD type = 0;
		if (RegQueryValueExW(hKey, L"FullPath", nullptr, &type, (LPBYTE)dir, &size) == ERROR_SUCCESS
				&& (type == REG_SZ || type == REG_EXPAND_SZ) && dir[0]) {
			path = std::wstring(dir) + L"\\_nvngx.dll";
			hRuntime = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		}
		RegCloseKey(hKey);
	}

	if (!hRuntime) {
		wchar_t windows[MAX_PATH] = {};
		GetSystemDirectoryW(windows, (UINT)std::size(windows));
		const std::wstring root = std::wstring(windows) + L"\\DriverStore\\FileRepository\\";
		ULONGLONG newest = 0;
		path.clear();
		WIN32_FIND_DATAW fd = {};
		HANDLE hFind = FindFirstFileW((root + L"nv_disp*").c_str(), &fd);
		if (hFind != INVALID_HANDLE_VALUE) {
			do {
				if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
					continue;
				}
				const std::wstring candidate = root + fd.cFileName + L"\\_nvngx.dll";
				WIN32_FILE_ATTRIBUTE_DATA data = {};
				if (GetFileAttributesExW(candidate.c_str(), GetFileExInfoStandard, &data)
						&& !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
					const ULONGLONG written = ((ULONGLONG)data.ftLastWriteTime.dwHighDateTime << 32) | data.ftLastWriteTime.dwLowDateTime;
					if (written >= newest) {
						newest = written;
						path = candidate;
					}
				}
			} while (FindNextFileW(hFind, &fd));
			FindClose(hFind);
		}
		if (!path.empty()) {
			hRuntime = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		}
	}

	if (pPath) {
		*pPath = hRuntime ? path : std::wstring();
	}
	return hRuntime;
}

} // namespace NgxRuntime
