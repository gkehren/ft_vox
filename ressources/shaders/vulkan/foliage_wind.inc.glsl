// Foliage wind from material table (FoliageWind flag bit 0). Keep in sync with shadow.vert.
vec3 applyFoliageWind(vec3 pos, uint texIdx, float time, vec2 uv)
{
    vec4 m = materialTable.mats[texIdx];
    float wind = m.x;
    uint flags = uint(m.w + 0.5);
    if ((flags & 1u) == 0u || wind < 1e-4)
        return pos;

    if ((flags & 8u) != 0u) wind *= 1.0 - uv.y;
    float h = fract(sin(dot(pos.xz, vec2(12.9898, 78.233))) * 43758.5453);
    float phase = pos.x * 0.65 + pos.z * 0.55 + h * 6.2831853;
    float s = sin(time * 1.7 + phase) * 0.65 + sin(time * 2.35 + phase * 1.3) * 0.35;
    float c = cos(time * 1.4 + phase * 0.9);
    float heightBoost = 0.70 + 0.30 * clamp((pos.y - 60.0) / 40.0, 0.0, 1.0);
    pos.x += s * wind * heightBoost;
    pos.z += c * wind * 0.55 * heightBoost;
    return pos;
}
