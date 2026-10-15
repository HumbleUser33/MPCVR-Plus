// Render targets and the renderer's vertex layout, for the suites that draw their own
// passes next to the filter's.
//
// Included by harness.cpp before the temporal suites. These lived in the shader
// detector's suite until the detector left the filter (and the bench) on 2026-10-15.

#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")

namespace temporal {

struct Vertex11 { float x, y, z, u, v; };   // the renderer's VERTEX layout

struct Target {
	CComPtr<ID3D11Texture2D> tex;
	CComPtr<ID3D11ShaderResourceView> srv;
	CComPtr<ID3D11RenderTargetView> rtv;
	UINT w = 0, h = 0, levels = 1;
};

static bool MakeTarget(ID3D11Device* dev, UINT w, UINT h, bool mips, Target& t,
                       DXGI_FORMAT format = DXGI_FORMAT_R16_FLOAT)
{
	D3D11_TEXTURE2D_DESC d = {};
	d.Width = w;
	d.Height = h;
	d.MipLevels = mips ? 0 : 1;
	d.ArraySize = 1;
	d.Format = format;
	d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_DEFAULT;
	d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	d.MiscFlags = mips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;

	t = Target{};
	if (FAILED(dev->CreateTexture2D(&d, nullptr, &t.tex))) {
		return false;
	}
	t.tex->GetDesc(&d);
	t.w = w;
	t.h = h;
	t.levels = d.MipLevels;

	D3D11_RENDER_TARGET_VIEW_DESC rd = {};
	rd.Format = d.Format;
	rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	return SUCCEEDED(dev->CreateShaderResourceView(t.tex, nullptr, &t.srv))
		&& SUCCEEDED(dev->CreateRenderTargetView(t.tex, &rd, &t.rtv));
}

} // namespace temporal
