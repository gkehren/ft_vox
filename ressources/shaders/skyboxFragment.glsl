#version 460 core
layout (location = 0) out vec4 FragColor;
layout (location = 1) out vec3 GodRaysSource;

in vec3 TexCoords;

uniform vec3 sunDir;
uniform vec3 moonDir;
uniform float time;
uniform vec3 fogColor;
uniform vec3 cameraPos;
uniform float dayFactor;
uniform float sunsetFactor;
uniform float nightFactor;

float hash2(vec2 p) {
    p = fract(p * vec2(443.8975, 441.4235));
    p += dot(p, p.yx + 19.19);
    return fract(p.x * p.y);
}

float noise2(vec2 p) {
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
        v += a * noise2(p);
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

    // 2. Compute zenith and horizon colors
    vec3 dayZenith = vec3(0.14, 0.34, 0.64);
    vec3 sunsetZenith = vec3(0.16, 0.10, 0.24);
    vec3 nightZenith = vec3(0.004, 0.010, 0.032);
    vec3 zenithColor = dayZenith * day + sunsetZenith * sunset + nightZenith * night;

    vec3 dayHorizon = vec3(0.48, 0.67, 0.82);
    vec3 sunsetHorizon = vec3(0.92, 0.42, 0.20);
    vec3 nightHorizon = vec3(0.025, 0.045, 0.09);
    vec3 horizonColor = dayHorizon * day + sunsetHorizon * sunset + nightHorizon * night;
    horizonColor = mix(horizonColor, fogColor, 0.35);

    // 3. Sky gradient (interpolate horizon to zenith)
    float grad = pow(1.0 - h, 3.4);
    vec3 skyColor = mix(zenithColor, horizonColor, grad);

    // Concentrate twilight warmth around the sun instead of tinting the full sky.
    vec2 viewH = normalize(V.xz + vec2(0.0001));
    vec2 sunH = normalize(sunDir.xz + vec2(0.0001));
    float horizonBand = pow(1.0 - h, 5.0);
    float sunsetFacing = pow(max(dot(viewH, sunH), 0.0), 4.0);
    skyColor += vec3(0.34, 0.10, 0.025) * sunset * horizonBand * sunsetFacing;

    // 4. Draw procedural stars (only visible at night)
    vec3 starsColor = vec3(0.0);
    if (night > 0.0) {
        // Offset view vector to keep coordinates positive (retains hash quality)
        vec3 starPos = (V + vec3(2.0)) * 150.0;
        vec3 grid = floor(starPos);
        vec3 f = fract(starPos);
        
        float hCell = hash3(grid);
        if (hCell > 0.98) { // Star probability
            vec3 starCenter = vec3(hash3(grid + 0.1), hash3(grid + 0.2), hash3(grid + 0.3));
            float d = length(f - starCenter);
            float intensity = 1.0 - smoothstep(0.0, 0.12, d);
            
            // Twinkle based on hash and time
            float twinkle = 0.5 + 0.5 * sin(time * 3.0 + hCell * 62.8);
            starsColor += vec3(intensity * twinkle) * night * 1.7;
        }
    }

    // 5. Draw Sun disc
    float sunGlow = max(dot(V, sunDir), 0.0);
    float sunDisc = smoothstep(0.99945, 0.99972, sunGlow);
    vec3 sunColor = vec3(1.0, 0.94, 0.78) * 2.0;
    vec3 sunGlowColor = vec3(1.0, 0.48, 0.14) * pow(sunGlow, 180.0) * 0.55;
    sunGlowColor += vec3(1.0, 0.42, 0.10) * pow(sunGlow, 18.0) * sunset * 0.18;
    vec3 sunComposite = (sunColor * sunDisc + sunGlowColor * (1.0 - sunDisc)) * smoothstep(-0.04, 0.08, sunHeight);

    // 6. Draw Moon disc
    float moonGlow = max(dot(V, moonDir), 0.0);
    float moonDisc = smoothstep(0.99966, 0.99982, moonGlow);
    vec3 moonColor = vec3(0.74, 0.84, 1.0) * 1.0;
    vec3 moonGlowColor = vec3(0.20, 0.30, 0.56) * pow(moonGlow, 110.0) * 0.20;
    vec3 moonComposite = (moonColor * moonDisc + moonGlowColor * (1.0 - moonDisc)) * smoothstep(0.02, 0.28, moonDir.y);

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
                vec3 dayCloud = vec3(0.82, 0.88, 0.93);
                vec3 sunsetCloud = vec3(0.96, 0.56, 0.38);
                vec3 nightCloud = vec3(0.07, 0.085, 0.14);
                vec3 cloudColor = dayCloud * day + sunsetCloud * sunset + nightCloud * night;
                
                if (side == 1) cloudColor *= 0.85;
                else if (side == 2) cloudColor *= 0.75;
                else {
                    if (cameraPos.y < hitBottom) cloudColor *= 0.60;
                    else cloudColor *= 1.0;
                }
                
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
    GodRaysSource = vec3(1.0, 0.62, 0.28) * raySource * rayVisibility * sunTransmission;
}
