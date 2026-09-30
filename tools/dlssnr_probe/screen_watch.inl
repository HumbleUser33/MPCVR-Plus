// What the screen really shows.
//
// A swap chain can present picture after picture, without error and in time, into a
// window the desktop no longer updates -- which is what a frozen picture with sound
// running looks like. Nothing inside the filter can see that, and neither can a GDI
// screen copy, which does not get the content of a flip model swap chain. The
// desktop duplication does: it hands out the desktop exactly as Windows composes it.

#include <dxgi1_5.h>

class CScreenWatch
{
	CComPtr<ID3D11Device> m_pDevice;
	CComPtr<ID3D11DeviceContext> m_pContext;
	CComPtr<IDXGIOutputDuplication> m_pDuplication;
	CComPtr<ID3D11Texture2D> m_pStaging;
	UINT m_width = 0;
	UINT m_height = 0;
	DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;  // what the desktop is composed in
	int m_left = 0;   // where the duplicated output starts on the desktop
	int m_top = 0;
	HRESULT m_hrHdr = E_NOINTERFACE;  // what asking for the half floats answered
	RECT m_lastRect = {};             // where on the desktop the reading was taken
	UINT m_logicalW = 0;              // the output as windows are placed on it
	UINT m_logicalH = 0;

public:
	// The output the window sits on, duplicated. Returns what went wrong.
	std::string Start(HWND hwnd)
	{
		const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
		D3D_FEATURE_LEVEL level = {};
		HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
			levels, (UINT)std::size(levels), D3D11_SDK_VERSION, &m_pDevice, &level, &m_pContext);
		if (FAILED(hr)) {
			return "no device for the screen watch";
		}
		CComQIPtr<IDXGIDevice> pDXGIDevice(m_pDevice.p);
		CComPtr<IDXGIAdapter> pAdapter;
		if (!pDXGIDevice || FAILED(pDXGIDevice->GetAdapter(&pAdapter))) {
			return "no adapter for the screen watch";
		}
		const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
		for (UINT i = 0; ; i++) {
			CComPtr<IDXGIOutput> pOutput;
			if (FAILED(pAdapter->EnumOutputs(i, &pOutput))) {
				break;
			}
			DXGI_OUTPUT_DESC desc = {};
			pOutput->GetDesc(&desc);
			if (desc.Monitor != monitor) {
				continue;
			}
			// DuplicateOutput alone hands the desktop over in 8-bit, converted: on an
			// HDR desktop that throws away the very thing worth reading. DuplicateOutput1
			// takes a list of formats, so ask for the half floats Windows composes in
			// and fall back to the old way where that is not available.
			CComQIPtr<IDXGIOutput5> pOutput5(pOutput.p);
			if (pOutput5) {
				const DXGI_FORMAT wanted[] = { DXGI_FORMAT_R16G16B16A16_FLOAT,
					DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM };
				m_hrHdr = pOutput5->DuplicateOutput1(m_pDevice, 0, (UINT)std::size(wanted), wanted, &m_pDuplication);
			}
			CComQIPtr<IDXGIOutput1> pOutput1(pOutput.p);
			if (!m_pDuplication && (!pOutput1 || FAILED(pOutput1->DuplicateOutput(m_pDevice, &m_pDuplication)))) {
				return "the desktop cannot be duplicated (another program may hold it)";
			}
			m_left = desc.DesktopCoordinates.left;
			m_top = desc.DesktopCoordinates.top;
			m_width = m_logicalW = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
			m_height = m_logicalH = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
			break;
		}
		if (!m_pDuplication) {
			return "the window is on no duplicated output";
		}

		return {};
	}

	bool Ready() const { return m_pDuplication != nullptr; }
	DXGI_FORMAT Format() const { return m_format; }
	HRESULT HdrAttempt() const { return m_hrHdr; }
	RECT LastRect() const { return m_lastRect; }

	// The place to read into, made to match the frame that actually arrived.
	bool Stage(ID3D11Texture2D* pDesktop)
	{
		D3D11_TEXTURE2D_DESC desc = {};
		pDesktop->GetDesc(&desc);
		if (m_pStaging && desc.Format == m_format && desc.Width == m_width && desc.Height == m_height) {
			return true;
		}
		m_pStaging.Release();
		m_format = desc.Format;
		m_width = desc.Width;
		m_height = desc.Height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.SampleDesc = { 1, 0 };
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.MiscFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		return SUCCEEDED(m_pDevice->CreateTexture2D(&desc, nullptr, &m_pStaging));
	}

	// What the window's part of the desktop actually holds. On an HDR desktop the
	// values are scRGB half floats: 1.0 is SDR white, and anything a tone mapper has
	// lifted above it lands over 1.0. That is the one reading that says whether tone
	// mapping really happened, rather than whether it was asked for.
	struct Reading {
		bool ok = false;
		double mean = 0;      // mean of R, G, B
		double p99 = 0;       // the top of the range
		double peak = 0;
		double colour = 0;    // mean spread between the channels
		double aboveWhite = 0; // share of pixels past SDR white, per cent
		double detail = 0;    // mean gradient, which is what sharpening moves
		// The window's own area, kept so it can be looked at. On an HDR desktop the
		// half floats are divided by a fixed 1.0 before the eight bits, never by the
		// picture's own peak, so two runs stay comparable to each other and anything
		// a tone mapper lifted past SDR white shows up clipped rather than rescaled.
		std::vector<BYTE> crop;   // BGRA, bottom-up, ready for SaveBmp
		int cropW = 0, cropH = 0;
	};

	Reading Measure(HWND hwnd)
	{
		Reading r;
		if (!m_pDuplication) {
			return r;
		}
		for (int i = 0; i < 8; i++) {
			DXGI_OUTDUPL_FRAME_INFO info = {};
			CComPtr<IDXGIResource> pResource;
			const HRESULT hr = m_pDuplication->AcquireNextFrame(120, &info, &pResource);
			if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
				break;
			}
			if (FAILED(hr)) {
				return r;
			}
			if (CComQIPtr<ID3D11Texture2D> pDesktop = pResource.p) {
				if (Stage(pDesktop)) {
					m_pContext->CopyResource(m_pStaging, pDesktop);
				}
			}
			m_pDuplication->ReleaseFrame();
		}

		RECT rc = {};
		GetClientRect(hwnd, &rc);
		POINT topLeft = { 0, 0 };
		ClientToScreen(hwnd, &topLeft);
		const double sx = m_logicalW ? (double)m_width / m_logicalW : 1.0;
		const double sy = m_logicalH ? (double)m_height / m_logicalH : 1.0;
		const int x0 = std::clamp<int>((int)std::lround((topLeft.x - m_left) * sx), 0, (int)m_width - 1);
		const int y0 = std::clamp<int>((int)std::lround((topLeft.y - m_top) * sy), 0, (int)m_height - 1);
		const int x1 = std::clamp<int>(x0 + (int)std::lround(rc.right * sx), 0, (int)m_width);
		const int y1 = std::clamp<int>(y0 + (int)std::lround(rc.bottom * sy), 0, (int)m_height);
		if (x1 - x0 < 8 || y1 - y0 < 8) {
			return r;
		}
		m_lastRect = { x0, y0, x1, y1 };

		D3D11_MAPPED_SUBRESOURCE mapped = {};
		if (!m_pStaging || FAILED(m_pContext->Map(m_pStaging, 0, D3D11_MAP_READ, 0, &mapped))) {
			return r;
		}
		const bool bHalf = (m_format == DXGI_FORMAT_R16G16B16A16_FLOAT);
		const bool bTenBit = (m_format == DXGI_FORMAT_R10G10B10A2_UNORM);
		const auto At = [&](int x, int y, int c) -> double {
			const BYTE* row = (const BYTE*)mapped.pData + (size_t)y * mapped.RowPitch;
			if (bHalf) {
				const uint16_t h = ((const uint16_t*)row)[(size_t)x * 4 + c];
				// half to float, the plain way: sign, exponent, mantissa.
				const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
				double v = 0;
				if (exp == 0) {
					v = std::ldexp((double)man, -24);
				} else if (exp != 31) {
					v = std::ldexp(1024.0 + man, (int)exp - 25);
				}
				return sign ? -v : v;
			}
			if (bTenBit) {
				// R10G10B10A2, and on an HDR desktop its curve is PQ: undo it, then
				// divide by SDR white so the scale is the half floats' -- 1.0 is SDR
				// white and a tone mapper's highlights land above it.
				const uint32_t v = ((const uint32_t*)row)[x];
				const uint32_t ch = (c == 0) ? (v & 0x3FF) : (c == 1) ? ((v >> 10) & 0x3FF) : ((v >> 20) & 0x3FF);
				const double e = ch / 1023.0;
				const double p = std::pow(e, 1.0 / 78.84375);
				const double num = std::max(p - 0.8359375, 0.0);
				const double den = 18.8515625 - 18.6875 * p;
				const double nits = 10000.0 * std::pow(den > 0 ? num / den : 0.0, 1.0 / 0.1593017578125);
				return nits / 80.0;
			}
			return row[(size_t)x * 4 + (c == 0 ? 2 : c == 1 ? 1 : 0)] / 255.0;
		};

		std::vector<double> lum;
		lum.reserve((size_t)(x1 - x0) * (y1 - y0) / 4);
		double sum = 0, colour = 0, peak = 0, above = 0, grad = 0;
		size_t n = 0, gradN = 0;
		for (int y = y0; y < y1; y += 2) {
			for (int x = x0; x < x1; x += 2) {
				const double rr = At(x, y, 0), gg = At(x, y, 1), bb = At(x, y, 2);
				const double l = (rr + gg + bb) / 3.0;
				sum += l;
				colour += std::max({ rr, gg, bb }) - std::min({ rr, gg, bb });
				peak = std::max(peak, std::max({ rr, gg, bb }));
				above += (std::max({ rr, gg, bb }) > 1.0) ? 1 : 0;
				lum.push_back(l);
				n++;
				if (x + 2 < x1) {
					grad += std::abs(l - (At(x + 2, y, 0) + At(x + 2, y, 1) + At(x + 2, y, 2)) / 3.0);
					gradN++;
				}
			}
		}
		r.cropW = x1 - x0;
		r.cropH = y1 - y0;
		r.crop.resize((size_t)r.cropW * r.cropH * 4);
		for (int y = y0; y < y1; y++) {
			BYTE* dst = r.crop.data() + (size_t)(y - y0) * r.cropW * 4;  // top-down, as SaveBmp writes
			for (int x = x0; x < x1; x++, dst += 4) {
				for (int c = 0; c < 3; c++) {
					const double v = std::clamp(At(x, y, c), 0.0, 1.0);
					// Back through the sRGB curve, since scRGB carries light and the
					// eight-bit picture carries what a screen is fed.
					const double e = (bHalf || bTenBit)
						? (v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055)
						: v;
					dst[2 - c] = (BYTE)std::lround(e * 255.0);
				}
				dst[3] = 255;
			}
		}

		m_pContext->Unmap(m_pStaging, 0);
		if (!n) {
			return r;
		}
		std::sort(lum.begin(), lum.end());
		r.ok = true;
		r.mean = sum / n;
		r.p99 = lum[(size_t)(0.99 * (lum.size() - 1))];
		r.peak = peak;
		r.colour = colour / n;
		r.aboveWhite = 100.0 * above / n;
		r.detail = gradN ? grad / gradN : 0;
		return r;
	}

	// The window's own area of the desktop, reduced to a number. Two readings that
	// match mean the screen did not move, whatever the renderer believes it drew.
	uint64_t Signature(HWND hwnd)
	{
		if (!m_pDuplication) {
			return 0;
		}
		// Take whatever the desktop has moved on to, newest first.
		for (int i = 0; i < 8; i++) {
			DXGI_OUTDUPL_FRAME_INFO info = {};
			CComPtr<IDXGIResource> pResource;
			const HRESULT hr = m_pDuplication->AcquireNextFrame(120, &info, &pResource);
			if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
				break;
			}
			if (FAILED(hr)) {
				return 0;
			}
			if (CComQIPtr<ID3D11Texture2D> pDesktop = pResource.p) {
				if (Stage(pDesktop)) {
					m_pContext->CopyResource(m_pStaging, pDesktop);
				}
			}
			m_pDuplication->ReleaseFrame();
		}

		RECT rc = {};
		GetClientRect(hwnd, &rc);
		POINT topLeft = { 0, 0 };
		ClientToScreen(hwnd, &topLeft);
		const double sx = m_logicalW ? (double)m_width / m_logicalW : 1.0;
		const double sy = m_logicalH ? (double)m_height / m_logicalH : 1.0;
		const int x0 = std::clamp<int>((int)std::lround((topLeft.x - m_left) * sx), 0, (int)m_width - 1);
		const int y0 = std::clamp<int>((int)std::lround((topLeft.y - m_top) * sy), 0, (int)m_height - 1);
		const int x1 = std::clamp<int>(x0 + (int)std::lround(rc.right * sx), 0, (int)m_width);
		const int y1 = std::clamp<int>(y0 + (int)std::lround(rc.bottom * sy), 0, (int)m_height);

		D3D11_MAPPED_SUBRESOURCE mapped = {};
		if (FAILED(m_pContext->Map(m_pStaging, 0, D3D11_MAP_READ, 0, &mapped))) {
			return 0;
		}
		uint64_t signature = 0;
		for (int y = y0; y < y1; y += 7) {
			const BYTE* row = (const BYTE*)mapped.pData + (size_t)y * mapped.RowPitch;
			for (int x = x0; x < x1; x += 11) {
				signature = signature * 131 + row[(size_t)x * 4] + row[(size_t)x * 4 + 1];
			}
		}
		m_pContext->Unmap(m_pStaging, 0);
		return signature;
	}

	// The same sampling, kept rather than hashed. A signature says whether a single
	// bit moved, which a dither pattern alone can do while the picture stands still;
	// two of these can be compared with a tolerance, so "the picture is the same
	// picture" can be asked instead of "the pixels are identical".
	// A part of the window rather than all of it, given as fractions of it. With the
	// renderer's statistics drawn in the corner, the corner and the picture can be
	// asked separately: a counter that goes on climbing over a picture that stands
	// still says the renderer is still drawing and still reaching the screen, which
	// is a different fault from nothing reaching the screen at all.
	std::vector<BYTE> Fingerprint(HWND hwnd, double fl, double ft, double fr, double fb,
		int stepX = 23, int stepY = 17)
	{
		std::vector<BYTE> taken;
		if (!m_pDuplication) {
			return taken;
		}
		for (int i = 0; i < 8; i++) {
			DXGI_OUTDUPL_FRAME_INFO info = {};
			CComPtr<IDXGIResource> pResource;
			const HRESULT hr = m_pDuplication->AcquireNextFrame(120, &info, &pResource);
			if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
				break;
			}
			if (FAILED(hr)) {
				return taken;
			}
			if (CComQIPtr<ID3D11Texture2D> pDesktop = pResource.p) {
				if (Stage(pDesktop)) {
					m_pContext->CopyResource(m_pStaging, pDesktop);
				}
			}
			m_pDuplication->ReleaseFrame();
		}

		RECT rc = {};
		GetClientRect(hwnd, &rc);
		POINT topLeft = { 0, 0 };
		ClientToScreen(hwnd, &topLeft);
		const double sx = m_logicalW ? (double)m_width / m_logicalW : 1.0;
		const double sy = m_logicalH ? (double)m_height / m_logicalH : 1.0;
		const int x0 = std::clamp<int>((int)std::lround((topLeft.x + rc.right * fl - m_left) * sx), 0, (int)m_width - 1);
		const int y0 = std::clamp<int>((int)std::lround((topLeft.y + rc.bottom * ft - m_top) * sy), 0, (int)m_height - 1);
		const int x1 = std::clamp<int>((int)std::lround((topLeft.x + rc.right * fr - m_left) * sx), 0, (int)m_width);
		const int y1 = std::clamp<int>((int)std::lround((topLeft.y + rc.bottom * fb - m_top) * sy), 0, (int)m_height);

		D3D11_MAPPED_SUBRESOURCE mapped = {};
		if (FAILED(m_pContext->Map(m_pStaging, 0, D3D11_MAP_READ, 0, &mapped))) {
			return taken;
		}
		const bool bHalf = (m_format == DXGI_FORMAT_R16G16B16A16_FLOAT);
		for (int y = y0; y < y1; y += stepY) {
			const BYTE* row = (const BYTE*)mapped.pData + (size_t)y * mapped.RowPitch;
			for (int x = x0; x < x1; x += stepX) {
				// Half floats hold the green channel in the high byte of its word;
				// eight and ten bit desktops are read straight.
				taken.push_back(bHalf ? ((const BYTE*)row)[(size_t)x * 8 + 3] : row[(size_t)x * 4 + 1]);
			}
		}
		m_pContext->Unmap(m_pStaging, 0);
		return taken;
	}

	std::vector<BYTE> Fingerprint(HWND hwnd) { return Fingerprint(hwnd, 0.0, 0.0, 1.0, 1.0); }

	// How much of the picture really changed, in per cent of the points sampled. A
	// changed point is one that moved by more than a dither step.
	static double Changed(const std::vector<BYTE>& a, const std::vector<BYTE>& b)
	{
		if (a.empty() || a.size() != b.size()) {
			return 0.0;
		}
		size_t moved = 0;
		for (size_t i = 0; i < a.size(); i++) {
			if (std::abs((int)a[i] - (int)b[i]) > 3) {
				moved++;
			}
		}
		return 100.0 * moved / a.size();
	}
};
