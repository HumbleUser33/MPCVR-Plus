// The cadence slips: CPresentSchedule::SetCadenceSlips, the scene cut detector they wait
// for (CSceneCut) and the picture made for one no cut came in time for (CEchoFlow, then
// CEchoSynth11), all the filter's own code.
//
//   --tcadence       the schedule against a simulated screen. A film at 23.976, 29.97 and
//                    59.94 pictures a second on a screen at 60 Hz, with cuts every 4 s, every
//                    29 s and none; 24, 25 and 30 left as they were; a film at the screen's own
//                    rate drifting both ways. Each slip is taken on a cut, forced at its limit,
//                    or covered by a made picture, and no picture is shown more than 13 ms
//                    before or after its time in a 3:2 cadence, 17.5 ms in a whole step
//   --tcadencecut FILM [--cutfrom s] [--cutfor s] [--hdr]
//                    the detector over a stretch of a film, beside ffmpeg's scene score, as
//                    --tcutscan does for DLSS SR's: where its thresholds are judged
//   --tcadencemade [--timage PICTURE]
//                    the made picture. EchoStorm's own synthesis, where its code is beside the
//                    harness (HAVE_ECHOSTORM), and the filter's Direct3D 11 port of it make
//                    the same picture of the same pair, to the byte, in cells of 2 and of 4;
//                    the motion from the picture before's pyramid alone (CEchoFlow::Prime) is
//                    the motion from its vectors, to the byte; how near it comes to the
//                    picture truly between them, beside the plain blend; and what the detector,
//                    the motion and the synthesis cost the GPU at 1080p and 4K

#include <set>
#include <wincodec.h>
#include "DLSS/DlssTiming.h"
#include "Cadence/SceneCut.h"
#include "FrameGen/echo/EchoFlow.h"
#include "FrameGen/echo/EchoSynth11.h"

#ifdef HAVE_ECHOSTORM
#include <d3d12.h>
#include <d3dcompiler.h>
#include "engine/own_synth.h"
#include "engine/flow_estimator.h"
#include "engine/flow_fix.h"
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3dcompiler.lib")

// EchoStorm keeps its compiled shaders in a cache under the user's profile; here they are
// compiled each time instead, and nothing is written (as the frame generation branch does).
namespace nr {
HRESULT CompileCached(const char* source, size_t length, const char* name, const D3D_SHADER_MACRO* defines, const char* entry,
	const char* target, UINT flags, ID3DBlob** code, ID3DBlob** errors)
{
	return D3DCompile(source, length, name, defines, nullptr, entry, target, flags, 0, code, errors);
}
} // namespace nr
#endif

namespace temporal {

namespace {

// ---- the schedule ---------------------------------------------------------------------

struct CadenceResult {
	unsigned pictures = 0, slipsOnCut = 0, slipsForced = 0, made = 0, repeated = 0, dropped = 0;
	double worstEarlyMs = 0, worstLateMs = 0;   // shown before / after its own time
	unsigned gaps[8] = {};                      // refreshes between two drawn pictures
};

// The filter's CPresentSchedule against a screen of periodMs, Present shown on the first
// refresh after its call, pictures due every frameMs on a clock that may drift against the
// screen's, scene cuts at the pictures given.
CadenceResult RunSchedule(double frameMs, double periodMs, int mode, const std::set<int>& cuts, int pictures, double clockPpm = 0)
{
	CPresentSchedule s;
	s.SetCadenceSlips(mode);
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	const double tpm = f.QuadPart / 1000.0;
	const long long t0 = 10000000000LL;
	auto refreshTime = [&](long long r) { return t0 + (long long)(r * periodMs * tpm); };
	auto refreshIndex = [&](long long tick) { return (double)(tick - t0) / (periodMs * tpm); };
	const unsigned sync0 = 1000;
	unsigned id = 0;
	struct Shown { unsigned id; long long refresh; };
	std::vector<Shown> shown;
	long long reported = -1;
	unsigned lastShownId = 0;
	long long lastShownRefresh = 0;
	auto seeUntil = [&](long long tick) {
		const long long upTo = (long long)std::floor(refreshIndex(tick));
		for (long long r = reported + 1; r <= upTo; r++) {
			for (const Shown& sh : shown) {
				if (sh.refresh == r) {
					lastShownId = sh.id;
					lastShownRefresh = r;
				}
			}
			DXGI_FRAME_STATISTICS st = {};
			st.PresentCount = lastShownId;
			st.PresentRefreshCount = sync0 + (unsigned)lastShownRefresh;
			st.SyncRefreshCount = sync0 + (unsigned)r;
			st.SyncQPCTime.QuadPart = refreshTime(r);
			s.Seen(lastShownId, st, periodMs);
			reported = r;
		}
	};
	CadenceResult res;
	const double frame = frameMs * (1.0 + clockPpm * 1e-6);
	const long long due0 = refreshTime(20) + (long long)(3.1 * tpm);
	long long now = refreshTime(2);
	long long lastDrawnRefresh = -1;
	seeUntil(now);
	for (int k = 0; k < pictures; k++) {
		const long long due = due0 + (long long)(k * frame * tpm);
		now = std::max(now + (long long)(1 * tpm), due - (long long)(60 * tpm));
		seeUntil(now);
		CPresentSchedule::Plan plan = s.Choose(now, due, frameMs, 8.0);
		if (!plan.bValid) {
			continue;
		}
		s.ApplyCut(plan, cuts.count(k) != 0);
		if (plan.bDrop) {
			continue;
		}
		if (plan.bSlipForced && plan.bSlipMade) {
			double t = 0;
			const CPresentSchedule::Plan made = s.MadePlan(now, plan, t);
			if (made.bValid) {
				now = std::max(now, made.tickCall);
				seeUntil(now);
				s.Presented(++id, made, now, 0.07, true);
				shown.push_back({ id, (long long)std::ceil(refreshIndex(now)) });
			}
		}
		now = std::max(now, plan.tickCall);
		seeUntil(now);
		s.Presented(++id, plan, now, 0.07, true);
		const long long on = (long long)std::ceil(refreshIndex(now));
		shown.push_back({ id, on });
		res.pictures++;
		const double offMs = (double)(refreshTime(on) - due) / tpm;
		if (k > 40) {
			res.worstEarlyMs = std::min(res.worstEarlyMs, offMs);
			res.worstLateMs = std::max(res.worstLateMs, offMs);
			if (lastDrawnRefresh >= 0) {
				res.gaps[std::min<long long>(on - lastDrawnRefresh, 7)]++;
			}
		}
		lastDrawnRefresh = on;
	}
	seeUntil(now + (long long)(200 * tpm));
	res.slipsOnCut = s.SlipsOnCut();
	res.slipsForced = s.SlipsForced();
	res.made = s.MadeShown();
	res.repeated = s.RepeatedCount();
	res.dropped = s.DroppedCount();
	return res;
}

void ReportSchedule(const char* name, const CadenceResult& r, bool ok)
{
	Out("  %-50s gaps 1-4 %5u %5u %5u %5u  on a cut %3u forced %3u made %3u  repeated %u dropped %u  %+6.1f %+6.1f ms\n",
		name, r.gaps[1], r.gaps[2], r.gaps[3], r.gaps[4], r.slipsOnCut, r.slipsForced, r.made, r.repeated, r.dropped,
		r.worstEarlyMs, r.worstLateMs);
	Check(ok, name);
}

// ---- the made picture -----------------------------------------------------------------

#ifdef HAVE_ECHOSTORM
// EchoStorm's own synthesis on a Direct3D 12 device of its own: the picture at t between a
// and c (BGRA8), from vectors of c to a, without the marks, which the filter's port leaves out.
class COwnSynthRun
{
public:
	~COwnSynthRun()
	{
		m_orig.Shutdown();
		if (m_event) {
			CloseHandle(m_event);
		}
	}

	bool Init(std::string& error, int scale = 2)
	{
		D3D12_COMMAND_QUEUE_DESC qd = {};
		qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_dev)))
				|| FAILED(m_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queue)))
				|| FAILED(m_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_alloc)))
				|| FAILED(m_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_alloc, nullptr, IID_PPV_ARGS(&m_list)))
				|| FAILED(m_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)))) {
			error = "no Direct3D 12 device and queue";
			return false;
		}
		m_list->Close();
		m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		m_orig.SetAnalysisScale(scale);
		if (!m_orig.Init(m_dev, [this](const char* s) { m_log = s; })) {
			error = "EchoStorm's own synthesis: " + m_log;
			return false;
		}
		return true;
	}

	bool Run(UINT W, UINT H, const std::vector<uint8_t>& a, const std::vector<uint8_t>& c, const std::vector<HALF>& v,
		float t, std::vector<uint8_t>& out)
	{
		bool ok = Tex(W, H, DXGI_FORMAT_B8G8R8A8_UNORM, false, m_a) && Tex(W, H, DXGI_FORMAT_B8G8R8A8_UNORM, false, m_c)
			&& Tex(W, H, DXGI_FORMAT_R16G16_FLOAT, false, m_v) && Tex(W, H, DXGI_FORMAT_B8G8R8A8_UNORM, true, m_out);
		D3D12_RESOURCE_DESC d = m_a->GetDesc();
		UINT64 bytes = 0;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT colorFoot = {}, vecFoot = {};
		m_dev->GetCopyableFootprints(&d, 0, 1, 0, &colorFoot, nullptr, nullptr, &bytes);
		CComPtr<ID3D12Resource> upA, upC, upV, back;
		ok = ok && Buf(bytes, D3D12_HEAP_TYPE_UPLOAD, upA) && Buf(bytes, D3D12_HEAP_TYPE_UPLOAD, upC) && Buf(bytes, D3D12_HEAP_TYPE_READBACK, back);
		d = m_v->GetDesc();
		m_dev->GetCopyableFootprints(&d, 0, 1, 0, &vecFoot, nullptr, nullptr, &bytes);
		ok = ok && Buf(bytes, D3D12_HEAP_TYPE_UPLOAD, upV) && m_orig.Ensure(W, H);
		if (!ok) {
			return false;
		}
		BYTE* p = nullptr;
		for (auto [up, src] : { std::pair{ upA.p, &a }, std::pair{ upC.p, &c } }) {
			if (FAILED(up->Map(0, nullptr, (void**)&p))) {
				return false;
			}
			for (UINT y = 0; y < H; y++) {
				memcpy(p + colorFoot.Offset + (size_t)colorFoot.Footprint.RowPitch * y, src->data() + 4 * (size_t)W * y, 4 * (size_t)W);
			}
			up->Unmap(0, nullptr);
		}
		if (FAILED(upV->Map(0, nullptr, (void**)&p))) {
			return false;
		}
		for (UINT y = 0; y < H; y++) {
			memcpy(p + vecFoot.Offset + (size_t)vecFoot.Footprint.RowPitch * y, v.data() + 2 * (size_t)W * y, 2 * sizeof(HALF) * (size_t)W);
		}
		upV->Unmap(0, nullptr);

		m_alloc->Reset();
		m_list->Reset(m_alloc, nullptr);
		for (auto [tex, up, foot] : { std::tuple{ m_a.p, upA.p, &colorFoot }, std::tuple{ m_c.p, upC.p, &colorFoot }, std::tuple{ m_v.p, upV.p, &vecFoot } }) {
			Barrier(tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
			D3D12_TEXTURE_COPY_LOCATION dst = { tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
			D3D12_TEXTURE_COPY_LOCATION src = { up, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
			src.PlacedFootprint = *foot;
			m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			Barrier(tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		}
		const nr::OwnSynth::Target to = { m_out, t };
		const bool bOrig = m_orig.Record(m_list, 0, m_a, DXGI_FORMAT_B8G8R8A8_UNORM, m_c, DXGI_FORMAT_B8G8R8A8_UNORM, m_v,
			DXGI_FORMAT_B8G8R8A8_UNORM, &to, 1, 0, 203.0f, 0.6f, 0.0f, nullptr, 1.0f);
		Barrier(m_out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
		D3D12_TEXTURE_COPY_LOCATION dst = { back, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
		dst.PlacedFootprint = colorFoot;
		D3D12_TEXTURE_COPY_LOCATION src = { m_out, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
		m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		Barrier(m_out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		if (FAILED(m_list->Close())) {
			return false;
		}
		ID3D12CommandList* lists[] = { m_list };
		m_queue->ExecuteCommandLists(1, lists);
		m_queue->Signal(m_fence, ++m_fenceValue);
		if (m_fence->GetCompletedValue() < m_fenceValue) {
			m_fence->SetEventOnCompletion(m_fenceValue, m_event);
			WaitForSingleObject(m_event, INFINITE);
		}
		if (!bOrig || FAILED(back->Map(0, nullptr, (void**)&p))) {
			return false;
		}
		out.resize(4 * (size_t)W * H);
		for (UINT y = 0; y < H; y++) {
			memcpy(out.data() + 4 * (size_t)W * y, p + colorFoot.Offset + (size_t)colorFoot.Footprint.RowPitch * y, 4 * (size_t)W);
		}
		const D3D12_RANGE none = { 0, 0 };
		back->Unmap(0, &none);
		return true;
	}

private:
	bool Tex(UINT w, UINT h, DXGI_FORMAT f, bool bUav, CComPtr<ID3D12Resource>& out)
	{
		D3D12_HEAP_PROPERTIES hp = {};
		hp.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC d = {};
		d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		d.Width = w;
		d.Height = h;
		d.DepthOrArraySize = 1;
		d.MipLevels = 1;
		d.Format = f;
		d.SampleDesc.Count = 1;
		d.Flags = bUav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
		out.Release();
		return SUCCEEDED(m_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
			bUav ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&out)));
	}
	bool Buf(UINT64 bytes, D3D12_HEAP_TYPE type, CComPtr<ID3D12Resource>& out)
	{
		D3D12_HEAP_PROPERTIES hp = {};
		hp.Type = type;
		D3D12_RESOURCE_DESC d = {};
		d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		d.Width = bytes;
		d.Height = 1;
		d.DepthOrArraySize = 1;
		d.MipLevels = 1;
		d.SampleDesc.Count = 1;
		d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		out.Release();
		return SUCCEEDED(m_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
			type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out)));
	}
	void Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
	{
		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to };
		m_list->ResourceBarrier(1, &b);
	}

	CComPtr<ID3D12Device> m_dev;
	CComPtr<ID3D12CommandQueue> m_queue;
	CComPtr<ID3D12CommandAllocator> m_alloc;
	CComPtr<ID3D12GraphicsCommandList> m_list;
	CComPtr<ID3D12Fence> m_fence;
	UINT64 m_fenceValue = 0;
	HANDLE m_event = nullptr;
	nr::OwnSynth m_orig;
	std::string m_log;
	CComPtr<ID3D12Resource> m_a, m_c, m_v, m_out;
};
#endif

// A texture of 8-bit colour from BGRA bytes, that a shader reads and that is copied out of.
bool UploadBgra(ID3D11Device* dev, UINT W, UINT H, const std::vector<uint8_t>& bgra, Tex2D_t& tex)
{
	if (FAILED(tex.CheckCreate(dev, DXGI_FORMAT_B8G8R8A8_UNORM, W, H, Tex2D_DefaultShader))) {
		return false;
	}
	CComPtr<ID3D11DeviceContext> ctx;
	dev->GetImmediateContext(&ctx);
	ctx->UpdateSubresource(tex.pTexture, 0, nullptr, bgra.data(), 4 * W, 0);
	return true;
}

// Any texture's bytes, row after row, element size `bytes`.
bool ReadTexture(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, UINT bytes, std::vector<uint8_t>& out)
{
	D3D11_TEXTURE2D_DESC desc = {};
	tex->GetDesc(&desc);
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	desc.MiscFlags = 0;
	CComPtr<ID3D11Texture2D> stage;
	if (FAILED(dev->CreateTexture2D(&desc, nullptr, &stage))) {
		return false;
	}
	ctx->CopyResource(stage, tex);
	D3D11_MAPPED_SUBRESOURCE mr = {};
	if (FAILED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &mr))) {
		return false;
	}
	out.resize((size_t)bytes * desc.Width * desc.Height);
	for (UINT y = 0; y < desc.Height; y++) {
		memcpy(out.data() + (size_t)bytes * desc.Width * y, (const uint8_t*)mr.pData + (size_t)mr.RowPitch * y, (size_t)bytes * desc.Width);
	}
	ctx->Unmap(stage, 0);
	return true;
}

// The picture moved by (sx, sy) pixels, bilinear, as BGRA bytes: what is at x was at x - s.
std::vector<uint8_t> Shifted(const std::vector<float>& rgba, int W, int H, double sx, double sy)
{
	std::vector<uint8_t> out(4 * (size_t)W * H);
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			const double fx = std::clamp(x - sx, 0.0, W - 1.0), fy = std::clamp(y - sy, 0.0, H - 1.0);
			const int x0 = (int)fx, y0 = (int)fy, x1 = std::min(x0 + 1, W - 1), y1 = std::min(y0 + 1, H - 1);
			const double ax = fx - x0, ay = fy - y0;
			for (int ch = 0; ch < 3; ch++) {
				auto at = [&](int xx, int yy) { return (double)rgba[4 * ((size_t)yy * W + xx) + ch]; };
				const double v = (1 - ay) * ((1 - ax) * at(x0, y0) + ax * at(x1, y0)) + ay * ((1 - ax) * at(x0, y1) + ax * at(x1, y1));
				out[4 * ((size_t)y * W + x) + (2 - ch)] = (uint8_t)std::clamp((int)std::lround(v * 255.0), 0, 255);
			}
			out[4 * ((size_t)y * W + x) + 3] = 255;
		}
	}
	return out;
}

// PSNR of two BGRA pictures (or RGBA against BGRA, with bSwap), on the colour channels,
// leaving a margin out where a moved picture runs out.
double PsnrBgra(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int W, int H, bool bSwap, int margin)
{
	double sum = 0;
	size_t n = 0;
	for (int y = margin; y < H - margin; y++) {
		for (int x = margin; x < W - margin; x++) {
			const size_t p = 4 * ((size_t)y * W + x);
			for (int ch = 0; ch < 3; ch++) {
				const double d = (double)a[p + ch] - (double)b[p + (bSwap ? 2 - ch : ch)];
				sum += d * d;
				n++;
			}
		}
	}
	const double mse = n ? sum / n : 0;
	return mse > 0 ? 10.0 * std::log10(255.0 * 255.0 / mse) : 99.0;
}

} // namespace

static int RunCadence()
{
	Head("Cadence slips: the schedule against a simulated screen");
	g_report = _wfopen(L"temporal_results_cadence.txt", L"w");
	const double f23 = 1000.0 * 1001.0 / 24000.0, f29 = 1000.0 * 1001.0 / 30000.0, f59 = 1000.0 * 1001.0 / 60000.0;
	const double p60 = 1000.0 / 60.0;
	const int n = 24 * 120;   // two minutes at 23.976
	std::set<int> none, cuts, rareCuts;
	for (int k = 50; k < 20000; k += 97) {
		cuts.insert(k);       // a cut every ~4 s at 23.976
	}
	for (int k = 50; k < 20000; k += 700) {
		rareCuts.insert(k);   // every ~29 s: the slips between are forced
	}
	using CS = CPresentSchedule;
	Out("  film@screen, setting: refreshes between drawn pictures (1-4), slips on a cut / forced / made,\n"
		"  repeated and dropped pictures, the earliest and latest a picture is shown against its time\n");
	const CadenceResult off = RunSchedule(f23, p60, CS::CadenceSlipsWhereTheyFall, none, n);
	ReportSchedule("23.976@60 where they fall", off, off.gaps[2] + off.gaps[3] > 0 && off.gaps[4] == 0 && off.slipsForced == 0);
	const CadenceResult oc = RunSchedule(f23, p60, CS::CadenceSlipsOnCut, cuts, n);
	ReportSchedule("23.976@60 on a cut, cuts every 4 s", oc,
		oc.slipsOnCut >= 10 && oc.slipsForced == 0 && oc.gaps[4] == 0 && oc.worstEarlyMs > -13.0 && oc.worstLateMs < 13.0);
	const CadenceResult rc = RunSchedule(f23, p60, CS::CadenceSlipsOnCut, rareCuts, n);
	ReportSchedule("23.976@60 on a cut, cuts every 29 s", rc, rc.slipsForced > 0 && rc.made == 0 && rc.gaps[4] == 0 && rc.worstEarlyMs > -13.0 && rc.worstLateMs < 13.0);
	const CadenceResult rm = RunSchedule(f23, p60, CS::CadenceSlipsOnCutOrMade, rareCuts, n);
	ReportSchedule("23.976@60 on a cut else made, cuts every 29 s", rm, rm.slipsForced > 0 && rm.made == rm.slipsForced && rm.worstEarlyMs > -13.0);
	const CadenceResult nm = RunSchedule(f23, p60, CS::CadenceSlipsOnCutOrMade, none, n);
	ReportSchedule("23.976@60 on a cut else made, no cuts", nm, nm.slipsForced >= 10 && nm.made == nm.slipsForced);

	const CadenceResult o29 = RunSchedule(f29, p60, CS::CadenceSlipsWhereTheyFall, none, 30 * 120);
	ReportSchedule("29.97@60 where they fall", o29, o29.slipsForced == 0 && o29.slipsOnCut == 0);
	const CadenceResult c29 = RunSchedule(f29, p60, CS::CadenceSlipsOnCut, cuts, 30 * 120);
	ReportSchedule("29.97@60 on a cut", c29, c29.slipsOnCut >= 5 && c29.slipsForced == 0 && c29.worstEarlyMs > -17.5);
	const CadenceResult m29 = RunSchedule(f29, p60, CS::CadenceSlipsOnCutOrMade, none, 30 * 120);
	ReportSchedule("29.97@60 else made, no cuts", m29, m29.slipsForced >= 5 && m29.made == m29.slipsForced);
	const CadenceResult o59 = RunSchedule(f59, p60, CS::CadenceSlipsWhereTheyFall, none, 60 * 120);
	ReportSchedule("59.94@60 where they fall", o59, o59.slipsForced == 0 && o59.slipsOnCut == 0);
	const CadenceResult c59 = RunSchedule(f59, p60, CS::CadenceSlipsOnCut, cuts, 60 * 120);
	ReportSchedule("59.94@60 on a cut", c59, c59.slipsOnCut >= 5 && c59.slipsForced == 0);
	const CadenceResult m59 = RunSchedule(f59, p60, CS::CadenceSlipsOnCutOrMade, none, 60 * 120);
	ReportSchedule("59.94@60 else made, no cuts", m59, m59.slipsForced >= 5 && m59.made == m59.slipsForced);

	// Those it leaves alone: the same with the setting as without.
	for (const double fps : { 24.0, 25.0, 30.0 }) {
		const CadenceResult a = RunSchedule(1000.0 / fps, p60, CS::CadenceSlipsWhereTheyFall, cuts, (int)(fps * 60));
		const CadenceResult b = RunSchedule(1000.0 / fps, p60, CS::CadenceSlipsOnCutOrMade, cuts, (int)(fps * 60));
		const bool same = a.gaps[1] == b.gaps[1] && a.gaps[2] == b.gaps[2] && a.gaps[3] == b.gaps[3] && a.gaps[4] == b.gaps[4]
			&& b.slipsOnCut == 0 && b.slipsForced == 0 && b.made == 0;
		const std::string name = std::format("{:.0f}@60 the same with the setting", fps);
		ReportSchedule(name.c_str(), b, same);
	}
	// A film at the screen's own rate, its clock drifting against the screen's both ways
	// (300 ppm, to see it in two minutes): a picture more owed, a repeat; one less, a drop.
	const CadenceResult nf = RunSchedule(f23, f23, CS::CadenceSlipsWhereTheyFall, none, n, 300);
	ReportSchedule("23.976@23.976 +300 ppm where it falls (a repeat)", nf, nf.repeated == 1);
	const CadenceResult nc = RunSchedule(f23, f23, CS::CadenceSlipsOnCut, cuts, n, 300);
	ReportSchedule("23.976@23.976 +300 ppm, the repeat on a cut", nc, nc.slipsOnCut == 1 && nc.slipsForced == 0 && nc.repeated == 0);
	const CadenceResult ns = RunSchedule(f23, f23, CS::CadenceSlipsOnCut, cuts, n, -300);
	ReportSchedule("23.976@23.976 -300 ppm, the drop on a cut", ns, ns.slipsOnCut == 1 && ns.dropped == 1);
	const CadenceResult nm2 = RunSchedule(f23, f23, CS::CadenceSlipsOnCutOrMade, none, n, 300);
	ReportSchedule("23.976@23.976 +300 ppm, no cut: a made picture", nm2, nm2.slipsForced == 1 && nm2.made == 1);
	const CadenceResult nd2 = RunSchedule(f23, f23, CS::CadenceSlipsOnCutOrMade, none, n, -300);
	ReportSchedule("23.976@23.976 -300 ppm, no cut: a drop stays one", nd2, nd2.dropped == 1 && nd2.made == 0);
	if (g_report) {
		fclose(g_report);
		g_report = nullptr;
	}
	return g_failures ? 1 : 0;
}

static int RunCadenceCut(ID3D11Device* dev, ID3D11DeviceContext* ctx, const wchar_t* film, double from, double seconds, bool bHdr)
{
	Head("Cadence slips: the scene cut detector over a stretch of a film");

	const std::wstring path(film);
	const std::wstring name = path.substr(path.find_last_of(L"\\/") + 1);
	const std::wstring range = std::format(L"-ss {:.3f} -t {:.3f}", from, seconds);
	int W = 0, H = 0;
	double fps = 0;
	if (FILE* probe = OpenPipe(std::format(L"ffprobe -v error -select_streams v:0 -show_entries stream=width,height,r_frame_rate -of csv=p=0 \"{}\"", path))) {
		char line[256] = {};
		if (fgets(line, sizeof(line), probe)) {
			int num = 0, den = 1;
			if (sscanf_s(line, "%d,%d,%d/%d", &W, &H, &num, &den) == 4 && den > 0) {
				fps = (double)num / den;
			}
		}
		_pclose(probe);
	}
	Check(W >= 64 && H >= 64 && fps > 0, "the film's size and rate (ffprobe)");
	if (W < 64 || H < 64 || fps <= 0) {
		return 1;
	}

	// ffmpeg's scene score of every picture, from a decode of its own.
	std::vector<double> scene;
	{
		const wchar_t* scores = L"cadencecut_scene.txt";
		DeleteFileW(scores);
		if (FILE* run = OpenPipe(std::format(L"ffmpeg -v error {} -i \"{}\" -an -sn -fps_mode passthrough -vf \"select='gte(scene,0)',metadata=print:file={}\" -f null - 2>&1", range, path, scores))) {
			char line[512];
			while (fgets(line, sizeof(line), run)) {
			}
			_pclose(run);
		}
		FILE* in = nullptr;
		if (!_wfopen_s(&in, scores, L"r") && in) {
			char line[512];
			while (fgets(line, sizeof(line), in)) {
				if (!strncmp(line, "frame:", 6)) {
					scene.push_back(-1);
				} else if (!scene.empty() && !strncmp(line, "lavfi.scene_score=", 18)) {
					scene.back() = atof(line + 18);
				}
			}
			fclose(in);
		}
		DeleteFileW(scores);
	}

	// The pictures as the back buffer would hold them: 8-bit, or 10-bit PQ for an HDR film.
	CSceneCut cut;
	Tex2D_t picture;
	const DXGI_FORMAT format = bHdr ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
	const bool ok = cut.Init(dev) && SUCCEEDED(picture.Create(dev, format, W, H, Tex2D_DefaultShader));
	Check(ok, "the detector and a picture of the film's size");
	if (!ok) {
		return 1;
	}
	FILE* frames = OpenPipe(std::format(L"ffmpeg -v error {} -i \"{}\" -an -sn -fps_mode passthrough -f rawvideo -pix_fmt rgba64le -", range, path));
	Check(frames != nullptr, "ffmpeg's pipe");
	if (!frames) {
		return 1;
	}
	g_report = _wfopen(std::format(L"temporal_results_cadencecut_{}.txt", name).c_str(), L"w");
	FILE* csv = _wfopen(std::format(L"cadencecut_{}.csv", name).c_str(), L"w");
	if (csv) {
		fprintf(csv, "frame,seconds,ffmpeg_scene,cut,histogram,ratio,residual,correlation\n");
	}
	std::vector<uint16_t> raw(4 * (size_t)W * H);
	std::vector<uint32_t> packed((size_t)W * H);
	struct Called { int frame; float hist, ratio, resid, corr; };
	std::vector<Called> called;
	int n = 0, measured = 0;
	double gpuMs = 0;
	for (; fread(raw.data(), 8 * (size_t)W * H, 1, frames) == 1; n++) {
		for (size_t i = 0; i < packed.size(); i++) {
			const uint16_t* q = &raw[4 * i];
			if (bHdr) {
				packed[i] = (q[0] >> 6) | ((uint32_t)(q[1] >> 6) << 10) | ((uint32_t)(q[2] >> 6) << 20) | (3u << 30);
			} else {
				packed[i] = (uint32_t)(q[2] >> 8) | ((uint32_t)(q[1] >> 8) << 8) | ((uint32_t)(q[0] >> 8) << 16) | 0xFF000000u;
			}
		}
		ctx->UpdateSubresource(picture.pTexture, 0, nullptr, packed.data(), 4 * W, 0);
		float state[16];
		std::fill(std::begin(state), std::end(state), -1.0f);
		if (cut.Measure(dev, ctx, picture.pShaderResource, W, H, bHdr ? 2 : 0, 203.0f, (uint64_t)n + 1)) {
			ctx->Flush();
			while (cut.Read(ctx, (uint64_t)n + 1, state) < 0) {
				Sleep(0);
			}
			measured++;
		}
		const double ff = n < (int)scene.size() ? scene[n] : -1;
		if (csv) {
			fprintf(csv, "%d,%.3f,%.4f,%d,%.4f,%.2f,%.6f,%.4f\n", n, from + n / fps, ff,
				state[0] < 0 ? -1 : state[0] > 0.5f ? 1 : 0, state[1], state[3], state[2], state[14]);
		}
		if (state[0] > 0.5f) {
			called.push_back({ n, state[1], state[3], state[2], state[14] });
		}
		if (n == 50) {
			// What the detector costs the GPU on a picture of this size.
			uint64_t id = 1000000;
			gpuMs = GpuMs(dev, ctx, 20, [&] { cut.Measure(dev, ctx, picture.pShaderResource, W, H, bHdr ? 2 : 0, 203.0f, ++id); });
		}
	}
	_pclose(frames);
	if (csv) {
		fclose(csv);
	}

	Out("\n%S from %.1f s for %.1f s: %d pictures of %dx%d at %.3f a second, %d measured, %.3f ms each on the GPU; ffmpeg scored %zu.\n",
		film, from, seconds, n, W, H, fps, measured, gpuMs, scene.size());
	Out("Called a cut (histogram, ratio, residual, correlation), with ffmpeg's scene score:\n");
	int calledLow = 0;
	for (const Called& c : called) {
		const double ff = c.frame < (int)scene.size() ? scene[c.frame] : -1.0;
		Out("  %6d  %9.3f s  %.2f  x%-7.0f %.4f  %5.2f   ffmpeg %.2f%s\n", c.frame, from + c.frame / fps, c.hist, c.ratio, c.resid,
			c.corr, ff, ff >= 0 && ff < 0.1 ? "   <- ffmpeg sees none" : "");
		calledLow += ff >= 0 && ff < 0.1 ? 1 : 0;
	}
	Out("ffmpeg's scene score past 0.3 on a picture not called:\n");
	int notCalled = 0;
	for (int i = 0; i < (int)scene.size() && i < n; i++) {
		if (scene[i] > 0.3 && std::none_of(called.begin(), called.end(), [i](const Called& c) { return c.frame == i; })) {
			Out("  %6d  %9.3f s  ffmpeg %.2f\n", i, from + i / fps, scene[i]);
			notCalled++;
		}
	}
	Out("  %zu called, %d of them where ffmpeg sees none (under 0.1); %d past 0.3 not called\n", called.size(), calledLow, notCalled);
	Check(n > 0 && measured > 0, "pictures measured");
	if (g_report) {
		fclose(g_report);
		g_report = nullptr;
	}
	return (n > 0 && measured > 0 && !g_failures) ? 0 : 1;
}

static int RunCadenceMade(ID3D11Device* dev, ID3D11DeviceContext* ctx, const wchar_t* image)
{
	Head("Cadence slips: the picture made between two");
	g_report = _wfopen(L"temporal_results_cadencemade.txt", L"w");
	CComPtr<IWICImagingFactory> factory;
	const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
	int rc = 0;
	for (const auto [W, H] : { std::pair{ 1920, 1080 }, std::pair{ 3840, 2160 } }) {
		std::vector<float> rgba;
		std::string error;
		if (!factory || !LoadPicture(factory, image, W, H, rgba, error)) {
			Check(false, ("the picture: " + error).c_str());
			rc = 1;
			break;
		}
		// A pan of (5.3, 1.7) pixels from a to c, and the picture truly a fraction t of the way.
		const double sx = 5.3 * W / 1920, sy = 1.7 * W / 1920;
		const float t = 0.4f;
		const std::vector<uint8_t> a = Shifted(rgba, W, H, 0, 0), c = Shifted(rgba, W, H, sx, sy), truth = Shifted(rgba, W, H, t * sx, t * sy);
		std::vector<uint8_t> blend(a.size());
		for (size_t i = 0; i < a.size(); i++) {
			blend[i] = (uint8_t)std::lround((1.0 - t) * a[i] + t * c[i]);
		}

		Tex2D_t texA, texC, texOut;
		CComPtr<ID3D11UnorderedAccessView> outUav;
		CEchoFlow flow;
		CEchoSynth11 synth;
		const bool ok = UploadBgra(dev, W, H, a, texA) && UploadBgra(dev, W, H, c, texC)
			&& SUCCEEDED(texOut.Create(dev, DXGI_FORMAT_R8G8B8A8_UNORM, W, H, Tex2D_DefaultShaderRTargetUAV))
			&& SUCCEEDED(dev->CreateUnorderedAccessView(texOut.pTexture, nullptr, &outUav))
			&& flow.Init(dev) && flow.Ensure(dev, W, H) && synth.Init(dev) && synth.Ensure(dev, W, H);
		Check(ok, std::format("EchoStorm's motion and the filter's EchoFG at {}x{} ({} / {})", W, H,
			(const char*)CW2A(flow.Status().c_str()), (const char*)CW2A(synth.Status().c_str())).c_str());
		if (!ok) {
			rc = 1;
			continue;
		}
		// As the renderer makes it (CadenceMake): the picture before's pyramid, then the pair.
		// Its own vectors from nothing (Measure with no picture before) would be zeros nothing
		// reads: the motion comes out the same to the byte either way.
		std::vector<uint8_t> fromNothing;
		flow.Forget();
		flow.Measure(ctx, texA.pShaderResource, nullptr, 0, 203.0f);
		flow.Measure(ctx, texC.pShaderResource, texA.pShaderResource, 0, 203.0f);
		const bool bFromNothing = ReadTexture(dev, ctx, flow.Motion(), 4, fromNothing);
		flow.Prime(ctx, texA.pShaderResource, 0, 203.0f);
		flow.Measure(ctx, texC.pShaderResource, texA.pShaderResource, 0, 203.0f);
		const bool made = synth.Make(ctx, texA.pShaderResource, texC.pShaderResource, flow.MotionView(), outUav, t, 0, 203.0f, 0.6f, 1.0f);
		std::vector<uint8_t> port, motion;
		const bool read = made && ReadTexture(dev, ctx, texOut.pTexture, 4, port) && ReadTexture(dev, ctx, flow.Motion(), 4, motion);
		Check(read, "the made picture and its motion read back");
		if (!read) {
			rc = 1;
			continue;
		}
		Check(bFromNothing && fromNothing == motion, std::format("the motion from the picture before's pyramid alone is the motion from its vectors at {}x{}", W, H).c_str());
		const int margin = (int)std::ceil(std::max(sx, sy)) + 4;
		Out("  %dx%d, a pan of (%.1f, %.1f) pixels, t %.1f: PSNR against the picture truly there %.2f dB, the plain blend %.2f dB\n",
			W, H, sx, sy, t, PsnrBgra(truth, port, W, H, true, margin), PsnrBgra(truth, blend, W, H, false, margin));

#ifdef HAVE_ECHOSTORM
		// In cells of 2, EchoStorm's default, and in cells of 4, which the renderer takes for a
		// picture more than 2560 wide.
		for (const uint32_t scale : { 2u, 4u }) {
			std::vector<uint8_t> portScaled = port;
			if (scale != 2) {
				// A synthesis of its own: its grain's seed counts from its first picture, as
				// EchoStorm's does.
				CEchoSynth11 scaled;
				scaled.SetAnalysisScale(scale);
				const bool bScaled = scaled.Init(dev) && scaled.Ensure(dev, W, H)
					&& scaled.Make(ctx, texA.pShaderResource, texC.pShaderResource, flow.MotionView(), outUav, t, 0, 203.0f, 0.6f, 1.0f)
					&& ReadTexture(dev, ctx, texOut.pTexture, 4, portScaled);
				Check(bScaled, std::format("the port in cells of {} at {}x{}", scale, W, H).c_str());
				if (!bScaled) {
					continue;
				}
			}
			COwnSynthRun orig;
			std::string err;
			std::vector<uint8_t> origOut;
			std::vector<HALF> vectors((size_t)2 * W * H);
			memcpy(vectors.data(), motion.data(), motion.size());
			const bool bRan = orig.Init(err, (int)scale) && orig.Run(W, H, a, c, vectors, t, origOut);
			Check(bRan, ("EchoStorm's own synthesis on the same pair " + err).c_str());
			if (bRan) {
				size_t differ = 0;
				int worst = 0;
				for (size_t p = 0; p < origOut.size(); p += 4) {
					for (int ch = 0; ch < 3; ch++) {
						const int d = std::abs((int)origOut[p + ch] - (int)portScaled[p + (2 - ch)]);
						differ += d ? 1 : 0;
						worst = std::max(worst, d);
					}
				}
				Out("  EchoStorm's own synthesis against the filter's Direct3D 11 port, cells of %u: %zu values differ, by %d at most\n",
					scale, differ, worst);
				Check(differ == 0, std::format("the port makes EchoStorm's picture to the byte at {}x{} in cells of {}", W, H, scale).c_str());
			}
		}
#else
		Out("  (EchoStorm's own code is not beside the harness: the port is not compared with it)\n");
#endif

		// What each part costs the GPU: the detector, the copy of the video area, the motion of
		// both pictures from nothing, the synthesis.
		CSceneCut cut;
		Tex2D_t copy;
		uint64_t id = 0;
		const bool bCost = cut.Init(dev) && SUCCEEDED(copy.Create(dev, DXGI_FORMAT_B8G8R8A8_UNORM, W, H, Tex2D_DefaultShader));
		if (bCost) {
			const D3D11_BOX box = { 0, 0, 0, (UINT)W, (UINT)H, 1 };
			const double copyMs = GpuMs(dev, ctx, 20, [&] { ctx->CopySubresourceRegion(copy.pTexture, 0, 0, 0, 0, texC.pTexture, 0, &box); });
			const double cutMs = GpuMs(dev, ctx, 20, [&] { cut.Measure(dev, ctx, texC.pShaderResource, W, H, 0, 203.0f, ++id); });
			const double flowMs = GpuMs(dev, ctx, 10, [&] {
				flow.Forget();
				flow.Measure(ctx, texA.pShaderResource, nullptr, 0, 203.0f);
				flow.Measure(ctx, texC.pShaderResource, texA.pShaderResource, 0, 203.0f);
			});
			const double synthMs = GpuMs(dev, ctx, 10, [&] {
				synth.Make(ctx, texA.pShaderResource, texC.pShaderResource, flow.MotionView(), outUav, t, 0, 203.0f, 0.6f, 1.0f);
			});
			Out("  GPU, %dx%d: every picture, the copy %.3f ms and the detector %.3f ms; a made picture, the motion %.2f ms and the synthesis %.2f ms\n",
				W, H, copyMs, cutMs, flowMs, synthMs);
		}
		Check(bCost, "the costs measured");
	}
	if (SUCCEEDED(coInit)) {
		CoUninitialize();
	}
	if (g_report) {
		fclose(g_report);
		g_report = nullptr;
	}
	return (rc || g_failures) ? 1 : 0;
}

} // namespace temporal
