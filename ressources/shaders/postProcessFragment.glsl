#version 460 core
out vec4 FragColor;

in vec2 TexCoord;

uniform sampler2D hdrBuffer;
uniform sampler2D bloomBuffer;
uniform sampler2D godRaysBuffer;

uniform float exposure    = 1.0;
uniform float bloomIntensity = 0.3;
uniform float gamma       = 2.2;
uniform bool  bloomEnabled = true;
uniform bool  fxaaEnabled  = true;
uniform bool  godRaysEnabled = true;
uniform int   toneMapper   = 0; // 0 = ACES, 1 = Reinhard

// ---------- Tone mapping operators ----------

vec3 acesFilm(vec3 x)
{
    // ACES filmic tone mapping (Narkowicz 2015)
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 reinhard(vec3 x)
{
    return x / (x + vec3(1.0));
}

// ---------- FXAA (quality preset 12) ----------

vec3 applyFXAA(sampler2D tex, vec2 uv)
{
    vec2 texelSize = 1.0 / textureSize(tex, 0);

    // Sample luminances around center
    float lumC  = dot(texture(tex, uv).rgb, vec3(0.299, 0.587, 0.114));

    float lumN  = dot(texture(tex, uv + vec2( 0.0,  texelSize.y)).rgb, vec3(0.299, 0.587, 0.114));
    float lumS  = dot(texture(tex, uv + vec2( 0.0, -texelSize.y)).rgb, vec3(0.299, 0.587, 0.114));
    float lumE  = dot(texture(tex, uv + vec2( texelSize.x,  0.0)).rgb, vec3(0.299, 0.587, 0.114));
    float lumW  = dot(texture(tex, uv + vec2(-texelSize.x,  0.0)).rgb, vec3(0.299, 0.587, 0.114));

    float lumMin = min(lumC, min(min(lumN, lumS), min(lumE, lumW)));
    float lumMax = max(lumC, max(max(lumN, lumS), max(lumE, lumW)));
    float lumRange = lumMax - lumMin;

    // Early out if contrast is very low
    if (lumRange < max(0.0312, lumMax * 0.125))
        return texture(tex, uv).rgb;

    float lumNW = dot(texture(tex, uv + vec2(-texelSize.x,  texelSize.y)).rgb, vec3(0.299, 0.587, 0.114));
    float lumNE = dot(texture(tex, uv + vec2( texelSize.x,  texelSize.y)).rgb, vec3(0.299, 0.587, 0.114));
    float lumSW = dot(texture(tex, uv + vec2(-texelSize.x, -texelSize.y)).rgb, vec3(0.299, 0.587, 0.114));
    float lumSE = dot(texture(tex, uv + vec2( texelSize.x, -texelSize.y)).rgb, vec3(0.299, 0.587, 0.114));

    // Compute edge direction
    float edgeH = abs(-2.0 * lumW + lumNW + lumSW) +
                  abs(-2.0 * lumC + lumN  + lumS ) * 2.0 +
                  abs(-2.0 * lumE + lumNE + lumSE);
    float edgeV = abs(-2.0 * lumN + lumNW + lumNE) +
                  abs(-2.0 * lumC + lumW  + lumE ) * 2.0 +
                  abs(-2.0 * lumS + lumSW + lumSE);

    bool isHorizontal = edgeH >= edgeV;

    // Choose sampling direction
    float stepLength = isHorizontal ? texelSize.y : texelSize.x;
    float lum1 = isHorizontal ? lumS : lumW;
    float lum2 = isHorizontal ? lumN : lumE;
    float gradient1 = abs(lum1 - lumC);
    float gradient2 = abs(lum2 - lumC);

    if (gradient1 < gradient2)
        stepLength = -stepLength;

    // Sub-pixel blending
    float subPixFactor = clamp(abs((lumN + lumS + lumE + lumW) * 0.25 - lumC) / lumRange, 0.0, 1.0);
    subPixFactor = smoothstep(0.0, 1.0, subPixFactor);
    subPixFactor = subPixFactor * subPixFactor * 0.75;

    vec2 blendUV = uv;
    if (isHorizontal)
        blendUV.y += stepLength * subPixFactor;
    else
        blendUV.x += stepLength * subPixFactor;

    return texture(tex, blendUV).rgb;
}

// ---------- Main ----------

void main()
{
    vec3 hdrColor;

    // FXAA on the HDR buffer — all neighbor samples come from the
    // same source for consistent luminance contrast detection
    if (fxaaEnabled)
        hdrColor = applyFXAA(hdrBuffer, TexCoord);
    else
        hdrColor = texture(hdrBuffer, TexCoord).rgb;

    // Add bloom
    if (bloomEnabled)
    {
        vec3 bloom = texture(bloomBuffer, TexCoord).rgb;
        hdrColor += bloom * bloomIntensity;
    }

    // Add god rays
    if (godRaysEnabled)
    {
        vec3 godRays = texture(godRaysBuffer, TexCoord).rgb;
        hdrColor += godRays;
    }

    // Exposure
    vec3 mapped = hdrColor * exposure;

    // Tone mapping
    if (toneMapper == 0)
        mapped = acesFilm(mapped);
    else
        mapped = reinhard(mapped);

    // Gamma correction
    mapped = pow(mapped, vec3(1.0 / gamma));

    FragColor = vec4(mapped, 1.0);
}
