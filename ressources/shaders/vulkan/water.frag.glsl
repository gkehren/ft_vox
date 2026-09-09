#version 450

layout(location = 0) in vec3 vFragPos;
layout(location = 5) flat in vec3 vGeoNormal;
layout(location = 6) in float vSkyLight;
layout(location = 7) in vec3 vBlockLightRGB;

#include "frame_ubo.inc.glsl"
#include "sky_radiance.inc.glsl"
#include "csm.inc.glsl"
#include "water_optics.inc.glsl"
#include "atmosphere_fog.inc.glsl"

// Opaque scene history (color) + depth history (real depth, not color)
layout(set = 2, binding = 0) uniform sampler2D sceneColor;
layout(set = 2, binding = 1) uniform sampler2D sceneDepth;

// invScreenSize.xy — matches framebuffer / gl_FragCoord (no manual Y flip)
layout(push_constant) uniform PC {
    vec2 invScreen;
    vec2 pad;
} pc;

layout(location = 0) out vec4 outColor;

// ---------------------------------------------------------------------------
// Fragment-only wave field (world-anchored, mesh-independent).
//
// The vertex stage no longer displaces water or perturbs normals: greedy
// rectangles only share geometry along their edges, so vertex animation made
// different-sized rectangles deform as different waves with visible seams.
// Everything below is evaluated from the interpolated WORLD position, so two
// adjacent rectangles produce exactly the same surface state at the same
// world location. Sine derivatives are analytic; the noise detail field uses
// fixed-world-unit finite differences. Amplitudes fade when an octave's
// feature size approaches the pixel footprint derived from the projection,
// resolution and ray incidence (see main), which removes horizon shimmer
// without touching the mesh.
// ---------------------------------------------------------------------------

// Sum of the two smooth directional sines + analytic XZ gradient.
void swellField(vec2 p, float t, float amp, out float h, out vec2 grad)
{
    // Long gentle swell (~14 m wavelength)
    vec2 d1 = vec2(0.809, 0.588); // normalize(0.8, 0.58..)
    float f1 = 0.45;
    float p1 = dot(p, d1) * f1 + t * 0.55;
    // Cross swell (~5 m wavelength)
    vec2 d2 = vec2(-0.514, 0.858);
    float f2 = 1.25;
    float p2 = dot(p, d2) * f2 + t * -0.42;

    h = sin(p1) * 0.62 + sin(p2) * 0.38;
    grad = (d1 * (f1 * cos(p1) * 0.62) + d2 * (f2 * cos(p2) * 0.38)) * 2.4;
    h *= amp;
    grad *= amp;
}

// Feature fade: 0 when the octave's world feature size is smaller than the
// pixel footprint (its gradient would alias), 1 when well magnified. Only
// bounded amplitudes hang on this — the derivative epsilons stay fixed so
// the field itself never jumps between primitives.
float octaveFade(float featureSize, float foot)
{
    return clamp(featureSize / (foot * 2.0), 0.0, 1.0);
}

// Complete wave state at a world XZ position: height (for foam modulation)
// and the surface normal. waveStr 0 -> strictly geometric normal.
//
// Continuity contract: evaluated purely from the world position with FIXED
// (world-unit) finite-difference epsilons, so adjacent greedy rectangles and
// neighbouring triangles produce exactly the same normal at the same world
// location. The three drifting octaves reuse the shipped detail texture
// (0.30/0.85/2.10, weights 0.55/0.30/0.15) — a single octave reads as
// bilinear value-noise cells instead of water.
vec3 waveSurface(vec2 p, float t, float waveStr, float foot, out float h)
{
    // Long analytic swell (sines, exact derivatives)
    float swellH;
    vec2 swellG;
    swellField(p, t, waveStr * 0.9, swellH, swellG);
    h = swellH;

    // Detail field: the old three-octave heightfield, now fragment-side.
    const float e = 0.25; // world units; fixed for continuity
    const float freqs[3] = float[](0.30, 0.85, 2.10);
    const float amps[3] = float[](0.55, 0.30, 0.15);
    const vec2 drifts[3] = vec2[](vec2(t * 0.10, t * 0.06),
                                  vec2(-t * 0.13, t * 0.11),
                                  vec2(t * 0.20, -t * 0.17));
    float noiseAmp = waveStr * 1.6;
    vec2 grad = swellG;
    for (int i = 0; i < 3; ++i)
    {
        float fade = octaveFade(1.0 / freqs[i], foot);
        if (fade < 0.001 || noiseAmp < 0.0001) continue;
        vec2 q = p * freqs[i] + drifts[i];
        float hC = wnoise(q);
        float hX = wnoise(q + vec2(freqs[i] * e, 0.0));
        float hZ = wnoise(q + vec2(0.0, freqs[i] * e));
        // The offset in q-space (freq * e) already makes the finite
        // difference a derivative in p-space; do NOT multiply by freq again
        // or the octave gradient scales as freq² and the highest octave
        // dominates the swell.
        vec2 g = vec2(hX - hC, hZ - hC) / e * (amps[i] * noiseAmp * fade);
        grad += g;
        h += (hC - 0.5) * (amps[i] * noiseAmp * fade);
    }

    return normalize(vec3(-grad.x, 1.0, -grad.y));
}

// ---------------------------------------------------------------------------
// Depth reconstruction (camera space). Same RH_ZO linearization and
// negative-height-viewport UV convention as the camera-underwater path in
// composite.frag.glsl: uv comes straight from gl_FragCoord / screen size and
// the inverse projection applies the vertical mirror (see viewPosAt).
// ---------------------------------------------------------------------------

vec3 viewPosAt(vec2 uv, float linDepth)
{
    // Vulkan negative-height viewport: ndc.y = +1 lands on framebuffer row 0
    // and gl_FragCoord.y grows top-down, so the inverse projection needs the
    // vertical MIRROR (1 - 2*uv.y), not (uv*2-1). The sign matters here: the
    // reconstructed world-space floor height drives the shoreline foam band —
    // a mirrored reconstruction places the floor above the camera and paints
    // foam over the whole surface. composite.frag's underwaterViewPos uses
    // the same convention.
    return vec3((uv.x * 2.0 - 1.0) * linDepth / frame.projection[0][0],
                (1.0 - 2.0 * uv.y) * linDepth / frame.projection[1][1], -linDepth);
}

// Depth is fetched without filtering: interpolation across a silhouette invents geometry.
float opaqueViewDepth(vec2 uv)
{
    ivec2 size = textureSize(sceneDepth, 0);
    float d = texelFetch(sceneDepth, clamp(ivec2(uv * vec2(size)), ivec2(0), size - 1), 0).r;
    return frame.projection[3][2] / (d + frame.projection[2][2]);
}

float rawOpaqueDepth(vec2 uv)
{
    ivec2 size = textureSize(sceneDepth, 0);
    return texelFetch(sceneDepth, clamp(ivec2(uv * vec2(size)), ivec2(0), size - 1), 0).r;
}

bool isSkyDepth(float raw) { return raw >= 0.999; }

// Validate the entire bilinear color footprint, not only its nearest depth texel.
// All four texels must sit behind the water surface (no foreground recovery)
// and within tolerance of `expected` (no disocclusion jumps across a silhouette).
bool refractionFootprintSafe(vec2 uv, float surface, float expected, float tolerance)
{
    vec2 size = vec2(textureSize(sceneDepth, 0));
    vec2 base = floor(uv * size - 0.5) + 0.5;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x)
        {
            float d = opaqueViewDepth((base + vec2(x, y)) / size);
            if (d <= surface + 0.02 || abs(d - expected) >= tolerance) return false;
        }
    return true;
}

bool projectWaterRay(vec3 p, out vec2 uv)
{
    vec4 clip = frame.projection * vec4(p, 1.0);
    if (clip.w <= 0.0) return false;
    vec3 ndc = clip.xyz / clip.w;
    // Production uses a negative-height Vulkan viewport.
    uv = ndc.xy * vec2(0.5, -0.5) + 0.5;
    vec2 margin = pc.invScreen * 1.5;
    return ndc.z > 0.0 && ndc.z < 1.0 &&
           all(greaterThan(uv, margin)) && all(lessThan(uv, 1.0 - margin));
}

// Screen-space reflection with validated hit + roughness-scaled neighbor blur.
// `surfaceDepth` locates the shading point along the view ray; the origin is
// offset along the GEOMETRIC normal (stable gating surface) while the ray
// follows the wave normal (optical direction). Returns the reflected color
// and its 0..1 confidence so failures can fade to the sky fallback without
// black bands or hard switches.
vec3 sceneReflection(vec3 R, float surfaceDepth, float roughness, vec3 fallback, out float confidence)
{
    confidence = 0.0;
    int steps = int(frame.waterQuality.x);
    // A top surface cannot reflect the submerged floor. Below-horizon rays
    // retain the unmodified direction and use the horizon fallback.
    if (steps == 0 || R.y <= 0.0) return fallback;
    vec3 origin = (frame.view * vec4(vFragPos + vGeoNormal * 0.08, 1.0)).xyz;
    vec3 direction = mat3(frame.view) * R;
    float previousT = 0.0;
    float previousDelta = -1.0;
    float range = frame.waterQuality.y;
    float thickness = frame.waterQuality.z; // world/view-space metres, not device depth
    for (int i = 1; i <= 48; ++i)
    {
        if (i > steps) break;
        float f = float(i) / float(steps);
        float t = 0.12 + range * f * f;
        vec3 ray = origin + direction * t;
        vec2 uv;
        if (!projectWaterRay(ray, uv)) break;
        float raw = rawOpaqueDepth(uv);
        if (isSkyDepth(raw)) break; // sky has no depth hit: keep the analytic fallback
        float delta = -ray.z - opaqueViewDepth(uv);
        if (delta >= 0.0 && previousDelta < 0.0)
        {
            float lo = previousT, hi = t;
            for (int j = 0; j < 5; ++j)
            {
                float mid = (lo + hi) * 0.5;
                vec3 probe = origin + direction * mid;
                vec2 probeUV;
                if (!projectWaterRay(probe, probeUV)) return fallback;
                if (-probe.z > opaqueViewDepth(probeUV)) hi = mid; else lo = mid;
            }
            ray = origin + direction * hi;
            if (!projectWaterRay(ray, uv)) return fallback;
            if (isSkyDepth(rawOpaqueDepth(uv))) return fallback;
            float hitDepth = opaqueViewDepth(uv);
            delta = -ray.z - hitDepth;
            if (delta >= 0.0 && delta < thickness)
            {
                vec2 edge = min(uv, 1.0 - uv);
                float conf = smoothstep(0.0, 0.08, min(edge.x, edge.y));
                conf *= 1.0 - smoothstep(range * 0.5, range, hi);
                conf *= 1.0 - smoothstep(thickness * 0.5, thickness, delta);
                conf *= smoothstep(0.15, 0.6, hi);
                conf *= smoothstep(0.0, 0.1, R.y);
                // Silhouette guard: the bilinear neighborhood of the hit must
                // be background-continuous. A neighbor pulled in front of the
                // hit (object edge crossing the reflection) smears the
                // reflected content — drop most of its weight instead.
                float tolerance = max(0.5, (hitDepth - surfaceDepth) * 0.3);
                conf *= refractionFootprintSafe(uv, surfaceDepth, hitDepth, tolerance) ? 1.0 : 0.35;

                // Roughness-scaled 4-tap blur: only depth-validated neighbors
                // join the average, so the blur never leaks foreground content.
                vec3 result = texture(sceneColor, uv).rgb;
                float weight = 1.0;
                float radiusTexels = 1.0 + roughness * 22.0;
                vec2 radius = radiusTexels * pc.invScreen;
                vec2 dirs[4] = vec2[](vec2(0.707, 0.707), vec2(-0.707, 0.707),
                                      vec2(0.707, -0.707), vec2(-0.707, -0.707));
                for (int k = 0; k < 4; ++k)
                {
                    vec2 tuv = clamp(uv + dirs[k] * radius, vec2(0.0), vec2(1.0));
                    float tRaw = rawOpaqueDepth(tuv);
                    if (isSkyDepth(tRaw)) continue;
                    float tDepth = opaqueViewDepth(tuv);
                    if (tDepth <= surfaceDepth + 0.02 ||
                        abs(tDepth - hitDepth) >= tolerance)
                        continue;
                    result += texture(sceneColor, tuv).rgb;
                    weight += 1.0;
                }
                confidence = conf;
                return mix(fallback, result / weight, conf);
            }
        }
        previousT = t;
        previousDelta = delta;
    }
    return fallback;
}

// GGX specular lobe with Schlick Fresnel (F0 = water 0.02): ONE roughness-
// controlled lobe per celestial body replaces the old pair of pow() lobes,
// so the highlight width follows the new roughness setting and distance
// filtering instead of two fixed exponents.
float specLobe(vec3 N, vec3 V, vec3 L, float rough)
{
    vec3 h = V + L;
    float h2 = dot(h, h);
    if (h2 < 1e-8) return 0.0; // V ≈ -L: undefined half vector
    vec3 H = h * inversesqrt(h2);
    // Bound the GGX alpha, not the denominator: flooring pi*d² makes the
    // peak intensity NON-monotone at low roughness (the floor dominates the
    // glossy end of the slider and the highlight would first grow with
    // roughness). d >= a2 always, so with a2 >= 2.5e-6 the division stays
    // well inside fp32 range and D_peak = 1/(pi·a2) decays monotonically.
    float a = max(rough * rough, 1e-4);
    float a2 = a * a;
    float ndh = max(dot(N, H), 0.0);
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    float D = a2 / (3.14159265 * d * d);
    float f = 0.02 + 0.98 * pow(1.0 - max(dot(V, H), 0.0), 5.0);
    return D * f;
}

void main()
{
    float time = frame.skyParams.x;
    float waveStr = frame.waterParams.x;
    float refractionStr = frame.waterParams.y;
    float specularStr = frame.waterParams.z;
    float foamStr = frame.waterParams.w;
    float roughness = frame.waterSurfaceParams.x;
    float debugView = frame.waterSurfaceParams.y;
    float dayFactor = frame.skyParams.y;
    float sunsetFactor = frame.skyParams.z;
    float nightFactor = frame.skyParams.w;

    // Geometric face normal, flat from the vertex stage. Gating (top-face
    // classification, SSR origin, CSM receiver bias) must key on this, not on
    // the wave-perturbed shading normal: the shading normal oscillates with
    // wave strength and phase, which would spatially/temporally toggle SSR
    // and shadow reception on true horizontal faces.
    vec3 geoN = normalize(vGeoNormal);
    vec3 V = normalize(frame.viewPos.xyz - vFragPos);
    float dist = length(vFragPos - frame.viewPos.xyz);

    // Fragment-level wave normals on top faces only (sides stay voxel-flat).
    // The pixel-footprint fade removes the fine octaves before they can alias
    // and the far field flattens toward the geometric normal — continuity
    // across greedy rectangles and a quiet horizon instead of more SSR steps.
    float topMask = smoothstep(0.7, 0.95, geoN.y);
    // World-space footprint of one output pixel on the surface, from the
    // actual projection and resolution (NOT a hardcoded angular size, and NOT
    // dFdx of the world position: that one is constant per primitive and
    // quantizes the fade per greedy triangle — visible as flat-shaded tiles
    // from a submerged camera). Footprint ≈ pixel size at this depth divided
    // by the ray's incidence on the plane (grazing stretch).
    float surfaceDepth = -(frame.view * vec4(vFragPos, 1.0)).z;
    float pixelX = 2.0 * surfaceDepth * pc.invScreen.x / abs(frame.projection[0][0]);
    float pixelY = 2.0 * surfaceDepth * pc.invScreen.y / abs(frame.projection[1][1]);
    float foot = max(pixelX, pixelY) / max(abs(dot(V, geoN)), 0.06);
    // Underside: a top face whose observer is below it (submerged camera).
    // Refraction and SSR are above-water constructs — from below they sample
    // sky/floor silhouette boundaries in the history and paint reflected
    // patchwork over the surface. The underside renders the transmitted
    // boundary background (analytic sky / opaque history) without medium, and
    // the camera->surface transport belongs to the underwater composite.
    bool underside = dot(geoN, V) < 0.0;
    vec3 N = geoN;
    float waveH = 0.0;
    if (topMask > 0.001 && waveStr > 0.0001)
    {
        N = waveSurface(vFragPos.xz, time, waveStr, foot, waveH);
        float farFlatten = 0.85 * smoothstep(40.0, 200.0, dist);
        N = normalize(mix(N, geoN, farFlatten));
    }

    // Fresnel against a normal that faces the observer. UNDERSIDE MUST FLIP
    // UNCONDITIONALLY: the per-pixel dot test flips between +N and -N wherever
    // a small wave tilt overcomes the grazing view elevation, which shatters
    // the surface Fresnel into hard-edged regions (plainly visible from a
    // submerged camera looking along the surface).
    vec3 Nf = underside ? -N : (dot(N, V) < 0.0 ? -N : N);
    float F0 = 0.02;
    float fres = F0 + (1.0 - F0) * pow(1.0 - max(dot(Nf, V), 0.0), 5.0);

    // ---------------------------------------------------------------------------
    // Camera-space refraction + true optical path.
    // ---------------------------------------------------------------------------
    vec2 margin = pc.invScreen * 1.5;
    vec2 screenUV = clamp(gl_FragCoord.xy * pc.invScreen, margin, 1.0 - margin);
    vec3 surfaceView = viewPosAt(screenUV, surfaceDepth);

    // Distort in CAMERA space: the screen offset of a refracted ray follows
    // the view-space normal, not the world-space one (the old world-space
    // offset rotated wrongly with the camera).
    float linOpaque = opaqueViewDepth(screenUV);
    float columnGate = clamp(linOpaque - surfaceDepth, 0.0, 64.0);
    vec2 edge = min(screenUV, 1.0 - screenUV);
    float edgeFade = smoothstep(0.0, 0.04, min(edge.x, edge.y));
    vec3 Nview = mat3(frame.view) * N;
    vec2 distort = underside
                       ? vec2(0.0)
                       : Nview.xy * refractionStr * 2.0 * edgeFade * smoothstep(0.0, 0.8, columnGate);
    vec2 refrUV = screenUV;
    // Reject foreground and disocclusion jumps. A bounded backoff preserves shorelines.
    for (int i = 0; i < 4; ++i)
    {
        vec2 candidate = clamp(screenUV + distort, margin, 1.0 - margin);
        if (refractionFootprintSafe(candidate, surfaceDepth, linOpaque, max(0.5, columnGate * 0.25)))
        {
            refrUV = candidate;
            break;
        }
        distort *= 0.5;
    }

    // Optical length = distance between the water surface and the ACCEPTED
    // background sample (both reconstructed in camera space) — not a screen-
    // axis depth difference. Pixels with no geometry behind them (sky through
    // the history) get the shared sky column cap so the deep-water body stays
    // stable instead of dividing by garbage.
    bool bgSky = isSkyDepth(rawOpaqueDepth(refrUV));
    float column;
    float depthBelow;
    vec3 scene = texture(sceneColor, refrUV).rgb;
    if (underside)
    {
        // Seen from below, the boundary background must NOT come from the
        // opaque history: the sky renders AFTER this pass (and can never fill
        // these pixels anymore, since we now write depth), and the medium
        // between the camera and the boundary is the underwater composite's
        // job. Transmit the analytic sky without extra in-water attenuation —
        // otherwise the boundary gets a fake 28 m teal and the composite
        // counts the medium twice. Opaque history content along an up-ray
        // (shore walls above the waterline) is likewise pure boundary.
        column = 0.0;
        depthBelow = WATER_SKY_COLUMN; // no contact foam on the underside
        if (bgSky)
            // -V = camera -> surface -> outside. V points at the camera
            // (downward here); sampling the sky with V would read the
            // sub-horizon half of the gradient for every pixel.
            scene = analyticSkyRadiance(normalize(-V), dayFactor, sunsetFactor, nightFactor);
    }
    else if (bgSky)
    {
        column = WATER_SKY_COLUMN;
        depthBelow = WATER_SKY_COLUMN;
    }
    else
    {
        vec3 backgroundView = viewPosAt(refrUV, opaqueViewDepth(refrUV));
        column = clamp(length(backgroundView - surfaceView), 0.0, 64.0);
        // Vertical surface->floor separation (world space) drives the shoreline
        // foam band; the optical path above drives absorption only.
        vec3 backgroundWorld = frame.viewPos.xyz + transpose(mat3(frame.view)) * backgroundView;
        depthBelow = vFragPos.y - backgroundWorld.y;
    }
    float skyReach = smoothstep(0.05, 0.45, vSkyLight);
    float directVisibility = 1.0;
    if (topMask > 0.001 && frame.waterQuality.w > 0.5)
        directVisibility -= sampleDirectionalShadow(vFragPos, geoN,
                            normalize(frame.lightDirection.xyz), surfaceDepth);
    directVisibility *= skyReach;
    vec3 localLight = max(vBlockLightRGB, vec3(0.0));
    localLight *= max(localLight.r, max(localLight.g, localLight.b)) * frame.lightingParams.x;

    // Beer-Lambert absorption: red dies first -> teal body (shared constants)
    vec3 absorb = exp(-column * WATER_SIGMA);
    float scatterAmt = 1.0 - exp(-column * WATER_SCATTER_RATE);
    float scatterLight = waterScatterAmbient(dayFactor, sunsetFactor, directVisibility);
    scatterLight = mix(0.03, scatterLight, skyReach);
    vec3 waterBody = scene * absorb + WATER_SCATTER_COLOR * scatterAmt * (vec3(scatterLight) + localLight * 0.2);

    // Fresnel + analytic sky reflection
    vec3 R = reflect(-V, Nf);
    vec3 refl = analyticSkyRadiance(R, dayFactor, sunsetFactor, nightFactor);
    // SSR misses must not reveal a blue outdoor sky inside a sealed cave.
    // A dim local diffuse fallback is not a mirror of offscreen geometry.
    vec3 caveReflection = vec3(0.006, 0.008, 0.012) + localLight * 0.035;
    refl = mix(caveReflection, refl, skyReach);

    float ssrConfidence = 0.0;
    if (topMask > 0.95 && !underside && frame.lightingParams.w < 0.5)
        refl = sceneReflection(R, surfaceDepth, roughness, refl, ssrConfidence);
    vec3 color = mix(waterBody, refl, fres);

    // Sun glitter: one roughness-controlled GGX lobe (distance-filtered
    // roughness keeps the far-field highlight from breaking up per pixel).
    float roughEff = min(roughness + 0.30 * smoothstep(40.0, 260.0, dist), 0.6);
    float sunVis = smoothstep(-0.04, 0.08, frame.sunDir.y);
    float sunLow = 1.0 - smoothstep(0.0, 0.35, frame.sunDir.y);
    vec3 sunTint = mix(vec3(1.0, 0.96, 0.72), vec3(1.0, 0.45, 0.12), sunLow * sunLow);
    color += sunTint * specLobe(Nf, V, frame.sunDir.xyz, roughEff) * specularStr * 0.45 *
             sunVis * directVisibility;

    // Moon glitter (cool tint, night only) — same single-lobe model.
    float moonVis = smoothstep(0.02, 0.28, frame.moonDir.y);
    vec3 moonTint = vec3(0.55, 0.68, 1.0);
    color += moonTint * specLobe(Nf, V, frame.moonDir.xyz, roughEff) * specularStr * 0.28 *
             moonVis * nightFactor * directVisibility;

    // Foam: thin shoreline band from the reconstructed vertical separation
    // (contacts only), modulated lightly by the wave height. Gated on TOP
    // faces: a vertical face rising next to similar-height terrain
    // reconstructs depthBelow ≈ 0 and would otherwise foam. No whitecaps in
    // open water and no foam on the underside — the optical `column` is
    // reserved for absorption.
    float foam = 0.0;
    if (foamStr > 0.001 && topMask > 0.001 && !underside)
    {
        float foamNoise = wnoise(vFragPos.xz * 1.8 + vec2(time * 0.35, -time * 0.25))
                        * wnoise(vFragPos.xz * 3.7 - vec2(time * 0.22, time * 0.30)) * 2.0;
        float shoreBand = 1.0 - smoothstep(0.10, 0.90, depthBelow);
        float breath = 0.7 + 0.3 * clamp(waveH * 2.0 + 0.5, 0.0, 1.0);
        float shoreFoam = shoreBand * smoothstep(0.25, 0.70, foamNoise + shoreBand * 0.35) * breath;
        foam = clamp(shoreFoam * topMask * foamStr, 0.0, 1.0);
    }
    vec3 foamColor = vec3(0.88, 0.93, 0.96) * (0.22 + 0.78 * dayFactor);
    foamColor = mix(foamColor, vec3(1.0, 0.72, 0.50) * (0.25 + 0.75 * dayFactor), sunsetFactor * 0.45);
    foamColor *= 1.0 - nightFactor * 0.75;
    foamColor = mix(vec3(0.025) + localLight * 0.18, foamColor, skyReach);
    color = mix(color, foamColor, foam * 0.85);

    // Camera-to-surface AIR aerial perspective (shared contract, issue #159),
    // applied to the fully composed surface (refraction + column transport +
    // reflection + foam) so far water hazes toward the same horizon atmosphere
    // as the shoreline at the same world position. The Beer-Lambert column
    // above is the WATER medium and is untouched by this air term. The shared
    // contract self-gates to zero when the camera is submerged
    // (frame.lightingParams.w); the underside test is then a pure early-out —
    // underside transport belongs to the camera-underwater composite either
    // way.
    if (!underside)
    {
        AtmosphereFog atmo = evaluateAtmosphereFog(vFragPos, skyReach);
        color = applyAtmosphereFog(color, atmo);
    }

    // Diagnostic views (Graphics > Water): raw surface terms rendered
    // through the normal pass pipeline.
    if (debugView > 0.5)
    {
        vec3 dbg;
        if (debugView < 1.5)
            dbg = N * 0.5 + 0.5;                          // wave normal (world)
        else if (debugView < 2.5)
            dbg = vec3(clamp(columnGate / 64.0, 0.0, 1.0),   // old Z-axis column
                       clamp(column / 64.0, 0.0, 1.0),       // new 3D optical path
                       clamp(depthBelow / 8.0, 0.0, 1.0));   // vertical separation
        else if (debugView < 3.5)
            dbg = vec3(fres);                             // Fresnel
        else
            dbg = mix(vec3(1.0, 0.1, 0.1), vec3(0.1, 1.0, 0.1), ssrConfidence); // SSR confidence
        outColor = vec4(dbg, 1.0);
        return;
    }

    // Single composition: the color above already contains the refracted
    // background, absorption, reflection and foam, so the pass writes opaque
    // (blending disabled in WaterPass). Depth write keeps the nearest visible
    // water surface and lets sky/SSAO/underwater paths see the surface.
    outColor = vec4(color, 1.0);
}
