#pragma once

#include <Chunk/TerrainGenerator.hpp>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string_view>

// Opt-in diagnostic export, independent of the Vulkan renderer. CSV remains
// the source of truth for overview plots and reproducible camera locations.
inline int exportTerrainAtlas(int argc, char **argv)
{
    if (argc != 6)
    {
        std::cerr << "usage: test_terrain --atlas size step seed output.csv\n";
        return 2;
    }
    auto parse = [](const char *s, int &out) {
        const std::string_view text(s);
        const auto r = std::from_chars(text.data(), text.data() + text.size(), out);
        return r.ec == std::errc{} && r.ptr == text.data() + text.size();
    };
    int size, step, seed;
    if (!parse(argv[2], size) || !parse(argv[3], step) || !parse(argv[4], seed) ||
        size < 2 || size > 1024 || step < 1 || step > 1024)
    {
        std::cerr << "atlas size must be 2..1024, step 1..1024, seed an int\n";
        return 2;
    }
    std::ofstream out(argv[5]);
    if (!out) { std::cerr << "cannot open atlas output\n"; return 2; }
    out << "x,z,post_erosion_height,biome,relief,continentality,erosion,weirdness,temperature,humidity,river\n";
    TerrainGenerator gen(seed);
    std::array<size_t, BIOME_COUNT> biomes{};
    std::array<size_t, 6> profiles{};
    std::vector<uint8_t> biomeMap(static_cast<size_t>(size) * size);
    std::vector<int> heights;
    heights.reserve(biomeMap.size());
    for (int z = 0; z < size; ++z)
        for (int x = 0; x < size; ++x)
        {
            const int wx = (x - size / 2) * step, wz = (z - size / 2) * step;
            const auto s = gen.getTerrainSample(wx, wz);
            const auto profile = std::max_element(s.reliefWeights.begin(), s.reliefWeights.end()) - s.reliefWeights.begin();
            out << wx << ',' << wz << ',' << s.postErosionHeight << ',' << int(s.biome) << ',' << profile << ','
                << s.continentality << ',' << s.erosion << ',' << s.weirdness << ',' << s.temperature << ','
                << s.humidity << ',' << s.river << '\n';
            ++biomes[s.biome]; ++profiles[profile];
            biomeMap[static_cast<size_t>(z) * size + x] = static_cast<uint8_t>(s.biome);
            heights.push_back(s.postErosionHeight);
        }
    if (!out) { std::cerr << "atlas write failed\n"; return 2; }
    // Connected regions at the explicit sampling resolution; boundary-touching
    // regions are censored by the window, not whole-biome area measurements.
    std::vector<bool> visited(biomeMap.size());
    std::vector<int> queue;
    std::array<size_t, BIOME_COUNT> largest{}, components{}, boundaryComponents{};
    for (int i = 0; i < size * size; ++i)
    {
        if (visited[i]) continue;
        queue.clear(); queue.push_back(i); visited[i] = true;
        const auto biome = biomeMap[i];
        bool boundary = false;
        for (size_t head = 0; head < queue.size(); ++head)
        {
            const int at = queue[head], x = at % size, z = at / size;
            boundary |= x == 0 || z == 0 || x == size - 1 || z == size - 1;
            for (const auto offset : {glm::ivec2(-1, 0), {1, 0}, {0, -1}, {0, 1}})
            {
                const int nx = x + offset.x, nz = z + offset.y;
                if (nx < 0 || nx >= size || nz < 0 || nz >= size) continue;
                const int next = nz * size + nx;
                if (!visited[next] && biomeMap[next] == biome)
                { visited[next] = true; queue.push_back(next); }
            }
        }
        ++components[biome]; boundaryComponents[biome] += boundary;
        largest[biome] = std::max(largest[biome], queue.size());
    }
    std::cout << "Terrain atlas seed=" << seed << " size=" << size << " step=" << step << " -> " << argv[5] << '\n';
    for (int i = 0; i < BIOME_COUNT; ++i)
        std::cout << biomeTypeString[i] << ": " << 100.0 * biomes[i] / biomeMap.size()
            << "% regions=" << components[i] << " boundaryRegions=" << boundaryComponents[i]
            << " largestSampledArea=" << largest[i] * size_t(step) * step << " blocks^2\n";
    std::cout << "Dominant relief fractions (rolling/plateau/massif/canyon/cliffs/basin):";
    for (auto count : profiles) std::cout << ' ' << 100.0 * count / biomeMap.size();
    std::sort(heights.begin(), heights.end());
    std::cout << "\nHeight min/p50/p95/p99/max: " << heights.front() << '/' << heights[heights.size() / 2]
        << '/' << heights[heights.size() * 95 / 100] << '/' << heights[heights.size() * 99 / 100]
        << '/' << heights.back() << '\n';
    return 0;
}
