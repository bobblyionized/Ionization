#pragma once
// HLSL source for every pass. Compiled at startup with D3DCompile (d3dcompiler_47 ships with Windows).
static const char kShaderSource[] = R"HLSL(
cbuffer P : register(b0) {
    float decay; float ringCount; float blurMode; float lutMix;
    float exposure; float contrast; float saturation; float vibrance;
    float temperature; float tint; float warmth; float lutSize;
    float vignette; float vignetteSoft; float grain; float frameSeed;
    float bloom; float bloomThresh; float sharpen; float chroma;
    float2 texel; float useLut; float aspect;
    float2 srcTexel; float ringHead; float ringCap;
};
Texture2D tCur : register(t0);
Texture2D tPrev : register(t1);
Texture2DArray tRing : register(t2);
Texture2D tBlur : register(t3);
Texture2D tBloom : register(t4);
Texture3D tLut : register(t5);
Texture2D tSrc : register(t6);
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

// Pass 1: temporal accumulation in linear light.
float4 PSAccum(V i) : SV_Target {
    int3 p = int3(i.pos.xy, 0);
    float3 cur = ToLin(tCur.Load(p).rgb);
    if (blurMode > 1.5) {
        int n = max((int)ringCount, 1);
        float3 s = 0;
        int cap = max((int)ringCap, 1);
        int head = (int)ringHead;
        [loop] for (int k = 0; k < n; k++) {
            int idx = (head - 1 - k + cap * 2) % cap;
            s += ToLin(tRing.Load(int4(p.xy, idx, 0)).rgb);
        }
        return float4(s / n, 1);
    }
    if (blurMode > 0.5) {
        float3 prev = tPrev.Load(p).rgb;
        return float4(lerp(cur, prev, decay), 1);
    }
    return float4(cur, 1);
}

// Bloom: quarter-res bright pass (4 bilinear taps cover a 4x4 block).
float4 PSBloomDown(V i) : SV_Target {
    float2 o = srcTexel;
    float3 c = tSrc.Sample(sLin, i.uv + o * float2(-1, -1)).rgb
             + tSrc.Sample(sLin, i.uv + o * float2( 1, -1)).rgb
             + tSrc.Sample(sLin, i.uv + o * float2(-1,  1)).rgb
             + tSrc.Sample(sLin, i.uv + o * float2( 1,  1)).rgb;
    c *= 0.25;
    float br = max(c.r, max(c.g, c.b));
    float knee = 0.2;
    float soft = clamp(br - bloomThresh + knee, 0, 2 * knee);
    soft = soft * soft / (4 * knee + 1e-5);
    float w = max(soft, br - bloomThresh) / max(br, 1e-5);
    return float4(c * w, 1);
}

float3 Gauss(float2 uv, float2 dir) {
    float3 c = tSrc.Sample(sLin, uv).rgb * 0.2270270270;
    c += tSrc.Sample(sLin, uv + dir * 1.3846153846).rgb * 0.3162162162;
    c += tSrc.Sample(sLin, uv - dir * 1.3846153846).rgb * 0.3162162162;
    c += tSrc.Sample(sLin, uv + dir * 3.2307692308).rgb * 0.0702702703;
    c += tSrc.Sample(sLin, uv - dir * 3.2307692308).rgb * 0.0702702703;
    return c;
}
float4 PSBlurH(V i) : SV_Target { return float4(Gauss(i.uv, float2(srcTexel.x * 2.0, 0)), 1); }
float4 PSBlurV(V i) : SV_Target { return float4(Gauss(i.uv, float2(0, srcTexel.y * 2.0)), 1); }

float Hash(float2 p) {
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

// Final pass: optics, white balance, tone, LUT, vignette, grain.
float4 PSGrade(V i) : SV_Target {
    float2 uv = i.uv;
    int3 p = int3(i.pos.xy, 0);
    float3 c;
    if (chroma > 0) {
        float2 d = (uv - 0.5) * chroma * 0.012;
        c = float3(tBlur.Sample(sLin, uv - d).r, tBlur.Load(p).g, tBlur.Sample(sLin, uv + d).b);
    } else {
        c = tBlur.Load(p).rgb;
    }
    if (sharpen > 0) {
        float3 n = tBlur.Load(p + int3(1, 0, 0)).rgb + tBlur.Load(p - int3(1, 0, 0)).rgb
                 + tBlur.Load(p + int3(0, 1, 0)).rgb + tBlur.Load(p - int3(0, 1, 0)).rgb;
        c = max(c + (c - n * 0.25) * sharpen * 2.0, 0);
    }
    if (bloom > 0) c += tBloom.Sample(sLin, uv).rgb * bloom * 2.0;

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
        float2 d = (uv - 0.5) * float2(aspect, 1);
        float r = length(d) / length(float2(aspect, 1) * 0.5);
        float inner = lerp(0.75, 0.05, vignetteSoft);
        s *= 1 - smoothstep(inner, 1.15, r) * vignette;
    }
    if (grain > 0) {
        float n = Hash(i.pos.xy + frameSeed * 113.7) - 0.5;
        s += n * grain * 0.16 * (1.0 - 0.6 * l);
    }
    return float4(saturate(s), 1);
}
)HLSL";
