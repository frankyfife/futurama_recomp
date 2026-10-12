# FuturamaLauncher - Qt 6 Widgets launcher for the PC port (64 bit, separate from the x86 loader project).
# Build: cmd /c "D:\FuturamaDecomp\build_launcher.cmd" [builddir] [Debug|Release] (vcvars64 + Qt 6.8.3).
#
# Targets
#   launcher_core     static: INI round trip, schema, validation, paths, game CLI, sessions (QtCore only)
#   launcher_ui       static: widgets (main window, wizard, editors, status panel)
#   FuturamaLauncher  the GUI executable (WIN32 subsystem)
#   launcher_tests    Qt Test unit tests (ctest: launcher_tests)
#   fake_futurama     test double of futurama.exe (contract CLI + a fake game run) in <build>\fakegame\,
#                     used by the tests and the screenshot smoke test until the host CLI exists
cmake_minimum_required(VERSION 3.28)
project(FuturamaLauncher VERSION 0.3.0 LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "FuturamaLauncher is a 64-bit program - configure it with vcvars64 (build_launcher.cmd)")
endif()
if(NOT MSVC)
    message(FATAL_ERROR "FuturamaLauncher is built with MSVC (cl.exe)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(LAUNCHER_WERROR "Treat compiler warnings as errors" OFF)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Test)
qt_standard_project_setup()

# Qt is a dynamic library built with /MD, so the launcher uses the DLL runtime (the game uses /MT).
set(LAUNCHER_COMPILE_OPTIONS /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus /external:W0)
if(LAUNCHER_WERROR)
    list(APPEND LAUNCHER_COMPILE_OPTIONS /WX)
endif()
set(LAUNCHER_DEFINITIONS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN QT_NO_CAST_TO_ASCII
    QT_NO_URL_CAST_FROM_STRING QT_USE_QSTRINGBUILDER
    LAUNCHER_VERSION="${PROJECT_VERSION}")

# --- core (no widgets) ---------------------------------------------------------------------------
add_library(launcher_core STATIC
    src/core/audio_output.cpp src/core/audio_output.h
    src/core/bindings.cpp src/core/bindings.h
    src/core/cli_cache.cpp src/core/cli_cache.h
    src/core/config_schema.cpp src/core/config_schema.h
    src/core/exit_codes.cpp src/core/exit_codes.h
    src/core/frame_rate.cpp src/core/frame_rate.h
    src/core/game_cli.cpp src/core/game_cli.h
    src/core/game_paths.cpp src/core/game_paths.h
    src/core/game_session.cpp src/core/game_session.h
    src/core/gpu_info.cpp src/core/gpu_info.h
    src/core/ini_document.cpp src/core/ini_document.h
    src/core/iso_check.cpp src/core/iso_check.h
    src/core/l10n.cpp src/core/l10n.h src/core/l10n_table.cpp
    src/core/log_tail.cpp src/core/log_tail.h
    src/core/pad_reader.cpp src/core/pad_reader.h
    src/core/presets.cpp src/core/presets.h
    src/core/settings_model.cpp src/core/settings_model.h
    src/core/validation.cpp src/core/validation.h
)
target_include_directories(launcher_core PUBLIC src)
target_compile_options(launcher_core PUBLIC ${LAUNCHER_COMPILE_OPTIONS})
target_compile_definitions(launcher_core PUBLIC ${LAUNCHER_DEFINITIONS})
target_link_libraries(launcher_core PUBLIC Qt6::Core)
# M5: audio output devices (WASAPI) and the test tone (XAudio2 2.9, part of Windows 10+).
target_link_libraries(launcher_core PUBLIC ole32 xaudio2)

# --- widgets ---------------------------------------------------------------------------------------
add_library(launcher_ui STATIC
    src/ui/app_context.cpp src/ui/app_context.h
    src/ui/app_style.cpp src/ui/app_style.h
    src/ui/audio_panel.cpp src/ui/audio_panel.h
    src/ui/automation.cpp src/ui/automation.h
    src/ui/binding_capture.cpp src/ui/binding_capture.h
    src/ui/controls_panel.cpp src/ui/controls_panel.h
    src/ui/iso_status.cpp src/ui/iso_status.h
    src/ui/key_editors.cpp src/ui/key_editors.h
    src/ui/main_window.cpp src/ui/main_window.h
    src/ui/pad_poller.cpp src/ui/pad_poller.h
    src/ui/screen_info.cpp src/ui/screen_info.h
    src/ui/settings_tab.cpp src/ui/settings_tab.h
    src/ui/status_panel.cpp src/ui/status_panel.h
    src/ui/wizard.cpp src/ui/wizard.h
)
target_link_libraries(launcher_ui PUBLIC launcher_core Qt6::Gui Qt6::Widgets)
qt_add_resources(launcher_ui "launcher_icons"
    PREFIX "/icons"
    BASE resources/icon
    FILES
        resources/icon/icon_16.png resources/icon/icon_20.png resources/icon/icon_24.png
        resources/icon/icon_32.png resources/icon/icon_40.png resources/icon/icon_48.png
        resources/icon/icon_64.png resources/icon/icon_128.png resources/icon/icon_256.png
)

# --- executable ------------------------------------------------------------------------------------
qt_add_executable(FuturamaLauncher WIN32 src/main.cpp resources/launcher.rc)
target_link_libraries(FuturamaLauncher PRIVATE launcher_ui)
# The manifest in launcher.rc declares longPathAware; DPI awareness stays with Qt (per-monitor v2).
target_link_options(FuturamaLauncher PRIVATE /MANIFEST:NO)
set_source_files_properties(resources/launcher.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon/launcher.ico;${CMAKE_CURRENT_SOURCE_DIR}/resources/launcher.manifest")

# --- test double of futurama.exe -------------------------------------------------------------------
add_executable(fake_futurama WIN32 fakegame/fake_futurama.cpp)
set_target_properties(fake_futurama PROPERTIES
    OUTPUT_NAME futurama
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/fakegame
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_options(fake_futurama PRIVATE ${LAUNCHER_COMPILE_OPTIONS})
target_compile_definitions(fake_futurama PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    FAKE_TESTDATA_DIR=L"${CMAKE_CURRENT_SOURCE_DIR}/testdata")
target_link_options(fake_futurama PRIVATE /ENTRY:wmainCRTStartup)
target_link_libraries(fake_futurama PRIVATE shell32)
add_custom_command(TARGET fake_futurama POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_BINARY_DIR}/fakegame/futurama_host.dll"
    COMMENT "fake_futurama: placeholder futurama_host.dll next to the test double")

# --- tests -----------------------------------------------------------------------------------------
enable_testing()
qt_add_executable(launcher_tests
    tests/test_main.cpp tests/test_support.h
    tests/tst_ini.cpp tests/tst_ini.h
    tests/tst_schema.cpp tests/tst_schema.h
    tests/tst_validation.cpp tests/tst_validation.h
    tests/tst_paths.cpp tests/tst_paths.h
    tests/tst_misc.cpp tests/tst_misc.h
    tests/tst_bindings.cpp tests/tst_bindings.h
)
target_link_libraries(launcher_tests PRIVATE launcher_core Qt6::Test)
target_compile_definitions(launcher_tests PRIVATE
    LAUNCHER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    LAUNCHER_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata"
    LAUNCHER_FAKE_GAME="${CMAKE_BINARY_DIR}/fakegame/futurama.exe")
add_dependencies(launcher_tests fake_futurama)
set_target_properties(launcher_tests PROPERTIES WIN32_EXECUTABLE OFF)

# Widget tests (QApplication, offscreen platform): main window edits/save/language/game run, wizard.
qt_add_executable(launcher_ui_tests tests/tst_ui.cpp tests/test_support.h)
target_link_libraries(launcher_ui_tests PRIVATE launcher_ui Qt6::Test)
target_compile_definitions(launcher_ui_tests PRIVATE
    LAUNCHER_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata"
    LAUNCHER_FAKE_GAME="${CMAKE_BINARY_DIR}/fakegame/futurama.exe")
add_dependencies(launcher_ui_tests fake_futurama)
set_target_properties(launcher_ui_tests PROPERTIES WIN32_EXECUTABLE OFF)

get_target_property(LAUNCHER_QMAKE Qt6::qmake IMPORTED_LOCATION)
get_filename_component(LAUNCHER_QT_BIN "${LAUNCHER_QMAKE}" DIRECTORY)
add_test(NAME launcher_tests COMMAND launcher_tests)
add_test(NAME launcher_ui_tests COMMAND launcher_ui_tests)
add_test(NAME launcher_screenshots
    COMMAND FuturamaLauncher -platform offscreen --screenshots=${CMAKE_BINARY_DIR}/screenshots
            --game-dir=${CMAKE_BINARY_DIR}/fakegame --config=${CMAKE_BINARY_DIR}/screenshots/futurama.ini)
# Review M3c finding 11: unexpected positional arguments (an unquoted path with blanks) are a usage error (exit code
# 64) instead of being ignored; the message names them and asks for quotes.
add_test(NAME launcher_cli_positional
    COMMAND FuturamaLauncher -platform offscreen --lang=en --game-dir=${CMAKE_BINARY_DIR}/fakegame
            --config=${CMAKE_BINARY_DIR}/cli_positional/my --set render.msaa=8 --save "games/futurama.ini")
set_tests_properties(launcher_cli_positional PROPERTIES
    PASS_REGULAR_EXPRESSION "Unexpected command line arguments: games/futurama.ini")
set_tests_properties(launcher_tests launcher_ui_tests launcher_screenshots launcher_cli_positional PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${LAUNCHER_QT_BIN};QT_QPA_PLATFORM=set:offscreen")
