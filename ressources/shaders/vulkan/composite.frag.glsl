#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

#include "colorspace.inc.glsl"
#include "frame_ubo.inc.glsl"
#include "water_optics.inc.glsl"

// Set 0 = FrameUBO (view/projection, camera position, sun/moon, day cycle),
// bound for the camera-underwater medium transport (issue #144).
// Set 1 = composite sources; binding 5 is the live scene depth used by that
// transport, binding 6 the auto-exposure history (issue #140).
layout(set = 1, binding = 0) uniform sampler2D hdrBuffer;
layout(set = 1, binding = 1) uniform sampler2D bloomBuffer;
layout(set = 1, binding = 2) uniform sampler2D godRaysBuffer;
layout(set = 1, binding = 3) uniform sampler2D ssaoBuffer;     // FINAL upsampled AO (full-res R8, linear)
layout(set = 1, binding = 4) uniform sampler2D ssaoRawBuffer;  // RAW half-res SSAO RGBA (linear: r = raw AO, gb = encoded normal)
layout(set = 1, binding = 5) uniform sampler2D sceneDepthBuffer; // full-res D32 (nearest)

// Auto-exposure state (issue #140), written by exposure_adapt.frag into the
// single shared history buffer. Must match autoexposure::ExposureGpuState.
layout(set = 1, binding = 6) readonly buffer ExposureState
{
    float adaptedExposure; // linear exposure multiplier
    float targetExposure;  // clamped target exposure this frame (debug)
    float meteredLogLum;   // log2 of clipped geometric-mean luminance (debug)
    uint clampState;       // 0 in range, 1 min clamp, 2 max clamp (debug)
} uExposure;

layout(push_constant) uniform PC {
    vec4 p0; // x=manualExposure, y=bloomIntensity, z=gamma, w=toneMapper
    vec4 p1; // x=bloomOn, y=unused (FXAA moved to its own pass, issue #143), z=godRaysOn, w=postSaturation
    vec4 p2; // xy=texelSize, z=postContrast, w=ssaoOn
    vec4 p3; // x=ssaoIntensity, y=underwater, z=underwaterStrength, w=time
    vec4 p4; // x=filmGrain, y=vignette, z=encodeSrgb, w=ssaoDebugView (0=Off 1=FinalAO 2=RawAO 3=Normals)
    vec4 p5; // x=useAutoExposure, yzw unused (issue #140)
    vec4 p6; // x=waterSurfaceY (1e9 = unknown), y=causticTier (0=off 1=simple 2=normal-gated 3=finer), zw unused (issue #144)
} pc;

// Tone mapping exposure: the GPU-adapted auto value, or the exact manual
// setting when auto exposure is disabled (deterministic path).
float resolveExposure()
{
    return pc.p5.x > 0.5 ? uExposure.adaptedExposure : pc.p0.x;
}

vec3 acesFilm(vec3 x)
{
    float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 reinhard(vec3 x)
{
    return x / (x + vec3(1.0));
}

// Cheap animated film grain
float filmNoise(vec2 uv, float time)
{
    vec2 p = uv * vec2(1280.0, 720.0) + vec2(time * 37.0, time * 19.0);
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

// View-space position from screen uv + raw depth. Same RH_ZO linearization
// as the water pass. The negative-height production viewport maps ndc.y = +1
// to framebuffer row 0, so the inverse projection needs the vertical mirror
// (1 - 2*uv.y) — the earlier (uv*2-1) form reconstructed a vertically
// mirrored world, which stayed invisible while the transport only consumed
// path lengths, but breaks any consumer that needs the true direction or
// height (water rework: detecting that a depth hit IS the water surface).
vec3 underwaterViewPos(vec2 uv, float depth)
{
    float t = frame.projection[3][2] / (depth + frame.projection[2][2]);
    return vec3((uv.x * 2.0 - 1.0) * t / frame.projection[0][0],
                (1.0 - 2.0 * uv.y) * t / frame.projection[1][1], -t);
}

void main()
{
    float exposure = resolveExposure();
    float bloomIntensity = pc.p0.y;
    float gamma = max(pc.p0.z, 0.001);
    int toneMapper = int(pc.p0.w + 0.5);
    bool bloomEnabled = pc.p1.x > 0.5;
    bool godRaysEnabled = pc.p1.z > 0.5;
    bool ssaoEnabled = pc.p2.w > 0.5;
    float ssaoIntensity = pc.p3.x;
    bool underwater = pc.p3.y > 0.5;
    float underwaterStrength = pc.p3.z;
    float time = pc.p3.w;
    float grainStrength = max(pc.p4.x, 0.0);
    float vignetteStrength = clamp(pc.p4.y, 0.0, 1.0);

    // SSAO debug views (Graphics panel): bypass tonemap/grade entirely —
    // intended for diagnostics only. The swapchain output-transfer contract
    // (linearToSrgb on UNORM + SRGB_NONLINEAR, pc.p4.z) still applies so
    // debug views read consistently with the rest of the composite.
    float dbg = pc.p4.w;
    bool debugView = false;
    vec3 debugOut = vec3(0.0);
    if (dbg > 0.5 && dbg < 1.5)
    {
        debugOut = vec3(texture(ssaoBuffer, vUV).r); // final upsampled AO
        debugView = true;
    }
    else if (dbg > 1.5 && dbg < 2.5)
    {
        debugOut = vec3(texture(ssaoRawBuffer, vUV).r); // raw half-res AO
        debugView = true;
    }
    else if (dbg > 2.5)
    {
        // Encoded view-space normal from the raw half-res target:
        // gb = xy, a = z (the z sign is stored, not reconstructed).
        vec4 nb = texture(ssaoRawBuffer, vUV);
        debugOut = clamp(nb.gba, 0.0, 1.0);
        debugView = true;
    }
    if (debugView)
    {
        if (pc.p4.z > 0.5)
            debugOut = linearToSrgb(debugOut);
        outColor = vec4(clamp(debugOut, 0.0, 1.0), 1.0);
        return;
    }

    // Anti-aliasing note (issue #143): the former in-composite FXAA
    // approximation (which sampled linear HDR pre-tonemap) is gone. When the
    // spatial AA toggle is on, this pass writes tone-mapped, sRGB-encoded LDR
    // into the full-res ldrColor target and fxaa.frag.glsl (FXAA 3.11) renders
    // the swapchain from it; with AA off this pass targets the swapchain
    // directly. The C++ side picks the pipeline/target and sets p4.z.
    vec3 hdrColor = texture(hdrBuffer, vUV).rgb;

    if (ssaoEnabled)
    {
        // Cap intensity (matches lighting::clampSsaoIntensity)
        float ao = texture(ssaoBuffer, vUV).r;
        float intens = clamp(ssaoIntensity, 0.0, 0.85);
        ao = mix(1.0, ao, intens);
        // Safety-only clamp — the horizon-based estimator no longer needs a
        // high global floor. Must match lighting::kSsaoAoFloor (Lighting.hpp).
        ao = max(ao, 0.10);
        hdrColor *= ao;
    }

    if (bloomEnabled)
        hdrColor += texture(bloomBuffer, vUV).rgb * bloomIntensity;
    if (godRaysEnabled)
        hdrColor += texture(godRaysBuffer, vUV).rgb * 0.85;

    // Camera-underwater medium transport (issue #144): depth-aware
    // Beer-Lambert extinction + in-scatter + world-anchored caustics,
    // applied to linear HDR before exposure so tonemapping stays consistent.
    // Responsibilities stay separated from auto exposure (issue #140): the
    // metering pass reads the raw HDR buffer before this block, so the
    // medium never feeds back into its own exposure.
    if (underwater)
    {
        // Submersion blends the medium in over roughly the first half metre
        // below the local surface; 1e9 sentinel = unknown surface (debug
        // toggles) -> fully submerged.
        float surfaceY = pc.p6.x;
        float submersion = surfaceY > 1e8 ? 1.0
                                          : clamp((surfaceY - frame.viewPos.y) * 2.0, 0.0, 1.0);
        float s = clamp(underwaterStrength, 0.0, 1.0) * submersion;
        if (s > 0.001)
        {
            ivec2 depthSize = ivec2(vec2(textureSize(sceneDepthBuffer, 0)));
            ivec2 texel = clamp(ivec2(vUV * vec2(depthSize)), ivec2(0), depthSize - 1);
            float depth = texelFetch(sceneDepthBuffer, texel, 0).r;
            bool skyPixel = depth >= 0.999;
            vec3 viewPos = underwaterViewPos(vUV, depth);
            float sceneLength = max(length(viewPos), 1e-4);
            // sceneDistance locates the reconstructed geometry; waterDistance
            // is the optical path through the medium. An upward ray leaves the
            // water at the local surface plane, so its extinction path ends
            // there — true for geometry hits and for sky seen through the
            // surface (the 28 m sky cap only bounds the reconstructed point).
            float sceneDistance = skyPixel ? WATER_SKY_COLUMN : sceneLength;
            float waterDistance = sceneDistance;
            vec3 worldDir = transpose(mat3(frame.view)) * (viewPos / sceneLength);
            vec3 worldPos = frame.viewPos.xyz + worldDir * sceneDistance;
            // The water pass writes its surface into the depth buffer, so an
            // upward ray's first depth hit is the water boundary itself: the
            // extinction path ends there and such pixels are the surface
            // (sky seen through the boundary), not floor to caustic-light.
            // Detection keys on the reconstructed point sitting on the local
            // surface plane (robust to surface-scan block/plane convention).
            bool hitSurface = surfaceY < 1e8 && worldPos.y > surfaceY - 0.5;
            if (surfaceY < 1e8 && worldDir.y > 1e-4)
            {
                float surfaceDistance = (surfaceY - frame.viewPos.y) / worldDir.y;
                if (surfaceDistance > 0.0)
                    waterDistance = min(waterDistance, surfaceDistance);
            }

            float column = clamp(waterDistance, 0.0, 64.0);
            vec3 transmittance = exp(-column * WATER_SIGMA);
            float scatterAmt = 1.0 - exp(-column * WATER_SCATTER_RATE);
            float ambient = waterScatterAmbient(frame.skyParams.y, frame.skyParams.z, 1.0);
            vec3 underwaterColor = hdrColor * transmittance
                                 + WATER_SCATTER_COLOR * scatterAmt * ambient;

            // Caustics: procedural from reconstructed world XZ, gated by sun
            // elevation, depth below the local surface, an upward-facing
            // factor (depth-gradient normal on High+) and the final AO term —
            // occluded/dark cave floors must not brighten as if sunlit.
            float causticTier = pc.p6.y;
            // hitSurface excludes the water boundary itself (its depth is now
            // in the buffer): caustics light the FLOOR, never the underside
            // of the surface seen from a submerged camera.
            if (causticTier > 0.5 && !skyPixel && !hitSurface)
            {
                float sunUp = smoothstep(0.02, 0.18, frame.sunDir.y) * frame.skyParams.y;
                float effSurfaceY = surfaceY > 1e8 ? frame.viewPos.y + 1.5 : surfaceY;
                float depthFade = exp(-max(effSurfaceY - worldPos.y, 0.0) * 0.08);
                if (sunUp > 0.001 && depthFade > 0.004)
                {
                    float upFactor = 1.0;
                    if (causticTier > 1.5)
                    {
                        // Unfiltered neighbor depths: interpolation across a
                        // silhouette would fabricate normals (water-pass policy).
                        // Cross order: with the corrected reconstruction,
                        // screen +v runs DOWNWARD in view space, so
                        // (pU - pC) x (pR - pC) is the camera-facing normal —
                        // a flat floor reconstructs world +Y.
                        vec3 pR = underwaterViewPos(vUV + vec2(pc.p2.x, 0.0),
                            texelFetch(sceneDepthBuffer, clamp(texel + ivec2(1, 0), ivec2(0), depthSize - 1), 0).r);
                        vec3 pU = underwaterViewPos(vUV + vec2(0.0, pc.p2.y),
                            texelFetch(sceneDepthBuffer, clamp(texel + ivec2(0, 1), ivec2(0), depthSize - 1), 0).r);
                        vec3 nView = normalize(cross(pU - viewPos, pR - viewPos));
                        upFactor = clamp((transpose(mat3(frame.view)) * nView).y, 0.0, 1.0);
                    }
                    float occlusion = clamp(texture(ssaoBuffer, vUV).r, 0.0, 1.0);
                    float pattern = causticPattern(worldPos.xz, time);
                    if (causticTier > 2.5)
                        pattern = mix(pattern, causticPattern(worldPos.xz * 1.7 + vec2(time * 0.05, -time * 0.04), time), 0.35);
                    float caustic = pattern * sunUp * upFactor * depthFade * mix(0.35, 1.0, occlusion);
                    underwaterColor += vec3(0.34, 0.36, 0.32) * caustic * 1.3;
                }
            }
            hdrColor = mix(hdrColor, underwaterColor, s);
        }
    }

    vec3 mapped = max(hdrColor * exposure, vec3(0.0));
    mapped = toneMapper == 0 ? acesFilm(mapped) : reinhard(mapped);

    // Creative midtone gamma grading (1.0 = neutral display-linear)
    if (abs(gamma - 1.0) > 0.001)
        mapped = pow(mapped, vec3(1.0 / gamma));

    float postContrast = max(pc.p2.z, 0.01);
    float postSat = max(pc.p1.w, 0.0);
    if (abs(postContrast - 1.0) > 0.001)
        mapped = gradeContrast(mapped, postContrast);
    if (abs(postSat - 1.0) > 0.001) {
        mapped = gradeSaturation(mapped, postSat);
    }

    // Global vignette (subtle edge darkening)
    if (vignetteStrength > 0.001)
    {
        float d = length(vUV - vec2(0.5));
        float vig = 1.0 - smoothstep(0.28, 0.92, d);
        mapped *= mix(1.0, vig, vignetteStrength);
    }

    // Film grain (after grade so it stays visible) — multiplicative, so the
    // absolute noise scales down with value and near-black night sky stays clean
    if (grainStrength > 0.0005)
    {
        float n = filmNoise(vUV, time);
        mapped *= 1.0 + (n - 0.5) * grainStrength;
    }

    mapped = clamp(mapped, 0.0, 1.0);

    // Format-dependent output transfer (decided CPU-side from the
    // {VkFormat, VkColorSpaceKHR} pair — see colorspace::classifyOutputTransfer;
    // issue #143 adds a second encode case: when the spatial AA pass is on,
    // this pass always targets the UNORM ldrColor AA-source intermediate, whose
    // stored values must be perceptual (encoded) for FXAA):
    // If the output image is sRGB and AA is off (pc.p4.z <= 0.5), we output
    // display-linear values; the sRGB framebuffer write performs hardware
    // linear->sRGB conversion.
    // If the output is UNORM + SRGB_NONLINEAR without AA, or the AA source
    // intermediate with AA on (pc.p4.z > 0.5), encode explicitly.
    if (pc.p4.z > 0.5)
        mapped = linearToSrgb(mapped);

    outColor = vec4(clamp(mapped, 0.0, 1.0), 1.0);
}
