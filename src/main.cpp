#include "frontend/Frontend.hpp"
#include "frontend/GamesScanner.hpp"
#include "common/Log.hpp"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <filesystem>

int main(int argc, char** argv) {
    using namespace ps96;

    Log::set_level(LogLevel::Info);
    Log::info("PS96 — PlaySaffy 96  |  C++20 PS1 Emulator + Kora Dashboard");

    bool help = false;
    bool force_bios = false;
    bool force_dash = false;
    bool show_boot_menu = false;
    bool skip_boot = false;
    bool skip_ps_boot = false;
    bool debug = false;
    bool gpu_trace = false;
    int max_frames = 0;
    std::string cli_bios;
    std::string cli_disc;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-help") == 0 || std::strcmp(argv[i], "--help") == 0) {
            help = true;
        } else if (std::strcmp(argv[i], "-bios") == 0 && i + 1 < argc) {
            cli_bios = argv[++i];
        } else if (std::strcmp(argv[i], "-disc") == 0 && i + 1 < argc) {
            cli_disc = argv[++i];
        } else if (std::strcmp(argv[i], "-dashboard") == 0 || std::strcmp(argv[i], "-korad") == 0) {
            force_dash = true;
        } else if (std::strcmp(argv[i], "-bootbios") == 0) {
            force_bios = true;
        } else if (std::strcmp(argv[i], "-menu") == 0) {
            show_boot_menu = true;
        } else if (std::strcmp(argv[i], "-noboot") == 0) {
            skip_boot = true;
        } else if (std::strcmp(argv[i], "-trace") == 0) {
            Log::set_cpu_trace(true);
        } else if (std::strcmp(argv[i], "-debug") == 0) {
            debug = true;
        } else if (std::strcmp(argv[i], "-gpu-trace") == 0) {
            debug = true;
            gpu_trace = true;
        } else if (std::strcmp(argv[i], "-skipboot") == 0) {
            skip_ps_boot = true;
        } else if (std::strcmp(argv[i], "-frames") == 0 && i + 1 < argc) {
            max_frames = std::atoi(argv[++i]);
        }
    }

    if (help) {
        std::printf("PS96 — PlaySaffy 96\n\n");
        std::printf("  ps96.exe                  Boot animation → Kora Dashboard\n");
        std::printf("  ps96.exe -noboot          Skip boot animation\n");
        std::printf("  ps96.exe -menu            Choose Dashboard or BIOS\n");
        std::printf("  ps96.exe -bootbios        BIOS only\n");
        std::printf("  ps96.exe -bios <path>     BIOS file\n");
        std::printf("  ps96.exe -disc <path>     Load disc and emulate\n  ps96.exe -skipboot        Fast-boot games (skip BIOS intro)\n  ps96.exe -frames N        Headless N frames then exit\n  ps96.exe -debug            Write detailed ps96_debug.log\n  ps96.exe -gpu-trace       Write every GP0 packet to ps96_debug.log\n\n");
        std::printf("Games/<Title>/*.cue + *.bin + cover.png next to the exe\n");
        return 0;
    }

    std::string base = GamesScanner::executable_dir();
    if (debug) {
        Log::set_level(LogLevel::Debug);
        const std::filesystem::path log_path = std::filesystem::path(base) / "ps96_debug.log";
        if (!Log::open_file(log_path.string()))
            std::fprintf(stderr, "[WARN] Could not open debug log: %s\n", log_path.string().c_str());
    }
    Log::info("Base directory: %s", base.c_str());

    Frontend frontend;
    if (gpu_trace) frontend.emulator().gpu().set_debug_trace(true);
    frontend.set_base_dir(base);
    frontend.set_skip_boot_intro(skip_boot);

    if (show_boot_menu)
        frontend.set_start_mode(AppMode::BootSelect);
    else if (force_bios || !cli_disc.empty())
        frontend.set_start_mode(AppMode::Emulating);
    else
        frontend.set_start_mode(AppMode::Dashboard);

    if (!frontend.init()) {
        Log::error("Failed to initialize frontend");
        return 1;
    }

    Config& cfg = frontend.config();
    if (!cli_bios.empty()) {
        cfg.bios_path = cli_bios;
        if (frontend.emulator().load_bios(cfg.bios_path)) {
            frontend.emulator().reset();
            Log::info("CLI BIOS ready: %s", cfg.bios_path.c_str());
        }
    }

    if (skip_ps_boot)
        cfg.skip_ps_boot = true;

    if (!cli_disc.empty()) {
        cfg.disc_path = cli_disc;
        if (frontend.emulator().load_disc(cfg.disc_path)) {
            frontend.emulator().reset();
            Log::info("CLI disc ready: %s", cfg.disc_path.c_str());
            // A disc does not imply a BIOS fast-boot. The normal path executes
            // the real retail BIOS, including its SCE/license sequence.
            frontend.set_skip_boot_intro(false);
            frontend.emulator().set_paused(false);
        }
    }

    if (!frontend.emulator().bios_loaded()) {
        Log::warn("NO BIOS — place SCPH1001.BIN / SCPH5501.BIN next to exe or in bios/");
    }

    if (max_frames > 0) {
        Log::info("Headless run for %d frames", max_frames);
        frontend.emulator().set_paused(false);
        for (int f = 0; f < max_frames; f++) {
            // Headless mode must not inject synthetic button presses. The
            // emulator core is tested with the exact same controller protocol
            // as interactive execution; this keeps validation free of
            // game-specific or test-only input behavior.
            frontend.emulator().run_frame();
        }
    } else {
        frontend.run();
    }
    frontend.emulator().gpu().dump_debug_stats();
    Log::flush();
    frontend.shutdown();
    Log::close_file();
    return 0;
}
