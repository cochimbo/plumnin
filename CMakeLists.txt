cmake_minimum_required(VERSION 3.16)
project(plumnin LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(PLUMNIN_FETCH_SDL2 "Download and build SDL2 if it is not installed" ON)
option(PLUMNIN_BUILD_TESTS "Build the test programs" ON)
# Off builds only the core, plumnin-headless and the tests: no SDL2, ImGui or OpenGL.
option(PLUMNIN_BUILD_FRONTEND "Build the emulator with its window (needs SDL2 and OpenGL)" ON)

if(MSVC)
  add_compile_options(/W4 /utf-8)
  # The CRT's "use the _s variant" deprecation notices (sscanf, fopen, localtime); the calls are bounded
  # and single-threaded, and the _s variants do not exist outside MSVC.
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra)
endif()

include(FetchContent)

# miniz (vendored, MIT): zip files, savestate compression and PNG screenshots.
add_library(plumnin_miniz STATIC third_party/miniz/miniz.c)
set_target_properties(plumnin_miniz PROPERTIES LINKER_LANGUAGE C)
target_include_directories(plumnin_miniz PUBLIC third_party/miniz)
target_compile_definitions(plumnin_miniz PUBLIC MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
if(MSVC)
  target_compile_options(plumnin_miniz PRIVATE /W0)
else()
  target_compile_options(plumnin_miniz PRIVATE -w)
endif()

# rcheevos (MIT, RetroAchievements) for the achievements: its client, runtime and web
# API, without its own hashing (plumnin hashes the games itself). Pinned to v12.5.0.
FetchContent_Declare(rcheevos
  GIT_REPOSITORY https://github.com/RetroAchievements/rcheevos.git
  GIT_TAG 1433173220a7eaede6a9ed7a18e94117be1821e0)
FetchContent_GetProperties(rcheevos)
if(NOT rcheevos_POPULATED)
  FetchContent_Populate(rcheevos)
endif()
file(GLOB PLUMNIN_RCHEEVOS_SOURCES ${rcheevos_SOURCE_DIR}/src/rapi/*.c ${rcheevos_SOURCE_DIR}/src/rcheevos/*.c)
add_library(plumnin_rcheevos STATIC ${PLUMNIN_RCHEEVOS_SOURCES}
  ${rcheevos_SOURCE_DIR}/src/rc_client.c
  ${rcheevos_SOURCE_DIR}/src/rc_compat.c
  ${rcheevos_SOURCE_DIR}/src/rc_util.c
  ${rcheevos_SOURCE_DIR}/src/rc_version.c
  ${rcheevos_SOURCE_DIR}/src/rhash/md5.c)
target_include_directories(plumnin_rcheevos SYSTEM PUBLIC ${rcheevos_SOURCE_DIR}/include)
find_package(Threads REQUIRED)
target_link_libraries(plumnin_rcheevos PUBLIC Threads::Threads)
if(MSVC)
  target_compile_options(plumnin_rcheevos PRIVATE /W0)
else()
  target_compile_options(plumnin_rcheevos PRIVATE -w)
endif()

# Emulator core: no SDL, no OS dependencies.
add_library(plumnin_core STATIC
  src/core/console.cpp
  src/core/consoles.cpp
  src/core/test_console.cpp
  src/core/gb/gb.cpp
  src/core/gb/gb_cpu.cpp
  src/core/gb/gb_ppu.cpp
  src/core/gb/gb_apu.cpp
  src/core/gb/gb_cart.cpp
  src/core/nes/nes.cpp
  src/core/nes/nes_cpu.cpp
  src/core/nes/nes_ppu.cpp
  src/core/nes/nes_apu.cpp
  src/core/nes/nes_cart.cpp
  src/core/snes/snes.cpp
  src/core/snes/snes_apu.cpp
  src/core/snes/snes_cart.cpp
  src/core/snes/snes_necdsp.cpp
  src/core/snes/snes_obc1.cpp
  src/core/snes/snes_sa1.cpp
  src/core/snes/snes_sdd1.cpp
  src/core/snes/snes_superfx.cpp
  src/core/snes/snes_cpu.cpp
  src/core/snes/snes_ppu.cpp
  src/core/snes/snes_smp.cpp
  src/core/system.cpp
  src/core/cheats.cpp
  src/core/rewind.cpp
  src/core/netplay.cpp
  src/core/recording.cpp
  src/core/i18n.cpp
  src/render/planes.cpp
  src/render/profile3d.cpp
)
# stb_image_write (public domain / MIT, vendored) encodes the JPEG frames of the AVI recordings.
target_include_directories(plumnin_core PUBLIC src src/core PRIVATE third_party/stb)
target_link_libraries(plumnin_core PUBLIC plumnin_miniz Threads::Threads)

# UDP sockets for netplay (the session itself is in the core).
add_library(plumnin_net STATIC src/net/udp.cpp)
target_link_libraries(plumnin_net PUBLIC plumnin_core)
if(WIN32)
  target_link_libraries(plumnin_net PUBLIC ws2_32)
endif()

# RetroAchievements: the game's hash, its memory as the achievements see it, and the
# rcheevos client with its requests made on worker threads.
add_library(plumnin_achievements STATIC src/frontend/achievements.cpp)
target_link_libraries(plumnin_achievements PUBLIC plumnin_core plumnin_rcheevos)
target_include_directories(plumnin_achievements PRIVATE ${rcheevos_SOURCE_DIR}/src/rhash)

# The emulator without a window, for tests, CI and reproducing bugs (see --help).
add_executable(plumnin-headless src/headless/main.cpp)
target_include_directories(plumnin-headless PRIVATE tests)
target_link_libraries(plumnin-headless PRIVATE plumnin_core plumnin_net)

# Everything below up to endif() is the windowed emulator.
if(PLUMNIN_BUILD_FRONTEND)
# SDL2: use an installed copy (vcpkg, system package, SDL2_DIR) or fetch it.
find_package(SDL2 CONFIG QUIET)
if(NOT SDL2_FOUND AND PLUMNIN_FETCH_SDL2)
  include(FetchContent)
  message(STATUS "SDL2 not found, fetching it")
  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-2.30.9
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(SDL2)
endif()

if(TARGET SDL2::SDL2)
  set(PLUMNIN_SDL2_TARGET SDL2::SDL2)
else()
  set(PLUMNIN_SDL2_TARGET SDL2::SDL2-static)
endif()

# Dear ImGui for the menus and option windows, drawn with OpenGL 3.
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.9b
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)
add_library(plumnin_imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(plumnin_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(plumnin_imgui PUBLIC ${PLUMNIN_SDL2_TARGET})

add_executable(plumnin WIN32
  src/frontend/main.cpp
  src/frontend/crash.cpp
  src/frontend/input.cpp
  src/frontend/audio.cpp
  src/frontend/settings.cpp
  src/frontend/game_options.cpp
  src/frontend/file_dialog.cpp
  src/frontend/ui.cpp
  src/frontend/handheld.cpp
  src/frontend/osk.cpp
  src/frontend/theme.cpp
  src/frontend/library.cpp
  src/frontend/texture_cache.cpp
  src/frontend/library_names.cpp
  src/frontend/game_info.cpp
  src/frontend/game_info_data.cpp
  src/frontend/http.cpp
  src/frontend/screenscraper.cpp
  src/frontend/screenscraper_data.cpp
  src/frontend/ui_media.cpp
  src/frontend/cheat_database.cpp
  src/frontend/languages.cpp
  src/frontend/achievements_ui.cpp
  src/render/gl.cpp
  src/render/gl_renderers.cpp
  src/render/upscale.cpp
)
# The plum icon of plumnin.exe (Explorer, taskbar, shortcuts).
if(WIN32)
  target_sources(plumnin PRIVATE src/frontend/plumnin.rc)
endif()
# stb_image (public domain / MIT, vendored) decodes the library's PNG and JPEG covers.
target_include_directories(plumnin PRIVATE third_party/stb)

# The PLUMNIN logo, the plum icon and the interface's fonts, built into the executable as
# byte arrays (logo_png.h, chakra_petch_regular_ttf.h...).
function(plumnin_embed file var)
  set(path ${CMAKE_CURRENT_SOURCE_DIR}/src/frontend/assets/${file})
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${path})
  file(READ ${path} hex HEX)
  string(REPEAT "[0-9a-f][0-9a-f]" 32 line)
  string(REGEX REPLACE "(${line})" "\\1\n" hex "${hex}")
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
  get_filename_component(name ${file} NAME)
  string(MAKE_C_IDENTIFIER ${name} header)
  set(out ${CMAKE_CURRENT_BINARY_DIR}/generated/${header}.h)
  file(WRITE ${out}.tmp
    "// Generated by CMakeLists.txt from src/frontend/assets/${file}.\n#pragma once\n"
    "static const unsigned char ${var}[] = {\n${hex}\n};\n")
  configure_file(${out}.tmp ${out} COPYONLY)
endfunction()
plumnin_embed(logo.png kLogoPng)
plumnin_embed(icon.png kIconPng)
plumnin_embed(fonts/chakra_petch_regular.ttf kChakraPetchRegular)
plumnin_embed(fonts/chakra_petch_bold_italic.ttf kChakraPetchBoldItalic)

# The translations (src/frontend/lang/*.po), built in the same way (lang_po.h), so a
# new language needs no file next to the executable or the installer.
set(PLUMNIN_LANGUAGES en fr pt)
set(lang_header "// Generated by CMakeLists.txt from src/frontend/lang/*.po.\n#pragma once\n")
foreach(lang ${PLUMNIN_LANGUAGES})
  set(po ${CMAKE_CURRENT_SOURCE_DIR}/src/frontend/lang/${lang}.po)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${po})
  file(READ ${po} hex HEX)
  string(REPEAT "[0-9a-f][0-9a-f]" 32 line)
  string(REGEX REPLACE "(${line})" "\\1\n" hex "${hex}")
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
  string(APPEND lang_header "static const unsigned char kLangPo_${lang}[] = {\n${hex}\n};\n")
endforeach()
set(out ${CMAKE_CURRENT_BINARY_DIR}/generated/lang_po.h)
file(WRITE ${out}.tmp "${lang_header}")
configure_file(${out}.tmp ${out} COPYONLY)
target_include_directories(plumnin PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
find_package(Threads REQUIRED)
target_link_libraries(plumnin PRIVATE plumnin_core plumnin_net plumnin_achievements plumnin_imgui Threads::Threads)
if(TARGET SDL2::SDL2main)
  target_link_libraries(plumnin PRIVATE SDL2::SDL2main)
endif()
target_link_libraries(plumnin PRIVATE ${PLUMNIN_SDL2_TARGET})
if(WIN32)
  target_link_libraries(plumnin PRIVATE comdlg32 wininet dbghelp advapi32)
endif()
# The version About and the crash reports show: -DPLUMNIN_VERSION=... (the release workflow
# passes the release's), else the tag and commit it was built from.
set(PLUMNIN_VERSION "" CACHE STRING "Version shown in About and in crash reports (empty: git describe)")
set(plumnin_version "${PLUMNIN_VERSION}")
find_package(Git QUIET)
if(NOT plumnin_version AND GIT_FOUND)
  execute_process(COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
                  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                  OUTPUT_VARIABLE plumnin_version OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
if(NOT plumnin_version)
  set(plumnin_version "dev")
endif()
set_source_files_properties(src/frontend/crash.cpp src/frontend/ui.cpp PROPERTIES COMPILE_DEFINITIONS "PLUMNIN_VERSION=\"${plumnin_version}\"")
# ScreenScraper's developer account for plumnin (it gives one to each program that asks):
# the installer's build passes it from the repository's secrets. Without one the user
# can type one in the library's media window.
set(PLUMNIN_SCREENSCRAPER_DEVID "" CACHE STRING "ScreenScraper developer id built into plumnin")
set(PLUMNIN_SCREENSCRAPER_DEVPASSWORD "" CACHE STRING "ScreenScraper developer password built into plumnin")
if(PLUMNIN_SCREENSCRAPER_DEVID)
  set_source_files_properties(src/frontend/screenscraper.cpp PROPERTIES COMPILE_DEFINITIONS
    "PLUMNIN_SS_DEVID=\"${PLUMNIN_SCREENSCRAPER_DEVID}\";PLUMNIN_SS_DEVPASSWORD=\"${PLUMNIN_SCREENSCRAPER_DEVPASSWORD}\"")
endif()

# Copy SDL2.dll next to plumnin.exe so it runs straight from the build folder.
if(WIN32 AND TARGET SDL2::SDL2)
  add_custom_command(TARGET plumnin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:plumnin>)
endif()
endif()  # PLUMNIN_BUILD_FRONTEND

if(PLUMNIN_BUILD_TESTS)
  enable_testing()
  add_executable(core_tests tests/core_tests.cpp src/frontend/library_names.cpp src/frontend/game_info_data.cpp
                 src/frontend/screenscraper_data.cpp src/frontend/texture_cache.cpp)
  target_include_directories(core_tests PRIVATE tests)
  target_compile_definitions(core_tests PRIVATE PLUMNIN_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests")
  target_link_libraries(core_tests PRIVATE plumnin_core plumnin_achievements)
  add_test(NAME core_tests COMMAND core_tests)

  # The Game Boy's test ROMs, when their folder is given: -DGB_TEST_ROMS=path/to/game-boy-test-roms
  # (a release of https://github.com/c-sp/game-boy-test-roms, unpacked).
  add_executable(gb-test-roms tests/gb_test_roms.cpp)
  target_include_directories(gb-test-roms PRIVATE third_party/stb)
  target_link_libraries(gb-test-roms PRIVATE plumnin_core)
  set(GB_TEST_ROMS "" CACHE PATH "Folder of the game-boy-test-roms release, for the gb_test_roms test")
  if(GB_TEST_ROMS)
    add_test(NAME gb_test_roms COMMAND gb-test-roms ${GB_TEST_ROMS})
  endif()

  # The NES's test ROMs, when their folder is given: -DNES_TEST_ROMS=path/to/nes-test-roms
  # (a clone of https://github.com/christopherpow/nes-test-roms).
  add_executable(nes-test-roms tests/nes_test_roms.cpp)
  target_include_directories(nes-test-roms PRIVATE third_party/stb)
  target_link_libraries(nes-test-roms PRIVATE plumnin_core)
  set(NES_TEST_ROMS "" CACHE PATH "Folder of the nes-test-roms repository, for the nes_test_roms test")
  if(NES_TEST_ROMS)
    add_test(NAME nes_test_roms COMMAND nes-test-roms ${NES_TEST_ROMS})
  endif()

  # The SNES's test ROMs, when their folder is given: -DSNES_TEST_ROMS=path/to/SNES
  # (a clone of https://github.com/PeterLemon/SNES).
  add_executable(snes-test-roms tests/snes_test_roms.cpp)
  target_include_directories(snes-test-roms PRIVATE third_party/stb)
  target_link_libraries(snes-test-roms PRIVATE plumnin_core)
  set(SNES_TEST_ROMS "" CACHE PATH "Folder of PeterLemon's SNES repository, for the snes_test_roms test")
  if(SNES_TEST_ROMS)
    add_test(NAME snes_test_roms COMMAND snes-test-roms ${SNES_TEST_ROMS})
  endif()

  # SingleStepTests' processor tests, when their folders are given:
  # -DSNES_CPU_TESTS=path/to/65816/v1 and -DSNES_SPC_TESTS=path/to/spc700/v1
  # (https://github.com/SingleStepTests/65816 and .../spc700; files cut short are fine).
  add_executable(snes-single-step tests/snes_single_step.cpp)
  target_link_libraries(snes-single-step PRIVATE plumnin_core)
  set(SNES_CPU_TESTS "" CACHE PATH "Folder of SingleStepTests' 65816 JSON files")
  set(SNES_SPC_TESTS "" CACHE PATH "Folder of SingleStepTests' SPC700 JSON files")
  if(SNES_CPU_TESTS)
    add_test(NAME snes_cpu_tests COMMAND snes-single-step 65816 ${SNES_CPU_TESTS})
  endif()
  if(SNES_SPC_TESTS)
    add_test(NAME snes_spc_tests COMMAND snes-single-step spc700 ${SNES_SPC_TESTS})
  endif()

  # Every text in tr() has its translation in every .po (tools/i18n.py --update adds the new ones).
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME i18n COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/i18n.py --check)
  endif()

  # plumnin-headless end to end: hashes, savestates, input scripts and checks.
  add_test(NAME headless COMMAND ${CMAKE_COMMAND} -DHEADLESS=$<TARGET_FILE:plumnin-headless>
           -DWORK=${CMAKE_CURRENT_BINARY_DIR}/headless_test
           -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/headless_tests.cmake)

  # The frontend's files (a game's own options and controls); needs SDL2 for the key names.
  if(PLUMNIN_BUILD_FRONTEND)
    add_executable(frontend_tests tests/frontend_tests.cpp src/frontend/game_options.cpp src/frontend/input.cpp
                   src/frontend/settings.cpp src/frontend/crash.cpp src/frontend/handheld.cpp)
    target_link_libraries(frontend_tests PRIVATE plumnin_core ${PLUMNIN_SDL2_TARGET})
    add_test(NAME frontend_tests COMMAND frontend_tests)
  endif()
endif()
