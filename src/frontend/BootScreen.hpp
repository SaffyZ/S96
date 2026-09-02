#pragma once

#include "common/Types.hpp"
#include <SDL.h>
#include <array>

namespace ps96 {

class BootScreen {
public:
    void reset();
    void set_size(int w, int h);
    bool update(float dt);
    void render(SDL_Renderer* ren);
    bool finished() const { return m_phase >= Phase::Done; }
    void skip() { m_phase = Phase::Done; m_t = 99.f; }

private:
    enum class Phase {
        Black, Spark, Orb, Shockwave, Title, Hold, FadeOut, Done
    };

    struct Particle {
        float x, y, vx, vy, life, max_life, size;
        u8 r, g, b;
    };

    Phase m_phase = Phase::Black;
    float m_t = 0.f;
    float m_total = 0.f;
    int m_w = 1280, m_h = 720;
    std::array<Particle, 180> m_parts{};
    int m_part_count = 0;
    float m_shake = 0.f;

    void spawn_burst(float cx, float cy, int n);
    void update_particles(float dt);
    void draw_particles(SDL_Renderer* ren);
    void draw_orb(SDL_Renderer* ren, float cx, float cy, float radius, float alpha, float pulse);
    void draw_ring(SDL_Renderer* ren, float cx, float cy, float radius, float thickness, u8 r, u8 g, u8 b, u8 a);
    void draw_scanlines(SDL_Renderer* ren, u8 alpha);
    void draw_vignette(SDL_Renderer* ren, float strength);
    void fill_gradient(SDL_Renderer* ren);
};

} // namespace ps96
