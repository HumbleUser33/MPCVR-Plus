// --tstab: the post-DLSS stabilizer on real pictures with known motion.
//
// Included by harness.cpp after flow_suite.inl. Runs DLSS 5 NR plainly, then the
// stabilizer of Shaders/d3d11/ps_dlss_stabilize.hlsl in its two variants -- EFFECT
// filters only out - in, OUTPUT filters the output -- with two sources of motion:
// NVIDIA Optical Flow and the true motion. Also Optical Flow vectors given to the
// network as DLSSNR.MVec. The shader detector measured here as a third source until
// it left the filter. Measures the shimmer
// (low-frequency frame-to-frame change, compensated by the true motion), how much
// of the network's effect survives, grain, and the deviation from plain DLSS
// overall, on edges and where a moving object was.
//
// --tstabport runs the renderer's CDlssStabilizer, shaders embedded the way the
// filter embeds them, next to these passes on the same pictures and the same
// network output, and redraws every fifth picture once more: its rows must
// measure what the rows above them measure, and every redraw must show exactly
// what its picture showed.

#include "DLSS/DlssStabilizer.h"

namespace temporal {

class CStabilizerPasses
{
public:
	struct Params {
		float minCurrent   = 0.25f;   // weight of the current frame where the history is trusted
		float tolerance    = 0.02f;
		float reprojection = 0.03f;
		bool  reset        = false;
		bool  outputMode   = false;
	};

	bool Init(ID3D11Device* dev, std::string& error);

	ID3D11VertexShader* VertexShader() const { return m_vs; }
	ID3D11InputLayout*  InputLayout() const { return m_layout; }
	ID3D11SamplerState* SamplerPoint() const { return m_point; }
	ID3D11SamplerState* SamplerLinear() const { return m_linear; }

	void FlowFrame(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* picture, ID3D11RenderTargetView* target,
	               UINT width, UINT height, int factor);
	void FlowMotion(ID3D11DeviceContext* ctx, const CDlssOpticalFlow& of, ID3D11RenderTargetView* motion,
	                ID3D11RenderTargetView* confidence, UINT width, UINT height);
	void Stabilize(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* current, ID3D11ShaderResourceView* network,
	               ID3D11ShaderResourceView* history, ID3D11ShaderResourceView* motion,
	               ID3D11ShaderResourceView* previous, ID3D11ShaderResourceView* confidence,
	               ID3D11RenderTargetView* historyOut, ID3D11RenderTargetView* colorOut,
	               UINT width, UINT height, const Params& p);

private:
	void Draw(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, std::initializer_list<ID3D11RenderTargetView*> targets,
	          UINT width, UINT height, std::initializer_list<ID3D11ShaderResourceView*> inputs,
	          const float* constants, size_t count);

	CComPtr<ID3D11VertexShader> m_vs;
	CComPtr<ID3D11InputLayout>  m_layout;
	CComPtr<ID3D11PixelShader>  m_ps[3];
	CComPtr<ID3D11SamplerState> m_point;
	CComPtr<ID3D11SamplerState> m_linear;
	CComPtr<ID3D11Buffer>       m_vb;
	CComPtr<ID3D11Buffer>       m_cb;
};

bool CStabilizerPasses::Init(ID3D11Device* dev, std::string& error)
{
	const wchar_t* psFile = L"..\\..\\Shaders\\d3d11\\ps_dlss_stabilize.hlsl";
	const wchar_t* vsFile = L"..\\..\\Shaders\\d3d11\\vs_simple.hlsl";

	CComPtr<ID3DBlob> code, errors;
	HRESULT hr = D3DCompileFromFile(vsFile, nullptr, nullptr, "main", "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
	if (FAILED(hr) || FAILED(dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &m_vs))) {
		error = errors ? std::string((const char*)errors->GetBufferPointer(), errors->GetBufferSize()) : "vs_simple.hlsl";
		return false;
	}
	const D3D11_INPUT_ELEMENT_DESC layout[] = {
		{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}
	};
	if (FAILED(dev->CreateInputLayout(layout, (UINT)std::size(layout), code->GetBufferPointer(), code->GetBufferSize(), &m_layout))) {
		error = "CreateInputLayout failed";
		return false;
	}

	for (int pass = 0; pass < 3; pass++) {
		const char passText[2] = { char('0' + pass), 0 };
		const D3D_SHADER_MACRO defines[] = { { "PASS", passText }, { nullptr, nullptr } };
		code.Release();
		errors.Release();
		hr = D3DCompileFromFile(psFile, defines, nullptr, "main", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
		if (FAILED(hr)) {
			error = std::string("ps_dlss_stabilize.hlsl PASS ") + passText + ": "
				+ (errors ? std::string((const char*)errors->GetBufferPointer(), errors->GetBufferSize()) : "not found");
			return false;
		}
		if (FAILED(dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &m_ps[pass]))) {
			error = "CreatePixelShader failed";
			return false;
		}
	}

	// The renderer's samplers.
	D3D11_SAMPLER_DESC sd = {};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	dev->CreateSamplerState(&sd, &m_point);
	sd.Filter = D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;
	dev->CreateSamplerState(&sd, &m_linear);

	// The renderer's quad (FillVertices, unrotated): survives back-face culling.
	const Vertex11 quad[4] = {
		{ -1, -1, 0, 0, 1 }, { -1, 1, 0, 0, 0 }, { 1, -1, 0, 1, 1 }, { 1, 1, 0, 1, 0 },
	};
	D3D11_BUFFER_DESC bd = { sizeof(quad), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
	D3D11_SUBRESOURCE_DATA init = { quad, 0, 0 };
	dev->CreateBuffer(&bd, &init, &m_vb);
	D3D11_BUFFER_DESC cbd = { 64, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
	dev->CreateBuffer(&cbd, nullptr, &m_cb);
	if (!m_point || !m_linear || !m_vb || !m_cb) {
		error = "pipeline state objects";
		return false;
	}
	return true;
}

void CStabilizerPasses::Draw(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, std::initializer_list<ID3D11RenderTargetView*> targets,
                             UINT width, UINT height, std::initializer_list<ID3D11ShaderResourceView*> inputs,
                             const float* constants, size_t count)
{
	ID3D11RenderTargetView* rtvs[2] = {};
	const UINT numTargets = (UINT)std::min<size_t>(targets.size(), std::size(rtvs));
	std::copy_n(targets.begin(), numTargets, rtvs);
	ID3D11ShaderResourceView* srvs[6] = {};
	std::copy_n(inputs.begin(), std::min<size_t>(inputs.size(), std::size(srvs)), srvs);

	if (constants && count) {
		D3D11_MAPPED_SUBRESOURCE mr = {};
		if (SUCCEEDED(ctx->Map(m_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr))) {
			float block[16] = {};
			std::copy_n(constants, std::min<size_t>(count, std::size(block)), block);
			memcpy(mr.pData, block, sizeof(block));
			ctx->Unmap(m_cb, 0);
		}
	}

	const D3D11_VIEWPORT vp = { 0, 0, (FLOAT)width, (FLOAT)height, 0, 1 };
	const UINT stride = sizeof(Vertex11), offset = 0;
	ID3D11Buffer* vb = m_vb;
	ID3D11Buffer* cb = m_cb;
	ID3D11SamplerState* samplers[2] = { m_point, m_linear };

	ctx->IASetInputLayout(m_layout);
	ctx->OMSetRenderTargets(numTargets, rtvs, nullptr);
	ctx->RSSetViewports(1, &vp);
	ctx->RSSetState(nullptr);
	ctx->OMSetBlendState(nullptr, nullptr, D3D11_DEFAULT_SAMPLE_MASK);
	ctx->VSSetShader(m_vs, nullptr, 0);
	ctx->PSSetShader(ps, nullptr, 0);
	ctx->PSSetShaderResources(0, (UINT)std::size(srvs), srvs);
	ctx->PSSetSamplers(0, 2, samplers);
	ctx->PSSetConstantBuffers(0, 1, &cb);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
	ctx->Draw(4, 0);

	ID3D11ShaderResourceView* noViews[6] = {};
	ctx->PSSetShaderResources(0, (UINT)std::size(noViews), noViews);
	ID3D11RenderTargetView* noTargets[2] = {};
	ctx->OMSetRenderTargets(2, noTargets, nullptr);
}

void CStabilizerPasses::FlowFrame(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* picture, ID3D11RenderTargetView* target,
                                  UINT width, UINT height, int factor)
{
	const float constants[4] = { (float)factor, 0, 0, 0 };
	Draw(ctx, m_ps[0], { target }, width, height, { picture }, constants, std::size(constants));
}

void CStabilizerPasses::FlowMotion(ID3D11DeviceContext* ctx, const CDlssOpticalFlow& of, ID3D11RenderTargetView* motion,
                                   ID3D11RenderTargetView* confidence, UINT width, UINT height)
{
	const float constants[12] = {
		(float)width / of.Width(), (float)height / of.Height(), (float)of.GridSize(), 1.0f,   // consistency: 1 flow pixel
		(float)of.Width(), (float)of.Height(), 48.0f, 160.0f,                               // cost range
		of.Bidirectional() ? 1.0f : 0.0f, of.HasCost() ? 1.0f : 0.0f, 0, 0
	};
	Draw(ctx, m_ps[1], { motion, confidence }, width, height,
	     { of.ForwardFlow(), of.BackwardFlow(), of.ForwardCost() }, constants, std::size(constants));
}

void CStabilizerPasses::Stabilize(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* current, ID3D11ShaderResourceView* network,
                                  ID3D11ShaderResourceView* history, ID3D11ShaderResourceView* motion,
                                  ID3D11ShaderResourceView* previous, ID3D11ShaderResourceView* confidence,
                                  ID3D11RenderTargetView* historyOut, ID3D11RenderTargetView* colorOut,
                                  UINT width, UINT height, const Params& p)
{
	const float constants[8] = {
		p.minCurrent, p.tolerance, 0, p.reprojection,
		p.reset ? 1.0f : 0.0f, p.outputMode ? 1.0f : 0.0f, 0, 0
	};
	Draw(ctx, m_ps[2], { historyOut, colorOut }, width, height,
	     { current, network, history, motion, previous, confidence }, constants, std::size(constants));
}

struct StabSequence {
	const char* name;
	float vx, vy;   // background content motion, pixels per frame
	bool  object;   // a window of another picture moving over it
	float ux, uy;   // object motion
	int   cutAt;    // frame where the view jumps to another part of the picture, or -1
};

enum StabMotion { MotionNone, MotionFlow, MotionTruth };

struct StabConfig {
	const char* name;
	int   motion;       // StabMotion
	bool  stabilize;
	bool  outputMode;
	bool  mvecToDlss;   // Optical Flow vectors given to the network
	float minCurrent;
	bool  renderer;     // motion and stabilizing by the renderer's CDlssStabilizer
};

// Bilinear RGBA window of a larger picture.
static void WindowRgba(const std::vector<float>& big, int BW, int BH, float ox, float oy, int W, int H, std::vector<float>& out)
{
	out.resize(4 * (size_t)W * H);
	for (int y = 0; y < H; y++) {
		const float fy = std::clamp(oy + y, 0.0f, (float)BH - 1.001f);
		const int y0 = (int)fy;
		const float ty = fy - y0;
		for (int x = 0; x < W; x++) {
			const float fx = std::clamp(ox + x, 0.0f, (float)BW - 1.001f);
			const int x0 = (int)fx;
			const float tx = fx - x0;
			const float* a = &big[4 * ((size_t)y0 * BW + x0)];
			const float* b = a + 4;
			const float* c = &big[4 * ((size_t)(y0 + 1) * BW + x0)];
			const float* d = c + 4;
			float* o = &out[4 * ((size_t)y * W + x)];
			for (int k = 0; k < 3; k++) {
				o[k] = (a[k] * (1 - tx) + b[k] * tx) * (1 - ty) + (c[k] * (1 - tx) + d[k] * tx) * ty;
			}
			o[3] = 1.0f;
		}
	}
}

// Every texel of an RGBA16F texture, through a staging copy of the same size.
static bool ReadBytes(ID3D11DeviceContext* ctx, ID3D11Texture2D* stage, ID3D11Texture2D* tex, std::vector<BYTE>& out)
{
	ctx->CopyResource(stage, tex);
	D3D11_TEXTURE2D_DESC desc = {};
	stage->GetDesc(&desc);
	D3D11_MAPPED_SUBRESOURCE mr = {};
	if (FAILED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &mr))) {
		return false;
	}
	const size_t rowBytes = 8 * (size_t)desc.Width;
	out.resize(rowBytes * desc.Height);
	for (UINT y = 0; y < desc.Height; y++) {
		memcpy(&out[rowBytes * y], (const BYTE*)mr.pData + (size_t)mr.RowPitch * y, rowBytes);
	}
	ctx->Unmap(stage, 0);
	return true;
}

static int RunStab(ID3D11Device* dev, ID3D11DeviceContext* ctx, CDlssNR& dlss, const wchar_t* imagePath, int frames, bool strong, bool port)
{
	const int W = 1920, H = 1080, BW = 2400, BH = 1350;
	const int w2 = W / 2, h2 = H / 2, w4 = W / 4, h4 = H / 4;
	const int warm = 16;
	const int OW = 480, OH = 320;
	const float originX = 400, originY = 240;      // leaves room for 72 frames of the fastest pan
	const float cutX = 60, cutY = 30;
	frames = std::max(frames, warm + 24);

	static const StabSequence seqs[] = {
		{ "T1 still picture, grain and 8x8 block noise",           0.0f, 0.0f, false, 0.0f, 0.0f, -1 },
		{ "T2 slow pan (0.5, 0.2) px/frame",                        0.5f, 0.2f, false, 0.0f, 0.0f, -1 },
		{ "T3 pan (3, 1) px/frame",                                 3.0f, 1.0f, false, 0.0f, 0.0f, -1 },
		{ "T4 object moving (4, 1) px/frame over a still picture",  0.0f, 0.0f, true,  4.0f, 1.0f, -1 },
		{ "T5 cut to another part of the picture at frame 40",      0.0f, 0.0f, false, 0.0f, 0.0f, 40 },
	};
	static const StabConfig measureCfgs[] = {
		{ "plain",                  MotionNone,     false, false, false, 1.0f,  false },
		{ "effect, optical flow",   MotionFlow,     true,  false, false, 0.25f, false },
		{ "effect, true motion",    MotionTruth,    true,  false, false, 0.25f, false },
		{ "output, optical flow",   MotionFlow,     true,  true,  false, 0.25f, false },
		{ "MVec from flow",         MotionFlow,     false, false, true,  1.0f,  false },
		{ "MVec + effect, flow",    MotionFlow,     true,  false, true,  0.25f, false },
	};
	static const StabConfig portCfgs[] = {
		{ "plain",                  MotionNone,     false, false, false, 1.0f,  false },
		{ "effect, optical flow",   MotionFlow,     true,  false, false, 0.25f, false },
		{ "  renderer class",       MotionFlow,     true,  false, false, 0.25f, true  },
		{ "MVec + effect, flow",    MotionFlow,     true,  false, true,  0.25f, false },
		{ "  renderer class",       MotionFlow,     true,  false, true,  0.25f, true  },
	};
	const StabConfig* const cfgs = port ? portCfgs : measureCfgs;
	const size_t cfgCount = port ? std::size(portCfgs) : std::size(measureCfgs);

	Head("Temporal suite: post-DLSS stabilizer on real pictures");
	const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	int rc = 0;
	{
		CComPtr<IWICImagingFactory> factory;
		Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))), "WIC factory");
		std::vector<float> background, objectPicture;
		std::string error;
		// The object is a Windows wallpaper the desktop had and the HTPC has not; there it
		// is the background picture itself, whole and smaller.
		const wchar_t* objectPath = L"C:\\Windows\\Web\\Wallpaper\\Spotlight\\img14.jpg";
		if (GetFileAttributesW(objectPath) == INVALID_FILE_ATTRIBUTES) {
			objectPath = imagePath;
		}
		const bool picturesOk = factory
			&& LoadPicture(factory, imagePath, BW, BH, background, error)
			&& LoadPicture(factory, objectPath, 960, 540, objectPicture, error);
		Check(picturesOk, "pictures decoded");
		if (!picturesOk) {
			printf("  %s\n", error.c_str());
			rc = 1;
		}

		CStabilizerPasses passes;
		CDlssOpticalFlow flow;
		CDlssStabilizer stabilizer;
		std::vector<BYTE> shownBytes, redrawBytes;
		Tex2D_t texIn, texOut, motionTex;
		Target confTex, history[2], finalTex, prevInput;
		CComPtr<ID3D11RenderTargetView> motionTarget;
		CComPtr<ID3D11Texture2D> stage;

		if (!rc) {
			const bool passesOk = passes.Init(dev, error);
			Check(passesOk, "stabilizer shaders compiled");
			if (!passesOk) {
				printf("  %s\n", error.c_str());
				rc = 1;
			}
		}
		if (!rc) {
			CDlssOpticalFlow::Options options;   // grid 4, medium, both directions, cost
			const bool texturesOk = MakeSharedPair(dev, texIn, texOut, W, H)
				&& SUCCEEDED(motionTex.CheckCreate(dev, DXGI_FORMAT_R16G16_FLOAT, W, H, Tex2D_DefaultShaderRTargetUAVShared))
				&& SUCCEEDED(dev->CreateRenderTargetView(motionTex.pTexture, nullptr, &motionTarget))
				&& MakeTarget(dev, W, H, false, confTex, DXGI_FORMAT_R8_UNORM)
				&& MakeTarget(dev, W, H, false, history[0], DXGI_FORMAT_R16G16B16A16_FLOAT)
				&& MakeTarget(dev, W, H, false, history[1], DXGI_FORMAT_R16G16B16A16_FLOAT)
				&& MakeTarget(dev, W, H, false, finalTex, DXGI_FORMAT_R16G16B16A16_FLOAT)
				&& MakeTarget(dev, W, H, false, prevInput, DXGI_FORMAT_R16G16B16A16_FLOAT);
			Check(texturesOk, "textures");
			const bool flowOk = flow.Init(dev, ctx, w2, h2, options);
			Check(flowOk, "Optical Flow at 960x540, grid 4");
			if (!flowOk) {
				printf("  %S\n", flow.GetStatusLine().c_str());
			}
			if (texturesOk) {
				D3D11_TEXTURE2D_DESC sd = texOut.desc;
				sd.Usage = D3D11_USAGE_STAGING;
				sd.BindFlags = 0;
				sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				sd.MiscFlags = 0;
				dev->CreateTexture2D(&sd, nullptr, &stage);
			}
			if (!texturesOk || !flowOk || !stage) {
				rc = 1;
			}
		}

		if (!rc) {
			CDlssNR::Params params;
			params.iStyle = 0;
			params.iPreset = 0;
			params.fIntensity = 1.50f;
			params.fLocalTone = strong ? 1.0f : 0.30f;
			params.fLocalStructure = strong ? 1.0f : 0.50f;
			params.fSkinStructure = 0.90f;
			params.bUseAutoMask = true;
			params.bNoHistory = false;

			const char* const reportName = port ? "temporal_results_stabport.txt" : "temporal_results_stab.txt";
			g_report = fopen(reportName, "w");
			Out("\nPost-DLSS stabilizer on %S (1920x1080 windows) -- %d frames per run, first %d skipped, %s strengths.\n",
			    imagePath, frames, warm, strong ? "strong (tone 1.00, structure 1.00)" : "default (tone 0.30, structure 0.50)");
			if (port) {
				Out("Port check: each \"renderer class\" row runs CDlssStabilizer (embedded shaders, its own Optical Flow,\n");
				Out("strength 1.0) on the network output of the row above it, and steadies every 5th picture twice.\n");
			}
			Out("Grain 1.2/255 and 8x8 block offsets of up to 1.5/255 per frame. Stabilizer: current-frame weight 0.25 where trusted,\n");
			Out("history clamped to the 3x3 neighbourhood +-0.02. Optical Flow at 960x540, grid 4, medium, both directions.\n");
			Out("LF flick = frame-to-frame change of blurred luma (sigma ~8 px), compensated by the true motion; the moving object's\n");
			Out("sweep and the cut are left out. effect = mean |Y - Y(input)|, of plain = its share of plain DLSS. grain = mean\n");
			Out("|Y - blurred Y|. dev = mean |Y - Y(plain)| at 1/4 resolution: overall, weighted by local contrast, and in the\n");
			Out("band the moving object sweeps.\n");

			std::vector<float> rgba(4 * (size_t)W * H), window, yIn, yOut, bIn, bOut, bInPrev, bOutPrev;
			std::vector<HALF> half(rgba.size());
			std::vector<HALF> motionHalf(2 * (size_t)W * H);
			std::vector<float> motionFloat(2 * (size_t)W * H);
			std::vector<uint8_t> confBytes((size_t)W * H);
			std::vector<float> lowres((size_t)w4 * h4);

			for (const StabSequence& s : seqs) {
				std::vector<float> reference;   // plain DLSS at 1/4 resolution, per measured frame
				double inputFlicker = 0, plainEffect = 0;
				int inputCount = 0;

				Out("\n%s\n", s.name);
				Out("  %-22s %9s %7s %9s %8s %8s %9s %9s %9s\n",
				    "config", "LF flick", "ratio", "effect", "of plain", "grain", "dev all", "dev edge", "dev band");

				for (size_t ci = 0; ci < cfgCount; ci++) {
					const StabConfig& c = cfgs[ci];
					const bool isPlain = (ci == 0);
					dlss.ReleaseFeature();
					dlss.SetGuides(CDlssNR::Guides{});   // before the class's vectors go away
					stabilizer.Release();
					if (c.renderer) {
						if (FAILED(stabilizer.Create(dev, ctx, W, H, passes.InputLayout(), passes.VertexShader(),
						                             passes.SamplerPoint(), passes.SamplerLinear()))
						    || !stabilizer.HasFlow()) {
							Out("  %-22s could not start the renderer class: %S\n", c.name, stabilizer.GetStatusLine().c_str());
							g_failures++;
							continue;
						}
					}
					CDlssNR::Guides g;
					if (c.mvecToDlss) {
						g.pMVec = c.renderer ? stabilizer.GetMotionVectors() : motionTex.pTexture.p;
					}
					if (!dlss.SetGuides(g) || !dlss.CreateFeature(texIn.pTexture, texOut.pTexture, W, H, params)) {
						Out("  %-22s could not set up: %S\n", c.name, dlss.GetStatusLine().c_str());
						g_failures++;
						continue;
					}
					dlss.RequestReset();
					flow.Reset();

					double flick = 0, effect = 0, grain = 0, devAll = 0, devEdge = 0, devBand = 0;
					int count = 0, flickCount = 0, bandCount = 0, failed = 0, redraws = 0, redrawsDiffering = 0;
					int historyIndex = 0;
					bool historyValid = false;

					for (int t = 0; t < frames; t++) {
						// The frame: a window of the background, maybe an object over it, then noise.
						const bool afterCut = s.cutAt >= 0 && t >= s.cutAt;
						const float ox = (afterCut ? cutX : originX) - s.vx * t;
						const float oy = (afterCut ? cutY : originY) - s.vy * t;
						WindowRgba(background, BW, BH, ox, oy, W, H, rgba);
						Rect objNow = { 0, 0, 0, 0 }, objPrev = { 0, 0, 0, 0 };
						if (s.object) {
							const float px = 300 + s.ux * t, py = 300 + s.uy * t;
							const float qx = 300 + s.ux * std::max(t - 1, 0), qy = 300 + s.uy * std::max(t - 1, 0);
							objNow = { (int)std::floor(px), (int)std::floor(py), (int)std::ceil(px) + OW, (int)std::ceil(py) + OH };
							objPrev = { (int)std::floor(qx), (int)std::floor(qy), (int)std::ceil(qx) + OW, (int)std::ceil(qy) + OH };
							for (int y = std::max(objNow.y0, 0); y < std::min(objNow.y1, H); y++) {
								for (int x = std::max(objNow.x0, 0); x < std::min(objNow.x1, W); x++) {
									const float lx = x - px, ly = y - py;
									if (lx < 0 || ly < 0 || lx >= OW - 1 || ly >= OH - 1) {
										continue;
									}
									const int x0 = (int)lx, y0 = (int)ly;
									const float tx = lx - x0, ty = ly - y0;
									const float* a = &objectPicture[4 * ((size_t)(y0 + 110) * 960 + x0 + 240)];
									const float* b = a + 4;
									const float* cc = &objectPicture[4 * ((size_t)(y0 + 111) * 960 + x0 + 240)];
									const float* d = cc + 4;
									float* o = &rgba[4 * ((size_t)y * W + x)];
									for (int k = 0; k < 3; k++) {
										o[k] = (a[k] * (1 - tx) + b[k] * tx) * (1 - ty) + (cc[k] * (1 - tx) + d[k] * tx) * ty;
									}
								}
							}
						}
						{
							Rng r(0xC0FFEEu + (uint32_t)t * 7919u);
							const int bw = (W + 7) / 8;
							std::vector<float> blockDc((size_t)bw * ((H + 7) / 8));
							for (float& v : blockDc) {
								v = (r.Uniform() * 2 - 1) * (1.5f / 255);
							}
							for (int y = 0; y < H; y++) {
								for (int x = 0; x < W; x++) {
									const float n = blockDc[(size_t)(y / 8) * bw + x / 8] + r.Gauss() * (1.2f / 255);
									float* o = &rgba[4 * ((size_t)y * W + x)];
									for (int k = 0; k < 3; k++) {
										o[k] = std::round(std::clamp(o[k] + n, 0.0f, 1.0f) * 255) / 255;
									}
								}
							}
						}
						DirectX::PackedVector::XMConvertFloatToHalfStream(half.data(), sizeof(HALF), rgba.data(), sizeof(float), rgba.size());
						ctx->UpdateSubresource(texIn.pTexture, 0, nullptr, half.data(), 4 * sizeof(HALF) * W, 0);

						// Motion.
						bool haveMotion = false;
						if (c.renderer) {
							stabilizer.PrepareMotion(ctx, texIn.pShaderResource);
						} else if (c.motion == MotionFlow) {
							passes.FlowFrame(ctx, texIn.pShaderResource, flow.FrameTarget(), w2, h2, 2);
							haveMotion = flow.Execute();
							if (haveMotion) {
								passes.FlowMotion(ctx, flow, motionTarget, confTex.rtv, W, H);
							}
						} else if (c.motion == MotionTruth) {
							const bool cutNow = s.cutAt >= 0 && t == s.cutAt;
							const Rect band = Expand(Union(objPrev, objNow), 2, W, H);
							for (int y = 0; y < H; y++) {
								for (int x = 0; x < W; x++) {
									const size_t i = (size_t)y * W + x;
									const bool inObject = s.object && Inside(objNow, x, y);
									motionFloat[2 * i]     = inObject ? -s.ux : -s.vx;
									motionFloat[2 * i + 1] = inObject ? -s.uy : -s.vy;
									bool trusted = !cutNow && t > 0;
									if (s.object && Inside(band, x, y)) {
										// Uncovered background, and the object's own edges.
										const Rect inner = { objNow.x0 + 2, objNow.y0 + 2, objNow.x1 - 2, objNow.y1 - 2 };
										trusted = trusted && Inside(inner, x, y);
									}
									confBytes[i] = trusted ? 255 : 0;
								}
							}
							DirectX::PackedVector::XMConvertFloatToHalfStream(motionHalf.data(), sizeof(HALF), motionFloat.data(), sizeof(float), motionFloat.size());
							ctx->UpdateSubresource(motionTex.pTexture, 0, nullptr, motionHalf.data(), 2 * sizeof(HALF) * W, 0);
							ctx->UpdateSubresource(confTex.tex, 0, nullptr, confBytes.data(), W, 0);
							haveMotion = true;
						}
						if (c.mvecToDlss && !c.renderer && !haveMotion) {
							const FLOAT zero[4] = { 0, 0, 0, 0 };
							ctx->ClearRenderTargetView(motionTarget, zero);
						}

						if (!dlss.Evaluate(params)) {
							if (++failed <= 2) {
								Out("  %-22s frame %d: Evaluate failed -- %S\n", c.name, t, dlss.GetStatusLine().c_str());
							}
							continue;
						}

						ID3D11Texture2D* shown = texOut.pTexture;
						if (c.stabilize && c.renderer) {
							stabilizer.Stabilize(ctx, texIn.pShaderResource, texOut.pShaderResource, 1.0f, true);
							shown = stabilizer.GetResult()->pTexture;
							if (t % 5 == 3) {
								// A redraw of this picture: the same result, and the history left
								// where it was -- the pictures after it are measured too.
								const bool readOk = ReadBytes(ctx, stage, shown, shownBytes);
								stabilizer.Stabilize(ctx, texIn.pShaderResource, texOut.pShaderResource, 1.0f, false);
								redraws++;
								if (!readOk || !ReadBytes(ctx, stage, shown, redrawBytes) || shownBytes != redrawBytes) {
									redrawsDiffering++;
								}
							}
						} else if (c.stabilize) {
							CStabilizerPasses::Params sp;
							sp.minCurrent = c.minCurrent;
							sp.tolerance = 0.02f;
							sp.reprojection = (c.motion == MotionFlow) ? 0.03f : 0.0f;
							sp.outputMode = c.outputMode;
							sp.reset = !historyValid || !haveMotion;
							const Target& read = history[historyIndex];
							const Target& write = history[1 - historyIndex];
							passes.Stabilize(ctx, texIn.pShaderResource, texOut.pShaderResource, read.srv, motionTex.pShaderResource,
							                 prevInput.srv, confTex.srv, write.rtv, finalTex.rtv, W, H, sp);
							historyIndex = 1 - historyIndex;
							historyValid = true;
							ctx->CopyResource(prevInput.tex, texIn.pTexture);
							shown = finalTex.tex;
						}

						ctx->CopyResource(stage, shown);
						if (!ReadLumaHalf(ctx, stage, W, H, yOut)) {
							failed++;
							continue;
						}
						LumaHalfFromFloat(rgba, W, H, yIn);
						Blur(yOut, w2, h2, 4, bOut);
						if (isPlain) {
							Blur(yIn, w2, h2, 4, bIn);
						}

						if (t >= warm) {
							const bool skipFlicker = s.cutAt >= 0 && (t == s.cutAt || t == s.cutAt + 1);
							const Rect sweepHalf = s.object ? Expand(Scale(Union(objPrev, objNow), 2), 24, w2, h2) : Rect{ 0, 0, 0, 0 };
							if (!skipFlicker && !bOutPrev.empty()) {
								flick += s.object ? OutsideDiff(bOut, bOutPrev, w2, h2, sweepHalf, 24)
								                  : WarpDiff(bOut, bOutPrev, w2, h2, s.vx / 2, s.vy / 2, 24, 0, w2);
								if (isPlain && !bInPrev.empty()) {
									inputFlicker += s.object ? OutsideDiff(bIn, bInPrev, w2, h2, sweepHalf, 24)
									                         : WarpDiff(bIn, bInPrev, w2, h2, s.vx / 2, s.vy / 2, 24, 0, w2);
									inputCount++;
								}
								flickCount++;
							}
							double e = 0, gr = 0;
							for (size_t p = 0; p < yOut.size(); p++) {
								e += std::abs(yOut[p] - yIn[p]);
								gr += std::abs(yOut[p] - bOut[p]);
							}
							effect += e / yOut.size();
							grain += gr / yOut.size();

							for (int y = 0; y < h4; y++) {
								for (int x = 0; x < w4; x++) {
									const size_t a = (size_t)(2 * y) * w2 + 2 * x;
									lowres[(size_t)y * w4 + x] = (yOut[a] + yOut[a + 1] + yOut[a + w2] + yOut[a + w2 + 1]) / 4;
								}
							}
							if (isPlain) {
								reference.insert(reference.end(), lowres.begin(), lowres.end());
							} else if (reference.size() >= (size_t)(count + 1) * w4 * h4) {
								const float* ref = &reference[(size_t)count * w4 * h4];
								const Rect bandQ = s.object ? Expand(Scale(Union(objPrev, objNow), 4), 4, w4, h4) : Rect{ 0, 0, 0, 0 };
								double sa = 0, se = 0, sw = 0, sb = 0;
								long long nb = 0;
								for (int y = 1; y < h4 - 1; y++) {
									for (int x = 1; x < w4 - 1; x++) {
										const size_t i = (size_t)y * w4 + x;
										const double d = std::abs(lowres[i] - ref[i]);
										const double contrast = std::abs(ref[i + 1] - ref[i - 1]) + std::abs(ref[i + w4] - ref[i - w4]);
										sa += d;
										se += contrast * d;
										sw += contrast;
										if (s.object && Inside(bandQ, x, y)) {
											sb += d;
											nb++;
										}
									}
								}
								devAll += sa / ((double)(w4 - 2) * (h4 - 2));
								devEdge += sw > 0 ? se / sw : 0;
								if (nb) {
									devBand += sb / nb;
									bandCount++;
								}
							}
							count++;
						}

						bOutPrev.swap(bOut);
						if (isPlain) {
							bInPrev.swap(bIn);
						}
					}
					bOutPrev.clear();
					bInPrev.clear();

					if (!count) {
						Out("  %-22s no usable frames (%d failed)\n", c.name, failed);
						g_failures++;
						continue;
					}
					const double inL = inputCount ? inputFlicker / inputCount : 0;
					const double l = flickCount ? flick / flickCount : 0;
					const double e = effect / count;
					if (isPlain) {
						plainEffect = e;
					}
					Out("  %-22s %9.5f %6.2fx %9.5f %7.0f%% %8.5f", c.name, l, inL > 0 ? l / inL : 0.0, e,
					    plainEffect > 0 ? 100.0 * e / plainEffect : 0.0, grain / count);
					if (isPlain) {
						Out(" %9s %9s %9s", "-", "-", "-");
					} else {
						Out(" %9.5f %9.5f", devAll / count, devEdge / count);
						if (bandCount) {
							Out(" %9.5f", devBand / bandCount);
						} else {
							Out(" %9s", "-");
						}
					}
					if (failed) {
						Out("   (%d failed)", failed);
					}
					if (c.renderer) {
						Out("   redraws %d/%d identical", redraws - redrawsDiffering, redraws);
						if (redrawsDiffering || !redraws) {
							g_failures++;
						}
					}
					Out("\n");
					if (isPlain) {
						Out("  %-22s %9.5f\n", "(input)", inL);
					}
				}
			}

			dlss.SetGuides(CDlssNR::Guides{});
			stabilizer.Release();
			fclose(g_report);
			g_report = nullptr;
			printf("\n  written to %s\n", reportName);
		}
	}
	if (SUCCEEDED(coInit)) {
		CoUninitialize();
	}
	return (rc || g_failures) ? 1 : 0;
}

// --tstabbench: what the renderer's CDlssStabilizer costs per picture, run the way
// DlssNRPass runs it, with Optical Flow at each divider the page offers.
static int RunStabBench(ID3D11Device* dev, ID3D11DeviceContext* ctx)
{
	Head("Temporal suite: stabilizer cost");

	CStabilizerPasses passes;   // for the renderer's vertex shader, input layout and samplers
	std::string error;
	const bool passesOk = passes.Init(dev, error);
	Check(passesOk, "pipeline objects");
	if (!passesOk) {
		printf("  %s\n", error.c_str());
		return 1;
	}

	CComPtr<ID3D11Query> disjoint, tsStart, tsMid, tsEnd;
	D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
	dev->CreateQuery(&qd, &disjoint);
	qd.Query = D3D11_QUERY_TIMESTAMP;
	dev->CreateQuery(&qd, &tsStart);
	dev->CreateQuery(&qd, &tsMid);
	dev->CreateQuery(&qd, &tsEnd);
	Check(disjoint && tsStart && tsMid && tsEnd, "timestamp queries");
	if (!disjoint || !tsStart || !tsMid || !tsEnd) {
		return 1;
	}

	const int kWarm = 8, kFrames = 64, kDistinct = 4;
	g_report = fopen("temporal_results_stabbench.txt", "w");
	Out("\nCDlssStabilizer cost -- GPU time from timestamp queries around PrepareMotion (motion) and Stabilize (the\n");
	Out("stabilize pass and the input copy), and the CPU time of both calls together. NVIDIA Optical Flow's own work\n");
	Out("lands in the motion column. Each picture waits for the previous one to finish, so no stall is counted.\n");
	Out("Moving pictures, %d warm-up frames, then %d measured.\n\n", kWarm, kFrames);
	Out("  %-10s %-28s %10s %10s %10s %10s %10s %10s\n", "size", "motion", "motion GPU", "stab GPU", "GPU total", "GPU p95", "CPU mean", "CPU p95");

	struct Size { UINT w, h; const char* name; };
	static const Size sizes[] = { { 1920, 1080, "1920x1080" }, { 3840, 2160, "3840x2160" } };

	LARGE_INTEGER qpf = {};
	QueryPerformanceFrequency(&qpf);

	auto summarise = [](std::vector<double>& v, double& mean, double& p95) {
		double sum = 0;
		for (double x : v) {
			sum += x;
		}
		mean = v.empty() ? 0 : sum / v.size();
		std::sort(v.begin(), v.end());
		p95 = v.empty() ? 0 : v[std::min(v.size() - 1, v.size() * 95 / 100)];
	};

	for (const Size& sz : sizes) {
		printf("  building %s pictures...\n", sz.name);
		Tex2D_t pictures[kDistinct];
		bool ok = true;
		{
			const Image bg = MakeBase((int)sz.w, (int)sz.h, 20260913u);
			const Image obj = MakeBase((int)sz.w, (int)sz.h, 777u);
			std::vector<float> rgba(4 * (size_t)sz.w * sz.h);
			std::vector<HALF> half(rgba.size());
			for (int i = 0; i < kDistinct && ok; i++) {
				RenderOracleFrame(bg, obj, 200.0f + 3.0f * i, 300.0f + 1.0f * i, (int)sz.w / 4, (int)sz.h / 3, i, rgba);
				DirectX::PackedVector::XMConvertFloatToHalfStream(half.data(), sizeof(HALF), rgba.data(), sizeof(float), rgba.size());
				ok = SUCCEEDED(pictures[i].CheckCreate(dev, DXGI_FORMAT_R16G16B16A16_FLOAT, sz.w, sz.h, Tex2D_DefaultShaderRTarget))
					&& pictures[i].pShaderResource;
				if (ok) {
					ctx->UpdateSubresource(pictures[i].pTexture, 0, nullptr, half.data(), 4 * sizeof(HALF) * sz.w, 0);
				}
			}
		}
		Check(ok, sz.name);
		if (!ok) {
			continue;
		}

		for (const UINT divider : { 4u, 3u, 2u, 1u }) {
			CDlssStabilizer stabilizer;
			CDlssStabilizer::FlowSettings flow;
			flow.flowFactor = divider;
			if (FAILED(stabilizer.Create(dev, ctx, sz.w, sz.h, passes.InputLayout(), passes.VertexShader(),
			                             passes.SamplerPoint(), passes.SamplerLinear(), false, flow))) {
				Out("  %-10s could not create the stabilizer\n", sz.name);
				g_failures++;
				continue;
			}

			std::vector<double> motionGpu, stabGpu, totalGpu, cpu;
			for (int f = 0; f < kWarm + kFrames; f++) {
				ID3D11ShaderResourceView* picture = pictures[f % kDistinct].pShaderResource;
				ID3D11ShaderResourceView* network = pictures[(f + 1) % kDistinct].pShaderResource;
				ctx->Begin(disjoint);
				ctx->End(tsStart);
				LARGE_INTEGER c0 = {}, c1 = {};
				QueryPerformanceCounter(&c0);
				stabilizer.PrepareMotion(ctx, picture);
				ctx->End(tsMid);
				stabilizer.Stabilize(ctx, picture, network, 1.0f, true);
				QueryPerformanceCounter(&c1);
				ctx->End(tsEnd);
				ctx->End(disjoint);

				D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
				UINT64 t0 = 0, t1 = 0, t2 = 0;
				while (ctx->GetData(disjoint, &dj, sizeof(dj), 0) == S_FALSE) {
					Sleep(0);
				}
				while (ctx->GetData(tsStart, &t0, sizeof(t0), 0) == S_FALSE) {
					Sleep(0);
				}
				while (ctx->GetData(tsMid, &t1, sizeof(t1), 0) == S_FALSE) {
					Sleep(0);
				}
				while (ctx->GetData(tsEnd, &t2, sizeof(t2), 0) == S_FALSE) {
					Sleep(0);
				}
				if (f >= kWarm && !dj.Disjoint && dj.Frequency) {
					motionGpu.push_back(double(t1 - t0) * 1000.0 / dj.Frequency);
					stabGpu.push_back(double(t2 - t1) * 1000.0 / dj.Frequency);
					totalGpu.push_back(double(t2 - t0) * 1000.0 / dj.Frequency);
					cpu.push_back(double(c1.QuadPart - c0.QuadPart) * 1000.0 / qpf.QuadPart);
				}
			}

			double mm = 0, mp = 0, sm = 0, sp = 0, tm = 0, tp = 0, cm = 0, cp = 0;
			summarise(motionGpu, mm, mp);
			summarise(stabGpu, sm, sp);
			summarise(totalGpu, tm, tp);
			summarise(cpu, cm, cp);
			Out("  %-10s %-28S %7.3f ms %7.3f ms %7.3f ms %7.3f ms %7.3f ms %7.3f ms   (%zu frames)\n",
			    sz.name, stabilizer.GetStatusLine().c_str(), mm, sm, tm, tp, cm, cp, cpu.size());
		}
	}

	if (g_report) {
		fclose(g_report);
		g_report = nullptr;
		printf("\n  written to temporal_results_stabbench.txt\n");
	}
	return g_failures ? 1 : 0;
}

} // namespace temporal
