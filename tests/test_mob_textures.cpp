#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4244) // vendored stb internals
#endif
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image/stb_image.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#include <Renderer/MobTextures.hpp>
#include <miniz.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>

namespace fs = std::filesystem;
#include "TestMain.hpp"
static void write(const fs::path &p, const std::vector<uint8_t> &data)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char *>(data.data()), data.size());
}
// Minimal RGBA PNG encoder for synthetic high-resolution resource-pack fixtures.
static std::vector<uint8_t> png(int w, int h)
{
    std::vector<uint8_t> out{137, 80, 78, 71, 13, 10, 26, 10};
    auto be = [](std::vector<uint8_t> &v, uint32_t n) {
        for (int s : {24, 16, 8, 0})
            v.push_back(uint8_t(n >> s));
    };
    auto chunk = [&](const char *type, const std::vector<uint8_t> &payload) {
        be(out, uint32_t(payload.size()));
        auto start = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), payload.begin(), payload.end());
        be(out, uint32_t(mz_crc32(0, out.data() + start, out.size() - start)));
    };
    std::vector<uint8_t> header;
    be(header, w);
    be(header, h);
    header.insert(header.end(), {8, 6, 0, 0, 0});
    chunk("IHDR", header);
    std::vector<uint8_t> raw(size_t(h) * (w * 4 + 1), 255);
    for (int y = 0; y < h; ++y)
    {
        raw[size_t(y) * (w * 4 + 1)] = 0;
        for (int x = 0; x < w; ++x)
            raw[size_t(y) * (w * 4 + 1) + 1 + x * 4] = uint8_t(x);
    }
    mz_ulong size = mz_compressBound(static_cast<mz_ulong>(raw.size()));
    std::vector<uint8_t> compressed(size);
    CHECK_EXPR(mz_compress(compressed.data(), &size, raw.data(), static_cast<mz_ulong>(raw.size())) == MZ_OK);
    compressed.resize(size);
    chunk("IDAT", compressed);
    chunk("IEND", {});
    return out;
}
int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    const std::string defaultPack = argv[1];
    const auto fixture = fs::temp_directory_path() /
                         ("ft-vox-mob-textures-" +
                          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(fixture);
    try
    {
        auto pack = loadMobTextures("", defaultPack);
        CHECK_EXPR(pack.report.hits == 0);
        CHECK_EXPR(pack.report.misses == 0);
        CHECK_EXPR(pack.images[0].width == 64 && pack.images[0].height == 64);
        CHECK_EXPR(pack.images[3].width == 64 && pack.images[3].height == 32);
        ResourcePackReader reader(defaultPack);
        std::vector<uint8_t> cow;
        CHECK_EXPR(reader.readEntityTexture(kMobTextures[0], cow));
        std::vector<uint8_t> scratch;
        CHECK_EXPR(!reader.readEntityTexture("../../block/stone.png", scratch));
        CHECK_EXPR(reader.readBlockTexture("stone.png", scratch));
        const auto custom = fixture / "custom";
        write(custom / "assets/minecraft/textures/entity/cow/cow_temperate.png", cow);
        pack = loadMobTextures(custom.string(), defaultPack);
        CHECK_EXPR(pack.report.hits == 1);
        CHECK_EXPR(pack.report.misses == 5);
        // A malformed custom image falls back even when the PNG entry exists.
        write(custom / "assets/minecraft/textures/entity/pig/pig_temperate.png", {1, 2, 3});
        pack = loadMobTextures(custom.string(), defaultPack);
        CHECK_EXPR(pack.report.hits == 1);
        CHECK_EXPR(pack.images[1].width == 64);
        const auto high = png(256, 128);
        write(custom / "assets/minecraft/textures/entity/chicken.png", high);
        pack = loadMobTextures(custom.string(), defaultPack);
        CHECK_EXPR(pack.report.hits == 2);
        CHECK_EXPR(pack.images[3].width == 256 && pack.images[3].height == 128);
        write(custom / "assets/minecraft/textures/entity/pig/pig.png", png(128, 64));
        pack = loadMobTextures(custom.string(), defaultPack);
        CHECK_EXPR(pack.report.hits == 3);
        CHECK_EXPR(pack.images[1].height == 64);
        // Wrong aspect ratio must not stretch a resource-pack texture.
        write(custom / "assets/minecraft/textures/entity/chicken/chicken_temperate.png", png(64, 64));
        pack = loadMobTextures(custom.string(), defaultPack);
        CHECK_EXPR(pack.images[3].width == 256);
        const auto zip = fixture / "entities-only.zip";
        mz_zip_archive archive{};
        CHECK_EXPR(mz_zip_writer_init_file(&archive, zip.string().c_str(), 0));
        CHECK_EXPR(mz_zip_writer_add_mem(&archive,
                                    "wrapped/assets/minecraft/textures/entity/cow/cow_temperate.png",
                                    cow.data(), cow.size(), MZ_BEST_SPEED));
        CHECK_EXPR(mz_zip_writer_finalize_archive(&archive));
        mz_zip_writer_end(&archive);
        pack = loadMobTextures(zip.string(), defaultPack);
        CHECK_EXPR(pack.report.hits == 1);
        CHECK_EXPR(pack.report.misses == 5);
        // Directory packs may wrap assets under one folder, like ZIP packs.
        const auto wrapped = fixture / "wrapped-pack";
        write(wrapped / "wrapper/assets/minecraft/textures/entity/cow/cow_temperate.png", cow);
        pack = loadMobTextures(wrapped.string(), defaultPack);
        CHECK_EXPR(pack.report.hits == 1);
        CHECK_EXPR(pack.report.misses == 5);
        pack = loadMobTextures((fixture / "missing").string(), defaultPack);
        CHECK_EXPR(pack.report.hits == 0);
        CHECK_EXPR(pack.report.misses == 6);
        bool threw = false;
        try
        {
            loadMobTextures("", (fixture / "missing").string());
        }
        catch (...)
        {
            threw = true;
        }
        CHECK_EXPR(threw);
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << "\n";
        ++testmain::fails;
    }
    // Only this invocation's unique temporary fixture is removed.
    fs::remove_all(fixture);
    std::cout << "Mob textures: " << testmain::fails << " testmain::fails\n";
    return testmain::exitCode();
}
