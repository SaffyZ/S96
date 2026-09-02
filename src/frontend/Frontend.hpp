#pragma once

#include "common/Types.hpp"
#include "emulator/Emulator.hpp"
#include "frontend/Input.hpp"
#include "frontend/Config.hpp"
#include "frontend/Dashboard.hpp"
#include "frontend/GamesScanner.hpp"
#include "frontend/BootScreen.hpp"
#include <SDL.h>
#include <string>
#include <vector>

namespace ps96 {

enum class AppMode {
    BootIntro,    // Animated iconic boot screen
    BootSelect,   // Choose KoraD or BIOS
    Dashboard,    // Kora Dashboard
    Emulating     // Running BIOS / game
};

class Frontend {
public:
    Frontend();
    ~Frontend();

    bool init();
    void shutdown();
    void run();

    Emulator& emulator() { return m_emu; }
    Config& config() { return m_config; }

    void set_start_mode(AppMode mode);
    void set_base_dir(const std::string& dir);
    void set_skip_boot_intro(bool v) { m_skip_boot_intro = v; }

private:
    Emulator m_emu;
    Input m_input;
    Config m_config;
    Dashboard m_dash;
    BootScreen m_boot;

    SDL_Window* m_window = nullptr;
    SDL_Renderer* m_renderer = nullptr;
    SDL_Texture* m_texture = nullptr;
    SDL_AudioDeviceID m_audio = 0;

    int m_win_w = 1280;
    int m_win_h = 720;
    bool m_running = false;
    AppMode m_mode = AppMode::BootIntro;
    AppMode m_mode_after_boot = AppMode::Dashboard;
    bool m_skip_boot_intro = false;
    std::string m_base_dir = ".";
    std::string m_status;
    std::string m_bios_display_name;

    bool m_show_launch_banner = false;
    std::string m_launch_title;
    float m_banner_timer = 0.f;
    double m_cycle_budget = 0.0;
    u64 m_last_emu_us = 0;
    u64 m_last_presented_frame = 0;
    int m_present_w = 0;
    int m_present_h = 0;

    std::vector<u8> m_fb;
    std::vector<GameEntry> m_games;

    void process_events();
    void update();
    void render();
    void render_boot_select();
    void render_emulating();
    void ensure_video_texture(int w, int h);
    void render_launch_banner();
    void enter_dashboard();
    void enter_emulating(bool with_disc, const GameEntry* game);
    void refresh_games();
    bool ensure_bios();
    void draw_soft_text(u8* fb, int w, int h, int x, int y, const char* s, u8 r, u8 g, u8 b);
    static void audio_callback(void* userdata, u8* stream, int len);
};

} // namespace ps96
