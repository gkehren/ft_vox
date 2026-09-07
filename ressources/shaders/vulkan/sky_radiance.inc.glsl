// Sky gradient identical to skybox.frag.glsl so reflections match the sky
vec3 analyticSkyRadiance(vec3 R, float day, float sunset, float night) {
    float total = max(day + sunset + night, 0.001);
    day /= total; sunset /= total; night /= total;
    float h = clamp(R.y, 0.0, 1.0);
    vec3 zenith = vec3(0.06, 0.24, 0.68) * day
                + vec3(0.18, 0.08, 0.28) * sunset
                + vec3(0.002, 0.005, 0.013) * night;
    vec3 horizon = vec3(0.36, 0.62, 0.92) * day
                 + vec3(0.95, 0.35, 0.12) * sunset
                 + vec3(0.006, 0.010, 0.024) * night;
    horizon = mix(horizon, frame.fogColor.rgb, 0.12);
    float grad = pow(1.0 - h, 2.8);
    vec3 sky = mix(zenith, horizon, grad);

    // Concentrated twilight warmth toward the sun azimuth
    vec2 viewH = normalize(R.xz + vec2(0.0001));
    vec2 sunH = normalize(frame.sunDir.xz + vec2(0.0001));
    float horizonBand = pow(1.0 - h, 4.5);
    float sunsetFacing = pow(max(dot(viewH, sunH), 0.0), 3.5);
    sky += vec3(0.30, 0.09, 0.03) * sunset * horizonBand * pow(sunsetFacing, 1.4);
    // Soft daytime sun scatter near horizon
    sky += vec3(0.12, 0.18, 0.28) * day * pow(1.0 - h, 6.0) * 0.35;
    // Faint residual horizon glow toward the moon azimuth at night
    vec2 moonH = normalize(frame.moonDir.xz + vec2(0.0001));
    float moonFacing = pow(max(dot(viewH, moonH), 0.0), 4.0);
    sky += vec3(0.020, 0.030, 0.055) * night * horizonBand * moonFacing;
    return sky;
}

