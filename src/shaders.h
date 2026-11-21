#pragma once
// HLSL for every pass, compiled at startup with D3DCompile (d3dcompiler_47 ships with Windows).
// PSComposite is compiled once per PATH:
//   0 = no blur (grade only)
//   1 = trail, full resolution, fused (writes output + next accumulator)
//   2 = trail, efficient: output = cur + upsample(accHalf - curHalf)   (linear space)
//   3 = blend, full resolution, fused (writes output + running sum + ring slot)
//   4 = blend, efficient: output = cur + upsample(avgHalf - curHalf)   (sRGB space)
// Accumulators are compact on purpose: this runs next to games on shared-memory GPUs,
// so bytes moved per pixel matter more than anything else.
static const char kShaderSource[] = R"HLSL(
#ifndef PATH
#define PATH 0
#endif
cbuffer P : register(b0) {
    float decay; float lutMix; float blendCount; float blendSub;
    float exposure; float contrast; float saturation; float vibrance;
    float temperature; float tint; float warmth; float lutSize;
    float vignette; float vignetteSoft; float grain; float frameSeed;
    float bloom; float bloomThresh; float sharpen; float chroma;
    float2 texel; float useLut; float aspect;
    float2 srcTexel; float2 pad0;
};
Texture2D tCur : register(t0);          // current desktop frame (sRGB values)
Texture2D tAccPrev : register(t1);      // trail accumulator, sqrt(linear) in 10-bit
Texture2D<uint4> tSumPrev : register(t2); // blend running sum of 8-bit values
Texture2D tOldest : register(t3);       // blend ring slot leaving the window
Texture2D tAccH : register(t4);         // efficient: half-res blurred
Texture2D tCurH : register(t5);         // efficient: half-res current
Texture2D tBloom : register(t6);
Texture3D tLut : register(t7);
Texture2D tSrc : register(t8);          // bloom chain input
SamplerState sLin : register(s0);

struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

V VS(uint id : SV_VertexID) {
    V o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float3 ToLin(float3 c) { return c <= 0.04045 ? c / 12.92 : pow(abs((c + 0.055) / 1.055), 2.4); }
float3 ToSrgb(float3 c) { c = saturate(c); return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
float3 Enc(float3 lin) { return sqrt(saturate(lin)); }
float3 Dec(float3 e) { return e * e; }
float Hash(float2 p) {
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}
float3 Dither(float2 p) {
    float s = frameSeed * 7.13;
    return float3(Hash(p + s), Hash(p + s + 17.31), Hash(p + s + 41.97)) - 0.5;
}

// ------------------------------------------------------------------ efficient accumulation (half res)
struct TrailHalfOut { float4 acc : SV_Target0; float4 cur : SV_Target1; };
TrailHalfOut PSTrailHalf(V i) {
    // A bilinear tap at a half-res texel centre is an exact 2x2 box of the full-res frame.
    float3 curLin = ToLin(tCur.SampleLevel(sLin, i.uv, 0).rgb);
    float3 prev = Dec(tAccPrev.Load(int3(i.pos.xy, 0)).rgb);
    float3 b = lerp(curLin, prev, decay);
    TrailHalfOut o;
    o.acc = float4(saturate(Enc(b) + Dither(i.pos.xy) / 1023.0), 1);
    o.cur = float4(Enc(curLin), 1);
    return o;
}

struct BlendHalfOut { uint4 sum : SV_Target0; float4 slot : SV_Target1; float4 acc : SV_Target2; };
BlendHalfOut PSBlendHalf(V i) {
    int3 p = int3(i.pos.xy, 0);
    uint3 q = (uint3)round(saturate(tCur.SampleLevel(sLin, i.uv, 0).rgb) * 255.0);
    uint3 s = tSumPrev.Load(p).rgb + q;
    if (blendSub > 0.5) s -= (uint3)round(tOldest.Load(p).rgb * 255.0);
    BlendHalfOut o;
    o.sum = uint4(s, 0);
    o.slot = float4(q / 255.0, 1);
    o.acc = float4(s / (255.0 * blendCount), 1);
    return o;
}

// ------------------------------------------------------------------ bloom (quarter res)
float4 PSBloomDown(V i) : SV_Target {
    float2 o = srcTexel;
    float3 c = ToLin(tSrc.SampleLevel(sLin, i.uv + o * float2(-1, -1), 0).rgb)
             + ToLin(tSrc.SampleLevel(sLin, i.uv + o * float2( 1, -1), 0).rgb)
             + ToLin(tSrc.SampleLevel(sLin, i.uv + o * float2(-1,  1), 0).rgb)
             + ToLin(tSrc.SampleLevel(sLin, i.uv + o * float2( 1,  1), 0).rgb);
    c *= 0.25;
    float br = max(c.r, max(c.g, c.b));
    float knee = 0.2;
    float soft = clamp(br - bloomThresh + knee, 0, 2 * knee);
    soft = soft * soft / (4 * knee + 1e-5);
    float w = max(soft, br - bloomThresh) / max(br, 1e-5);
    return float4(c * w, 1);
}
float3 Gauss(float2 uv, float2 dir) {
    float3 c = tSrc.SampleLevel(sLin, uv, 0).rgb * 0.2270270270;
    c += tSrc.SampleLevel(sLin, uv + dir * 1.3846153846, 0).rgb * 0.3162162162;
    c += tSrc.SampleLevel(sLin, uv - dir * 1.3846153846, 0).rgb * 0.3162162162;
    c += tSrc.SampleLevel(sLin, uv + dir * 3.2307692308, 0).rgb * 0.0702702703;
    c += tSrc.SampleLevel(sLin, uv - dir * 3.2307692308, 0).rgb * 0.0702702703;
    return c;
}
float4 PSBlurH(V i) : SV_Target { return float4(Gauss(i.uv, float2(srcTexel.x * 2.0, 0)), 1); }
float4 PSBlurV(V i) : SV_Target { return float4(Gauss(i.uv, float2(0, srcTexel.y * 2.0)), 1); }

// ------------------------------------------------------------------ blurred image, per path (linear)
float3 BlurredAt(float2 uv) {
#if PATH == 1
    return lerp(ToLin(tCur.SampleLevel(sLin, uv, 0).rgb), Dec(tAccPrev.SampleLevel(sLin, uv, 0).rgb), decay);
#elif PATH == 2
    float3 cur = ToLin(tCur.SampleLevel(sLin, uv, 0).rgb);
    float3 aE = tAccH.SampleLevel(sLin, uv, 0).rgb, cE = tCurH.SampleLevel(sLin, uv, 0).rgb;
    float3 d = Dec(aE) - Dec(cE);
    if (max(abs(aE.r - cE.r), max(abs(aE.g - cE.g), abs(aE.b - cE.b))) < 1.5 / 1023.0) d = 0;
    return max(cur + d, 0);
#elif PATH == 3
    int3 p = int3(uv / texel, 0);
    uint3 q = (uint3)round(saturate(tCur.Load(p).rgb) * 255.0);
    uint3 s = tSumPrev.Load(p).rgb + q;
    if (blendSub > 0.5) s -= (uint3)round(tOldest.Load(p).rgb * 255.0);
    return ToLin(s / (255.0 * blendCount));
#elif PATH == 4
    float3 cur = tCur.SampleLevel(sLin, uv, 0).rgb;
    float3 a = tAccH.SampleLevel(sLin, uv, 0).rgb, c = tCurH.SampleLevel(sLin, uv, 0).rgb;
    float3 d = a - c;
    if (max(abs(d.r), max(abs(d.g), abs(d.b))) < 0.6 / 255.0) d = 0;
    return ToLin(saturate(cur + d));
#else
    return ToLin(tCur.SampleLevel(sLin, uv, 0).rgb);
#endif
}

float3 Grade(float3 c, float2 uv, float2 pos) {
    if (chroma > 0) {
        float2 d = (uv - 0.5) * chroma * 0.012;
        c.r = BlurredAt(uv - d).r;
        c.b = BlurredAt(uv + d).b;
    }
    if (sharpen > 0) {
        float3 n = BlurredAt(uv + float2(texel.x, 0)) + BlurredAt(uv - float2(texel.x, 0))
                 + BlurredAt(uv + float2(0, texel.y)) + BlurredAt(uv - float2(0, texel.y));
        c = max(c + (c - n * 0.25) * sharpen * 2.0, 0);
    }
    if (bloom > 0) c += tBloom.SampleLevel(sLin, uv, 0).rgb * bloom * 2.0;
    c *= exp2(exposure);
    c *= float3(1 + temperature * 0.10, 1 - tint * 0.08, 1 - temperature * 0.10);
    c *= lerp(float3(1, 1, 1), float3(1.0, 0.80, 0.55), warmth);
    float3 s = ToSrgb(c);
    s = saturate((s - 0.5) * (1 + contrast) + 0.5);
    float l = dot(s, float3(0.2126, 0.7152, 0.0722));
    s = lerp(l.xxx, s, saturation);
    float mx = max(s.r, max(s.g, s.b));
    float mn = min(s.r, min(s.g, s.b));
    s = saturate(lerp(l.xxx, s, 1 + vibrance * (1 - (mx - mn))));
    if (useLut > 0.5) {
        float3 lc = tLut.SampleLevel(sLin, s * ((lutSize - 1) / lutSize) + 0.5 / lutSize, 0).rgb;
        s = lerp(s, lc, lutMix);
    }
    if (vignette > 0) {
        float2 dd = (uv - 0.5) * float2(aspect, 1);
        float r = length(dd) / length(float2(aspect, 1) * 0.5);
        float inner = lerp(0.75, 0.05, vignetteSoft);
        s *= 1 - smoothstep(inner, 1.15, r) * vignette;
    }
    if (grain > 0) {
        float n = Hash(pos + frameSeed * 113.7) - 0.5;
        s += n * grain * 0.16 * (1.0 - 0.6 * l);
    }
    return saturate(s);
}

// ------------------------------------------------------------------ final composite
#if PATH == 1
struct CompOut { float4 color : SV_Target0; float4 acc : SV_Target1; };
#elif PATH == 3
struct CompOut { float4 color : SV_Target0; uint4 sum : SV_Target1; float4 slot : SV_Target2; };
#else
struct CompOut { float4 color : SV_Target0; };
#endif

CompOut PSComposite(V i) {
    CompOut o;
    int3 p = int3(i.pos.xy, 0);
    float3 c;
#if PATH == 1
    float3 cur = ToLin(tCur.Load(p).rgb);
    float3 prev = Dec(tAccPrev.Load(p).rgb);
    c = lerp(cur, prev, decay);
    o.acc = float4(saturate(Enc(c) + Dither(i.pos.xy) / 1023.0), 1);
#elif PATH == 3
    uint3 q = (uint3)round(saturate(tCur.Load(p).rgb) * 255.0);
    uint3 s = tSumPrev.Load(p).rgb + q;
    if (blendSub > 0.5) s -= (uint3)round(tOldest.Load(p).rgb * 255.0);
    o.sum = uint4(s, 0);
    o.slot = float4(q / 255.0, 1);
    c = ToLin(s / (255.0 * blendCount));
#else
    c = BlurredAt(i.uv);
#endif
    o.color = float4(Grade(c, i.uv, i.pos.xy), 1);
    return o;
}
)HLSL";
