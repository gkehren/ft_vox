#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D hdrBuffer;
layout(set = 0, binding = 1) uniform sampler2D bloomBuffer;
layout(set = 0, binding = 2) uniform sampler2D godRaysBuffer;

// std430-friendly push constants (matches C++ CompPC exactly)
layout(push_constant) uniform PC {
    vec4 p0; // x=exposure, y=bloomIntensity, z=gamma, w=toneMapper
    vec4 p1; // x=bloomOn, y=fxaaOn, z=godRaysOn, w=postSaturation
    vec4 p2; // xy=texelSize, z=postContrast
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

void main()
{
    float exposure = pc.p0.x;
    float bloomIntensity = pc.p0.y;
    float gamma = max(pc.p0.z, 0.001);
    int toneMapper = int(pc.p0.w + 0.5);
    bool bloomEnabled = pc.p1.x > 0.5;
    bool fxaaEnabled = pc.p1.y > 0.5;
    bool godRaysEnabled = pc.p1.z > 0.5;

    vec3 hdrColor = fxaaEnabled ? applyFXAA(vUV) : texture(hdrBuffer, vUV).rgb;
    if (bloomEnabled)
        hdrColor += texture(bloomBuffer, vUV).rgb * bloomIntensity;
    if (godRaysEnabled)
        hdrColor += texture(godRaysBuffer, vUV).rgb * 0.85;

    vec3 mapped = max(hdrColor * exposure, vec3(0.0));
    mapped = toneMapper == 0 ? acesFilm(mapped) : reinhard(mapped);
    mapped = pow(mapped, vec3(1.0 / gamma));

    // Optional mild post contrast / saturation (defaults near 1.0 = neutral)
    float postContrast = max(pc.p2.z, 0.01);
    float postSat = max(pc.p1.w, 0.0);
    if (abs(postContrast - 1.0) > 0.001)
        mapped = (mapped - vec3(0.5)) * postContrast + vec3(0.5);
    if (abs(postSat - 1.0) > 0.001) {
        float lum = dot(max(mapped, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722));
        mapped = mix(vec3(lum), mapped, postSat);
    }

    outColor = vec4(clamp(mapped, 0.0, 1.0), 1.0);
}
