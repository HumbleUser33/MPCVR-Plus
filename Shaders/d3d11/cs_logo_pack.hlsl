// The decoded planes copied out for hiding a channel's logo, whatever their format, as
// 16-bit planes the rest of the work reads (Source/DX11VideoProcessorLogo.cpp).
//
// MODE 0: the luma the analysis looks for the logo in, at most 1920 across: the plane
//         itself, or its 2 x 2 means (scale 2).
// MODE 1: one plane's box, which the analysis reads back to tell the logo from the
//         picture under it: offset and size in the plane's own pixels.
// MODE 2: a whole plane, into the plane the logo is taken out of (only its box changes).
// CHANNELS 1: one plane; 2: the two chroma components of an interleaved plane.

#ifndef CHANNELS
#define CHANNELS 1
#endif
#ifndef MODE
#define MODE 0
#endif

#if CHANNELS == 2
#define VEC float2
#else
#define VEC float
#endif

Texture2D<VEC>   src : register(t0);
RWTexture2D<VEC> dst : register(u0);

cbuffer PackConstants : register(b0)
{
	uint2 offset;   // MODE 1: the box's corner in the plane
	uint2 size;     // what is written
	uint  scale;    // MODE 0: 1 or 2
	uint3 pad;
};

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= size.x || id.y >= size.y) {
		return;
	}
#if MODE == 0
	if (scale == 2) {
		const uint2 p = id.xy * 2;
		dst[id.xy] = (src[p] + src[p + uint2(1, 0)] + src[p + uint2(0, 1)] + src[p + uint2(1, 1)]) * 0.25;
	} else {
		dst[id.xy] = src[id.xy];
	}
#elif MODE == 1
	dst[id.xy] = src[offset + id.xy];
#else
	dst[id.xy] = src[id.xy];
#endif
}
