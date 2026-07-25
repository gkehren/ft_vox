#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D hdrBuffer;
layout(set = 0, binding = 1) uniform sampler2D bloomBuffer;
layout(set = 0, binding = 2) uniform sampler2D godRaysBuffer;
layout(set = 0, binding = 3) uniform sampler2D ssaoBuffer;

layout(push_constant) uniform PC {
    vec4 p0; // x=exposure, y=bloomIntensity, z=gamma, w=toneMapper
    vec4 p1; // x=bloomOn, y=fxaaOn, z=godRaysOn, w=postSaturation
    vec4 p2; // xy=texelSize, z=postContrast, w=ssaoOn
    vec4 p3; // x=ssaoIntensity, y=underwater, z=underwaterStrength, w=time
    vec4 p4; // x=filmGrain, y=vignette, z=unused, w=unused
} pc;

vec3 acesFilm(vec3 x)
{
    float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 reinhard(vec3 x)
{
    return x / (x + vec3(1.0));
}

float getLDRLuminance(vec2 uv)
{
    float exposure = pc.p0.x;
    float gamma = max(pc.p0.z, 0.001);
    int toneMapper = int(pc.p0.w + 0.5);
    vec3 hdrColor = texture(hdrBuffer, uv).rgb;
    vec3 mapped = hdrColor * exposure;
    mapped = toneMapper == 0 ? acesFilm(mapped) : reinhard(mapped);
    mapped = pow(max(mapped, vec3(0.0)), vec3(1.0 / gamma));
    return dot(mapped, vec3(0.299, 0.587, 0.114));
}

vec3 applyFXAA(vec2 uv)
{
    vec2 texelSize = pc.p2.xy;
    float lumC = getLDRLuminance(uv);
    float lumN = getLDRLuminance(uv + vec2(0.0, texelSize.y));
    float lumS = getLDRLuminance(uv + vec2(0.0, -texelSize.y));
    float lumE = getLDRLuminance(uv + vec2(texelSize.x, 0.0));
    float lumW = getLDRLuminance(uv + vec2(-texelSize.x, 0.0));
    float lumMin = min(lumC, min(min(lumN, lumS), min(lumE, lumW)));
    float lumMax = max(lumC, max(max(lumN, lumS), max(lumE, lumW)));
    float lumRange = lumMax - lumMin;
    if (lumRange < max(0.0312, lumMax * 0.125))
        return texture(hdrBuffer, uv).rgb;

    float lumNW = getLDRLuminance(uv + vec2(-texelSize.x, texelSize.y));
    float lumNE = getLDRLuminance(uv + vec2(texelSize.x, texelSize.y));
    float lumSW = getLDRLuminance(uv + vec2(-texelSize.x, -texelSize.y));
    float lumSE = getLDRLuminance(uv + vec2(texelSize.x, -texelSize.y));
    float edgeH = abs(-2.0 * lumW + lumNW + lumSW) + abs(-2.0 * lumC + lumN + lumS) * 2.0 + abs(-2.0 * lumE + lumNE + lumSE);
    float edgeV = abs(-2.0 * lumN + lumNW + lumNE) + abs(-2.0 * lumC + lumW + lumE) * 2.0 + abs(-2.0 * lumS + lumSW + lumSE);
    bool isHorizontal = edgeH >= edgeV;
    float stepLength = isHorizontal ? texelSize.y : texelSize.x;
    float lum1 = isHorizontal ? lumS : lumW;
    float lum2 = isHorizontal ? lumN : lumE;
    if (abs(lum1 - lumC) >= abs(lum2 - lumC))
        stepLength = -stepLength;
    float subPixFactor = clamp(abs((lumN + lumS + lumE + lumW) * 0.25 - lumC) / lumRange, 0.0, 1.0);
    subPixFactor = smoothstep(0.0, 1.0, subPixFactor);
    subPixFactor = subPixFactor * subPixFactor * 0.75;
    vec2 blendUV = uv;
    if (isHorizontal)
        blendUV.y += stepLength * subPixFactor;
    else
        blendUV.x += stepLength * subPixFactor;
    return texture(hdrBuffer, blendUV).rgb;
}

// Cheap animated film grain
float filmNoise(vec2 uv, float time)
{
    vec2 p = uv * vec2(1280.0, 720.0) + vec2(time * 37.0, time * 19.0);
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

void main()
{
    float exposure = pc.p0.x;
    float bloomIntensity = pc.p0.y;
    float gamma = max(pc.p0.z, 0.001);
    int toneMapper = int(pc.p0.w + 0.5);
    bool bloomEnabled = pc.p1.x > 0.5;
    bool fxaaEnabled = pc.p1.y > 0.5;
    bool godRaysEnabled = pc.p1.z > 0.5;
    bool ssaoEnabled = pc.p2.w > 0.5;
    float ssaoIntensity = pc.p3.x;
    bool underwater = pc.p3.y > 0.5;
    float underwaterStrength = pc.p3.z;
    float time = pc.p3.w;
    float grainStrength = max(pc.p4.x, 0.0);
    float vignetteStrength = clamp(pc.p4.y, 0.0, 1.0);

    vec3 hdrColor = fxaaEnabled ? applyFXAA(vUV) : texture(hdrBuffer, vUV).rgb;

    if (ssaoEnabled)
    {
        // Cap intensity (matches lighting::clampSsaoIntensity) — avoids milky full-frame veil
        float ao = texture(ssaoBuffer, vUV).r;
        float intens = clamp(ssaoIntensity, 0.0, 0.85);
        ao = mix(1.0, ao, intens);
        // Never crush outdoor slopes / dark caves below a soft floor (lighting::kSsaoAoFloor)
        ao = max(ao, 0.62);
        hdrColor *= ao;
    }

    if (bloomEnabled)
        hdrColor += texture(bloomBuffer, vUV).rgb * bloomIntensity;
    if (godRaysEnabled)
        hdrColor += texture(godRaysBuffer, vUV).rgb * 0.85;

    vec3 mapped = max(hdrColor * exposure, vec3(0.0));
    mapped = toneMapper == 0 ? acesFilm(mapped) : reinhard(mapped);
    mapped = pow(mapped, vec3(1.0 / gamma));

    float postContrast = max(pc.p2.z, 0.01);
    float postSat = max(pc.p1.w, 0.0);
    if (abs(postContrast - 1.0) > 0.001)
        mapped = (mapped - vec3(0.5)) * postContrast + vec3(0.5);
    if (abs(postSat - 1.0) > 0.001) {
        float lum = dot(max(mapped, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722));
        mapped = mix(vec3(lum), mapped, postSat);
    }

    // Global vignette (subtle edge darkening)
    if (vignetteStrength > 0.001)
    {
        float d = length(vUV - vec2(0.5));
        float vig = smoothstep(0.92, 0.28, d);
        mapped *= mix(1.0, vig, vignetteStrength);
    }

    // Underwater look: teal grade, slight blur-like soft + extra vignette
    if (underwater)
    {
        float s = clamp(underwaterStrength, 0.0, 1.5);
        vec3 underTint = vec3(0.15, 0.45, 0.55);
        mapped = mix(mapped, mapped * underTint * 1.4, 0.55 * s);
        mapped.r *= mix(1.0, 0.65, s);
        float vig = smoothstep(0.95, 0.25, length(vUV - 0.5));
        mapped *= mix(1.0, vig, 0.35 * s);
        // Caustic shimmer
        float c = 0.5 + 0.5 * sin(vUV.x * 40.0 + time * 2.0) * sin(vUV.y * 35.0 - time * 1.5);
        mapped += vec3(0.0, 0.04, 0.05) * c * s;
    }

    // Film grain (after grade so it stays visible), weighted by luminance:
    // near-black areas (night sky) stay clean instead of turning into grey noise
    if (grainStrength > 0.0005)
    {
        float n = filmNoise(vUV, time);
        float glum = dot(mapped, vec3(0.299, 0.587, 0.114));
        mapped += (n - 0.5) * grainStrength * (0.20 + 0.80 * smoothstep(0.05, 0.35, glum));
    }

    outColor = vec4(clamp(mapped, 0.0, 1.0), 1.0);
}
