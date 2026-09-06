#include "MobTextures.hpp"
#include <stb_image/stb_image.h>
#include <memory>
#include <stdexcept>

MobTexturePack loadMobTextures(const std::string &customPath, const std::string &defaultPath)
{
    ResourcePackReader custom(customPath), fallback(defaultPath);
    MobTexturePack result;
    const std::array<const char *, 6> aliases{{"cow/cow.png", "pig/pig.png", "sheep/sheep.png", "chicken.png",
                                               "sheep/sheep_fur.png", "sheep/sheep_wool_undercoat.png"}};
    for (size_t i = 0; i < kMobTextures.size(); ++i)
    {
        auto decode = [&](ResourcePackReader &reader, const char *name) {
            std::vector<uint8_t> png;
            if (!reader.readEntityTexture(name, png))
                return false;
            int w = 0, h = 0, c = 0;
            if (!stbi_info_from_memory(png.data(), int(png.size()), &w, &h, &c) || w < 1 || h < 1 ||
                w > 4096 || h > 4096)
                return false;
            // Both modern 64x64 and classic 64x32 cow/pig layouts are supported.
            if (w % 64 || (h != w && h * 2 != w) || (i >= 2 && h * 2 != w))
                return false;
            std::unique_ptr<unsigned char, decltype(&stbi_image_free)> pixels(
                stbi_load_from_memory(png.data(), int(png.size()), &w, &h, &c, 4), stbi_image_free);
            if (!pixels)
                return false;
            result.images[i] = {w, h, std::vector<uint8_t>(pixels.get(), pixels.get() + size_t(w) * h * 4)};
            return true;
        };
        if (decode(custom, kMobTextures[i]) || decode(custom, aliases[i]))
            ++result.report.hits;
        else
        {
            if (!customPath.empty())
                ++result.report.misses;
            if (!decode(fallback, kMobTextures[i]))
                throw std::runtime_error(std::string("Missing default mob texture: ") + kMobTextures[i]);
        }
    }
    return result;
}
