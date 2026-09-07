#version 450
// Horizon-based screen-space ambient occlusion (GTAO-inspired slice
// horizons), rendered at half resolution from the full-res scene depth.
//
// Per pixel: reconstruct the view-space position P and a camera-facing
// normal from depth (edge-aware central differences). For each screen-space
// slice direction build the exact in-slice frame:
//
//   zeta = normalize(cross(dq, V))   // slice-plane normal (perpendicular
//                                    // to the slice — NOT an in-plane axis)
//   xi   = normalize(cross(V, zeta)) // in-plane horizontal axis
//   eta  = -V                        // in-plane elevation axis (toward camera)
//
// where dq is the constant-depth lateral step along the slice and every
// sample vector v = SP - P lies inside the slice plane span{dq, V}. Angles
// are measured from the equator (the plane through P perpendicular to V):
// d(theta) = cos(theta)*xi + sin(theta)*eta. The Lambert factor of the
// surface for an in-slice direction is dot(N, d) = m * cos(theta - thetaN),
// where m = |(dot(N, xi), dot(N, eta))| is the length of N projected into
// the slice and thetaN its angle; a slice with m ~ 0 (N parallel to the
// slice normal) carries no weight and is skipped. The occluded arc runs
// from the tangent plane (thetaN - pi/2) to the sample horizon h and its
// exact integral is m * (1 + sin(h - thetaN)), evaluated per sample in
// sin/cos space and accumulated with the distance falloff applied BEFORE
// the max so adding a farther occluder can never reduce a slice's
// occlusion (monotone horizon). Samples outside the framebuffer count as
// absent geometry. Slices are rotated per pixel with deterministic
// interleaved-gradient-noise jitter — NO time term — so frames stay
// reproducible for the visual-regression harness.
//
// Occluder acceptance: any non-sky sample whose in-slice horizontal
// component is forward counts (the signed horizon angle versus the tangent
// plane decides occlusion; the canonical "object on the ground in front of
// P" and "crease beside P" cases both register). Self-occlusion of P's own
// surface is suppressed by the tangent-plane clamp in the arc, not by a
// depth test.
//
// Metric parameter is VIEW-SPACE METERS:
//   radius (pc.p0.z) = occluder search radius around P; the UV step is
//                      derived per direction from the per-axis FOV scales
//                      (invProj[0][0] horizontal, invProj[1][1] vertical)
//                      so the radius is isotropic in meters
//
// Output (half-res RGBA8 UNORM target):
//   r  = raw AO in [0, 1] (1 = unoccluded). No internal floor, no damping
//        and no intensity application — composite.frag owns intensity and
//        the safety floor (lighting::kSsaoAoFloor).
//   gb = view-space normal.xy encoded as *0.5 + 0.5
//   a  = view-space normal.z encoded as *0.5 + 0.5 (camera-facing normals
//        can still have negative z near frustum edges — the channel is
//        authoritative, do not reconstruct the sign)

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D depthBuffer; // full-res scene depth (nearest)

layout(push_constant) uniform PC {
    vec4 p0; // xy = inv full-res resolution, z = radius (view-space METERS), w = unused
    vec4 p1; // x = directions (float, 4..8), y = steps (float, 1..4), z,w unused
    mat4 invProj;
} pc;

const float kSkyDepth = 0.9990;

vec3 viewPosFromDepth(vec2 uv, float depth)
{
    vec4 clip = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 view = pc.invProj * clip;
    return view.xyz / max(view.w, 1e-5);
}

bool isSky(float depth)
{
    return depth >= kSkyDepth;
}

// Depth sample that reports "sky" outside the framebuffer. The sampler
// clamps to the edge texel, which would combine an edge depth with a
// reconstruction position outside the frustum and fabricate horizons near
// the screen borders — treat out-of-bounds taps as absent geometry instead.
float depthAt(vec2 uv)
{
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
        return 1.0;
    return texture(depthBuffer, uv).r;
}

// Unoccluded output with a neutral (view-forward) encoded normal: N=(0,0,1).
void outputUnoccluded()
{
    outColor = vec4(1.0, 0.5, 0.5, 1.0);
}

void main()
{
    const vec2 t = pc.p0.xy; // full-res texel size in UV

    float depth = texture(depthBuffer, vUV).r;
    // Sky / far plane: never occluded, never an occluder.
    if (isSky(depth))
    {
        outputUnoccluded();
        return;
    }

    vec3 P = viewPosFromDepth(vUV, depth);
    // Reject invalid reconstructions (behind camera / explosion)
    if (P.z > -0.05 || any(isnan(P)))
    {
        outputUnoccluded();
        return;
    }

    // --- Normal reconstruction: edge-aware central differences at full-res
    // texel spacing. Each side's validity is tracked explicitly — an invalid
    // (sky / off-screen) neighbor must NOT compete in the smaller-delta pick
    // (substituting it with P would win with a fake zero delta). Selection
    // policy mirrored by lighting::ssaoPickAxisDelta (unit-tested).
    float dRd = depthAt(vUV + vec2(t.x, 0.0));
    float dLd = depthAt(vUV - vec2(t.x, 0.0));
    float dUd = depthAt(vUV + vec2(0.0, t.y));
    float dDd = depthAt(vUV - vec2(0.0, t.y));
    const bool validR = !isSky(dRd);
    const bool validL = !isSky(dLd);
    const bool validU = !isSky(dUd);
    const bool validD = !isSky(dDd);
    const vec3 vpR = validR ? viewPosFromDepth(vUV + vec2(t.x, 0.0), dRd) : P;
    const vec3 vpL = validL ? viewPosFromDepth(vUV - vec2(t.x, 0.0), dLd) : P;
    const vec3 vpU = validU ? viewPosFromDepth(vUV + vec2(0.0, t.y), dUd) : P;
    const vec3 vpD = validD ? viewPosFromDepth(vUV - vec2(0.0, t.y), dDd) : P;

    // Per axis: when both neighbors are valid, take the side with the
    // smaller view-z delta; a single valid side wins; with none, the axis
    // delta stays zero — the cross product then degenerates into the safe
    // view-forward fallback below.
    vec3 dpx = vec3(0.0);
    if (validR && validL)
        dpx = abs(vpR.z - P.z) <= abs(vpL.z - P.z) ? vpR - P : P - vpL;
    else if (validR)
        dpx = vpR - P;
    else if (validL)
        dpx = P - vpL;
    vec3 dpy = vec3(0.0);
    if (validU && validD)
        dpy = abs(vpU.z - P.z) <= abs(vpD.z - P.z) ? vpU - P : P - vpD;
    else if (validU)
        dpy = vpU - P;
    else if (validD)
        dpy = P - vpD;

    vec3 N = cross(dpx, dpy);
    float nLen = length(N);
    if (nLen < 1e-6)
        N = vec3(0.0, 0.0, 1.0);
    else
        N /= nLen;
    // Orient camera-facing. P points away from the camera by construction, so
    // front-face normals must satisfy dot(N, P) < 0. This check is
    // convention-independent — do NOT replace it with UV/y-flip reasoning.
    if (dot(N, normalize(P)) > 0.0)
        N = -N;

    // --- Isotropic view-space radius -> UV step. GLM perspective:
    // invProj[0][0] = aspect * tan(fovY/2) (horizontal scale),
    // invProj[1][1] = tan(fovY/2) (vertical). A UV step o along direction
    // a2 spans o * |z| * 2 * length(a2 * fovScale) view-meters (per-axis
    // meters-per-UV are |z|*2*fovScale), so invert per direction to reach
    // exactly `radius` meters on every slice.
    const vec2 fovScale = vec2(pc.invProj[0][0], pc.invProj[1][1]);
    const float viewDist = max(-P.z, 1e-4);

    // Deterministic per-pixel rotation jitter (interleaved gradient noise).
    float ign = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));

    const int dirs = clamp(int(pc.p1.x), 1, 8);
    const int steps = clamp(int(pc.p1.y), 1, 4);

    vec3 V = normalize(P); // camera -> P
    float sum = 0.0;

    for (int d = 0; d < dirs; ++d)
    {
        // Slices are symmetric, so a half-turn (pi) span covers all angles.
        float phi = 3.14159265 * ((float(d) + ign) / float(dirs));
        vec2 a2 = vec2(cos(phi), sin(phi));

        float uvRadius = pc.p0.z / (viewDist * 2.0 * length(a2 * fovScale));

        // Exact slice frame. dq is the constant-depth lateral step along the
        // slice, so every sample vector v = SP - P lies in the plane
        // span{dq, V}. zeta = cross(dq, V) is that plane's NORMAL — it can
        // never serve as an elevation axis (dot(v, zeta) == 0 identically).
        // The in-plane axes are xi = cross(V, zeta) (horizontal) and
        // eta = -V (elevation, toward the camera).
        vec3 dq = viewPosFromDepth(vUV + a2 * t, depth) - P;
        vec3 zeta = cross(dq, V);
        float zLen = length(zeta);
        if (zLen < 1e-8)
            continue;
        zeta /= zLen;
        vec3 xi = normalize(cross(V, zeta));
        vec3 eta = -V;

        // Lambert-weighted slice integration. In-slice directions are
        // d(theta) = cos(theta)*xi + sin(theta)*eta, and the Lambert factor
        // for the surface is dot(N, d) = m * cos(theta - thetaN) where
        // (cN, sN) = (dot(N, xi), dot(N, eta)) and m = |(cN, sN)| is the
        // length of N projected into the slice plane. The occluded arc runs
        // from the tangent plane (thetaN - pi/2) to the sample horizon h, so
        // its exact integral is m * (1 + sin(h - thetaN)) — evaluated in
        // sin/cos space, no atan needed.
        float cN = dot(N, xi);
        float sN = dot(N, eta);
        float projLen = sqrt(cN * cN + sN * sN);
        // Edge-on slice (N parallel to zeta): every in-slice direction is
        // perpendicular to N, so this slice carries zero Lambert weight —
        // skip it entirely (do NOT open the tangent window).
        if (projLen < 1e-6)
            continue;
        float cosN = cN / projLen;  // cos(thetaN), +xi side frame
        float sinN = sN / projLen;  // sin(thetaN)
        float cosNneg = -cosN;      // -xi side frame
        // Tangent bound in sin space: sin(thetaN - pi/2) = -cos(thetaN).
        const float sinLowerPos = -cosN;
        const float sinLowerNeg = -cosNneg;

        // Occlusion per side = max over samples of the weighted above-tangent
        // arc segment. The falloff is applied per sample BEFORE the max:
        // adding a farther occluder can never reduce a side's occlusion
        // (monotone horizon — a higher raw angle with a tiny weight must not
        // override a lower closer one).
        float arcPos = 0.0;
        float arcNeg = 0.0;

        for (int s = 1; s <= steps; ++s)
        {
            // Mid-bin placement: the outermost sample must not sit on the
            // radius boundary, where the distance falloff reaches zero.
            float r = (float(s) - 0.5) / float(steps);
            vec2 off = a2 * uvRadius * r;

            // + side
            {
                vec2 suv = vUV + off;
                float sd = depthAt(suv);
                if (!isSky(sd)) // sky / off-screen samples never occlude
                {
                    vec3 SP = viewPosFromDepth(suv, sd);
                    vec3 v = SP - P;
                    float dist = length(v);
                    float lat = dot(v, xi); // in-slice horizontal run
                    if (lat > 1e-4)         // keeps (lat, b) non-degenerate
                    {
                        float b = dot(v, eta); // in-slice elevation run
                        float rho = sqrt(lat * lat + b * b);
                        float sinH = b / rho;
                        float cosH = lat / rho;
                        if (sinH > sinLowerPos)
                        {
                            // m * (1 + sin(h - thetaN)): exact integral of
                            // dot(N, d) from the tangent plane to the horizon.
                            float w = 1.0 - clamp(dist / pc.p0.z, 0.0, 1.0); // linear distance falloff
                            arcPos = max(arcPos, projLen * (1.0 + (sinH * cosN - cosH * sinN)) * w);
                        }
                    }
                }
            }

            // - side (mirrored: measured on the -xi axis so the angle stays
            // in [-pi/2, pi/2] instead of wrapping through +/-pi)
            {
                vec2 suv = vUV - off;
                float sd = depthAt(suv);
                if (!isSky(sd))
                {
                    vec3 SP = viewPosFromDepth(suv, sd);
                    vec3 v = SP - P;
                    float dist = length(v);
                    float lat = -dot(v, xi);
                    if (lat > 1e-4)
                    {
                        float b = dot(v, eta);
                        float rho = sqrt(lat * lat + b * b);
                        float sinH = b / rho;
                        float cosH = lat / rho;
                        if (sinH > sinLowerNeg)
                        {
                            float w = 1.0 - clamp(dist / pc.p0.z, 0.0, 1.0);
                            arcNeg = max(arcNeg, projLen * (1.0 + (sinH * cosNneg - cosH * sinN)) * w);
                        }
                    }
                }
            }
        }

        sum += arcPos + arcNeg;
    }

    // Raw AO out — composite applies intensity and the safety-only floor.
    // Each side's arc is in [0, 2], so the fully-enclosed normalization is
    // 4 * directions.
    float ao = clamp(1.0 - sum / (4.0 * float(dirs)), 0.0, 1.0);

    outColor = vec4(ao, N.x * 0.5 + 0.5, N.y * 0.5 + 0.5, N.z * 0.5 + 0.5);
}
