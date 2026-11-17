// mpv / libplacebo user shaders on the harness's Direct3D 11 device.
//
// Included by harness.cpp before upscale_suite.inl. mpv_shaders.py translates a
// user shader into HLSL passes and a manifest; this runs them the way libplacebo
// does. Each pass renders, or dispatches, into a texture of the size its WIDTH
// and HEIGHT expressions give, reading the planes, the textures earlier passes
// saved and the shader's own lookup textures. A pass that saves nothing replaces
// the plane it hooks, which is how a doubler hands back a plane twice the size.
//
// Supported: pixel and compute passes, WHEN/WIDTH/HEIGHT expressions, //!TEXTURE
// lookup tables in rgba16f, parameters at their defaults, OFFSET ALIGN. A pixel
// offset (NNEDI3's half-pixel shifts) is not compensated and is reported instead.

#include <functional>
#include <sstream>

namespace temporal {

static constexpr int kMpvMaxBinds = 8;
static constexpr int kMpvMaxParams = 8;

// A plane or a saved texture: what a pass reads and writes.
struct MpvTexture {
	CComPtr<ID3D11Texture2D> tex;
	CComPtr<ID3D11ShaderResourceView> srv;
	CComPtr<ID3D11RenderTargetView> rtv;
	CComPtr<ID3D11UnorderedAccessView> uav;
	UINT w = 0, h = 0;
};

static bool MakeMpvTexture(ID3D11Device* dev, UINT w, UINT h, MpvTexture& t, const void* data = nullptr, UINT stride = 0)
{
	D3D11_TEXTURE2D_DESC d = {};
	d.Width = w;
	d.Height = h;
	d.MipLevels = 1;
	d.ArraySize = 1;
	d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;   // libplacebo's default for pass outputs
	d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_DEFAULT;
	d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
	const D3D11_SUBRESOURCE_DATA init = { data, stride, 0 };
	t = MpvTexture{};
	if (FAILED(dev->CreateTexture2D(&d, data ? &init : nullptr, &t.tex))) {
		return false;
	}
	t.w = w;
	t.h = h;
	return SUCCEEDED(dev->CreateShaderResourceView(t.tex, nullptr, &t.srv))
		&& SUCCEEDED(dev->CreateRenderTargetView(t.tex, nullptr, &t.rtv))
		&& SUCCEEDED(dev->CreateUnorderedAccessView(t.tex, nullptr, &t.uav));
}

// libplacebo's expressions: reverse polish, floats only.
static bool EvalRpn(const std::string& expr, const std::function<bool(const std::string&, double&)>& lookup, double& result)
{
	std::vector<double> stack;
	std::istringstream in(expr);
	std::string tok;
	while (in >> tok) {
		if (tok == "!") {
			if (stack.empty()) {
				return false;
			}
			stack.back() = (stack.back() == 0.0) ? 1.0 : 0.0;
			continue;
		}
		if (tok.size() == 1 && strchr("+-*/%<>=", tok[0])) {
			if (stack.size() < 2) {
				return false;
			}
			const double b = stack.back();
			stack.pop_back();
			double& a = stack.back();
			switch (tok[0]) {
			case '+': a = a + b; break;
			case '-': a = a - b; break;
			case '*': a = a * b; break;
			case '/': a = a / b; break;
			case '%': a = std::fmod(a, b); break;
			case '>': a = (a > b) ? 1.0 : 0.0; break;
			case '<': a = (a < b) ? 1.0 : 0.0; break;
			case '=': a = (std::abs(a - b) <= 1e-6 * std::max(std::abs(a), std::abs(b))) ? 1.0 : 0.0; break;
			}
			continue;
		}
		char* end = nullptr;
		const double v = strtod(tok.c_str(), &end);
		if (end && *end == '\0') {
			stack.push_back(v);
			continue;
		}
		double value = 0;
		if (!lookup(tok, value)) {
			return false;
		}
		stack.push_back(value);
	}
	if (stack.size() != 1) {
		return false;
	}
	result = stack[0];
	return true;
}

class CMpvShader
{
public:
	bool Load(ID3D11Device* dev, const std::wstring& dir, std::string& error);

	const std::string& Name() const { return m_name; }
	bool Hooks(const char* plane) const;
	int PassCount() const { return (int)m_passes.size(); }
	// A pass may write its result part of a pixel off -- NNEDI3 does, half a pixel
	// on each axis -- which mpv's main scaler takes back when it samples the result.
	// Here the shifts are added up, in output pixels, for the caller to undo.
	bool HasPixelOffset() const { return PixelOffsetX() != 0 || PixelOffsetY() != 0; }

	double PixelOffsetX() const { return PixelOffset(0); }
	double PixelOffsetY() const { return PixelOffset(1); }

	double PixelOffset(int axis) const
	{
		double total = 0;
		for (const Pass& p : m_passes) {
			if (p.offset == "-" || p.offset == "ALIGN") {
				continue;
			}
			double x = 0, y = 0;
			if (sscanf_s(p.offset.c_str(), "%lf %lf", &x, &y) == 2) {
				total += axis ? y : x;
			}
		}
		return total;
	}

	// Runs every pass that hooks `plane`. planes holds LUMA and, for a chroma shader,
	// CHROMA; a pass that saves nothing replaces the hooked plane. outW x outH is
	// the OUTPUT size the expressions see. Returns the passes run, -1 on failure.
	int Run(ID3D11Device* dev, ID3D11DeviceContext* ctx, const char* plane,
	        std::map<std::string, MpvTexture>& planes, UINT outW, UINT outH, std::string& error);

private:
	struct Pass {
		std::string desc;
		std::vector<std::string> hooks, binds;
		std::string save, width, height, when, offset;
		UINT bw = 0, bh = 0;   // compute blocks; 0 for a pixel pass
		CComPtr<ID3D11PixelShader> ps;
		CComPtr<ID3D11ComputeShader> cs;
		MpvTexture out;        // reused while its size holds
	};
	struct Lut {
		MpvTexture tex;
		bool linear = true;
	};

	std::string m_name;
	std::vector<Pass> m_passes;
	std::map<std::string, Lut> m_luts;
	std::vector<std::pair<std::string, float>> m_params;
	CComPtr<ID3D11VertexShader> m_vs;
	CComPtr<ID3D11Buffer> m_cb;
	CComPtr<ID3D11SamplerState> m_linear, m_point;
};

bool CMpvShader::Hooks(const char* plane) const
{
	for (const Pass& p : m_passes) {
		if (std::find(p.hooks.begin(), p.hooks.end(), plane) != p.hooks.end()) {
			return true;
		}
	}
	return false;
}

bool CMpvShader::Load(ID3D11Device* dev, const std::wstring& dir, std::string& error)
{
	FILE* f = nullptr;
	if (_wfopen_s(&f, (dir + L"\\manifest.txt").c_str(), L"rt") != 0 || !f) {
		error = "no manifest";
		return false;
	}
	std::vector<std::string> lines;
	char buf[4096];
	while (fgets(buf, sizeof(buf), f)) {
		std::string line(buf);
		while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
			line.pop_back();
		}
		lines.push_back(line);
	}
	fclose(f);

	auto words = [](const std::string& s) {
		std::vector<std::string> v;
		std::istringstream in(s);
		std::string w;
		while (in >> w) {
			v.push_back(w);
		}
		return v;
	};
	auto readFile = [](const std::wstring& path, std::vector<BYTE>& data) {
		FILE* file = nullptr;
		if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) {
			return false;
		}
		fseek(file, 0, SEEK_END);
		const long size = ftell(file);
		fseek(file, 0, SEEK_SET);
		data.resize((size_t)std::max(size, 0L));
		const bool ok = size >= 0 && fread(data.data(), 1, data.size(), file) == data.size();
		fclose(file);
		return ok;
	};

	Pass* current = nullptr;
	for (const std::string& line : lines) {
		const size_t space = line.find(' ');
		const std::string key = line.substr(0, space);
		const std::string rest = (space == std::string::npos) ? std::string() : line.substr(space + 1);
		if (key == "shader") {
			m_name = rest.substr(0, rest.find_last_of('.'));
		} else if (key == "param") {
			const auto w = words(rest);
			if (w.size() == 2) {
				m_params.push_back({ w[0], (float)atof(w[1].c_str()) });
			}
		} else if (key == "texture") {
			// name w h format filter border file
			const auto w = words(rest);
			if (w.size() != 7 || w[3] != "rgba16f") {
				error = "unsupported texture: " + rest;
				return false;
			}
			std::vector<BYTE> data;
			const UINT tw = (UINT)atoi(w[1].c_str()), th = (UINT)atoi(w[2].c_str());
			const size_t texels = (size_t)tw * th;
			if (!readFile(dir + L"\\" + std::wstring(w[6].begin(), w[6].end()), data)
					|| (data.size() != 8 * texels && data.size() != 16 * texels)) {
				error = "texture data: " + w[0];
				return false;
			}
			if (data.size() == 16 * texels) {
				// libplacebo's rgba16f is a half-float texture whose data comes as 32-bit
				// floats (rgba16hf would be half floats already): RAVU's tables are.
				std::vector<BYTE> half(8 * texels);
				DirectX::PackedVector::XMConvertFloatToHalfStream((HALF*)half.data(), sizeof(HALF),
					(const float*)data.data(), sizeof(float), 4 * texels);
				data.swap(half);
			}
			Lut lut;
			lut.linear = (w[4] == "LINEAR");
			if (!MakeMpvTexture(dev, tw, th, lut.tex, data.data(), 8 * tw)) {
				error = "texture creation: " + w[0];
				return false;
			}
			m_luts[w[0]] = lut;
		} else if (key == "pass") {
			m_passes.emplace_back();
			current = &m_passes.back();
		} else if (current) {
			if (key == "desc") {
				current->desc = rest;
			} else if (key == "hooks") {
				current->hooks = words(rest);
			} else if (key == "binds") {
				current->binds = words(rest);
			} else if (key == "save") {
				current->save = rest;
			} else if (key == "width") {
				current->width = rest;
			} else if (key == "height") {
				current->height = rest;
			} else if (key == "when") {
				current->when = rest;
			} else if (key == "offset") {
				current->offset = rest;
			} else if (key == "compute") {
				const auto w = words(rest);
				if (w.size() >= 2) {
					current->bw = (UINT)atoi(w[0].c_str());
					current->bh = (UINT)atoi(w[1].c_str());
				}
			} else if (key == "hlsl") {
				// Compiled once, then kept next to the source.
				const std::wstring source = dir + L"\\" + std::wstring(rest.begin(), rest.end());
				const std::wstring object = source.substr(0, source.size() - 5) + L".cso";
				std::vector<BYTE> code;
				WIN32_FILE_ATTRIBUTE_DATA src = {}, obj = {};
				const bool haveSource = GetFileAttributesExW(source.c_str(), GetFileExInfoStandard, &src);
				const bool fresh = GetFileAttributesExW(object.c_str(), GetFileExInfoStandard, &obj)
					&& CompareFileTime(&obj.ftLastWriteTime, &src.ftLastWriteTime) >= 0;
				if (!haveSource) {
					error = "missing " + rest;
					return false;
				}
				if (!fresh || !readFile(object, code)) {
					CComPtr<ID3DBlob> blob, errors;
					const HRESULT hr = D3DCompileFromFile(source.c_str(), nullptr, nullptr, "main",
						current->bw ? "cs_5_0" : "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &blob, &errors);
					if (FAILED(hr)) {
						error = rest + ": " + (errors ? std::string((const char*)errors->GetBufferPointer(), errors->GetBufferSize()) : "compile failed");
						return false;
					}
					code.assign((BYTE*)blob->GetBufferPointer(), (BYTE*)blob->GetBufferPointer() + blob->GetBufferSize());
					FILE* out = nullptr;
					if (_wfopen_s(&out, object.c_str(), L"wb") == 0 && out) {
						fwrite(code.data(), 1, code.size(), out);
						fclose(out);
					}
				}
				const HRESULT hr = current->bw
					? dev->CreateComputeShader(code.data(), code.size(), nullptr, &current->cs)
					: dev->CreatePixelShader(code.data(), code.size(), nullptr, &current->ps);
				if (FAILED(hr)) {
					error = rest + ": shader creation";
					return false;
				}
			}
		}
	}
	if (m_passes.empty()) {
		error = "no passes";
		return false;
	}

	// A triangle over the target, with the pixel centres' normalized position in TEXCOORD0.
	static const char vsCode[] =
		"struct VSOut { float2 pos : TEXCOORD0; float4 position : SV_Position; };\n"
		"VSOut main(uint id : SV_VertexID) {\n"
		"    VSOut o;\n"
		"    float2 uv = float2((id << 1) & 2, id & 2);\n"
		"    o.pos = uv;\n"
		"    o.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
		"    return o;\n"
		"}\n";
	CComPtr<ID3DBlob> vsBlob, vsErrors;
	if (FAILED(D3DCompile(vsCode, sizeof(vsCode) - 1, "mpv_vs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &vsErrors))
			|| FAILED(dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vs))) {
		error = "vertex shader";
		return false;
	}
	D3D11_BUFFER_DESC cbd = { 4 * 4 * (3 + kMpvMaxBinds + kMpvMaxBinds + kMpvMaxParams), D3D11_USAGE_DYNAMIC,
		D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
	D3D11_SAMPLER_DESC sd = {};
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	dev->CreateSamplerState(&sd, &m_linear);
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	dev->CreateSamplerState(&sd, &m_point);
	if (FAILED(dev->CreateBuffer(&cbd, nullptr, &m_cb)) || !m_linear || !m_point) {
		error = "pipeline objects";
		return false;
	}
	return true;
}

int CMpvShader::Run(ID3D11Device* dev, ID3D11DeviceContext* ctx, const char* plane,
                    std::map<std::string, MpvTexture>& planes, UINT outW, UINT outH, std::string& error)
{
	std::map<std::string, MpvTexture> saved;
	const UINT inputW = planes["LUMA"].w, inputH = planes["LUMA"].h;
	int ran = 0;

	for (Pass& p : m_passes) {
		if (std::find(p.hooks.begin(), p.hooks.end(), plane) == p.hooks.end()) {
			continue;
		}
		const MpvTexture hooked = planes[plane];

		auto lookup = [&](const std::string& tok, double& v) -> bool {
			const size_t dot = tok.find('.');
			if (dot == std::string::npos) {
				for (const auto& [name, value] : m_params) {
					if (name == tok) {
						v = value;
						return true;
					}
				}
				return false;
			}
			const std::string name = tok.substr(0, dot), field = tok.substr(dot + 1);
			const bool wide = (field == "w" || field == "width");
			if (!wide && field != "h" && field != "height") {
				return false;
			}
			UINT tw = 0, th = 0;
			if (name == "OUTPUT") {
				tw = outW; th = outH;
			} else if (name == "HOOKED") {
				tw = hooked.w; th = hooked.h;
			} else if (planes.count(name)) {
				tw = planes[name].w; th = planes[name].h;
			} else if (saved.count(name)) {
				tw = saved[name].w; th = saved[name].h;
			} else if (name == "NATIVE" || name == "NATIVE_CROPPED" || name == "MAIN" || name == "MAINPRESUB") {
				tw = inputW; th = inputH;
			} else {
				return false;
			}
			v = wide ? tw : th;
			return true;
		};

		double when = 1.0;
		if (p.when != "-" && !EvalRpn(p.when, lookup, when)) {
			error = p.desc + ": WHEN " + p.when;
			return -1;
		}
		if (when == 0.0) {
			continue;
		}
		// OFFSET is the pass saying where its result lands; the caller undoes it.

		double dw = hooked.w, dh = hooked.h;
		if ((p.width != "-" && !EvalRpn(p.width, lookup, dw)) || (p.height != "-" && !EvalRpn(p.height, lookup, dh))) {
			error = p.desc + ": size";
			return -1;
		}
		const UINT w = (UINT)std::lround(dw), h = (UINT)std::lround(dh);
		if ((p.out.w != w || p.out.h != h) && !MakeMpvTexture(dev, w, h, p.out)) {
			error = p.desc + ": output texture";
			return -1;
		}

		// Constants: see the MpvGlobals block of mpv_shaders.py.
		float constants[4 * (3 + kMpvMaxBinds + kMpvMaxBinds + kMpvMaxParams)] = {};
		constants[0] = (float)w; constants[1] = (float)h; constants[2] = 1.0f / w; constants[3] = 1.0f / h;
		constants[4] = (float)inputW; constants[5] = (float)inputH; constants[6] = (float)outW; constants[7] = (float)outH;
		constants[10] = 0.5f;   // random
		ID3D11ShaderResourceView* srvs[1 + kMpvMaxBinds] = {};
		ID3D11SamplerState* samplers[1 + kMpvMaxBinds] = {};
		for (size_t i = 0; i < p.binds.size() && i < kMpvMaxBinds; i++) {
			const std::string& name = p.binds[i];
			const MpvTexture* t = nullptr;
			bool linear = true;
			if (name == "HOOKED") {
				t = &planes[plane];
			} else if (planes.count(name)) {
				t = &planes[name];
			} else if (saved.count(name)) {
				t = &saved[name];
			} else if (m_luts.count(name)) {
				t = &m_luts[name].tex;
				linear = m_luts[name].linear;
			}
			if (!t || !t->srv) {
				error = p.desc + ": nothing bound to " + name;
				return -1;
			}
			srvs[1 + i] = t->srv;
			samplers[1 + i] = linear ? m_linear : m_point;
			constants[4 * (3 + i) + 0] = (float)t->w;
			constants[4 * (3 + i) + 1] = (float)t->h;
			constants[4 * (3 + i) + 2] = 1.0f / t->w;
			constants[4 * (3 + i) + 3] = 1.0f / t->h;
		}
		for (size_t i = 0; i < m_params.size() && i < kMpvMaxParams; i++) {
			constants[4 * (3 + 2 * kMpvMaxBinds + i)] = m_params[i].second;
		}
		D3D11_MAPPED_SUBRESOURCE mr = {};
		if (FAILED(ctx->Map(m_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr))) {
			error = "constants";
			return -1;
		}
		memcpy(mr.pData, constants, sizeof(constants));
		ctx->Unmap(m_cb, 0);

		ID3D11Buffer* cb = m_cb;
		const UINT slots = (UINT)(1 + p.binds.size());
		if (p.cs) {
			ID3D11UnorderedAccessView* uav = p.out.uav;
			ctx->CSSetShader(p.cs, nullptr, 0);
			ctx->CSSetConstantBuffers(0, 1, &cb);
			ctx->CSSetShaderResources(0, slots, srvs);
			ctx->CSSetSamplers(0, slots, samplers);
			ctx->CSSetUnorderedAccessViews(kMpvMaxBinds + 1, 1, &uav, nullptr);
			ctx->Dispatch((w + p.bw - 1) / p.bw, (h + p.bh - 1) / p.bh, 1);
			ID3D11UnorderedAccessView* noUav = nullptr;
			ID3D11ShaderResourceView* noSrvs[1 + kMpvMaxBinds] = {};
			ctx->CSSetUnorderedAccessViews(kMpvMaxBinds + 1, 1, &noUav, nullptr);
			ctx->CSSetShaderResources(0, slots, noSrvs);
		} else {
			ID3D11RenderTargetView* rtv = p.out.rtv;
			const D3D11_VIEWPORT vp = { 0, 0, (FLOAT)w, (FLOAT)h, 0, 1 };
			ctx->OMSetRenderTargets(1, &rtv, nullptr);
			ctx->RSSetViewports(1, &vp);
			ctx->OMSetBlendState(nullptr, nullptr, D3D11_DEFAULT_SAMPLE_MASK);
			ctx->IASetInputLayout(nullptr);
			ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			ctx->VSSetShader(m_vs, nullptr, 0);
			ctx->PSSetShader(p.ps, nullptr, 0);
			ctx->PSSetConstantBuffers(0, 1, &cb);
			ctx->PSSetShaderResources(0, slots, srvs);
			ctx->PSSetSamplers(0, slots, samplers);
			ctx->Draw(3, 0);
			ID3D11ShaderResourceView* noSrvs[1 + kMpvMaxBinds] = {};
			ctx->PSSetShaderResources(0, slots, noSrvs);
			ID3D11RenderTargetView* noRtv = nullptr;
			ctx->OMSetRenderTargets(1, &noRtv, nullptr);
		}

		// Saved under the hooked plane's own name, as Anime4K's last pass does
		// (//!SAVE MAIN), the result replaces the plane, as libplacebo has it.
		if (p.save != "-" && p.save != plane) {
			saved[p.save] = p.out;
		} else {
			planes[plane] = p.out;
		}
		ran++;
	}
	return ran;
}

} // namespace temporal
