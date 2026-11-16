// What EchoFG costs the GPU, pass by pass, and whether the filter's Direct3D 11 port of it
// costs more than EchoStorm's own Direct3D 12 code.
//
//   --techoperf [--timage PICTURE] [--techoruns N]
//                    a pan of the picture, at 1920x1080, 3840x1600 and 3840x2160, SDR and HDR:
//                    the made picture as the renderer makes it (CadenceMake: the motion of the
//                    picture before from nothing, then of the picture after, then the
//                    synthesis), each pass timed on the GPU; then the whole, interleaved, run
//                    after run, with EchoStorm's FlowEstimator, FlowFix and OwnSynth driven the
//                    same way on a Direct3D 12 device of their own, where that code is beside
//                    the harness (HAVE_ECHOSTORM). The GPU's clock is read from nvidia-smi
//                    before and after each size: a card that is not at its full clock makes
//                    every number here larger, the same for both. Then the other ways it can
//                    be run -- the picture before's pyramid alone (CEchoFlow::Prime), the
//                    synthesis in cells of 1, 2 and 4, the motion at a quarter of the frames'
//                    size and at their full size -- each timed, and its picture against the
//                    one truly between the two and against the first's.

#include <map>

namespace temporal {

namespace {

// The passes' timestamps, from the hooks CEchoFlow and CEchoSynth11 call around each
// dispatch. A key is the pass (CEchoFlow's) or 100 + the entry point (CEchoSynth11's).
struct EchoPassProfile {
	ID3D11Device* dev = nullptr;
	std::vector<CComPtr<ID3D11Query>> pool;
	size_t used = 0, open = 0;
	int phase = 0;   // 0 the picture before's motion, 1 the picture after's, 2 the synthesis
	struct Rec { int phase, key; size_t q0, q1; };
	std::vector<Rec> recs;

	ID3D11Query* Next()
	{
		if (used == pool.size()) {
			CComPtr<ID3D11Query> q;
			D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP, 0 };
			dev->CreateQuery(&qd, &q);
			pool.push_back(q);
		}
		return pool[used++];
	}
	void Hook(ID3D11DeviceContext* ctx, int key, bool bEnd)
	{
		if (!bEnd) {
			open = used;
			ctx->End(Next());
		} else {
			const size_t q1 = used;
			ctx->End(Next());
			recs.push_back({ phase, key, open, q1 });
		}
	}
};
EchoPassProfile g_echoProfile;

const char* EchoPassName(int key)
{
	static const char* flow[] = { "Luma", "Down", "Global", "Search", "Median", "Pixel", "FixLuma", "FixTiles", "FixDecide", "FixApply" };
	static const char* synth[] = { "Clear", "Err0", "Box5", "ErrA", "Splat", "Hyp1", "Hyp2", "Holes", "Synth", "MarkPrep", "MarkLarge",
		"MarkUpdate", "GrainStat" };
	if (key >= 100 && key - 100 < (int)std::size(synth)) {
		return synth[key - 100];
	}
	return key >= 0 && key < (int)std::size(flow) ? flow[key] : "?";
}

double Median(std::vector<double> v)
{
	if (v.empty()) {
		return -1;
	}
	std::sort(v.begin(), v.end());
	return v[v.size() / 2];
}

std::string GpuClocks()
{
	std::string s;
	if (FILE* p = OpenPipe(L"nvidia-smi --query-gpu=clocks.gr,clocks.mem,pstate,utilization.gpu --format=csv,noheader")) {
		char line[256] = {};
		if (fgets(line, sizeof(line), p)) {
			s = line;
			while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
				s.pop_back();
			}
		}
		_pclose(p);
	}
	return s.empty() ? "(nvidia-smi said nothing)" : s;
}

#ifdef HAVE_ECHOSTORM
// EchoStorm's FlowEstimator, FlowFix and OwnSynth on a Direct3D 12 device of their own, given
// the pair the port is given and driven in the renderer's order: the picture before from
// nothing, the picture after, the synthesis, in one command list with a timestamp after each.
class CEchoOrigTimed
{
public:
	~CEchoOrigTimed()
	{
		m_estimate.Shutdown();
		m_fix.Shutdown();
		m_synth.Shutdown();
		if (m_event) {
			CloseHandle(m_event);
		}
	}

	bool Init(std::string& error)
	{
		D3D12_COMMAND_QUEUE_DESC qd = {};
		qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		D3D12_QUERY_HEAP_DESC hd = {};
		hd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
		hd.Count = 4;
		if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_dev)))
				|| FAILED(m_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queue)))
				|| FAILED(m_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_alloc)))
				|| FAILED(m_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_alloc, nullptr, IID_PPV_ARGS(&m_list)))
				|| FAILED(m_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)))
				|| FAILED(m_dev->CreateQueryHeap(&hd, IID_PPV_ARGS(&m_queries)))
				|| !Buffer(4 * sizeof(UINT64), D3D12_HEAP_TYPE_READBACK, m_stamps)
				|| FAILED(m_queue->GetTimestampFrequency(&m_freq))) {
			error = "no Direct3D 12 device, queue and timestamps";
			return false;
		}
		m_list->Close();
		m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		auto log = [this](const char* s) { m_log = s; };
		if (!m_estimate.Init(m_dev, log) || !m_fix.Init(m_dev, log) || !m_synth.Init(m_dev, log)) {
			error = "EchoStorm's own code: " + m_log;
			return false;
		}
		return true;
	}

	bool Prepare(UINT W, UINT H, const std::vector<uint8_t>& a, const std::vector<uint8_t>& c, int synthScale, int flowScale)
	{
		m_W = W;
		m_H = H;
		const UINT scale = flowScale > 0 ? (UINT)flowScale : W > 2560 ? 2 : 1;
		const UINT aw = (W + scale - 1) / scale, ah = (H + scale - 1) / scale;
		m_estimate.SetInputScale((int)scale);
		m_synth.SetAnalysisScale(synthScale);
		bool ok = m_estimate.Ensure(aw, ah) && m_fix.Ensure(aw, ah, scale, W, H) && m_synth.Ensure(W, H)
			&& Texture(W, H, DXGI_FORMAT_B8G8R8A8_UNORM, false, D3D12_RESOURCE_STATE_COPY_DEST, m_a)
			&& Texture(W, H, DXGI_FORMAT_B8G8R8A8_UNORM, false, D3D12_RESOURCE_STATE_COPY_DEST, m_c)
			&& Texture(aw, ah, DXGI_FORMAT_R16G16_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, m_motion)
			&& Texture(aw, ah, DXGI_FORMAT_R8_UNORM, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, m_distrust)
			&& Texture(W, H, DXGI_FORMAT_R16G16_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, m_vectors)
			&& Texture(W, H, DXGI_FORMAT_B8G8R8A8_UNORM, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, m_out);
		if (!ok) {
			return false;
		}
		D3D12_RESOURCE_DESC d = m_a->GetDesc();
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot = {};
		UINT64 bytes = 0;
		m_dev->GetCopyableFootprints(&d, 0, 1, 0, &foot, nullptr, nullptr, &bytes);
		CComPtr<ID3D12Resource> upA, upC;
		if (!Buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, upA) || !Buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, upC)) {
			return false;
		}
		for (auto [up, src] : { std::pair{ upA.p, &a }, std::pair{ upC.p, &c } }) {
			BYTE* p = nullptr;
			if (FAILED(up->Map(0, nullptr, (void**)&p))) {
				return false;
			}
			for (UINT y = 0; y < H; y++) {
				memcpy(p + foot.Offset + (size_t)foot.Footprint.RowPitch * y, src->data() + 4 * (size_t)W * y, 4 * (size_t)W);
			}
			up->Unmap(0, nullptr);
		}
		m_alloc->Reset();
		m_list->Reset(m_alloc, nullptr);
		for (auto [tex, up] : { std::pair{ m_a.p, upA.p }, std::pair{ m_c.p, upC.p } }) {
			D3D12_TEXTURE_COPY_LOCATION dst = { tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
			D3D12_TEXTURE_COPY_LOCATION src = { up, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
			src.PlacedFootprint = foot;
			m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			Barrier(tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		}
		return Submit();
	}

	// One made picture; ms[0..2]: the picture before's motion, the picture after's, the
	// synthesis. False when the GPU says the timestamps are not usable.
	bool Time(uint32_t encoding, double ms[3])
	{
		m_alloc->Reset();
		m_list->Reset(m_alloc, nullptr);
		const int slot = (int)(m_slot++ % FlowEstimator::kSlots);
		const DXGI_FORMAT f = DXGI_FORMAT_B8G8R8A8_UNORM;
		const float white = 203.0f;
		m_list->EndQuery(m_queries, D3D12_QUERY_TYPE_TIMESTAMP, 0);
		m_estimate.Forget();
		for (int k = 0; k < 2; k++) {
			ID3D12Resource* now = k ? m_c.p : m_a.p;
			m_estimate.Record(m_list, slot, now, f, m_motion, m_distrust, 0.0f, encoding, white);
			Barrier(m_motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			Barrier(m_distrust, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			m_fix.Record(m_list, slot, m_a, f, now, f, m_motion, m_distrust, m_vectors, encoding, white, k == 1);
			Barrier(m_motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			Barrier(m_distrust, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			m_list->EndQuery(m_queries, D3D12_QUERY_TYPE_TIMESTAMP, 1 + k);
		}
		Barrier(m_vectors, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		const nr::OwnSynth::Target to = { m_out, 0.5f };
		m_synth.Record(m_list, slot, m_a, f, m_c, f, m_vectors, f, &to, 1, encoding, white, 0.6f, 0.0f, nullptr, encoding == 0 ? 1.0f : 0.0f);
		Barrier(m_vectors, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		m_list->EndQuery(m_queries, D3D12_QUERY_TYPE_TIMESTAMP, 3);
		m_list->ResolveQueryData(m_queries, D3D12_QUERY_TYPE_TIMESTAMP, 0, 4, m_stamps, 0);
		if (!Submit()) {
			return false;
		}
		UINT64* t = nullptr;
		const D3D12_RANGE all = { 0, 4 * sizeof(UINT64) };
		if (FAILED(m_stamps->Map(0, &all, (void**)&t))) {
			return false;
		}
		for (int i = 0; i < 3; i++) {
			ms[i] = (double)(t[i + 1] - t[i]) * 1000.0 / (double)m_freq;
		}
		const D3D12_RANGE none = { 0, 0 };
		m_stamps->Unmap(0, &none);
		return true;
	}

private:
	bool Submit()
	{
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
		return true;
	}
	bool Texture(UINT w, UINT h, DXGI_FORMAT f, bool bUav, D3D12_RESOURCE_STATES state, CComPtr<ID3D12Resource>& out)
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
		return SUCCEEDED(m_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&out)));
	}
	bool Buffer(UINT64 bytes, D3D12_HEAP_TYPE type, CComPtr<ID3D12Resource>& out)
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
	CComPtr<ID3D12QueryHeap> m_queries;
	CComPtr<ID3D12Resource> m_stamps;
	UINT64 m_fenceValue = 0, m_freq = 1;
	HANDLE m_event = nullptr;
	FlowEstimator m_estimate;
	nr::FlowFix m_fix;
	nr::OwnSynth m_synth;
	std::string m_log;
	UINT m_W = 0, m_H = 0;
	unsigned m_slot = 0;
	CComPtr<ID3D12Resource> m_a, m_c, m_motion, m_distrust, m_vectors, m_out;
};
#endif

} // namespace

// What CadenceMake does, and what it might do instead: the picture before's pyramid alone
// (Prime) in place of its whole motion from nothing, the synthesis in cells of 4, the motion
// analysed at a quarter of the frames' size.
struct EchoVariant {
	const char* name;
	bool bPrime;
	uint32_t synthScale;   // CEchoSynth11's cells
	UINT flowScale;        // CEchoFlow's analysis scale, 0 by the width
};

static bool PictureSize(IWICImagingFactory* factory, const wchar_t* path, int& W, int& H)
{
	CComPtr<IWICBitmapDecoder> decoder;
	CComPtr<IWICBitmapFrameDecode> frame;
	UINT w = 0, h = 0;
	if (!factory || FAILED(factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))
			|| FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&w, &h))) {
		return false;
	}
	W = (int)w;
	H = (int)h;
	return true;
}

// triplet: PREFIX_1.png, PREFIX_2.png, PREFIX_3.png, three pictures of a film in a row: the
// one made half way between the first and the third is held against the second, at their own
// size, in place of the pan. bHdr: their bytes are PQ code values.
static int RunEchoPerf(ID3D11Device* dev, ID3D11DeviceContext* ctx, const wchar_t* image, int runs, const wchar_t* triplet, bool bHdr)
{
	Head("EchoFG's cost on the GPU, pass by pass, and the port against EchoStorm's own code");
	g_report = _wfopen(L"temporal_results_echoperf.txt", L"w");
	CComPtr<IWICImagingFactory> factory;
	const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
	runs = std::clamp(runs, 3, 200);
	int rc = 0;

#ifdef HAVE_ECHOSTORM
	CEchoOrigTimed orig;
	std::string origError;
	const bool bOrig = orig.Init(origError);
	Check(bOrig, ("EchoStorm's own motion and synthesis on Direct3D 12 " + origError).c_str());
#else
	Out("  (EchoStorm's own code is not beside the harness: the port is timed alone)\n");
#endif

	static const EchoVariant variants[] = {
		{ "the whole motion of the picture before, cells of 2", false, 2, 0 },
		{ "the picture before's pyramid alone, cells of 2", true, 2, 0 },
		{ "the picture before's pyramid alone, cells of 1", true, 1, 0 },
		{ "the picture before's pyramid alone, cells of 4", true, 4, 0 },
		{ "  and the motion at a quarter size", true, 4, 4 },
		{ "the motion at the full size, cells of 2", true, 2, 1 },
		{ "the motion at the full size, cells of 4", true, 4, 1 },
	};
	struct Case { int W, H; uint32_t encoding; };
	std::vector<Case> cases = { { 1920, 1080, 0 }, { 3840, 1600, 2 }, { 3840, 2160, 2 }, { 3840, 2160, 0 } };
	const std::wstring prefix = triplet ? triplet : L"";
	if (!prefix.empty()) {
		int tw = 0, th = 0;
		if (!PictureSize(factory, (prefix + L"_1.png").c_str(), tw, th)) {
			Check(false, "the first of the three pictures");
			return 1;
		}
		cases = { { tw, th, bHdr ? 2u : 0u } };
	}
	for (const Case cs : cases) {
		const int W = cs.W, H = cs.H;
		const uint32_t enc = cs.encoding;
		std::vector<uint8_t> a, c, truth;
		int margin = 8;
		if (prefix.empty()) {
			std::vector<float> rgba;
			std::string error;
			if (!factory || !LoadPicture(factory, image, W, H, rgba, error)) {
				Check(false, ("the picture: " + error).c_str());
				rc = 1;
				break;
			}
			// A pan of (5.3, 1.7) pixels at 1080p, and the picture truly half way.
			const double sx = 5.3 * W / 1920, sy = 1.7 * W / 1920;
			a = Shifted(rgba, W, H, 0, 0);
			c = Shifted(rgba, W, H, sx, sy);
			truth = Shifted(rgba, W, H, 0.5 * sx, 0.5 * sy);
			margin = (int)std::ceil(std::max(sx, sy)) + 4;
		} else {
			std::vector<uint8_t>* outs[3] = { &a, &truth, &c };
			for (int k = 0; k < 3; k++) {
				std::vector<float> rgba;
				std::string error;
				if (!LoadPicture(factory, std::format(L"{}_{}.png", prefix, k + 1).c_str(), W, H, rgba, error)) {
					Check(false, ("the three pictures: " + error).c_str());
					return 1;
				}
				*outs[k] = Shifted(rgba, W, H, 0, 0);
			}
			std::vector<uint8_t> blend(a.size());
			for (size_t i = 0; i < a.size(); i++) {
				blend[i] = (uint8_t)((a[i] + c[i] + 1) / 2);
			}
			Out("\n  %S, the second of three against the first and the third: PSNR %.2f dB to the first, %.2f to the plain blend\n",
				prefix.c_str(), PsnrBgra(truth, a, W, H, false, margin), PsnrBgra(truth, blend, W, H, false, margin));
		}
		Tex2D_t texA, texC, texOut;
		CComPtr<ID3D11UnorderedAccessView> outUav;
		CEchoFlow flow;
		CEchoSynth11 synth;
		const bool ok = UploadBgra(dev, W, H, a, texA) && UploadBgra(dev, W, H, c, texC)
			&& SUCCEEDED(texOut.Create(dev, DXGI_FORMAT_B8G8R8A8_UNORM, W, H, Tex2D_DefaultShaderRTargetUAV))
			&& SUCCEEDED(dev->CreateUnorderedAccessView(texOut.pTexture, nullptr, &outUav))
			&& flow.Init(dev) && synth.Init(dev);
		Check(ok, std::format("the port at {}x{}", W, H).c_str());
		if (!ok) {
			rc = 1;
			continue;
		}
		Out("\n  %dx%d %s; GPU before: %s\n", W, H, enc ? "HDR10 (the picture's bytes taken as PQ)" : "SDR", GpuClocks().c_str());
		const char* truthName = prefix.empty() ? "the picture truly half way" : "the second picture";

		std::vector<uint8_t> reference;
		for (const EchoVariant& v : variants) {
			flow.SetAnalysisScale(v.flowScale);
			synth.SetAnalysisScale(v.synthScale);
			if (!flow.Ensure(dev, W, H) || !synth.Ensure(dev, W, H)) {
				Check(false, std::format("{} at {}x{}", v.name, W, H).c_str());
				rc = 1;
				continue;
			}
			auto make = [&](EchoPassProfile* prof) {
				flow.Forget();
				if (prof) {
					prof->phase = 0;
				}
				if (v.bPrime) {
					flow.Prime(ctx, texA.pShaderResource, enc, 203.0f);
				} else {
					flow.Measure(ctx, texA.pShaderResource, nullptr, enc, 203.0f);
				}
				if (prof) {
					prof->phase = 1;
				}
				flow.Measure(ctx, texC.pShaderResource, texA.pShaderResource, enc, 203.0f);
				if (prof) {
					prof->phase = 2;
				}
				synth.Make(ctx, texA.pShaderResource, texC.pShaderResource, flow.MotionView(), outUav, 0.5f, enc, 203.0f, 0.6f,
					enc == 0 ? 1.0f : 0.0f);
			};
			Out("    %s: motion at 1/%u, synthesis in cells of %u\n", v.name, flow.AnalysisScale(), synth.AnalysisScale());

			// The picture it makes: against the one truly there, and against the renderer's.
			make(nullptr);
			std::vector<uint8_t> made;
			if (ReadTexture(dev, ctx, texOut.pTexture, 4, made)) {
				if (reference.empty()) {
					reference = made;
				}
				size_t differ = 0;
				for (size_t i = 0; i < made.size(); i++) {
					differ += made[i] != reference[i] ? 1 : 0;
				}
				Out("      PSNR against %s %.2f dB; %s\n", truthName, PsnrBgra(truth, made, W, H, false, margin),
					&v == &variants[0] ? "the reference for the others"
					: differ ? std::format("{:.2f} dB against the renderer's, {} values differ", PsnrBgra(reference, made, W, H, false, margin), differ).c_str()
					: "the renderer's picture to the byte");
			}

			// Each pass: the median of the runs, a pass run more than once summed.
			if (&v == &variants[1] || &v == &variants[2] || &v == &variants[3]) {
				g_echoProfile.dev = dev;
				CEchoFlow::s_passHook = [](ID3D11DeviceContext* x, int pass, bool bEnd) { g_echoProfile.Hook(x, pass, bEnd); };
				CEchoSynth11::s_passHook = [](ID3D11DeviceContext* x, int entry, bool bEnd) { g_echoProfile.Hook(x, 100 + entry, bEnd); };
				CComPtr<ID3D11Query> disjoint;
				D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
				dev->CreateQuery(&qd, &disjoint);
				std::map<std::pair<int, int>, std::vector<double>> perPass;
				std::map<std::pair<int, int>, int> calls;
				std::vector<double> phaseMs[3];
				for (int r = 0; r < runs; r++) {
					g_echoProfile.used = 0;
					g_echoProfile.recs.clear();
					ctx->Begin(disjoint);
					make(&g_echoProfile);
					ctx->End(disjoint);
					D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
					while (ctx->GetData(disjoint, &dj, sizeof(dj), 0) == S_FALSE) {
						Sleep(0);
					}
					if (dj.Disjoint || !dj.Frequency) {
						continue;
					}
					std::vector<UINT64> t(g_echoProfile.used);
					for (size_t i = 0; i < t.size(); i++) {
						while (ctx->GetData(g_echoProfile.pool[i], &t[i], sizeof(UINT64), 0) == S_FALSE) {
							Sleep(0);
						}
					}
					std::map<std::pair<int, int>, double> sum;
					std::map<std::pair<int, int>, int> n;
					double phaseSum[3] = {};
					for (const auto& rec : g_echoProfile.recs) {
						const double ms = (double)(t[rec.q1] - t[rec.q0]) * 1000.0 / (double)dj.Frequency;
						sum[{ rec.phase, rec.key }] += ms;
						n[{ rec.phase, rec.key }]++;
						phaseSum[rec.phase] += ms;
					}
					for (const auto& [k, value] : sum) {
						perPass[k].push_back(value);
					}
					calls = n;
					for (int p = 0; p < 3; p++) {
						phaseMs[p].push_back(phaseSum[p]);
					}
				}
				CEchoFlow::s_passHook = nullptr;
				CEchoSynth11::s_passHook = nullptr;
				static const char* phases[] = { "the picture before", "the picture after", "the synthesis" };
				for (int p = 0; p < 3; p++) {
					Out("      %-20s %7.3f ms:", phases[p], Median(phaseMs[p]));
					for (const auto& [k, value] : perPass) {
						if (k.first == p) {
							const int nCalls = calls[k];
							if (nCalls > 1) {
								Out("  %s %.3f (x%d)", EchoPassName(k.second), Median(value), nCalls);
							} else {
								Out("  %s %.3f", EchoPassName(k.second), Median(value));
							}
						}
					}
					Out("\n");
				}
			}

			// The whole, the port's and the original's in turn, so that both see the same clock.
			std::vector<double> port, origMs[3], origAll;
			CComPtr<ID3D11Query> disjoint, t0, t1;
			D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
			dev->CreateQuery(&qd, &disjoint);
			qd.Query = D3D11_QUERY_TIMESTAMP;
			dev->CreateQuery(&qd, &t0);
			dev->CreateQuery(&qd, &t1);
#ifdef HAVE_ECHOSTORM
			const bool bOrigHere = bOrig && !v.bPrime && orig.Prepare(W, H, a, c, (int)v.synthScale, (int)v.flowScale);
#else
			const bool bOrigHere = false;
#endif
			for (int r = 0; r < runs; r++) {
				ctx->Begin(disjoint);
				ctx->End(t0);
				make(nullptr);
				ctx->End(t1);
				ctx->End(disjoint);
				D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
				UINT64 x0 = 0, x1 = 0;
				while (ctx->GetData(disjoint, &dj, sizeof(dj), 0) == S_FALSE) {
					Sleep(0);
				}
				while (ctx->GetData(t0, &x0, sizeof(x0), 0) == S_FALSE) {
					Sleep(0);
				}
				while (ctx->GetData(t1, &x1, sizeof(x1), 0) == S_FALSE) {
					Sleep(0);
				}
				if (!dj.Disjoint && dj.Frequency) {
					port.push_back((double)(x1 - x0) * 1000.0 / (double)dj.Frequency);
				}
#ifdef HAVE_ECHOSTORM
				double ms[3] = {};
				if (bOrigHere && orig.Time(enc, ms)) {
					for (int p = 0; p < 3; p++) {
						origMs[p].push_back(ms[p]);
					}
					origAll.push_back(ms[0] + ms[1] + ms[2]);
				}
#endif
			}
			if (!port.empty()) {
				const auto [lo, hi] = std::minmax_element(port.begin(), port.end());
				Out("      the port, whole: median %7.3f ms (%.3f to %.3f, %zu runs)", Median(port), *lo, *hi, port.size());
				if (!origAll.empty()) {
					Out("; EchoStorm's own on Direct3D 12: median %.3f ms (before %.3f, after %.3f, synthesis %.3f): the port takes %.2f of its time",
						Median(origAll), Median(origMs[0]), Median(origMs[1]), Median(origMs[2]), Median(port) / Median(origAll));
				}
				Out("\n");
			}
		}
		Out("    GPU after: %s\n", GpuClocks().c_str());
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
