cmake_minimum_required(VERSION 3.16)
project(PS96 LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
elseif(MSVC)
  set(CMAKE_CXX_FLAGS_RELEASE "/O2 /DNDEBUG")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(PS96_BUILD_FRONTEND "Build the SDL2 desktop frontend" ON)
option(PS96_BUILD_TESTS "Build the hardware core validation tests" ON)

set(PS96_CORE_SOURCES
    src/common/Types.cpp
    src/common/Log.cpp
    src/cpu/R3000A.cpp
    src/cpu/Cop0.cpp
    src/cpu/Disassembler.cpp
    src/memory/Bus.cpp
    src/memory/RAM.cpp
    src/gpu/GPU.cpp
    src/gpu/Renderer.cpp
    src/gte/GTE.cpp
    src/dma/DMA.cpp
    src/cdrom/CDROM.cpp
    src/cdrom/Disc.cpp
    src/spu/SPU.cpp
    src/mdec/MDEC.cpp
    src/timers/Timers.cpp
    src/controllers/Controllers.cpp
    src/memorycard/MemoryCard.cpp
    src/bios/BIOS.cpp
    src/emulator/Emulator.cpp
    src/emulator/Scheduler.cpp
    src/emulator/InterruptController.cpp
)

if(PS96_BUILD_FRONTEND)
  # SDL2 — prefer a proper CMake target, then pkg-config, then common paths.
  find_package(SDL2 QUIET)
  if(NOT SDL2_FOUND AND NOT TARGET SDL2::SDL2)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(SDL2 QUIET sdl2)
    endif()
  endif()
  if(NOT SDL2_FOUND AND NOT TARGET SDL2::SDL2)
    find_path(SDL2_INCLUDE_DIRS SDL.h PATH_SUFFIXES SDL2)
    find_library(SDL2_LIBRARIES NAMES SDL2 SDL2main)
    if(SDL2_INCLUDE_DIRS AND SDL2_LIBRARIES)
      set(SDL2_FOUND TRUE)
    endif()
  endif()
  if(NOT SDL2_FOUND AND NOT TARGET SDL2::SDL2)
    message(FATAL_ERROR
      "SDL2 not found.\n"
      "MSYS2 UCRT64: pacman -S mingw-w64-ucrt-x86_64-SDL2 mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-gcc\n"
      "Ubuntu/Debian: sudo apt install libsdl2-dev")
  endif()

  if(WIN32 AND TARGET SDL2::SDL2)
    get_target_property(_sdl_if SDL2::SDL2 INTERFACE_LINK_LIBRARIES)
    if(_sdl_if)
      list(FILTER _sdl_if EXCLUDE REGEX "SDL2main")
      set_property(TARGET SDL2::SDL2 PROPERTY INTERFACE_LINK_LIBRARIES "${_sdl_if}")
    endif()
  endif()

  set(PS96_FRONTEND_SOURCES
      src/main.cpp
      src/frontend/Frontend.cpp
      src/frontend/Input.cpp
      src/frontend/Config.cpp
      src/frontend/Dashboard.cpp
      src/frontend/GamesScanner.cpp
      src/frontend/Text.cpp
      src/frontend/BootScreen.cpp
      src/frontend/ImageLoader.cpp
  )

  add_executable(ps96 ${PS96_CORE_SOURCES} ${PS96_FRONTEND_SOURCES})
  target_include_directories(ps96 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${SDL2_INCLUDE_DIRS})
  target_compile_definitions(ps96 PRIVATE SDL_MAIN_HANDLED)
  set_target_properties(ps96 PROPERTIES WIN32_EXECUTABLE FALSE)
  if(TARGET SDL2::SDL2)
    target_link_libraries(ps96 PRIVATE SDL2::SDL2)
  elseif(SDL2_LIBRARIES)
    target_link_libraries(ps96 PRIVATE ${SDL2_LIBRARIES})
  endif()
  if(WIN32)
    if(MSVC)
      set_target_properties(ps96 PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")
    else()
      target_link_options(ps96 PRIVATE -mconsole -Wl,--subsystem,console)
    endif()
  endif()
endif()

if(PS96_BUILD_TESTS)
  add_executable(ps96_tests tests/test_main.cpp ${PS96_CORE_SOURCES})
  target_include_directories(ps96_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  set_target_properties(ps96_tests PROPERTIES WIN32_EXECUTABLE FALSE)
  if(WIN32)
    if(MSVC)
      set_target_properties(ps96_tests PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")
    else()
      target_link_options(ps96_tests PRIVATE -mconsole -Wl,--subsystem,console)
    endif()
  endif()
endif()

message(STATUS "PS96 - C++20 PS1 emulator")
if(PS96_BUILD_FRONTEND)
  message(STATUS "Frontend: ON (SDL2)")
else()
  message(STATUS "Frontend: OFF")
endif()
if(PS96_BUILD_TESTS)
  message(STATUS "Tests: ON")
else()
  message(STATUS "Tests: OFF")
endif()
