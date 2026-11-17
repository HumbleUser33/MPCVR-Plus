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

#include <format>
#include <string>

// What the NVIDIA app has the driver do with RTX Video. The driver applies Super
// Resolution and RTX Video HDR only when the app has them on, and RTX Video HDR to the
// brightness the app sets; none of that comes back through Direct3D, which accepts the
// request either way. The app keeps its switches as driver values of the display
// adapter, read here the way FrankyRenderer (mryx007, GPL-3.0-or-later) reads them.
//
// They are not a public interface, so a value that is missing is unknown, and nothing is
// decided on any of them: the renderer asks the driver exactly as before, and the
// statistics only say what the app has it set to.
class CNvidiaAppSettings
{
public:
	struct Values {
		int superRes = -1;   // 0 off, else the app's quality level
		int trueHdr = -1;    // RTX Video HDR: 0 off, 1 on
		// RTX HDR's sliders, which the app keeps beside the switch: the peak in nits,
		// the middle grey, the contrast and the saturation.
		int peak = -1;
		int middleGrey = -1;
		int contrast = -1;
		int saturation = -1;
		bool Known() const { return superRes >= 0 || trueHdr >= 0; }
	};

	// Read again at most every two seconds, so that a change made in the app while a
	// film plays shows in the statistics.
	const Values& Get()
	{
		const ULONGLONG now = GetTickCount64();
		if (!m_bRead || now - m_readAt >= 2000) {
			m_bRead = true;
			m_readAt = now;
			m_values = Read();
		}
		return m_values;
	}

	// For the statistics: the two switches, then RTX HDR's sliders while it is on.
	static std::wstring DescribeSwitches(const Values& v)
	{
		if (!v.Known()) {
			return L"its settings could not be read";
		}
		std::wstring text;
		if (v.superRes == 0) {
			text = L"Super Resolution off";
		} else if (v.superRes > 0) {
			text = std::format(L"Super Resolution {}", v.superRes);
		}
		if (v.trueHdr >= 0) {
			if (!text.empty()) {
				text += L", ";
			}
			text += v.trueHdr ? L"RTX Video HDR on" : L"RTX Video HDR off";
		}
		return text;
	}

	static std::wstring DescribeHdr(const Values& v)
	{
		if (v.trueHdr != 1 || v.peak <= 0) {
			return {};
		}
		std::wstring text = std::format(L"peak {} nits", v.peak);
		if (v.middleGrey > 0 && v.contrast > 0 && v.saturation > 0) {
			text += std::format(L", mid grey {}, contrast {}, saturation {}", v.middleGrey, v.contrast, v.saturation);
		}
		return text;
	}

private:
	Values m_values;
	ULONGLONG m_readAt = 0;
	bool m_bRead = false;

	static int Value(HKEY key, const wchar_t* name)
	{
		DWORD value = 0, size = sizeof(value), type = 0;
		if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) != ERROR_SUCCESS
				|| type != REG_DWORD) {
			return -1;
		}
		return static_cast<int>(value & 0x7fffffff);
	}

	// The first display adapter that carries the app's values.
	static Values Read()
	{
		Values v;
		HKEY hAdapters = nullptr;
		if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
				L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}",
				0, KEY_READ, &hAdapters) != ERROR_SUCCESS) {
			return v;
		}
		wchar_t name[64];
		for (DWORD i = 0; !v.Known(); i++) {
			DWORD length = std::size(name);
			if (RegEnumKeyExW(hAdapters, i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
				break;
			}
			HKEY hAdapter = nullptr;
			if (RegOpenKeyExW(hAdapters, name, 0, KEY_QUERY_VALUE, &hAdapter) != ERROR_SUCCESS) {
				continue;
			}
			v.superRes   = Value(hAdapter, L"_User_Global_VAL_SuperResolution");
			v.trueHdr    = Value(hAdapter, L"_User_Global_VAL_TrueHDR");
			v.peak       = Value(hAdapter, L"_User_Global_VAL_TrueHDR_P");
			v.middleGrey = Value(hAdapter, L"_User_Global_VAL_TrueHDR_M");
			v.contrast   = Value(hAdapter, L"_User_Global_VAL_TrueHDR_C");
			v.saturation = Value(hAdapter, L"_User_Global_VAL_TrueHDR_S");
			RegCloseKey(hAdapter);
		}
		RegCloseKey(hAdapters);
		return v;
	}
};
