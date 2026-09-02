#pragma once
#include <SDL.h>
#include <string>

namespace ps96 {

// Load PNG/JPG/BMP/TGA via stb_image into an SDL_Texture (RGBA).
// Returns nullptr on failure; caller owns the texture.
SDL_Texture* load_texture(SDL_Renderer* ren, const std::string& path);

} // namespace ps96
