#include "frontend/ImageLoader.hpp"
#include "common/Log.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#include "frontend/stb_image.h"

#include <cstdio>
#include <vector>

namespace ps96 {

SDL_Texture* load_texture(SDL_Renderer* ren, const std::string& path) {
    if (!ren || path.empty()) return nullptr;

    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        Log::warn("Cover open failed: %s", path.c_str());
        return nullptr;
    }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 32 * 1024 * 1024) {
        std::fclose(f);
        return nullptr;
    }
    std::vector<stbi_uc> buf(static_cast<size_t>(sz));
    if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) {
        std::fclose(f);
        return nullptr;
    }
    std::fclose(f);

    int w = 0, h = 0, comp = 0;
    stbi_uc* pixels = stbi_load_from_memory(buf.data(), static_cast<int>(buf.size()),
                                            &w, &h, &comp, 4);
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels) stbi_image_free(pixels);
        Log::warn("Cover decode failed: %s", path.c_str());
        return nullptr;
    }

    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(
        pixels, w, h, 32, w * 4, SDL_PIXELFORMAT_RGBA32);
    if (!surf) {
        stbi_image_free(pixels);
        return nullptr;
    }
    SDL_Texture* tex = SDL_CreateTextureFromSurface(ren, surf);
    SDL_FreeSurface(surf);
    stbi_image_free(pixels);
    if (tex) {
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        Log::info("Cover loaded: %s (%dx%d)", path.c_str(), w, h);
    }
    return tex;
}

} // namespace ps96
