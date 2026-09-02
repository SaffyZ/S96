#include "frontend/Frontend.hpp"
#include "frontend/Text.hpp"
#include "common/Log.hpp"
#include <cstring>
#include <algorithm>
#include <thread>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <cmath>

namespace ps96 {

namespace {
const u8 FONT5x7[][7] = {
    {0x6,0x9,0x9,0x9,0x9,0x9,0x6},{0x2,0x6,0x2,0x2,0x2,0x2,0x7},
    {0x6,0x9,0x1,0x2,0x4,0x8,0xF},{0x6,0x9,0x1,0x6,0x1,0x9,0x6},
    {0x1,0x3,0x5,0x9,0xF,0x1,0x1},{0xF,0x8,0xE,0x1,0x1,0x9,0x6},
    {0x6,0x8,0x8,0xE,0x9,0x9,0x6},{0xF,0x1,0x2,0x4,0x4,0x4,0x4},
    {0x6,0x9,0x9,0x6,0x9,0x9,0x6},{0x6,0x9,0x9,0x7,0x1,0x1,0x6},
};
void put_px(u8* fb, int w, int h, int x, int y, u8 r, u8 g, u8 b) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    int i = (y * w + x) * 4;
    fb[i] = r; fb[i+1] = g; fb[i+2] = b; fb[i+3] = 255;
}
void fill_rect_fb(u8* fb, int w, int h, int x0, int y0, int rw, int rh, u8 r, u8 g, u8 b) {
    for (int y = y0; y < y0 + rh; y++)
        for (int x = x0; x < x0 + rw; x++)
            put_px(fb, w, h, x, y, r, g, b);
}
}

Frontend::Frontend() = default;
Frontend::~Frontend() { shutdown(); }

void Frontend::set_start_mode(AppMode mode) {
    if (mode == AppMode::BootIntro) {
        m_mode = AppMode::BootIntro;
        m_mode_after_boot = AppMode::Dashboard;
    } else {
        m_mode_after_boot = mode;
        m_mode = AppMode::BootIntro;
    }
}

void Frontend::set_base_dir(const std::string& dir) { m_base_dir = dir; }

bool Frontend::init() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        Log::error("SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    // Input owns SDL controller handles, so initialize it only after SDL has
    // been initialized. Constructing Input before SDL_Init made a controller
    // that was already plugged in at process start invisible to game input.
    m_input.initialize();

    m_window = SDL_CreateWindow("PS96 — Kora", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                m_win_w, m_win_h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
    if (!m_window) { Log::error("Window failed: %s", SDL_GetError()); return false; }

    // Presentation must not clock the emulated machine. PCSX-Redux keeps
    // emulation timing separate from rendering; PS96 now does the same.
    m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_ACCELERATED);
    if (!m_renderer)
        m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_SOFTWARE);
    if (!m_renderer) { Log::error("Renderer failed: %s", SDL_GetError()); return false; }
    SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);

    m_present_w = 320; m_present_h = 240;
    m_texture = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, m_present_w, m_present_h);
    if (!m_texture) { Log::error("Texture failed: %s", SDL_GetError()); return false; }
    m_fb.resize(size_t(m_present_w) * size_t(m_present_h) * 4, 0);

    SDL_AudioSpec want{}, have{};
    want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 512;
    want.callback = audio_callback; want.userdata = this;
    m_audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (m_audio) {
        SDL_PauseAudioDevice(m_audio, 0);
        Log::info("Audio: %d Hz, %d ch, device=%u", have.freq, have.channels, (unsigned)m_audio);
    } else {
        Log::warn("Audio device open failed: %s", SDL_GetError());
    }

    m_config.load((std::filesystem::path(m_base_dir) / "ps96.cfg").string());
    // Video modes control the emulated framebuffer/presentation path, not the
    // physical host window. Native mode must stay comfortably visible and
    // scale its real 240-line framebuffer into the host window; shrinking the
    // whole SDL window to 320x240 made fullscreen/native testing unnecessarily
    // awkward without making the emulation any more accurate.
    if (m_config.video_mode == VideoMode::Overclocked60) {
        m_win_w = 1920; m_win_h = 1080;
        SDL_SetWindowSize(m_window, m_win_w, m_win_h);
    }
    if (m_config.bios_path.empty())
        m_config.bios_path = GamesScanner::find_bios(m_base_dir);
    if (!m_config.bios_path.empty() && m_emu.load_bios(m_config.bios_path)) {
        m_bios_display_name = std::filesystem::path(m_config.bios_path).filename().string();
        m_status = "BIOS ready";
        Log::info("BIOS loaded: %s", m_config.bios_path.c_str());
    }

    m_emu.load_memory_card(0, m_config.memcard0_path);
    m_emu.load_memory_card(1, m_config.memcard1_path);

    m_dash.set_window_size(m_win_w, m_win_h);
    m_dash.set_bios_loaded(m_emu.bios_loaded());
    m_dash.set_bios_name(m_bios_display_name);
    m_dash.set_skip_ps_boot(m_config.skip_ps_boot);
    m_dash.set_video_mode(m_config.video_mode);
    m_dash.set_theme(m_config.theme);
    m_boot.set_size(m_win_w, m_win_h);
    m_boot.reset();
    refresh_games();

    if (m_skip_boot_intro) {
        m_mode = m_mode_after_boot;
        if (m_mode == AppMode::Emulating && ensure_bios()) {
            m_emu.reset();
            m_emu.set_paused(false);
        }
    }

    m_running = true;
    Log::info("PS96 ready — mode %d", int(m_mode));
    return true;
}

void Frontend::shutdown() {
    m_config.save((std::filesystem::path(m_base_dir) / "ps96.cfg").string());
    if (m_audio) SDL_CloseAudioDevice(m_audio);
    if (m_texture) SDL_DestroyTexture(m_texture);
    if (m_renderer) SDL_DestroyRenderer(m_renderer);
    if (m_window) SDL_DestroyWindow(m_window);
    SDL_Quit();
    m_audio = 0; m_texture = nullptr; m_renderer = nullptr; m_window = nullptr;
}

void Frontend::audio_callback(void* userdata, u8* stream, int len) {
    auto* self = static_cast<Frontend*>(userdata);
    std::memset(stream, 0, len);
    if (self->m_mode != AppMode::Emulating) return;
    int samples = len / 4; // stereo S16
    self->m_emu.spu().mix(reinterpret_cast<s16*>(stream), samples);
}

void Frontend::refresh_games() {
    m_games = GamesScanner::scan(m_base_dir);
    m_dash.set_games(m_games);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%zu game(s)", m_games.size());
    m_dash.set_status(buf);
}

bool Frontend::ensure_bios() {
    if (m_emu.bios_loaded()) return true;
    if (!m_config.bios_path.empty() && m_emu.load_bios(m_config.bios_path)) {
        m_bios_display_name = std::filesystem::path(m_config.bios_path).filename().string();
        m_dash.set_bios_loaded(true);
        m_dash.set_bios_name(m_bios_display_name);
        return true;
    }
    auto found = GamesScanner::find_bios(m_base_dir);
    if (!found.empty() && m_emu.load_bios(found)) {
        m_config.bios_path = found;
        m_bios_display_name = std::filesystem::path(found).filename().string();
        m_dash.set_bios_loaded(true);
        m_dash.set_bios_name(m_bios_display_name);
        return true;
    }
    m_status = "No BIOS";
    return false;
}

void Frontend::enter_dashboard() {
    m_mode = AppMode::Dashboard;
    m_show_launch_banner = false;
    m_emu.set_paused(true);
    SDL_SetWindowTitle(m_window, "PS96 — Kora Dashboard");
    m_dash.set_skip_ps_boot(m_config.skip_ps_boot);
    m_dash.set_video_mode(m_config.video_mode);
    refresh_games();
}

void Frontend::enter_emulating(bool with_disc, const GameEntry* game) {
    if (!ensure_bios()) {
        m_status = "Cannot start without BIOS";
        return;
    }
    m_emu.reset();

    if (with_disc && game) {
        bool ok = false;
        std::string used;
        if (!game->cue_path.empty()) {
            ok = m_emu.load_disc(game->cue_path);
            used = game->cue_path;
        }
        if (!ok && !game->bin_path.empty()) {
            Log::warn("CUE load failed — trying BIN directly: %s", game->bin_path.c_str());
            ok = m_emu.load_disc(game->bin_path);
            used = game->bin_path;
        }
        if (!ok) {
            m_status = "Failed to load disc — check BIN next to CUE";
            Log::error("Disc load failed for %s", game->folder_name.c_str());
            // Stay on dashboard rather than black screen
            m_mode = AppMode::Dashboard;
            m_dash.set_status("Disc load failed");
            return;
        }
        m_status = "Running: " + game->folder_name;
        m_launch_title = game->folder_name;
        m_show_launch_banner = true;
        m_banner_timer = 0.f;
        Log::info("Launching: %s (%s)", game->folder_name.c_str(), used.c_str());

        // Full BIOS is the normal path. Fast boot is an explicit opt-in only.
        if (m_config.skip_ps_boot) {
            if (m_emu.fast_boot_disc()) {
                m_status = "Fast boot: " + game->folder_name;
                Log::info("Fast boot engaged (skip BIOS intro + license gate)");
            } else {
                Log::warn("Fast boot failed — falling back to the real BIOS path");
                // Keep disc loaded; reset CPU only
                m_emu.reset();
                m_emu.load_disc(used);
            }
        } else {
            Log::info("Full BIOS boot (skip_ps_boot OFF)");
        }
    } else {
        m_status = "BIOS only";
        m_show_launch_banner = false;
        m_launch_title.clear();
    }

    m_cycle_budget = 0.0;
    m_last_presented_frame = 0;
    m_mode = AppMode::Emulating;
    m_emu.set_paused(false);
    SDL_SetWindowTitle(m_window, "PS96 — Emulating");
}

void Frontend::run() {
    using clock = std::chrono::steady_clock;

    // Guest execution is driven by elapsed emulated master-clock time, not by
    // how quickly the host renderer happens to finish a frame. This is the
    // critical distinction between an emulator that merely displays at 60 Hz
    // and one that keeps CPU/GPU/CD/SIO timing coherent when rendering gets
    // expensive. One PS1 master clock is 33,868,800 Hz.
    constexpr double kMasterClockHz = 33'868'800.0;
    constexpr double kMaxCreditSeconds = 0.100; // bound runaway catch-up
    constexpr s64 kGuestSlice = 65536;

    auto last_guest_time = clock::now();
    auto next_present = clock::now();
    double guest_credit = 0.0;
    u64 host_measure_frames = 0;
    auto host_measure_start = clock::now();

    while (m_running) {
        process_events();

        if (m_mode != AppMode::Emulating) {
            update();
            render();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            last_guest_time = clock::now();
            next_present = last_guest_time;
            guest_credit = 0.0;
            continue;
        }

        m_input.update(m_emu.controllers());

        if (m_emu.paused()) {
            update();
            render();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            last_guest_time = clock::now();
            next_present = last_guest_time;
            guest_credit = 0.0;
            continue;
        }

        const auto now = clock::now();
        const double elapsed = std::chrono::duration<double>(now - last_guest_time).count();
        last_guest_time = now;
        guest_credit = std::min(kMaxCreditSeconds, guest_credit + std::max(0.0, elapsed));

        const double budget_cycles = guest_credit * kMasterClockHz;
        s64 cycles_to_run = static_cast<s64>(budget_cycles);
        cycles_to_run = std::min<s64>(cycles_to_run, static_cast<s64>(kMaxCreditSeconds * kMasterClockHz));

        while (cycles_to_run >= kGuestSlice && m_running && m_mode == AppMode::Emulating && !m_emu.paused()) {
            // Run the emulated machine in reasonably large slices. Polling SDL
            // and rebuilding the controller state every 4,096 CPU cycles was
            // catastrophically expensive: a 33.8 MHz PS1 required more than
            // eight thousand host-side input/event passes per second. That
            // overhead was large enough to become the dominant cost in the
            // software renderer and was directly responsible for the 5-15 FPS
            // slideshow seen in real games. One 65,536-cycle slice is ~1.94 ms
            // of guest time, while the outer loop continues to service input
            // between slices.
            m_emu.run_cycles(kGuestSlice);
            cycles_to_run -= kGuestSlice;
            guest_credit -= double(kGuestSlice) / kMasterClockHz;
            if ((cycles_to_run >= kGuestSlice) && m_running && m_mode == AppMode::Emulating && !m_emu.paused()) {
                process_events();
                if (!m_running || m_mode != AppMode::Emulating) break;
                m_input.update(m_emu.controllers());
            }
        }

        // Preserve sub-slice elapsed time so the next iteration continues from
        // the exact same guest clock rather than throwing away fractional CPU
        // cycles every host loop.
        if (cycles_to_run > 0 && m_running && m_mode == AppMode::Emulating && !m_emu.paused()) {
            const s64 small = cycles_to_run;
            m_emu.run_cycles(small);
            guest_credit -= double(small) / kMasterClockHz;
        }

        guest_credit = std::max(0.0, guest_credit);

        const auto present_now = clock::now();
        const bool pal = m_emu.gpu().is_pal();
        const auto present_period = pal
            ? std::chrono::milliseconds(20)
            : std::chrono::microseconds(16667);

        if (present_now >= next_present) {
            update();
            render();
            ++host_measure_frames;

            // Advance the presentation deadline by the region clock rather
            // than resetting it to "now". This prevents small render stalls
            // from becoming permanent timing drift while also avoiding a
            // burst of catch-up presents after a heavy frame.
            next_present += present_period;
            if (next_present + present_period < present_now)
                next_present = present_now + present_period;

            if (Log::level() <= LogLevel::Debug && host_measure_frames >= 60) {
                const double seconds = std::chrono::duration<double>(present_now - host_measure_start).count();
                const double fps = seconds > 0.0 ? double(host_measure_frames) / seconds : 0.0;
                Log::debug("HOST PRESENT STATS: frames=%llu elapsed=%.3fs fps=%.2f guest_frame=%llu scanline=%d credit=%.3fms",
                           static_cast<unsigned long long>(host_measure_frames), seconds, fps,
                           static_cast<unsigned long long>(m_emu.frame_count()), m_emu.gpu().scanline(),
                           guest_credit * 1000.0);
                host_measure_start = present_now;
                host_measure_frames = 0;
            }
        } else {
            // Yield only while the guest is caught up. We intentionally do not
            // sleep for a whole video frame here: that was the old source of
            // slideshow behavior when the software renderer became expensive.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

void Frontend::process_events() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) { m_running = false; return; }
        if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            m_win_w = e.window.data1; m_win_h = e.window.data2;
            m_dash.set_window_size(m_win_w, m_win_h);
            m_boot.set_size(m_win_w, m_win_h);
        }

        // Controller connect/disconnect must be tracked regardless of which
        // screen is active. SDL only fires CONTROLLERDEVICEADDED once per
        // connection — including once for a controller that was already
        // plugged in before the app started, right after the game
        // controller subsystem initializes. Previously this event only
        // reached Input::handle_event() while mode==Emulating, so a
        // controller connected at launch had its one and only ADDED event
        // silently dropped during the boot intro / dashboard, leaving it
        // permanently undetected for the rest of the session even though
        // it was plugged in the whole time.
        if (e.type == SDL_CONTROLLERDEVICEADDED || e.type == SDL_CONTROLLERDEVICEREMOVED) {
            m_input.handle_event(e);
        }

        if (m_mode == AppMode::BootIntro) {
            if (e.type == SDL_KEYDOWN || e.type == SDL_CONTROLLERBUTTONDOWN ||
                e.type == SDL_MOUSEBUTTONDOWN)
                m_boot.skip();
            continue;
        }

        if (m_mode == AppMode::Dashboard) {
            m_dash.handle_event(e);
        } else if (m_mode == AppMode::BootSelect) {
            if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.sym == SDLK_1 || e.key.keysym.sym == SDLK_RETURN)
                    enter_dashboard();
                else if (e.key.keysym.sym == SDLK_2 || e.key.keysym.sym == SDLK_b)
                    enter_emulating(false, nullptr);
                else if (e.key.keysym.sym == SDLK_ESCAPE)
                    m_running = false;
            } else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_A) enter_dashboard();
                else if (e.cbutton.button == SDL_CONTROLLER_BUTTON_B) enter_emulating(false, nullptr);
            }
        } else if (m_mode == AppMode::Emulating) {
            m_input.handle_event(e);
            if (e.type == SDL_KEYDOWN) {
                switch (e.key.keysym.sym) {
                    case SDLK_ESCAPE: enter_dashboard(); break;
                    case SDLK_F5: m_emu.reset(); m_status = "Reset"; break;
                    case SDLK_SPACE: m_emu.set_paused(!m_emu.paused()); break;
                    case SDLK_F11: {
                        Uint32 flags = SDL_GetWindowFlags(m_window);
                        SDL_SetWindowFullscreen(m_window,
                            (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                        break;
                    }
                    case SDLK_F1:
                        if (ensure_bios()) { m_emu.reset(); m_status = "BIOS reloaded"; }
                        break;
                    default: break;
                }
            }
        }
    }
}

void Frontend::update() {
    if (m_mode == AppMode::BootIntro) {
        if (m_boot.finished()) {
            m_mode = m_mode_after_boot;
            if (m_mode == AppMode::Emulating && ensure_bios()) {
                m_emu.reset();
                m_emu.set_paused(false);
            } else if (m_mode == AppMode::Dashboard) {
                enter_dashboard();
            }
        }
        return;
    }

    if (m_mode == AppMode::Dashboard) {
        m_dash.set_input_device(m_input.device_name());
        m_dash.update();
        if (m_dash.video_mode() != m_config.video_mode) {
            m_config.video_mode = m_dash.video_mode();
            if (m_config.video_mode == VideoMode::Overclocked60) {
                // Enhanced presentation target: 1080p host window. The guest
                // GPU remains at its real PS1 resolution/timing.
                m_win_w = 1920; m_win_h = 1080;
                SDL_SetWindowSize(m_window, m_win_w, m_win_h);
            }
            // Native mode changes ONLY the guest framebuffer presentation to
            // 240p. Do not shrink the host window: fullscreen/native should
            // scale the 240p image cleanly into the existing display.
            m_dash.set_window_size(m_win_w, m_win_h);
            m_boot.set_size(m_win_w, m_win_h);
            m_config.save((std::filesystem::path(m_base_dir) / "ps96.cfg").string());
            Log::info("Video mode = %s", m_config.video_mode == VideoMode::Native240p ? "native240p" : "overclocked60");
        }
        if (m_dash.theme() != m_config.theme) {
            m_config.theme = m_dash.theme();
            m_config.save((std::filesystem::path(m_base_dir) / "ps96.cfg").string());
            Log::info("Kora theme changed");
        }
        switch (m_dash.consume_action()) {
            case DashAction::LaunchGame:
                if (auto* g = m_dash.selected_game()) enter_emulating(true, g);
                break;
            case DashAction::BootBiosOnly: enter_emulating(false, nullptr); break;
            case DashAction::ToggleSkipBoot:
                m_config.skip_ps_boot = m_dash.skip_ps_boot();
                m_config.save((std::filesystem::path(m_base_dir) / "ps96.cfg").string());
                Log::info("skip_ps_boot = %s", m_config.skip_ps_boot ? "ON" : "OFF");
                break;
            case DashAction::RefreshLibrary: refresh_games(); break;
            case DashAction::Quit: m_running = false; break;
            default: break;
        }
    } else if (m_mode == AppMode::Emulating) {
        if (m_show_launch_banner) {
            m_banner_timer += 1.f / 60.f;
            if (m_banner_timer > 4.5f) m_show_launch_banner = false; // auto-dismiss
        }
    }
}

void Frontend::draw_soft_text(u8* fb, int w, int h, int x, int y, const char* s, u8 r, u8 g, u8 b) {
    while (*s) {
        if (*s == ' ') { x += 12; s++; continue; }
        int idx = (*s >= '0' && *s <= '9') ? *s - '0' : -1;
        if (idx >= 0) {
            for (int row = 0; row < 7; row++) {
                u8 bits = FONT5x7[idx][row];
                for (int col = 0; col < 4; col++)
                    if (bits & (8 >> col))
                        for (int sy = 0; sy < 2; sy++)
                            for (int sx = 0; sx < 2; sx++)
                                put_px(fb, w, h, x + col * 2 + sx, y + row * 2 + sy, r, g, b);
            }
        }
        x += 12; s++;
    }
}

void Frontend::render_boot_select() {
    for (int y = 0; y < m_win_h; y++) {
        float t = float(y) / float(std::max(1, m_win_h - 1));
        u8 r = u8(8 + t * 12);
        u8 g = u8(14 + (1.f - std::fabs(t - 0.35f)) * 28);
        u8 b = u8(22 + (1.f - t) * 30);
        SDL_SetRenderDrawColor(m_renderer, r, g, b, 255);
        SDL_RenderDrawLine(m_renderer, 0, y, m_win_w, y);
    }
    int cx = m_win_w / 2, cy = m_win_h / 2;
    auto card = [&](int x, int y, int w, int h, bool primary) {
        SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(m_renderer, 16, 28, 40, 230);
        SDL_Rect r{x, y, w, h}; SDL_RenderFillRect(m_renderer, &r);
        SDL_SetRenderDrawColor(m_renderer, primary ? 0 : 60, primary ? 190 : 140, primary ? 230 : 160, 255);
        SDL_RenderDrawRect(m_renderer, &r);
    };
    card(cx - 300, cy - 90, 280, 180, true);
    card(cx + 20, cy - 90, 280, 180, false);
    draw_string(m_renderer, cx - 260, cy - 50, "1  KORA DASHBOARD", 100, 220, 255, 2);
    draw_string(m_renderer, cx - 260, cy - 10, "Games library + covers", 160, 190, 210, 2);
    draw_string(m_renderer, cx + 60, cy - 50, "2  BOOT BIOS", 180, 200, 220, 2);
    draw_string(m_renderer, cx + 60, cy - 10, "No disc — BIOS only", 140, 160, 180, 2);
    draw_string(m_renderer, cx - 140, 50, "PS96  PLAYSAFFY 96", 0, 200, 255, 3);
    draw_string(m_renderer, cx - 200, m_win_h - 56, "Enter = Dashboard    B = BIOS    Esc = Quit", 120, 140, 160, 2);
    SDL_RenderPresent(m_renderer);
}

void Frontend::render_launch_banner() {
    int pw = std::min(700, m_win_w - 80);
    int ph = 120;
    int px = (m_win_w - pw) / 2;
    int py = 36;
    float fade = 1.f;
    if (m_banner_timer > 3.5f) fade = 1.f - (m_banner_timer - 3.5f) / 1.0f;
    u8 a = u8(std::clamp(fade * 230.f, 0.f, 230.f));

    SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(m_renderer, 10, 20, 32, a);
    SDL_Rect panel{px, py, pw, ph};
    SDL_RenderFillRect(m_renderer, &panel);
    SDL_SetRenderDrawColor(m_renderer, 0, 190, 230, a);
    SDL_RenderDrawRect(m_renderer, &panel);

    char line[256];
    std::snprintf(line, sizeof(line), "Launching  %s", m_launch_title.c_str());
    draw_string(m_renderer, px + 24, py + 28, line, 180, 230, 255, 2);
    draw_string(m_renderer, px + 24, py + 60, "Disc loaded — emulator running", 140, 180, 200, 2);
    draw_string(m_renderer, px + 24, py + 88, "Esc = Dashboard", 100, 140, 160, 2);
}

void Frontend::ensure_video_texture(int w, int h) {
    w = std::max(1,w); h=std::max(1,h);
    if (m_texture && m_present_w==w && m_present_h==h) return;
    if (m_texture) SDL_DestroyTexture(m_texture);
    m_texture = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, w, h);
    m_present_w=w; m_present_h=h;
    m_fb.assign(size_t(w)*size_t(h)*4,0);
}

void Frontend::render_emulating() {
    const bool native = (m_config.video_mode == VideoMode::Native240p);

    // Never invent a 640x480 intermediate framebuffer for the "enhanced"
    // mode. The PS1 GPU always produces its actual display rectangle;
    // overclocked mode only translates that image to the host's 1080p window.
    const int psx_w = std::max(1, m_emu.gpu().display_width());
    const int psx_h = std::max(1, m_emu.gpu().display_height());
    const int tw = psx_w;
    const int th = native ? std::min(psx_h, 240) : psx_h;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    ensure_video_texture(tw, th);
    if (!m_emu.bios_loaded()) {
        std::fill(m_fb.begin(), m_fb.end(), 0);
        fill_rect_fb(m_fb.data(), tw, th, 0, 0, tw, th, 12, 14, 22);
        draw_soft_text(m_fb.data(), tw, th, 20, 20, "NO BIOS", 220, 80, 80);
    } else {
        m_emu.gpu().copy_display_to(m_fb.data(), tw, th);
    }

    SDL_SetRenderDrawColor(m_renderer, 5, 7, 13, 255); SDL_RenderClear(m_renderer);
    void* pixels=nullptr; int pitch=0;
    if (SDL_LockTexture(m_texture,nullptr,&pixels,&pitch)==0) {
        u8* dst=static_cast<u8*>(pixels);
        const int row_bytes=tw*4;
        for(int y=0;y<th;y++) std::memcpy(dst+y*pitch,m_fb.data()+size_t(y)*row_bytes,row_bytes);
        SDL_UnlockTexture(m_texture);
    }

    float aspect = 4.f/3.f; int rw=m_win_w,rh=m_win_h;
    if(float(rw)/rh>aspect) rw=int(rh*aspect); else rh=int(rw/aspect);
    // Native mode uses integer-ish 240p presentation. Enhanced mode keeps the
    // existing 640x480 current-resolution presentation. Both remain driven by
    // the real PS1 clock; this is presentation quality, not a game hack.
    SDL_Rect dst{(m_win_w-rw)/2,(m_win_h-rh)/2,rw,rh};
    SDL_RenderCopy(m_renderer,m_texture,nullptr,&dst);

    if (m_show_launch_banner) render_launch_banner();
    char title[384];
    std::snprintf(title,sizeof(title),"PS96 | %s | %s | Frame %llu | PC=%08X",
        m_status.c_str(), native ? "Native 240p" : "Overclocked 60 FPS",
        (unsigned long long)m_emu.frame_count(),m_emu.cpu().pc());
    SDL_SetWindowTitle(m_window,title);
    SDL_RenderPresent(m_renderer);
}

void Frontend::render() {
    if (m_mode == AppMode::BootIntro) {
        m_boot.render(m_renderer);
        SDL_RenderPresent(m_renderer);
    } else if (m_mode == AppMode::Dashboard) {
        m_dash.set_bios_loaded(m_emu.bios_loaded());
        m_dash.set_bios_name(m_bios_display_name);
        m_dash.render(m_renderer);
        SDL_RenderPresent(m_renderer);
    } else if (m_mode == AppMode::BootSelect) {
        render_boot_select();
    } else {
        render_emulating();
    }
}

} // namespace ps96
