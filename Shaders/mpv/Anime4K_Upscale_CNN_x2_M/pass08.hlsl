// 文档 https://github.com/hooke007/mpv_PlayKit/wiki/4_GLSL
//
// Pass 8 of Anime4K_Upscale_CNN_x2_M.glsl ([Anime4K_Upscale_CNN_x2_M][v3.2] Depth-to-Space), translated to HLSL by Shaders/mpv/mpv_shaders.py
// (glslang, then SPIRV-Cross): do not edit.

cbuffer MpvGlobals : register(b0)
{
    float4 _24_pl_out_size : packoffset(c0);
    float4 _24_pl_input_target : packoffset(c1);
    float4 _24_pl_misc : packoffset(c2);
    float4 _24_pl_tex_size[8] : packoffset(c3);
    float4 _24_pl_tex_shift[8] : packoffset(c11);
    float4 _24_pl_params[8] : packoffset(c19);
};

Texture2D<float4> conv2d_last_tf_raw : register(t2);
SamplerState _conv2d_last_tf_raw_sampler : register(s2);
Texture2D<float4> LUMA_raw : register(t1);
SamplerState _LUMA_raw_sampler : register(s1);

static float2 pl_pos;
static float4 pl_out;

struct SPIRV_Cross_Input
{
    float2 pl_pos : TEXCOORD0;
};

struct SPIRV_Cross_Output
{
    float4 pl_out : SV_Target0;
};

float4 hook()
{
    float2 f0 = frac((pl_pos + _24_pl_tex_shift[1].xy) * _24_pl_tex_size[1].xy);
    int2 i0 = int2(f0 * 2.0f.xx);
    float c0 = (conv2d_last_tf_raw.SampleLevel(_conv2d_last_tf_raw_sampler, ((0.5f.xx - f0) * _24_pl_tex_size[1].zw) + (pl_pos + _24_pl_tex_shift[1].xy), 0.0f) * 1.0f)[(i0.y * 2) + i0.x];
    float c1 = c0;
    float c2 = c1;
    float c3 = c2;
    return float4(c0, c1, c2, c3) + (LUMA_raw.SampleLevel(_LUMA_raw_sampler, pl_pos + _24_pl_tex_shift[0].xy, 0.0f) * 1.0f);
}

void frag_main()
{
    pl_out = hook();
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    pl_pos = stage_input.pl_pos;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.pl_out = pl_out;
    return stage_output;
}
