#pragma once

#include "common/Types.hpp"
#include "frontend/GamesScanner.hpp"
#include "frontend/Config.hpp"
#include <SDL.h>
#include <string>
#include <vector>
#include <unordered_map>

namespace ps96 {

enum class DashAction {
    None,
    LaunchGame,
    BootBiosOnly,
    RefreshLibrary,
    Quit,
    ToggleSkipBoot
};

class Dashboard {
public:
    Dashboard();
    ~Dashboard();

    void set_window_size(int w, int h);
    void set_games(std::vector<GameEntry> games);
    void set_bios_loaded(bool v) { m_bios_loaded = v; }
    void set_bios_name(const std::string& n) { m_bios_name = n; }
    void set_status(const std::string& s) { m_status = s; }
    void set_renderer(SDL_Renderer* ren) { m_renderer = ren; }
    void set_skip_ps_boot(bool v) { m_skip_ps_boot = v; }
    bool skip_ps_boot() const { return m_skip_ps_boot; }
    void set_input_device(const std::string& name) { m_input_device = name; }
    void set_video_mode(VideoMode mode) { m_video_mode = mode; }
    VideoMode video_mode() const { return m_video_mode; }
    void set_theme(Theme t) { m_theme = t; }
    Theme theme() const { return m_theme; }

    void handle_event(const SDL_Event& e);
    void update();
    void render(SDL_Renderer* ren);

    DashAction consume_action();
    const GameEntry* selected_game() const;
    int selected_index() const { return m_selected; }

private:
    std::vector<GameEntry> m_games;
    int m_selected = 0;
    int m_scroll = 0;
    int m_win_w = 1280;
    int m_win_h = 720;
    bool m_bios_loaded = false;
    std::string m_bios_name;
    std::string m_status;
    DashAction m_pending = DashAction::None;
    float m_time = 0.f;
    SDL_Renderer* m_renderer = nullptr;

    bool m_show_settings = false;
    bool m_show_controls = false;
    bool m_skip_ps_boot = false;
    std::string m_input_device = "Keyboard";
    VideoMode m_video_mode = VideoMode::Native240p;
    Theme m_theme = Theme::Neon;
    int m_settings_item = 0;

    std::unordered_map<std::string, SDL_Texture*> m_covers;

    void ensure_covers_loaded();
    void clear_covers();
    SDL_Texture* cover_for(const GameEntry& g);

    void layout_metrics(int& header_h, int& footer_h, int& card_w, int& card_h,
                        int& gap, int& cols, int& content_top) const;
    void draw_background(SDL_Renderer* ren);
    void draw_header(SDL_Renderer* ren, int header_h);
    void draw_footer(SDL_Renderer* ren, int footer_h);
    void draw_game_grid(SDL_Renderer* ren, int content_top, int footer_h);
    void draw_empty_library(SDL_Renderer* ren);
    void draw_settings_panel(SDL_Renderer* ren);
    void draw_controls_panel(SDL_Renderer* ren);
    void draw_text(SDL_Renderer* ren, int x, int y, const char* text, u8 r, u8 g, u8 b, int scale = 2);
    void fill_round_rect(SDL_Renderer* ren, int x, int y, int w, int h, int radius, u8 r, u8 g, u8 b, u8 a = 255);
    void cover_color(const std::string& name, u8& r, u8& g, u8& b) const;
    void accent_color(u8& r, u8& g, u8& b) const;
    const char* theme_name() const;
};

} // namespace ps96
