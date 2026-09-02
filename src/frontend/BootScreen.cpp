#include "frontend/BootScreen.hpp"
#include "frontend/Text.hpp"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <cstdlib>

namespace ps96 {

void BootScreen::reset() {
    m_phase = Phase::Black;
    m_t = 0.f;
    m_total = 0.f;
    m_part_count = 0;
    m_shake = 0.f;
}

void BootScreen::set_size(int w, int h) {
    m_w = std::max(320, w);
    m_h = std::max(240, h);
}

void BootScreen::spawn_burst(float cx, float cy, int n) {
    for (int i = 0; i < n && m_part_count < (int)m_parts.size(); i++) {
        auto& p = m_parts[m_part_count++];
        float ang = (float(rand()) / RAND_MAX) * 6.28318f;
        float spd = 80.f + (float(rand()) / RAND_MAX) * 420.f;
        p.x = cx; p.y = cy;
        p.vx = std::cos(ang) * spd;
        p.vy = std::sin(ang) * spd;
        p.max_life = 0.6f + (float(rand()) / RAND_MAX) * 1.2f;
        p.life = p.max_life;
        p.size = 1.5f + (float(rand()) / RAND_MAX) * 3.5f;
        // Cyan / white / electric blue
        int roll = rand() % 3;
        if (roll == 0) { p.r = 180; p.g = 240; p.b = 255; }
        else if (roll == 1) { p.r = 40; p.g = 180; p.b = 255; }
        else { p.r = 255; p.g = 255; p.b = 255; }
    }
}

void BootScreen::update_particles(float dt) {
    for (int i = 0; i < m_part_count; ) {
        auto& p = m_parts[i];
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        p.vx *= 0.98f;
        p.vy *= 0.98f;
        p.life -= dt;
        if (p.life <= 0.f) {
            m_parts[i] = m_parts[--m_part_count];
        } else {
            i++;
        }
    }
}

bool BootScreen::update(float dt) {
    m_t += dt;
    m_total += dt;
    update_particles(dt);
    m_shake *= 0.90f;

    switch (m_phase) {
        case Phase::Black:
            if (m_t > 0.25f) {
                m_phase = Phase::Spark; m_t = 0.f;
                spawn_burst(m_w * 0.5f, m_h * 0.42f, 90);
                m_shake = 8.f;
            }
            break;
        case Phase::Spark:
            if (m_t > 0.55f) { m_phase = Phase::Orb; m_t = 0.f; }
            break;
        case Phase::Orb:
            if (m_t > 1.1f) {
                m_phase = Phase::Shockwave; m_t = 0.f;
                spawn_burst(m_w * 0.5f, m_h * 0.42f, 70);
                m_shake = 14.f;
            }
            break;
        case Phase::Shockwave:
            if (m_t > 1.2f) { m_phase = Phase::Title; m_t = 0.f; }
            break;
        case Phase::Title:
            if (m_t > 1.6f) { m_phase = Phase::Hold; m_t = 0.f; }
            break;
        case Phase::Hold:
            if (m_t > 0.9f) { m_phase = Phase::FadeOut; m_t = 0.f; }
            break;
        case Phase::FadeOut:
            if (m_t > 0.65f) { m_phase = Phase::Done; m_t = 0.f; }
            break;
        case Phase::Done:
            return true;
    }
    return false;
}

void BootScreen::draw_ring(SDL_Renderer* ren, float cx, float cy, float radius, float thickness,
                           u8 r, u8 g, u8 b, u8 a) {
    if (a == 0 || radius < 1.f) return;
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, r, g, b, a);
    const int segments = 128;
    for (int i = 0; i < segments; i++) {
        float a0 = (i / float(segments)) * 6.2831853f;
        float a1 = ((i + 1) / float(segments)) * 6.2831853f;
        for (float t = 0; t < thickness; t += 1.0f) {
            float rr = radius - t;
            if (rr < 1.f) continue;
            SDL_RenderDrawLine(ren,
                int(cx + std::cos(a0) * rr), int(cy + std::sin(a0) * rr),
                int(cx + std::cos(a1) * rr), int(cy + std::sin(a1) * rr));
        }
    }
}

void BootScreen::draw_orb(SDL_Renderer* ren, float cx, float cy, float radius, float alpha, float pulse) {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    int R = int(radius * (1.f + pulse * 0.08f));
    for (int y = -R; y <= R; y++) {
        for (int x = -R; x <= R; x++) {
            float d = std::sqrt(float(x * x + y * y));
            if (d > radius * 1.15f) continue;
            float edge = 1.f - (d / (radius * 1.15f));
            float glow = edge * edge;
            float core = d < radius * 0.35f ? 1.f : glow;
            u8 r = u8(std::min(255.f, 140.f + core * 115.f));
            u8 g = u8(std::min(255.f, 200.f + core * 55.f));
            u8 b = 255;
            u8 a = u8(std::clamp(alpha * (0.08f + glow * 0.92f) * 255.f, 0.f, 255.f));
            SDL_SetRenderDrawColor(ren, r, g, b, a);
            SDL_RenderDrawPoint(ren, int(cx) + x, int(cy) + y);
        }
    }
    // Outer halo rings
    draw_ring(ren, cx, cy, radius * 1.35f, 2.f, 60, 180, 255, u8(50 * alpha));
    draw_ring(ren, cx, cy, radius * 1.7f, 1.5f, 30, 120, 220, u8(30 * alpha));
}

void BootScreen::draw_particles(SDL_Renderer* ren) {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < m_part_count; i++) {
        const auto& p = m_parts[i];
        float t = p.life / p.max_life;
        u8 a = u8(std::clamp(t * 255.f, 0.f, 255.f));
        SDL_SetRenderDrawColor(ren, p.r, p.g, p.b, a);
        int s = int(p.size * (0.5f + t));
        SDL_Rect rc{int(p.x) - s / 2, int(p.y) - s / 2, std::max(1, s), std::max(1, s)};
        SDL_RenderFillRect(ren, &rc);
    }
}

void BootScreen::draw_scanlines(SDL_Renderer* ren, u8 alpha) {
    if (alpha == 0) return;
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, alpha);
    for (int y = 0; y < m_h; y += 3)
        SDL_RenderDrawLine(ren, 0, y, m_w, y);
}

void BootScreen::draw_vignette(SDL_Renderer* ren, float strength) {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    int bands = 48;
    for (int i = 0; i < bands; i++) {
        float t = float(i) / float(bands);
        u8 a = u8(t * t * strength * 180.f);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, a);
        SDL_Rect top{0, i, m_w, 1};
        SDL_Rect bot{0, m_h - 1 - i, m_w, 1};
        SDL_RenderFillRect(ren, &top);
        SDL_RenderFillRect(ren, &bot);
        SDL_Rect left{i, 0, 1, m_h};
        SDL_Rect right{m_w - 1 - i, 0, 1, m_h};
        SDL_RenderFillRect(ren, &left);
        SDL_RenderFillRect(ren, &right);
    }
}

void BootScreen::fill_gradient(SDL_Renderer* ren) {
    for (int y = 0; y < m_h; y++) {
        float t = float(y) / float(std::max(1, m_h - 1));
        float mid = 1.f - std::fabs(t - 0.4f) * 1.4f;
        mid = std::clamp(mid, 0.f, 1.f);
        u8 r = u8(2 + mid * 10);
        u8 g = u8(4 + mid * 28);
        u8 b = u8(10 + mid * 40);
        SDL_SetRenderDrawColor(ren, r, g, b, 255);
        SDL_RenderDrawLine(ren, 0, y, m_w, y);
    }
}

void BootScreen::render(SDL_Renderer* ren) {
    float sx = (m_shake > 0.1f) ? ((float(rand()) / RAND_MAX) - 0.5f) * m_shake * 2.f : 0.f;
    float sy = (m_shake > 0.1f) ? ((float(rand()) / RAND_MAX) - 0.5f) * m_shake * 2.f : 0.f;
    float cx = m_w * 0.5f + sx;
    float cy = m_h * 0.42f + sy;

    fill_gradient(ren);

    if (m_phase == Phase::Black) {
        draw_vignette(ren, 1.f);
        return;
    }

    float fade = 1.f;
    if (m_phase == Phase::FadeOut) fade = 1.f - std::min(1.f, m_t / 0.65f);

    // Shockwave rings
    if (m_phase == Phase::Shockwave || m_phase == Phase::Title || m_phase == Phase::Hold || m_phase == Phase::FadeOut) {
        float base_t = (m_phase == Phase::Shockwave) ? m_t : 1.2f + m_t * 0.15f;
        for (int i = 0; i < 5; i++) {
            float pulse = base_t * 1.1f + i * 0.22f;
            float rad = 40.f + pulse * 140.f;
            float a = fade * std::max(0.f, 1.f - pulse / 2.8f) * 0.85f;
            u8 aa = u8(std::clamp(a * 200.f, 0.f, 200.f));
            draw_ring(ren, cx, cy, rad, 2.5f + i * 0.3f, 0, 200, 255, aa);
            draw_ring(ren, cx, cy, rad * 0.92f, 1.f, 120, 220, 255, u8(aa / 2));
        }
    }

    // Orb
    if (m_phase == Phase::Orb || m_phase == Phase::Shockwave || m_phase == Phase::Title ||
        m_phase == Phase::Hold || m_phase == Phase::FadeOut) {
        float grow = 1.f;
        float alpha = fade;
        if (m_phase == Phase::Orb) {
            grow = std::min(1.f, m_t / 0.85f);
            grow = 1.f - (1.f - grow) * (1.f - grow) * (1.f - grow);
            alpha *= std::min(1.f, m_t / 0.25f);
        }
        float pulse = std::sin(m_total * 5.f) * 0.5f + 0.5f;
        draw_orb(ren, cx, cy, 22.f + grow * 58.f, alpha, pulse);
    }

    // Tiny spark point during Spark
    if (m_phase == Phase::Spark) {
        float a = std::min(1.f, m_t * 3.f);
        draw_orb(ren, cx, cy, 6.f + m_t * 20.f, a, 1.f);
    }

    draw_particles(ren);

    // Title
    if (m_phase == Phase::Title || m_phase == Phase::Hold || m_phase == Phase::FadeOut) {
        float ta = fade;
        if (m_phase == Phase::Title) ta *= std::min(1.f, m_t / 0.5f);

        const char* letters = "KORA";
        int scale = 6;
        int letter_w = 5 * scale + 4;
        int total_w = 4 * letter_w;
        int start_x = (m_w - total_w) / 2;

        for (int i = 0; i < 4; i++) {
            float delay = i * 0.12f;
            float la = ta;
            if (m_phase == Phase::Title)
                la *= std::clamp((m_t - delay) / 0.35f, 0.f, 1.f);
            if (la <= 0.01f) continue;
            char buf[2] = { letters[i], 0 };
            float bob = std::sin(m_total * 3.f + i) * 2.f;
            int lx = start_x + i * letter_w;
            int ly = int(cy + 95 + bob);
            // Soft glow pass
            draw_string(ren, lx + 1, ly + 1, buf, u8(0 * la), u8(80 * la), u8(120 * la), scale);
            draw_string(ren, lx, ly, buf, u8(200 * la), u8(240 * la), 255, scale);
        }

        // Accent line
        float line_a = ta;
        if (m_phase == Phase::Title) line_a *= std::clamp((m_t - 0.5f) / 0.4f, 0.f, 1.f);
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 0, 220, 255, u8(line_a * 230));
        int lx0 = m_w / 2 - int(60 * line_a);
        int lx1 = m_w / 2 + int(60 * line_a);
        SDL_RenderDrawLine(ren, lx0, int(cy + 145), lx1, int(cy + 145));
        SDL_RenderDrawLine(ren, lx0, int(cy + 146), lx1, int(cy + 146));

        // Subtitles
        float sub = ta;
        if (m_phase == Phase::Title) sub *= std::clamp((m_t - 0.7f) / 0.4f, 0.f, 1.f);
        if (sub > 0.05f) {
            const char* l2 = "PLAYSTATION 1";
            const char* l3 = "PlaySaffy 96";
            int w2 = int(std::strlen(l2)) * 11;
            int w3 = int(std::strlen(l3)) * 11;
            draw_string(ren, (m_w - w2) / 2, int(cy + 160), l2,
                        u8(100 * sub), u8(180 * sub), u8(220 * sub), 2);
            draw_string(ren, (m_w - w3) / 2, int(cy + 188), l3,
                        u8(70 * sub), u8(130 * sub), u8(160 * sub), 2);
        }
    }

    draw_scanlines(ren, 28);
    draw_vignette(ren, 0.85f);

    if (m_total > 1.2f && m_phase != Phase::Done && m_phase != Phase::FadeOut) {
        draw_string(ren, m_w / 2 - 90, m_h - 36, "Press any key to skip", 70, 90, 110, 2);
    }

    // Fade to black overlay
    if (m_phase == Phase::FadeOut) {
        u8 a = u8(std::min(1.f, m_t / 0.65f) * 255.f);
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, a);
        SDL_Rect full{0, 0, m_w, m_h};
        SDL_RenderFillRect(ren, &full);
    }
}

} // namespace ps96
