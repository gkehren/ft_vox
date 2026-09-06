#pragma once
#include <Renderer/ResourcePackReader.hpp>
#include <array>
#include <vector>
#include <string>

struct MobTextureReport
{
    int hits{}, misses{};
};
struct MobTexturePixels
{
    int width{}, height{};
    std::vector<uint8_t> rgba;
};
inline constexpr std::array<const char *, 6> kMobTextures{
    {"cow/cow_temperate.png", "pig/pig_temperate.png", "sheep/sheep.png", "chicken/chicken_temperate.png",
     "sheep/sheep_wool.png", "sheep/sheep_wool_undercoat.png"}};
struct MobTexturePack
{
    std::array<MobTexturePixels, 6> images;
    MobTextureReport report;
};
MobTexturePack loadMobTextures(const std::string &customPath, const std::string &defaultPath);
