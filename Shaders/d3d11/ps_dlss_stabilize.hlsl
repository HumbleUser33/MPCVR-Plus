// Temporal stabilizer for DLSS 5 NR, applied after the network.
//
// The network's own history cannot be steadied through DLSSNR.ControlMask: any
// mask strips about 90 % of the effect (tools/dlssnr_probe --teffect). These
// passes steady the result instead. Two variants, measured against each other:
//   EFFECT  only the network's change to the picture, E = out - in, is filtered
//           over time and added to the current frame: the video underneath is
//           never delayed, only the enhancement is
//   OUTPUT  the output itself is filtered, TAA-style
// Either way the history is clamped to the neighbourhood of the current value,
// widened by a tolerance, so a wrong motion estimate cannot drag old content far.
//
// Motion comes from NVIDIA Optical Flow: vectors with a confidence built from
// forward/backward consistency, matching cost and the input itself, so the history
// follows the picture and moving areas are steadied too. A shader detector that only
// said where something had moved stood in for it once; it is gone.
//
//   PASS 0  flow frame: luma of the working-size picture, box-filtered by a whole
//           factor of 1 to 4, into the R8 texture NVIDIA Optical Flow reads; the box
//           can reach further, to take grain out of what the engine matches
//   PASS 1  flow -> motion (MRT): target 0 motion in working-size pixels, current
//           to previous (RG16F, also usable as DLSSNR.MVec); target 1 confidence
//   PASS 2  stabilize (MRT): target 0 history for the next frame, target 1 the
//           picture to show
//   PASS 4  block motion for DLSS Super Resolution, a pixel per vector block: each
//           block drawn to the picture's global motion (cs_dlss_global_motion.hlsl)
//           where it lies within the noise of it, and what keeps its own vector
//           steadied across the blocks that move alike and across pictures
//   PASS 3  PASS 4's blocks -> motion at the working size, for DLSS Super Resolution
//
// Every fetch is a Load, or a bilinear built from Loads: the renderer's linear
// sampler only filters when magnifying, and integer textures cannot be sampled.

struct PS_INPUT
{
	float4 Pos : SV_POSITION;
	float2 Tex : TEXCOORD;
};

#ifndef PASS
#define PASS 2
#endif

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

#if PASS == 0

Texture2D picture : register(t0);

cbuffer PS_DLSS_FLOW_FRAME : register(b0)
{
	float factor;   // working pixels per flow pixel: 1 to 4
	float reach;    // working pixels the box reaches beyond its flow pixel on each side: 0..4
	float2 reserved;
};

float4 main(PS_INPUT input) : SV_Target
{
	uint w, h;
	picture.GetDimensions(w, h);
	const int2 last = int2(w, h) - 1;
	const int f = clamp((int)factor, 1, 4);
	const int r = clamp((int)reach, 0, 4);
	const int n = f + 2 * r;
	const int2 base = int2(input.Pos.xy) * f - r;
	float3 c = 0;
	[loop] for (int y = 0; y < n; y++) {
		[loop] for (int x = 0; x < n; x++) {
			c += picture.Load(int3(clamp(base + int2(x, y), int2(0, 0), last), 0)).rgb;
		}
	}
	c /= (float)(n * n);
	return float4(dot(saturate(c), kLuma), 0, 0, 1);
}

#elif PASS == 1

Texture2D<int4>  flowForward  : register(t0);   // R16G16_SINT, 1/32 flow pixel, current -> previous
Texture2D<int4>  flowBackward : register(t1);   // previous -> current
Texture2D<uint4> costForward  : register(t2);   // R8_UINT, higher is worse

cbuffer PS_DLSS_FLOW_MOTION : register(b0)
{
	float2 flowScale;     // working pixels per flow pixel, x and y
	float  grid;          // flow pixels per vector
	float  consistency;   // forward/backward mismatch (flow pixels) where confidence starts to fall
	float2 flowSize;      // flow frame size, pixels
	float  costLow;       // cost where confidence starts to fall
	float  costHigh;      // and where it reaches 0
	float  useBackward;   // 0 when the engine gave no backward flow
	float  useCost;       // 0 when it gave no cost
	float2 reserved;
};

struct MOTION_OUTPUT
{
	float4 motion     : SV_Target0;
	float4 confidence : SV_Target1;
};

MOTION_OUTPUT main(PS_INPUT input)
{
	uint gw, gh;
	flowForward.GetDimensions(gw, gh);
	const int2 last = int2(gw, gh) - 1;

	// This pixel in the flow frame, and a bilinear of the four vector blocks
	// around it (block centres sit at (b + 0.5) * grid).
	const float2 q = input.Pos.xy / flowScale;
	const float2 bf = q / grid - 0.5;
	const int2 b0 = (int2)floor(bf);
	const float2 t = bf - (float2)b0;
	float2 f = 0;
	for (int j = 0; j < 2; j++) {
		for (int i = 0; i < 2; i++) {
			const int2 b = clamp(b0 + int2(i, j), int2(0, 0), last);
			const float weight = (i ? t.x : 1.0 - t.x) * (j ? t.y : 1.0 - t.y);
			f += weight * (float2)flowForward.Load(int3(b, 0)).xy;
		}
	}
	f /= 32.0;

	const float2 qPrevious = q + f;
	float confidence = 1.0;

	// Where this content was, the backward flow should point straight back.
	if (useBackward > 0.5) {
		const int2 bb = clamp((int2)floor(qPrevious / grid), int2(0, 0), last);
		const float2 back = (float2)flowBackward.Load(int3(bb, 0)).xy / 32.0;
		confidence *= 1.0 - smoothstep(consistency, 2.0 * consistency, length(f + back));
	}
	if (useCost > 0.5) {
		const int2 bc = clamp((int2)floor(q / grid), int2(0, 0), last);
		confidence *= 1.0 - smoothstep(costLow, costHigh, (float)costForward.Load(int3(bc, 0)).x);
	}
	if (any(qPrevious < 0.0) || any(qPrevious > flowSize)) {
		confidence = 0.0;
	}

	MOTION_OUTPUT o;
	o.motion = float4(f * flowScale, 0, 1);
	o.confidence = float4(confidence, 0, 0, 1);
	return o;
}

#elif PASS == 4

// Block motion for DLSS Super Resolution, one pixel per vector block. DLSS resamples
// its history along these vectors, so their noise shows: on a still background,
// block vectors a fraction of a pixel off in every direction, smoothed between blocks
// and new on every picture, make it shimmer like heat haze. So each block is first
// drawn to the picture's global motion (cs_dlss_global_motion.hlsl) where it lies
// within that noise: a still background gets exactly zero, a pan exactly the pan. A
// block that points out of the picture where the global motion does not is noise as
// well -- the engine's edge blocks have no neighbours on one side -- and DLSS drops its
// history wherever a vector leaves the picture.
//
// What moves on its own keeps its vector, and with it the engine's error: on a moving
// subject under grain, half of Optical Flow's block vectors are off by 0.9 pixel or more,
// 2 under heavy grain (--tsrstill), which makes the subject shimmer the same way. With
// shift > 0 the block climbs from its own vector to the motion most of the trusted
// blocks around it share (mean shift: a few steps of a mean weighted by trust and by
// closeness), and keeps it if enough of them do; the background, a pixel or more away,
// never joins in. Then comes a blend with where the same content was one picture
// earlier, when they agree.
//
// Under heavy grain the engine also underestimates what moves, by 0.4 to 0.8 pixel of
// 2.9 on a test subject, the same way on every block, which no mean undoes. With
// refine > 0 the kept vector is searched again on the two flow frames themselves:
// the offset, in halving steps around it, that best matches the block and its margin
// from one picture to the other. Only where the picture has texture enough: on a flat,
// grained area such a search drifts, and the global motion stays.
//
// Target: xy the block's own vector, cleaned, flow pixels; z how much of it it keeps,
// the rest being the global motion. PASS 3 blends the blocks into the working size.
// On a cut (cs_dlss_scene_cut.hlsl) no block keeps anything, so that the next picture
// does not blend in vectors measured across it.

Texture2D<int4>   flowForward  : register(t0);   // R16G16_SINT, 1/32 flow pixel, current -> previous
Texture2D<int4>   flowBackward : register(t1);   // previous -> current
Texture2D<uint4>  costForward  : register(t2);   // R8_UINT, higher is worse
Texture2D<float4> globalMotion : register(t3);   // 1x1: flow pixels
Texture2D<float4> lastField    : register(t4);   // this pass's target for the last picture
Texture2D<float>  frameNow     : register(t5);   // the flow frame the engine just ran on
Texture2D<float>  frameLast    : register(t6);   // and the one before it
Buffer<float>     cutState     : register(t7);   // [0] 1 on a cut; unbound, 0

cbuffer PS_DLSS_BLOCK_MOTION : register(b0)
{
	float2 flowScale;     // working pixels per flow pixel, x and y
	float  grid;          // flow pixels per vector
	float  consistency;   // forward/backward mismatch (flow pixels) where confidence starts to fall
	float2 flowSize;      // flow frame size, pixels
	float  costLow;       // cost where confidence starts to fall
	float  costHigh;      // and where it reaches 0
	float  useBackward;   // 0 when the engine gave no backward flow
	float  useCost;       // 0 when it gave no cost
	float  snapLow;       // distance to the global motion, working pixels, where a block starts to keep its vector
	float  snapHigh;      // and where it keeps it whole
	float  gate;          // 0: by distance; 1: by confidence; 2: only far and trusted blocks keep theirs
	float  deadZone;      // a global motion shorter than this, working pixels, is no motion at all
	float  radius;        // blocks on each side averaged with this one
	float  sigma;         // working pixels: how alike two vectors must be to be averaged
	float  temporal;      // weight of this picture's vector against the last one's; 1: the last is not used
	float  history;       // 1 when lastField holds the last picture
	float  shift;         // mean shift steps; 0: one mean around the block's own vector, kept by its own trust
	float  supportLow;    // with shift: the share of the window's trust that must agree to begin keeping
	float  supportHigh;   // and to keep whole
	float  refine;        // halving search steps around a kept vector, from half a flow pixel; 0: none
	float  reach;         // flow pixels on each side of the block centre the search compares
	float  textureMin;    // the least luma deviation over that window for a search to be trusted
	float  evidence;      // with shift: trusted blocks' worth that must share the motion to keep it whole
	float3 reserved;
};

bool Outside(float2 q)
{
	return any(q < 0.0) || any(q > flowSize);
}

// PASS 1's confidence at a block, 0..1; none for a vector that leaves the picture.
float Confidence(int2 b, float2 v, int2 last)
{
	const float2 q = ((float2)b + 0.5) * grid + v;
	if (Outside(q)) {
		return 0.0;
	}
	float confidence = 1.0;
	if (useBackward > 0.5) {
		const int2 bb = clamp((int2)floor(q / grid), int2(0, 0), last);
		const float2 back = (float2)flowBackward.Load(int3(bb, 0)).xy / 32.0;
		confidence *= 1.0 - smoothstep(consistency, 2.0 * consistency, length(v + back));
	}
	if (useCost > 0.5) {
		confidence *= 1.0 - smoothstep(costLow, costHigh, (float)costForward.Load(int3(b, 0)).x);
	}
	return confidence;
}

// How much of its own vector a block keeps, 0..1; the rest is the global motion.
float Keep(int2 b, float2 v, float2 g, int2 last)
{
	const float2 centre = ((float2)b + 0.5) * grid;
	if (Outside(centre + v) && !Outside(centre + g)) {
		return 0.0;
	}
	const float byDistance = smoothstep(snapLow, snapHigh, length((v - g) * flowScale));
	if (gate < 0.5) {
		return byDistance;
	}
	const float confidence = Confidence(b, v, last);
	return (gate < 1.5) ? confidence : byDistance * confidence;
}

// Bilinear of the last flow frame at a position in pixels (centres at +0.5), from Loads.
float LastAt(float2 pos, int2 lastPixel)
{
	const float2 f = pos - 0.5;
	const int2 i0 = (int2)floor(f);
	const float2 t = f - (float2)i0;
	const float a = frameLast.Load(int3(clamp(i0, int2(0, 0), lastPixel), 0));
	const float b = frameLast.Load(int3(clamp(i0 + int2(1, 0), int2(0, 0), lastPixel), 0));
	const float c = frameLast.Load(int3(clamp(i0 + int2(0, 1), int2(0, 0), lastPixel), 0));
	const float d = frameLast.Load(int3(clamp(i0 + int2(1, 1), int2(0, 0), lastPixel), 0));
	return lerp(lerp(a, b, t.x), lerp(c, d, t.x), t.y);
}

// How badly the window around a block centre matches the last frame moved by v.
float Mismatch(int2 origin, int size, float2 v, int2 lastPixel)
{
	float sum = 0.0;
	for (int y = 0; y < size; y++) {
		for (int x = 0; x < size; x++) {
			const int2 p = clamp(origin + int2(x, y), int2(0, 0), lastPixel);
			const float e = frameNow.Load(int3(p, 0)) - LastAt((float2)p + 0.5 + v, lastPixel);
			sum += e * e;
		}
	}
	return sum;
}

float4 main(PS_INPUT input) : SV_Target
{
	if (cutState.Load(0) > 0.5) {
		return float4(0, 0, 0, 1);
	}
	uint gw, gh;
	flowForward.GetDimensions(gw, gh);
	const int2 last = int2(gw, gh) - 1;
	const int2 b = int2(input.Pos.xy);
	float2 g = globalMotion.Load(int3(0, 0, 0)).xy;
	if (length(g * flowScale) < deadZone) {
		g = 0.0;
	}

	const float2 v = (float2)flowForward.Load(int3(b, 0)).xy / 32.0;
	float keep = 0.0;
	float2 own = v;
	if (shift > 0.5) {
		// The window's vectors and their trust, once.
		const int r = min((int)radius, 3);
		const int count = (2 * r + 1) * (2 * r + 1);
		float2 vectors[49];
		float trust[49];
		float totalTrust = 0.0;
		int k = 0;
		for (int j = -r; j <= r; j++) {
			for (int i = -r; i <= r; i++) {
				// Beyond the grid there is no block: a clamped window would count the
				// edge's own blocks several times over, and they are the engine's worst.
				const int2 n = b + int2(i, j);
				const bool inside = all(n >= int2(0, 0)) && all(n <= last);
				vectors[k] = inside ? (float2)flowForward.Load(int3(n, 0)).xy / 32.0 : float2(0.0, 0.0);
				trust[k] = inside ? Confidence(n, vectors[k], last) : 0.0;
				totalTrust += trust[k];
				k++;
			}
		}
		// From the block's own vector to the motion the trusted blocks around share.
		float weights = 0.0;
		for (int step = 0; step < (int)shift; step++) {
			float2 sum = 0.0;
			weights = 0.0;
			for (int m = 0; m < count; m++) {
				const float d = length((vectors[m] - own) * flowScale) / sigma;
				const float w = trust[m] * exp(-d * d);
				sum += w * vectors[m];
				weights += w;
			}
			// A thousandth of a trusted block is none: far from every trusted vector, a
			// block stays where it is, whatever the hardware makes of such tiny weights.
			if (weights > 1e-3) {
				own = sum / weights;
			}
		}
		// Kept if it is not the global motion and enough of the window's trust shares it
		// -- a share, so that heavy grain, which leaves fewer blocks trusted, does not
		// take a moving subject's motion away -- with evidence blocks' worth of it at
		// least: a few flat, grained blocks agreeing on a false motion are not a subject.
		const float2 centre = ((float2)b + 0.5) * grid;
		if (!(Outside(centre + own) && !Outside(centre + g))) {
			keep = smoothstep(snapLow, snapHigh, length((own - g) * flowScale))
				* smoothstep(supportLow, supportHigh, weights / max(totalTrust, 1e-4))
				* smoothstep(0.5 * evidence, evidence, weights);
		}
	} else {
		keep = Keep(b, v, g, last);
		if (keep > 0.0) {
			// The trusted blocks around that move alike.
			const int r = (int)radius;
			float2 sum = 0.0;
			float weights = 0.0;
			for (int j = -r; j <= r; j++) {
				for (int i = -r; i <= r; i++) {
					const int2 n = clamp(b + int2(i, j), int2(0, 0), last);
					const float2 vn = (float2)flowForward.Load(int3(n, 0)).xy / 32.0;
					const float d = length((vn - v) * flowScale) / sigma;
					const float w = Confidence(n, vn, last) * exp(-d * d);
					sum += w * vn;
					weights += w;
				}
			}
			if (weights > 0.0) {
				own = sum / weights;
			}
		}
	}
	if (keep > 0.0 && refine > 0.5) {
		// The kept vector searched again on the frames themselves, where they have texture.
		uint fw, fh;
		frameNow.GetDimensions(fw, fh);
		const int2 lastPixel = int2(fw, fh) - 1;
		const int size = 2 * (int)reach;
		const int2 origin = (int2)(((float2)b + 0.5) * grid) - (int)reach;
		float mean = 0.0, square = 0.0;
		for (int y = 0; y < size; y++) {
			for (int x = 0; x < size; x++) {
				const float a = frameNow.Load(int3(clamp(origin + int2(x, y), int2(0, 0), lastPixel), 0));
				mean += a;
				square += a * a;
			}
		}
		const float n = (float)(size * size);
		mean /= n;
		if (sqrt(max(square / n - mean * mean, 0.0)) >= textureMin) {
			float2 best = own;
			float bestCost = Mismatch(origin, size, best, lastPixel);
			float step = 0.5;
			for (int level = 0; level < (int)refine; level++) {
				const float2 centre = best;
				for (int j = -1; j <= 1; j++) {
					for (int i = -1; i <= 1; i++) {
						if (i == 0 && j == 0) {
							continue;
						}
						const float2 candidate = centre + float2(i, j) * step;
						const float cost = Mismatch(origin, size, candidate, lastPixel);
						if (cost < bestCost) {
							best = candidate;
							bestCost = cost;
						}
					}
				}
				step *= 0.5;
			}
			own = best;
		}
	}
	if (keep > 0.0) {
		// Where this content was one picture earlier, if its vector agrees.
		if (history > 0.5 && temporal < 1.0) {
			const float2 q = ((float2)b + 0.5) * grid + own;
			const int2 bp = clamp((int2)floor(q / grid), int2(0, 0), last);
			const float4 previous = lastField.Load(int3(bp, 0));
			if (previous.z > 0.5 && length((previous.xy - own) * flowScale) < snapHigh) {
				own = lerp(previous.xy, own, temporal);
			}
		}
	}
	return float4(own, keep, 1.0);
}

#elif PASS == 3

// Motion for DLSS Super Resolution at the working size: a bilinear of PASS 4's four
// blocks around each pixel, each its own vector for what it keeps and the global
// motion for the rest, as PASS 1 blends the raw ones. On a cut (cs_dlss_scene_cut.hlsl)
// every vector leaves the picture instead, and DLSS drops the history of the shot before.

Texture2D<float4> field        : register(t0);   // PASS 4
Texture2D<float4> globalMotion : register(t1);   // 1x1: flow pixels
Buffer<float>     cutState     : register(t2);   // [0] 1 on a cut; unbound, 0

cbuffer PS_DLSS_SNAP_MOTION : register(b0)
{
	float2 flowScale;     // working pixels per flow pixel, x and y
	float  grid;          // flow pixels per vector
	float  deadZone;      // as PASS 4
	float2 away;          // a vector that leaves the picture from anywhere in it, working pixels
	float2 reserved;
};

float4 main(PS_INPUT input) : SV_Target
{
	if (cutState.Load(0) > 0.5) {
		return float4(away, 0, 1);
	}
	uint gw, gh;
	field.GetDimensions(gw, gh);
	const int2 last = int2(gw, gh) - 1;
	float2 g = globalMotion.Load(int3(0, 0, 0)).xy;
	if (length(g * flowScale) < deadZone) {
		g = 0.0;
	}

	const float2 q = input.Pos.xy / flowScale;
	const float2 bf = q / grid - 0.5;
	const int2 b0 = (int2)floor(bf);
	const float2 t = bf - (float2)b0;
	float2 f = 0;
	for (int j = 0; j < 2; j++) {
		for (int i = 0; i < 2; i++) {
			const int2 b = clamp(b0 + int2(i, j), int2(0, 0), last);
			const float weight = (i ? t.x : 1.0 - t.x) * (j ? t.y : 1.0 - t.y);
			const float4 block = field.Load(int3(b, 0));
			f += weight * lerp(g, block.xy, block.z);
		}
	}
	return float4(f * flowScale, 0, 1);
}

#else

Texture2D current    : register(t0);   // the network's input this frame
Texture2D network    : register(t1);   // its output
Texture2D history    : register(t2);   // last frame's target 0
Texture2D motion     : register(t3);   // PASS 1 target 0
Texture2D previous   : register(t4);   // last frame's network input
Texture2D confidence : register(t5);   // PASS 1 target 1

cbuffer PS_DLSS_STABILIZE : register(b0)
{
	float  minCurrent;     // weight of the current frame where the history is trusted
	float  tolerance;      // how far the history may stray from the current neighbourhood
	float  reserved0;      // was the detector's switch
	float  reprojection;   // input luma error where confidence starts to fall; 0 = off
	float  reset;          // 1: no history this frame
	float  outputMode;     // 0: EFFECT, 1: OUTPUT
	float  reserved1;      // was the detector's age map scale
	float  reserved2;
};

struct STABILIZE_OUTPUT
{
	float4 history : SV_Target0;
	float4 color   : SV_Target1;
};

float3 Fetch(Texture2D tex, int2 p, int2 last)
{
	return tex.Load(int3(clamp(p, int2(0, 0), last), 0)).rgb;
}

// Bilinear at a position in pixels (centres at +0.5), from Loads.
float3 Bilinear(Texture2D tex, float2 pos, int2 last)
{
	const float2 f = pos - 0.5;
	const int2 i0 = (int2)floor(f);
	const float2 t = f - (float2)i0;
	const float3 top    = lerp(Fetch(tex, i0, last), Fetch(tex, i0 + int2(1, 0), last), t.x);
	const float3 bottom = lerp(Fetch(tex, i0 + int2(0, 1), last), Fetch(tex, i0 + int2(1, 1), last), t.x);
	return lerp(top, bottom, t.y);
}

float3 Value(int2 p, int2 last)
{
	const float3 result = Fetch(network, p, last);
	return (outputMode > 0.5) ? result : result - Fetch(current, p, last);
}

STABILIZE_OUTPUT main(PS_INPUT input)
{
	uint w, h;
	current.GetDimensions(w, h);
	const int2 last = int2(w, h) - 1;
	const int2 p = int2(input.Pos.xy);

	// The quantity being steadied, and its 3x3 neighbourhood.
	const float3 value = Value(p, last);
	float3 low = value;
	float3 high = value;
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			const float3 v = Value(p + int2(x, y), last);
			low = min(low, v);
			high = max(high, v);
		}
	}

	const float2 previousPos = input.Pos.xy + motion.Load(int3(p, 0)).rg;
	float trust = confidence.Load(int3(p, 0)).r;
	if (reprojection > 0.0) {
		const float error = abs(dot(Fetch(current, p, last) - Bilinear(previous, previousPos, last), kLuma));
		trust *= 1.0 - smoothstep(reprojection, 2.0 * reprojection, error);
	}
	if (any(previousPos < 0.0) || any(previousPos > float2(w, h))) {
		trust = 0.0;
	}
	trust *= 1.0 - reset;

	const float3 old = clamp(Bilinear(history, previousPos, last), low - tolerance, high + tolerance);
	const float3 steady = lerp(old, value, lerp(1.0, minCurrent, trust));

	STABILIZE_OUTPUT o;
	o.history = float4(steady, 1);
	o.color = float4((outputMode > 0.5) ? steady : Fetch(current, p, last) + steady, 1);
	return o;
}

#endif
