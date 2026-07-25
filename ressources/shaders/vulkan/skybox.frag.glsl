#version 450
layout (location = 0) out vec4 FragColor;
layout (location = 1) out vec4 GodRaysSource;

layout(location = 0) in vec3 TexCoords;

#include "frame_ubo.inc.glsl"

#define sunDir frame.sunDir.xyz
#define moonDir frame.moonDir.xyz
#define time frame.skyParams.x
#define fogColor frame.fogColor.rgb
#define cameraPos frame.viewPos.xyz
#define dayFactor frame.skyParams.y
#define sunsetFactor frame.skyParams.z
#define nightFactor frame.skyParams.w

float hash2(vec2 p) {
    p = fract(p * vec2(443.8975, 441.4235));
    p += dot(p, p.yx + 19.19);
    return fract(p.x * p.y);
}

// Named valueNoise2 to avoid clashing with GLSL built-in noise2()
float valueNoise2(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    
    float a = hash2(i);
    float b = hash2(i + vec2(1.0, 0.0));
    float c = hash2(i + vec2(0.0, 1.0));
    float d = hash2(i + vec2(1.0, 1.0));
    
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float fbm(vec2 p) {
    float v = 0.0;
    float a = 0.5;
    vec2 shift = vec2(100.0);
    mat2 rot = mat2(cos(0.5), sin(0.5), -sin(0.5), cos(0.5));
    for (int i = 0; i < 2; ++i) {
        v += a * valueNoise2(p);
        p = rot * p * 2.0 + shift;
        a *= 0.5;
    }
    return v;
}

float hash3(vec3 p) {
    p = fract(p * vec3(443.8975, 441.4235, 437.1953));
    p += dot(p, p.yzx + 19.19);
    return fract((p.x + p.y) * p.z);
}

void main()
{
    vec3 V = normalize(TexCoords);
    float h = max(V.y, 0.0);
    float sunHeight = sunDir.y;
    float factorTotal = max(dayFactor + sunsetFactor + nightFactor, 0.001);
    float day = dayFactor / factorTotal;
    float sunset = sunsetFactor / factorTotal;
    float night = nightFactor / factorTotal;

    // 2. Zenith / horizon — deep day blue, near-black cinematic night
    vec3 dayZenith = vec3(0.06, 0.24, 0.68);
    vec3 sunsetZenith = vec3(0.18, 0.08, 0.28);
    vec3 nightZenith = vec3(0.002, 0.005, 0.013);
    vec3 zenithColor = dayZenith * day + sunsetZenith * sunset + nightZenith * night;

    vec3 dayHorizon = vec3(0.36, 0.62, 0.92);
    vec3 sunsetHorizon = vec3(0.95, 0.35, 0.12);
    vec3 nightHorizon = vec3(0.006, 0.010, 0.024);
    vec3 horizonColor = dayHorizon * day + sunsetHorizon * sunset + nightHorizon * night;
    horizonColor = mix(horizonColor, fogColor, 0.12);

    // 3. Sky gradient (interpolate horizon to zenith)
    float grad = pow(1.0 - h, 2.8);
    vec3 skyColor = mix(zenithColor, horizonColor, grad);

    // Concentrate twilight warmth around the sun instead of tinting the full sky.
    vec2 viewH = normalize(V.xz + vec2(0.0001));
    vec2 sunH = normalize(sunDir.xz + vec2(0.0001));
    float horizonBand = pow(1.0 - h, 4.5);
    float sunsetFacing = pow(max(dot(viewH, sunH), 0.0), 3.5);
    // Concentrated twilight warmth (was 0.48 additive → clipped to white near sun)
    skyColor += vec3(0.30, 0.09, 0.03) * sunset * horizonBand * pow(sunsetFacing, 1.4);
    // Soft daytime sun scatter near horizon
    skyColor += vec3(0.12, 0.18, 0.28) * day * pow(1.0 - h, 6.0) * 0.35;
    // Faint residual horizon glow toward the moon azimuth at night
    vec2 moonH = normalize(moonDir.xz + vec2(0.0001));
    float moonFacing = pow(max(dot(viewH, moonH), 0.0), 4.0);
    skyColor += vec3(0.020, 0.030, 0.055) * night * horizonBand * moonFacing;

    float sunGlow = max(dot(V, sunDir), 0.0);
    float moonGlow = max(dot(V, moonDir), 0.0);

    // 4. Procedural stars — two layers, per-star tint/brightness/twinkle (night only)
    vec3 starsColor = vec3(0.0);
    if (night > 0.0) {
        float horizonFade = smoothstep(-0.02, 0.12, V.y);

        // Layer 1: bright sparse stars with color variety
        vec3 p1 = (V + vec3(2.0)) * 150.0;
        vec3 g1 = floor(p1);
        vec3 f1 = fract(p1);
        float h1 = hash3(g1);
        if (h1 > 0.992) {
            vec3 c1 = vec3(hash3(g1 + 0.1), hash3(g1 + 0.2), hash3(g1 + 0.3));
            float d = length(f1 - c1);
            float size = 0.05 + 0.09 * hash3(g1 + 0.7);
            float intensity = 1.0 - smoothstep(0.0, size, d);
            float bright = 0.6 + 2.2 * hash3(g1 + 0.5);
            float tw = 0.55 + 0.45 * sin(time * (1.5 + 3.0 * hash3(g1 + 0.9)) + h1 * 62.8);
            float tintSel = hash3(g1 + 1.3);
            vec3 tint = tintSel < 0.33 ? vec3(0.72, 0.82, 1.0)
                      : (tintSel < 0.66 ? vec3(1.0) : vec3(1.0, 0.86, 0.68));
            starsColor += tint * intensity * bright * tw;
        }

        // Layer 2: dense faint background stars
        vec3 p2 = (V + vec3(3.7)) * 320.0;
        vec3 g2 = floor(p2);
        vec3 f2 = fract(p2);
        float h2 = hash3(g2);
        if (h2 > 0.978) {
            vec3 c2 = vec3(hash3(g2 + 0.1), hash3(g2 + 0.2), hash3(g2 + 0.3));
            float d = length(f2 - c2);
            float intensity = 1.0 - smoothstep(0.0, 0.05, d);
            starsColor += vec3(0.75, 0.82, 1.0) * intensity * (0.15 + 0.35 * hash3(g2 + 0.4));
        }

        starsColor *= night * horizonFade;
        starsColor *= 1.0 - 0.55 * pow(moonGlow, 30.0); // washed out near the moon
    }

    // 5. Draw Sun disc — white-yellow high in the sky, deep orange near the horizon
    float sunDisc = smoothstep(0.9994, 0.9997, sunGlow);
    float sunLow = 1.0 - smoothstep(0.0, 0.35, sunHeight);
    vec3 sunColor = mix(vec3(1.0, 0.96, 0.72), vec3(1.0, 0.45, 0.12), sunLow * sunLow) * 2.6;
    vec3 sunGlowColor = mix(vec3(1.0, 0.55, 0.18), vec3(1.0, 0.30, 0.06), sunLow) * pow(sunGlow, 140.0) * 0.72;
    sunGlowColor += vec3(1.0, 0.48, 0.12) * pow(sunGlow, 14.0) * (0.12 + sunset * 0.35);
    vec3 sunComposite = (sunColor * sunDisc + sunGlowColor * (1.0 - sunDisc)) * smoothstep(-0.04, 0.08, sunHeight);

    // 6. Moon: HDR disc with procedural maria/craters + tight halo (cinematic night)
    float moonDisc = smoothstep(0.99966, 0.99982, moonGlow);
    vec3 moonUpRef = abs(moonDir.y) > 0.99 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    vec3 mRight = normalize(cross(moonDir, moonUpRef));
    vec3 mUp = normalize(cross(mRight, moonDir));
    vec2 mUV = vec2(dot(V, mRight), dot(V, mUp)) / 0.019; // ~[-1,1] across the disc
    float maria = fbm(mUV * 2.5 + vec2(41.0, 17.0));
    float craters = fbm(mUV * 6.0 + vec2(13.7, 5.2));
    vec3 moonAlbedo = vec3(0.86, 0.90, 0.98);
    moonAlbedo *= 0.72 + 0.28 * smoothstep(0.30, 0.62, maria);   // large dark seas
    moonAlbedo *= 0.80 + 0.20 * smoothstep(0.35, 0.65, craters); // fine detail
    moonAlbedo *= 1.0 - 0.25 * smoothstep(0.55, 1.0, dot(mUV, mUV)); // limb darkening
    vec3 moonHalo = vec3(0.35, 0.45, 0.70) * pow(moonGlow, 600.0) * 0.35
                  + vec3(0.10, 0.14, 0.28) * pow(moonGlow, 48.0) * 0.05;
    vec3 moonComposite = (moonAlbedo * 3.0 * moonDisc + moonHalo) * smoothstep(0.02, 0.28, moonDir.y);

    // Celestial bodies are part of the sky so clouds can occlude them.
    vec3 baseSky = skyColor + starsColor + sunComposite + moonComposite;
    float sunTransmission = 1.0;

    // 7. Draw 3D volumetric Minecraft clouds with varying heights
    float globalBottom = 240.0;
    float globalTop = 360.0;
    float scale = 32.0; // Size of a cloud block in XZ
    
    if (abs(V.y) > 0.001) {
        float t1 = (globalBottom - cameraPos.y) / V.y;
        float t2 = (globalTop - cameraPos.y) / V.y;
        float tEnter = min(t1, t2);
        float tExit = max(t1, t2);

        if (tExit > 0.0 && tEnter < 1600.0) {
        if (tEnter < 0.0) tEnter = 0.0;
        tExit = min(tExit, 1600.0);
        
        vec2 cloudVel = vec2(8.0, 2.0); // Wind speed
        vec2 rayStart = (cameraPos.xz + V.xz * tEnter - time * cloudVel) / scale;
        vec2 rayEnd = (cameraPos.xz + V.xz * tExit - time * cloudVel) / scale;
        
        vec2 rayDir = rayEnd - rayStart;
        float maxDist = length(rayDir);
        vec2 dirNorm = (maxDist > 0.0001) ? (rayDir / maxDist) : vec2(0.0);
        
        if (maxDist > 0.0001) {
            vec2 mapPos = floor(rayStart);
            vec2 deltaDist = abs(1.0 / dirNorm);
            vec2 stepDir = sign(dirNorm);
            
            vec2 sideDist;
            sideDist.x = (stepDir.x > 0.0) ? (mapPos.x + 1.0 - rayStart.x) * deltaDist.x : (rayStart.x - mapPos.x) * deltaDist.x;
            sideDist.y = (stepDir.y > 0.0) ? (mapPos.y + 1.0 - rayStart.y) * deltaDist.y : (rayStart.y - mapPos.y) * deltaDist.y;
            
            bool hit = false;
            int side = 0; // 0: top/bottom, 1: X-side, 2: Z-side
            float finalTHit = 0.0;
            float hitBottom = 0.0;
            
            float d0 = 0.0;
            int lastSide = 0; // 1 for X, 2 for Z
            
            for (int i = 0; i < 75; i++) {
                // Evaluate current block
                float cNoise = fbm(mapPos * 0.15);
                if (cNoise > 0.52) {
                    float h1 = hash2(mapPos);
                    float h2 = hash2(mapPos + vec2(1.0, -1.0));
                    
                    float blockBottom = 250.0 + h1 * 70.0; // Varies between 250 and 320
                    float blockTop = blockBottom + 10.0 + h2 * 30.0; // Thickness between 10 and 40
                    
                    float d1 = min(sideDist.x, sideDist.y);
                    if (d1 > maxDist) d1 = maxDist;
                    
                    float t0 = tEnter + (tExit - tEnter) * (d0 / maxDist);
                    float t1_3d = tEnter + (tExit - tEnter) * (d1 / maxDist);
                    
                    float y0 = cameraPos.y + V.y * t0;
                    float y1 = cameraPos.y + V.y * t1_3d;
                    
                    float yMin = min(y0, y1);
                    float yMax = max(y0, y1);
                    
                    if (yMax >= blockBottom && yMin <= blockTop) {
                        hit = true;
                        hitBottom = blockBottom;
                        
                        if (V.y > 0.0) { // Ray going UP
                            if (y0 < blockBottom) {
                                finalTHit = (blockBottom - cameraPos.y) / V.y;
                                side = 0;
                            } else {
                                finalTHit = t0;
                                side = lastSide;
                            }
                        } else { // Ray going DOWN
                            if (y0 > blockTop) {
                                finalTHit = (blockTop - cameraPos.y) / V.y;
                                side = 0;
                            } else {
                                finalTHit = t0;
                                side = lastSide;
                            }
                        }
                        break;
                    }
                }
                
                // Advance DDA
                d0 = min(sideDist.x, sideDist.y);
                if (d0 > maxDist) break;
                
                if (sideDist.x < sideDist.y) {
                    sideDist.x += deltaDist.x;
                    mapPos.x += stepDir.x;
                    lastSide = 1;
                } else {
                    sideDist.y += deltaDist.y;
                    mapPos.y += stepDir.y;
                    lastSide = 2;
                }
            }
            
            if (hit) {
                vec3 dayCloud = vec3(0.92, 0.95, 0.98);
                vec3 sunsetCloud = vec3(1.0, 0.58, 0.32);
                vec3 nightCloud = vec3(0.012, 0.018, 0.035);
                vec3 cloudColor = dayCloud * day + sunsetCloud * sunset + nightCloud * night;

                if (side == 1) cloudColor *= 0.80;
                else if (side == 2) cloudColor *= 0.68;
                else {
                    if (cameraPos.y < hitBottom) cloudColor *= 0.50;
                    else cloudColor *= 1.0;
                }

                // Warm rim on the sun side by day
                cloudColor += vec3(0.10, 0.06, 0.02) * pow(sunGlow, 6.0) * day;
                // Silver lining: clouds near the moon catch its light at night
                cloudColor += vec3(0.30, 0.38, 0.55) * pow(moonGlow, 6.0) * night * 0.35;
                
                float fogFactor = smoothstep(600.0, 1600.0, finalTHit);
                float cloudAlpha = (1.0 - fogFactor) * 0.84;
                
                baseSky = mix(baseSky, cloudColor, cloudAlpha);
                sunTransmission *= 1.0 - cloudAlpha;
            }
        }
    }
    }

    float raySource = sunDisc * 2.0;
    raySource += pow(sunGlow, 72.0) * 0.72;
    raySource += pow(sunGlow, 16.0) * sunset * 0.20;
    float rayVisibility = smoothstep(-0.10, 0.04, sunHeight);

    FragColor = vec4(baseSky, 1.0);
    GodRaysSource = vec4(vec3(1.0, 0.62, 0.28) * raySource * rayVisibility * sunTransmission, 1.0);
}
