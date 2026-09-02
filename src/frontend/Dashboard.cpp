#include "frontend/Dashboard.hpp"
#include "frontend/Text.hpp"
#include "frontend/ImageLoader.hpp"
#include "common/Log.hpp"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <cctype>

namespace ps96 {

Dashboard::Dashboard() = default;

Dashboard::~Dashboard() {
    clear_covers();
}

void Dashboard::clear_covers() {
    for (auto& kv : m_covers) {
        if (kv.second) SDL_DestroyTexture(kv.second);
    }
    m_covers.clear();
}

void Dashboard::set_window_size(int w, int h) {
    m_win_w = w > 0 ? w : 1280;
    m_win_h = h > 0 ? h : 720;
}

void Dashboard::set_games(std::vector<GameEntry> games) {
    clear_covers();
    m_games = std::move(games);
    if (m_selected >= static_cast<int>(m_games.size()))
        m_selected = m_games.empty() ? 0 : static_cast<int>(m_games.size()) - 1;
    m_scroll = 0;
}

void Dashboard::ensure_covers_loaded() {
    if (!m_renderer) return;
    for (auto& g : m_games) {
        if (!g.has_cover || g.cover_path.empty()) continue;
        if (m_covers.count(g.cover_path)) continue;
        SDL_Texture* t = load_texture(m_renderer, g.cover_path);
        m_covers[g.cover_path] = t; // may be nullptr; still cache miss-once
    }
}

SDL_Texture* Dashboard::cover_for(const GameEntry& g) {
    if (!g.has_cover || g.cover_path.empty()) return nullptr;
    auto it = m_covers.find(g.cover_path);
    if (it == m_covers.end()) return nullptr;
    return it->second;
}

const GameEntry* Dashboard::selected_game() const {
    if (m_games.empty() || m_selected < 0 || m_selected >= static_cast<int>(m_games.size()))
        return nullptr;
    return &m_games[m_selected];
}

DashAction Dashboard::consume_action() {
    DashAction a = m_pending;
    m_pending = DashAction::None;
    return a;
}

void Dashboard::layout_metrics(int& header_h, int& footer_h, int& card_w, int& card_h,
                               int& gap, int& cols, int& content_top) const {
    header_h = 92;
    footer_h = 64;
    gap = 24;
    content_top = header_h + 26;
    // Kora is intentionally a living-room style launcher: one large hero
    // tile with supporting tiles around it rather than a dense PC grid.
    cols = 5;
    card_w = 184;
    card_h = 248;
}

void Dashboard::cover_color(const std::string& name, u8& r, u8& g, u8& b) const {
    u32 h = 2166136261u;
    for (unsigned char c : name) { h ^= c; h *= 16777619u; }
    r = u8(30 + (h & 0x3F));
    g = u8(50 + ((h >> 7) & 0x5F));
    b = u8(90 + ((h >> 14) & 0x6F));
}

void Dashboard::accent_color(u8& r, u8& g, u8& b) const {
    switch (m_theme) {
        case Theme::Plasma:  r=165; g=95;  b=255; break;
        case Theme::Crimson: r=255; g=78;  b=110; break;
        case Theme::Carbon:  r=190; g=205; b=220; break;
        case Theme::Sunset:  r=255; g=145; b=72;  break;
        default:             r=55;  g=225; b=255; break;
    }
}

const char* Dashboard::theme_name() const {
    switch (m_theme) {
        case Theme::Plasma: return "PLASMA";
        case Theme::Crimson: return "CRIMSON";
        case Theme::Carbon: return "CARBON";
        case Theme::Sunset: return "SUNSET";
        default: return "NEON";
    }
}

void Dashboard::fill_round_rect(SDL_Renderer* ren, int x, int y, int w, int h, int radius,
                                u8 r, u8 g, u8 b, u8 a) {
    if (w <= 0 || h <= 0) return;
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, r, g, b, a);
    if (radius <= 0) {
        SDL_Rect full{x, y, w, h};
        SDL_RenderFillRect(ren, &full);
        return;
    }
    radius = std::min(radius, std::min(w, h) / 2);
    SDL_Rect body{x + radius, y, w - 2 * radius, h};
    SDL_RenderFillRect(ren, &body);
    SDL_Rect mid{x, y + radius, w, h - 2 * radius};
    SDL_RenderFillRect(ren, &mid);
    for (int i = 0; i < radius; i++) {
        float t = 1.f - float(i) / float(radius);
        int inset = int(radius * (1.f - std::sqrt(std::max(0.f, 1.f - (1.f - t) * (1.f - t)))));
        SDL_RenderDrawLine(ren, x + inset, y + i, x + w - 1 - inset, y + i);
        SDL_RenderDrawLine(ren, x + inset, y + h - 1 - i, x + w - 1 - inset, y + h - 1 - i);
    }
}

void Dashboard::draw_text(SDL_Renderer* ren, int x, int y, const char* text, u8 r, u8 g, u8 b, int scale) {
    draw_string(ren, x, y, text, r, g, b, scale);
}

void Dashboard::draw_background(SDL_Renderer* ren) {
    u8 ar,ag,ab; accent_color(ar,ag,ab);
    for (int y=0; y<m_win_h; ++y) {
        float t=float(y)/float(std::max(1,m_win_h-1));
        float wave=0.5f+0.5f*std::sin(m_time*0.35f+t*4.0f);
        u8 r=u8(3 + ar*0.08f + wave*ar*0.06f);
        u8 g=u8(6 + ag*0.12f + (1.f-t)*ag*0.08f);
        u8 b=u8(14 + ab*0.16f + t*ab*0.05f);
        SDL_SetRenderDrawColor(ren,r,g,b,255);
        SDL_RenderDrawLine(ren,0,y,m_win_w,y);
    }
    SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND);
    // Large moving light ribbons: a more premium console-dashboard feel.
    for (int band=0; band<3; ++band) {
        float phase=m_time*0.18f + band*1.9f;
        int cy=int(m_win_h*(0.30f+0.15f*std::sin(phase)));
        for (int w=26; w>0; --w) {
            u8 alpha=u8((26-w)*1.5f);
            SDL_SetRenderDrawColor(ren,ar,ag,ab,alpha);
            SDL_RenderDrawLine(ren,0,cy-w,m_win_w,cy+w);
        }
    }
    // Fine grid + vignette.
    SDL_SetRenderDrawColor(ren,ar,ag,ab,18);
    for(int x=0;x<m_win_w;x+=64) SDL_RenderDrawLine(ren,x,0,x,m_win_h);
    for(int y=0;y<m_win_h;y+=64) SDL_RenderDrawLine(ren,0,y,m_win_w,y);
    for(int x=0;x<90;x++){
        u8 a=u8((90-x)*0.55f);
        SDL_SetRenderDrawColor(ren,0,0,0,a);
        SDL_RenderDrawLine(ren,x,0,x,m_win_h);
        SDL_RenderDrawLine(ren,m_win_w-1-x,0,m_win_w-1-x,m_win_h);
    }
}

void Dashboard::draw_header(SDL_Renderer* ren, int header_h) {
    u8 ar,ag,ab; accent_color(ar,ag,ab);
    SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren,3,5,12,242);
    SDL_Rect bar{0,0,m_win_w,header_h}; SDL_RenderFillRect(ren,&bar);
    // Kora emblem.
    float pulse=0.5f+0.5f*std::sin(m_time*2.3f);
    for(int r=31;r>=5;--r){
        SDL_SetRenderDrawColor(ren,ar,ag,ab,u8(5+(31-r)*2));
        for(int a=0;a<64;a++){
            float t=a*6.2831853f/64.f;
            SDL_RenderDrawPoint(ren,44+int(std::cos(t)*r),43+int(std::sin(t)*r));
        }
    }
    SDL_SetRenderDrawColor(ren,230,250,255,255); SDL_Rect core{40,39,8,8}; SDL_RenderFillRect(ren,&core);
    draw_text(ren,78,12,"KORA",ar,ag,ab,6);
    draw_text(ren,80,58,"PS96  •  CONSOLE MODE",150,175,195,2);
    const char* tabs[]={"HOME","LIBRARY","SETTINGS"};
    int tx=365;
    for(int i=0;i<3;i++){
        bool active=(i==0)||((i==2)&&m_show_settings);
        draw_text(ren,tx,34,tabs[i],active?ar:120,active?ag:145,active?ab:165,active?3:2);
        if(active){SDL_SetRenderDrawColor(ren,ar,ag,ab,230);SDL_Rect u{tx,70,76,3};SDL_RenderFillRect(ren,&u);}
        tx+=128;
    }
    char status[192];
    std::snprintf(status,sizeof(status),"%s  •  %s",theme_name(),m_bios_loaded?"BIOS READY":"NO BIOS");
    int sw=int(std::strlen(status))*10;
    draw_text(ren,m_win_w-sw-28,35,status,ar,ag,ab,2);
    SDL_SetRenderDrawColor(ren,ar,ag,ab,95); SDL_RenderDrawLine(ren,0,header_h-1,m_win_w,header_h-1);
    (void)pulse;
}

void Dashboard::draw_footer(SDL_Renderer* ren, int footer_h) {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    for (int y = 0; y < footer_h; y++) {
        SDL_SetRenderDrawColor(ren, 6, 10, 18, 230);
        SDL_RenderDrawLine(ren, 0, m_win_h - footer_h + y, m_win_w, m_win_h - footer_h + y);
    }
    SDL_SetRenderDrawColor(ren, 0, 180, 220, 40);
    SDL_RenderDrawLine(ren, 0, m_win_h - footer_h, m_win_w, m_win_h - footer_h);

    u8 ar,ag,ab; accent_color(ar,ag,ab);
    draw_text(ren, 28, m_win_h - footer_h + 18,
              "A / ENTER  LAUNCH     Y / R  REFRESH     F2  SETTINGS     F1  CONTROLS", 145, 170, 195, 2);
    draw_text(ren, 28, m_win_h - footer_h + 39,
              "KORA", ar, ag, ab, 2);
    if (!m_status.empty()) {
        int sw = int(m_status.size()) * 11;
        draw_text(ren, m_win_w - sw - 28, m_win_h - footer_h + 20, m_status.c_str(), 70, 200, 255, 2);
    }
}

void Dashboard::draw_empty_library(SDL_Renderer* ren) {
    const char* lines[] = {
        "No games found",
        "",
        "Put titles in Games/<name>/",
        "  game.cue  +  game.bin  +  cover.png",
        "",
        "Kora will pick up covers automatically",
    };
    int y = m_win_h / 2 - 70;
    for (const char* line : lines) {
        int w = int(std::strlen(line)) * 11;
        draw_text(ren, (m_win_w - w) / 2, y, line, 140, 170, 200, 2);
        y += 26;
    }
}

void Dashboard::draw_game_grid(SDL_Renderer* ren, int content_top, int footer_h) {
    ensure_covers_loaded();
    if (m_games.empty()) return;

    const int cx = m_win_w / 2;
    const int hero_w = std::min(420, m_win_w / 3);
    const int hero_h = 500;
    const int hero_x = cx - hero_w/2;
    const int hero_y = content_top + 8;

    // Large atmospheric stage behind the selected title.
    auto* hero = selected_game();
    u8 hr=20,hg=80,hb=120;
    if (hero) cover_color(hero->folder_name,hr,hg,hb);
    fill_round_rect(ren, hero_x-110, hero_y-30, hero_w+220, hero_h+80, 32, hr/4, hg/5, hb/4, 100);

    auto draw_card = [&](int index, int x, int y, int w, int h, bool selected) {
        if (index < 0 || index >= (int)m_games.size()) return;
        const auto& g=m_games[index];
        u8 cr,cg,cb; cover_color(g.folder_name,cr,cg,cb);
        if (selected) {
            fill_round_rect(ren,x-8,y-8,w+16,h+16,24,0,200,255,48);
            fill_round_rect(ren,x-3,y-3,w+6,h+6,22,0,220,255,130);
        } else {
            fill_round_rect(ren,x+8,y+12,w,h,18,0,0,0,90);
        }
        SDL_Texture* tex=cover_for(g);
        if (tex) {
            SDL_Rect d{x,y,w,h}; SDL_RenderCopy(ren,tex,nullptr,&d);
        } else {
            for(int yy=0;yy<h;yy++){
                float t=float(yy)/std::max(1,h-1);
                SDL_SetRenderDrawColor(ren,u8(cr*(.55f+.45f*(1-t))),u8(cg*(.55f+.45f*(1-t))),u8(cb*(.62f+.38f*(1-t))),255);
                SDL_RenderDrawLine(ren,x,y+yy,x+w-1,y+yy);
            }
            std::string first=g.folder_name.empty()?"?":std::string(1,g.folder_name[0]);
            draw_text(ren,x+w/2-12,y+h/2-25,first.c_str(),245,250,255,5);
        }
        if (selected) {
            SDL_SetRenderDrawColor(ren,0,210,255,255); SDL_Rect line{x,y+h+8,w,4}; SDL_RenderFillRect(ren,&line);
        }
    };

    // Side cards / coverflow.
    for (int d=-2; d<=2; ++d) {
        int idx=m_selected+d;
        if (idx<0 || idx>=(int)m_games.size() || d==0) continue;
        int w = (d== -1 || d==1) ? 150 : 124;
        int h = (d== -1 || d==1) ? 208 : 170;
        int x = cx + d*190 - w/2;
        int y = hero_y + 68 + std::abs(d)*32;
        draw_card(idx,x,y,w,h,false);
    }
    draw_card(m_selected,hero_x,hero_y,hero_w,hero_h,true);

    if (hero) {
        std::string title=hero->folder_name; if(title.size()>30) title=title.substr(0,28)+"..";
        int tw=int(title.size())*12;
        draw_text(ren,cx-tw/2,hero_y+hero_h+26,title.c_str(),235,250,255,3);
        draw_text(ren,cx-142,hero_y+hero_h+58,hero->has_disc?"DISC READY":"NO DISC",hero->has_disc?80:225,hero->has_disc?235:105,hero->has_disc?165:105,2);
        draw_text(ren,cx-142,hero_y+hero_h+84,"A / ENTER   LAUNCH",130,200,225,2);
    }

    // Small runtime rail gives the screen a proper console-dashboard feel.
    int rail_y=m_win_h-footer_h-48;
    draw_text(ren,28,rail_y,"PS96 CORE",80,210,245,2);
    const char* vm = (m_video_mode == VideoMode::Native240p) ? "NATIVE 240P" : "OVERCLOCKED 60 FPS";
    draw_text(ren,m_win_w-210,rail_y,vm,120,225,255,2);
}

void Dashboard::handle_event(const SDL_Event& e) {
    if (e.type == SDL_KEYDOWN) {
        int cols = 4;
        { int hh, fh, cw, ch, gap, c, ct; layout_metrics(hh, fh, cw, ch, gap, c, ct); cols = c; }
        switch (e.key.keysym.sym) {
            case SDLK_LEFT: case SDLK_a:
                if (m_show_settings) { if(m_settings_item==0) m_video_mode = VideoMode::Native240p; else if(m_settings_item==1) m_theme = Theme((int(m_theme)+4)%5); }
                else if (m_selected > 0) m_selected--;
                break;
            case SDLK_RIGHT: case SDLK_d:
                if (m_show_settings) { if(m_settings_item==0) m_video_mode = VideoMode::Overclocked60; else if(m_settings_item==1) m_theme = Theme((int(m_theme)+1)%5); }
                else if (m_selected + 1 < int(m_games.size())) m_selected++;
                break;
            case SDLK_UP: case SDLK_w:
                if (m_show_settings) m_settings_item = std::max(0,m_settings_item-1);
                else if (m_selected - cols >= 0) m_selected -= cols;
                break;
            case SDLK_DOWN: case SDLK_s:
                if (m_show_settings) m_settings_item = std::min(2,m_settings_item+1);
                else if (m_selected + cols < int(m_games.size())) m_selected += cols;
                break;
            case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
                if (m_show_settings) {
                    if (m_settings_item==2) { m_skip_ps_boot = !m_skip_ps_boot; m_pending = DashAction::ToggleSkipBoot; }
                } else if (m_show_controls) {
                    m_show_controls = false;
                } else if (!m_games.empty()) {
                    m_pending = DashAction::LaunchGame;
                }
                break;
            case SDLK_b: m_pending = DashAction::BootBiosOnly; break;
            case SDLK_r: m_pending = DashAction::RefreshLibrary; break;
            case SDLK_F1: case SDLK_h:
                m_show_controls = !m_show_controls;
                m_show_settings = false;
                break;
            case SDLK_F2: case SDLK_TAB:
                m_show_settings = !m_show_settings;
                m_show_controls = false;
                break;
            case SDLK_ESCAPE: case SDLK_q:
                if (m_show_settings || m_show_controls) {
                    m_show_settings = false;
                    m_show_controls = false;
                } else {
                    m_pending = DashAction::Quit;
                }
                break;
            default: break;
        }
    } else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
        int cols = 4;
        { int hh, fh, cw, ch, gap, c, ct; layout_metrics(hh, fh, cw, ch, gap, c, ct); cols = c; }
        switch (e.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT: if (m_show_settings) { if(m_settings_item==0) m_video_mode = VideoMode::Native240p; else if(m_settings_item==1) m_theme = Theme((int(m_theme)+4)%5); } else if (m_selected > 0) m_selected--; break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: if (m_show_settings) { if(m_settings_item==0) m_video_mode = VideoMode::Overclocked60; else if(m_settings_item==1) m_theme = Theme((int(m_theme)+1)%5); } else if (m_selected + 1 < int(m_games.size())) m_selected++; break;
            case SDL_CONTROLLER_BUTTON_DPAD_UP: if (m_show_settings) m_settings_item=std::max(0,m_settings_item-1); else if (m_selected - cols >= 0) m_selected -= cols; break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN: if (m_show_settings) m_settings_item=std::min(2,m_settings_item+1); else if (m_selected + cols < int(m_games.size())) m_selected += cols; break;
            case SDL_CONTROLLER_BUTTON_A: case SDL_CONTROLLER_BUTTON_START:
                if (m_show_settings) { if (m_settings_item==2) { m_skip_ps_boot=!m_skip_ps_boot; m_pending=DashAction::ToggleSkipBoot; } }
                else if (!m_games.empty()) m_pending = DashAction::LaunchGame; break;
            case SDL_CONTROLLER_BUTTON_B: if (m_show_settings || m_show_controls) { m_show_settings=false; m_show_controls=false; } else m_pending = DashAction::BootBiosOnly; break;
            case SDL_CONTROLLER_BUTTON_Y: m_pending = DashAction::RefreshLibrary; break;
            case SDL_CONTROLLER_BUTTON_BACK: if (m_show_settings || m_show_controls) { m_show_settings=false; m_show_controls=false; } else m_pending = DashAction::Quit; break;
            default: break;
        }
    } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
        int header_h, footer_h, card_w, card_h, gap, cols, content_top;
        layout_metrics(header_h, footer_h, card_w, card_h, gap, cols, content_top);
        int mx = e.button.x, my = e.button.y;
        if (my < content_top || my > m_win_h - footer_h) return;
        int col = (mx - 28) / (card_w + gap);
        int row = (my - content_top) / (card_h + gap) + m_scroll;
        if (col >= 0 && col < cols) {
            int idx = row * cols + col;
            if (idx >= 0 && idx < int(m_games.size())) {
                if (idx == m_selected) m_pending = DashAction::LaunchGame;
                else m_selected = idx;
            }
        }
    } else if (e.type == SDL_MOUSEWHEEL) {
        m_scroll -= e.wheel.y;
        if (m_scroll < 0) m_scroll = 0;
    }
}

void Dashboard::update() {
    m_time += 1.f / 60.f;
}

void Dashboard::render(SDL_Renderer* ren) {
    m_renderer = ren;
    draw_background(ren);
    int header_h, footer_h, card_w, card_h, gap, cols, content_top;
    layout_metrics(header_h, footer_h, card_w, card_h, gap, cols, content_top);
    draw_header(ren, header_h);
    if (m_games.empty()) draw_empty_library(ren);
    else draw_game_grid(ren, content_top, footer_h);
    draw_footer(ren, footer_h);
    if (m_show_settings) draw_settings_panel(ren);
    if (m_show_controls) draw_controls_panel(ren);
}


void Dashboard::draw_settings_panel(SDL_Renderer* ren) {
    int pw=820,ph=500; int px=(m_win_w-pw)/2, py=(m_win_h-ph)/2;
    u8 ar,ag,ab; accent_color(ar,ag,ab);
    fill_round_rect(ren,px-8,py-8,pw+16,ph+16,32,ar,ag,ab,35);
    fill_round_rect(ren,px,py,pw,ph,28,7,9,18,250);
    SDL_SetRenderDrawColor(ren,ar,ag,ab,220); SDL_Rect border{px,py,pw,ph}; SDL_RenderDrawRect(ren,&border);
    draw_text(ren,px+34,py+26,"KORA  //  SETTINGS",ar,ag,ab,4);
    draw_text(ren,px+34,py+70,"PERSONALIZE YOUR PS96 LAUNCHER",140,165,185,2);
    const int ry=py+112;
    const char* labels[]={"VIDEO PRESENTATION","KORA THEME","BIOS BOOT"};
    const char* values[]={m_video_mode==VideoMode::Native240p?"NATIVE 240P":"OVERCLOCKED 60 FPS",theme_name(),m_skip_ps_boot?"SKIP INTRO":"FULL BIOS BOOT"};
    for(int i=0;i<3;i++){
        bool sel=m_settings_item==i;
        int y=ry+i*104;
        fill_round_rect(ren,px+26,y,pw-52,82,18,sel?ar:18,sel?ag:21,sel?ab:35,sel?45:225);
        draw_text(ren,px+50,y+18,labels[i],sel?240:145,sel?248:170,sel?255:190,2);
        draw_text(ren,px+50,y+44,values[i],ar,ag,ab,3);
        draw_text(ren,px+pw-235,y+33,(i<2)?"LEFT / RIGHT":"ENTER",150,175,195,2);
    }
    draw_text(ren,px+34,py+440,"UP/DOWN select    LEFT/RIGHT change    ENTER toggle    ESC close",130,155,175,2);
}

void Dashboard::draw_controls_panel(SDL_Renderer* ren) {
    int pw = 560, ph = 360;
    int px = (m_win_w - pw) / 2, py = (m_win_h - ph) / 2;
    fill_round_rect(ren, px, py, pw, ph, 12, 10, 16, 28, 240);
    SDL_SetRenderDrawColor(ren, 0, 200, 240, 180);
    SDL_Rect border{px, py, pw, ph};
    SDL_RenderDrawRect(ren, &border);
    draw_text(ren, px + 24, py + 20, "CONTROLS", 100, 230, 255, 3);
    char dev[128];
    std::snprintf(dev, sizeof(dev), "Input: %s", m_input_device.c_str());
    draw_text(ren, px + 24, py + 56, dev, 80, 255, 180, 2);
    const char* lines[] = {
        "Keyboard:",
        "  Arrows / WASD  D-Pad",
        "  Z  Cross     X  Circle",
        "  A  Square    S  Triangle",
        "  Enter Start  RShift Select",
        "  Q/W  L1/R1   1/2  L2/R2",
        "",
        "Controller: standard Xbox layout",
        "  A Cross  B Circle  X Square  Y Triangle",
        "  LB/RB L1/R1   LT/RT L2/R2",
        "  Start/Back  D-Pad  Sticks",
        "",
        "F1 / ESC close",
    };
    int y = py + 90;
    for (const char* line : lines) {
        draw_text(ren, px + 24, y, line, 180, 200, 220, 2);
        y += 18;
    }
}

} // namespace ps96
