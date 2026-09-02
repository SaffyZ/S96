#pragma once
#include "common/Types.hpp"
#include <SDL.h>

namespace ps96 {

// Shared 5x7 bitmap font for SDL_Renderer (dashboard + overlays)
void draw_string(SDL_Renderer* ren, int x, int y, const char* text, u8 r, u8 g, u8 b, int scale = 2);

} // namespace ps96
