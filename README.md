# S96 — PlayStation 1 Emulator

S96 is a C++20 PlayStation 1 emulator with the Kora dashboard/frontend.

The current source deliberately uses **general hardware emulation fixes only**. There are no per-game CRC hacks, title checks, BIOS-specific branches, or NFS/GT/etc. rendering exceptions.

## Controls

### In games

| Keyboard | PS1 control |
|---|---|
| Arrow keys | D-pad |
| `I` | Cross |
| `J` | Circle |
| `K` | Square |
| `L` | Triangle |
| `A` | L1 |
| `D` | L2 |
| `Q` | R1 |
| `E` | R2 |
| `Enter` | Start |
| `Right Shift` | Select |
| `W` / `S` | **Unmapped** |

The keyboard mapping is applied from both SDL's held-key state and event-latched transitions so short presses are not lost between host-frame samples.

### Frontend

The dashboard may use its own navigation keys while it has focus; the gamepad mapping above is the emulated-controller mapping.

## Folder layout

```text
S96/
  CMakeLists.txt
  README.md
  bios/
    SCPH5501.BIN
    openbios.bin
  Games/
    <Game Name>/
      <game>.cue
      <game>.bin
      cover.png        # optional
  src/
  tests/
```

Use BIOS and game images that you are legally entitled to use.

## Windows build — MSYS2 UCRT64

This is the normal Windows workflow:

```powershell
$Env:Path += ";C:\msys64\ucrt64\bin"
cd C:\Users\Saffy\Downloads\S96\S96
Remove-Item -Recurse -Force build -ErrorAction SilentlyContinue
cmake -B build
cmake --build build
.\build\ps96.exe -bios "C:\Users\Saffy\Downloads\SCPH5501.bin"
```

Recommended dependencies:

```powershell
pacman -S mingw-w64-ucrt-x86_64-SDL2 mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-gcc
```

The CMake Release configuration enables link-time optimization automatically when the compiler supports it.

## Linux/core validation build

The frontend can be disabled to validate the hardware core without SDL2 development headers:

```bash
cmake -B build -DPS96_BUILD_FRONTEND=OFF -DPS96_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/ps96_tests
```

Expected result:

```text
PS96 core validation: PASS
```

The validation suite has been run in Release, Debug, and AddressSanitizer/UndefinedBehaviorSanitizer builds.

## Running games

Direct disc boot:

```powershell
.\build\ps96.exe -bios "C:\path\to\SCPH5501.bin" -disc "C:\path\to\game.cue"
```

Useful options:

```text
-help
-debug
-gpu-trace
-bootbios
-dashboard
-menu
-skipboot
-frames N
-trace
```

`-skipboot` is a normal emulator option for fast booting. It is not a game-specific compatibility hack.

## Debugging

### Normal debug log

```powershell
.\build\ps96.exe -bios "C:\path\to\SCPH5501.bin" -debug
```

After exiting, inspect `ps96_debug.log` beside the executable. The normal log includes:

- host-frame timing and effective FPS
- guest frame/scanline position
- GPU display ranges and display mode changes
- GPU command histogram
- GPU draw/texture counts
- texture transparency skip counts
- unknown GP0 command count
- controller traffic changes
- BIOS/disc/reset events

### Full GPU command trace

```powershell
.\build\ps96.exe -bios "C:\path\to\SCPH5501.bin" -gpu-trace
```

This records the GP0 packet stream in addition to the normal debug log. It is intentionally generic and does not insert game-specific behavior.

## Current hardware fixes

### GTE

The GTE lighting/color pipeline was substantially corrected from the previous implementation. The affected operations now follow the documented PS1 stages instead of collapsing multiple operations into an approximation:

- NCS / NCT
- NCCS / NCCT
- NCDS / NCDT
- CC / CDP
- DCPL / DPCS / DPCT
- INTPL
- GPF / GPL
- MVMVA `sf`/`lm`
- SQR / OP `sf` handling
- RTPS / RTPT projection scaling and SZ handling

The depth-cue path includes the hardware intermediate clamp of the `FC-MAC` delta before multiplying by IR0. The color FIFO preserves the RGBC code byte.

These are general GTE fixes, not title-specific workarounds.

### GPU

The software rasterizer was tightened and made much cheaper for textured geometry:

- affine texture interpolation uses a fixed-point reciprocal instead of a 64-bit divide for every pixel
- flat textured polygons no longer calculate Gouraud color interpolation that they do not use
- the raster work area is clipped once against drawing-area/VRAM limits before entering the pixel loop
- texture page and CLUT state are kept per-draw
- indexed texture transparency is based on the resolved texel color `0000h`, matching PS1 hardware behavior; CLUT index 0 is not intrinsically transparent
- GP0 rectangle/VRAM transfer size masking follows the hardware field widths
- flat-quad mask-test/set behavior goes through the normal pixel write path
- hardware quad triangulation remains `(v0,v1,v2)` plus `(v1,v2,v3)`
- 15-bit display conversion preserves the full RGB range

### Timing/performance

The emulator still advances the guest using PS1 clock/event timing. It does not speed up or slow down the emulated machine to hide performance problems.

The Release build uses `-O3`; the GPU hot path also avoids avoidable divisions and interpolation work. The goal is to increase real throughput while keeping guest timing deterministic.

## Reference work

S96 development uses public PS1 hardware documentation and open-source emulator implementations such as PCSX-Redux as reference material. The implementation remains hardware-oriented and keeps fixes general rather than copying game-specific compatibility behavior.

## Status

S96 is still an actively developed emulator. The current build contains meaningful improvements to GTE lighting, GPU rasterization, texture handling, timing, and input polling, but broad commercial compatibility still needs validation across a larger game set.


## Latest fixes — September 2026

This build contains the latest general hardware/timing pass from the multi-game debug investigation.

## Interlaced GPU/region timing

S96 now models the PS1 interlaced draw rule at the raster level: during interlaced
video the GPU writes only the even or odd VRAM rows appropriate to the current
field, and GP0(E1).10 reverses that target when a title explicitly asks to draw to
the displayed field. GPUSTAT bit 13/31 are maintained from the field/raster state
and the GP1(05) display-address Y LSB during retrace. This is a general hardware
rule, not a game compatibility switch. The CRTC uses the programmed NTSC/PAL mode
for 263/314 scanlines (nominal 60/50 Hz), so the BIOS/guest-selected video region
controls the emulated raster cadence.

- SIO0 controller transfers now use the documented `0x0088` baud timing (~250 kHz), rather than an incorrectly halved byte period. The controller also emits the hardware-style `/ACK` pulse on the final byte instead of silently dropping it.
- GPUSTAT command/VRAM/DMA-ready bits no longer advertise the GPU as idle while a multiword GP0 packet is still being collected. Polygon/line packets clear the command/DMA-ready state as soon as their command word is accepted.
- CD-ROM sector delivery now uses the PS1 master-clock interval for 75 sectors/sec normal speed or 150 sectors/sec when Setmode bit 7 selects double speed, instead of an artificial 8000-cycle interval.
- A generic repeated-PC watchdog was added to `-debug` logging. It reports long instruction loops with PC, instruction, COP0 cause/status/EPC, without hard-coding any game or address.
- Emulation device servicing remains quantized to 8 CPU cycles to reduce peripheral/interrupt timing jitter while preserving deterministic guest timing.
- The frontend continues pumping SDL events and refreshing controller state during short emulation slices, so slow scenes do not starve host input.
- No game-specific or BIOS-specific compatibility hack was added.

The latest core validation result is:

`PS96 core validation: PASS`

For diagnostics:

`ps96.exe -bios "C:\Users\Saffy\Downloads\SCPH5501.bin" -debug`

`ps96.exe -bios "C:\Users\Saffy\Downloads\SCPH5501.bin" -gpu-trace`

## Latest hardware/timing pass

This build includes a CRTC-style interlace field model with separate displayed-field and active-render parity, including interlaced fill/rectangle drawing. Host presentation is decoupled from guest execution: the PS1 master clock advances from elapsed real time instead of one guest frame per host render iteration. The controller configuration path also accepts the documented 0x44 analog-mode packet position used by DualShock-capable software.

For diagnostics, use `-debug` or `-gpu-trace` as described above. The debug log's host FPS is presentation/logging performance, not a claim about guest clock speed.

## Final performance/correctness pass — September 2026

This pass addresses three concrete defects found in the latest multi-game run:

- SDL event/controller polling is no longer performed every 4,096 CPU cycles. The frontend now runs 65,536-cycle guest slices (~1.94 ms) and services host events between slices. The previous 4,096-cycle loop caused thousands of SDL/input passes per second and could dominate the software-renderer workload.
- Texture transparency now follows the resolved 16-bit texel: `0000h` is transparent, while a non-zero CLUT entry at palette index 0 is drawable. This matches the current DuckStation software rasterizer and PSX-SPX hardware documentation.
- Textured/Gouraud triangle interpolation now computes its clipped raster origin before calculating edge/attribute values. Previously an off-screen triangle could calculate its interpolation state from an unclipped negative origin and then start drawing from a different pixel, producing displaced textures, dark/black strips, and block artifacts. Attribute gradients are now converted to incremental fixed-point values once per triangle instead of multiplying by a reciprocal at every pixel.

The guest clock remains independent of host presentation. A slow renderer may drop host presents, but it must not invent a different guest clock.

No game-specific or BIOS-specific compatibility condition was added.
