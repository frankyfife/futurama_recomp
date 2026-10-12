// Configuration schema: definition table + validation (see config_schema.h).
//
// Evidence for defaults and accepted values (the modules that read the keys):
//   [paths]    core/config.cpp (iso, xbe, userdata), vfs/vfs_init.cpp (disc_dir)
//   [debug]    core/config.cpp, core/crash.cpp (minidump, mmio_trap), platform/platform.cpp (timeout),
//              core/host_main.cpp (exit_hang_test), core/boot.cpp (stop_at, crash_test), gfx/gfx_config.cpp (frames)
//              input/input_script.cpp (input_script, M4), video/xmv_hle.cpp (movies, M6)
//   [kernel]   kernel/kernel_settings.cpp (min_guest_stack 0x10000..0x4000000), kernel/mm_kernel_stack.cpp
//              (fiber_stack, clamped), kernel/kernel_api.cpp (strict)
//   [log]      core/log.cpp ParseLevel (error|warn|info|debug|trace, "warning" = warn)
//   [settings] kernel/kernel_settings.cpp kLanguages + KernelSettings defaults
//   [saves]    kernel/kernel_settings.cpp (key_mode, signature_key, xbox_cert_key)
//   [render]   gfx/gfx_config.cpp (M2/M3/M3b keys); M3c keys resolution, render_scale, msaa, alpha_to_coverage,
//              anisotropy (shared contract item 5, read by the renderer); M3d interpolation, interpolation_debug,
//              interpolation_hierarchy (gfx/interp_history.cpp SettingsFromConfig)
//   [audio]    M5 enabled, device, master_volume, limiter (audio/audio_engine.cpp, DSOUND core agent; dummy_streams
//              is obsolete since M5); M5 capture keys capture_wav, capture_format, capture_stems, event_log
//              (audio/capture_session.cpp, tooling agent); M5 speakers, reverb (audio/audio_engine.cpp) and
//              positional, trim_3d (audio/spatial_x3d.cpp, codec/3D/reverb agent)
//   [game]     xapi/game_timing.cpp (tick_rate, frame_clock); M6 video/movie_skip.cpp (skip_videos)
//   [input]    input/input_config.cpp (M4 input agent; binding lists input/bindings.h, src/INTERFACES.md 4.26)
#include "core/config_schema.h"

#include "input/bindings.h"  // M4: validators of the [input] binding lists

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace core {
namespace {

using T = ConfigKeyType;
using Tab = ConfigUiTab;
using W = ConfigUiWidget;

// ---- sections ----------------------------------------------------------------------------------------------
constexpr ConfigSectionDef kSections[] = {
    {"game", {"Spiellogik", "Game logic"}},
    {"render", {"Grafik", "Graphics"}},
    {"input", {"Eingabegeräte und Vibration", "Input devices and rumble"}},
    {"freecam", {"Freie Kamera", "Free camera"}},
    {"paths", {"Pfade", "Paths"}},
    {"settings", {"Xbox-Systemeinstellungen", "Xbox system settings"}},
    {"saves", {"Spielstände", "Save games"}},
    {"audio", {"Audio", "Audio"}},
    {"log", {"Protokoll", "Logging"}},
    {"debug", {"Fehlersuche", "Debugging"}},
    {"kernel", {"Xbox-Kernel", "Xbox kernel"}},
    {"hooks", {"Hooks", "Hooks"}},
};

// ---- choice lists ------------------------------------------------------------------------------------------
// Names of kLanguages in kernel/kernel_settings.cpp (unit test "kernel: every [settings] language choice ...").
// The European disc has five languages; the other Xbox languages make the game fall back to English.
constexpr ConfigChoice kLanguageChoices[] = {
    {"english", {"Englisch", "English"}},
    {"german", {"Deutsch", "German"}},
    {"french", {"Französisch", "French"}},
    {"spanish", {"Spanisch", "Spanish"}},
    {"italian", {"Italienisch", "Italian"}},
    {"japanese", {"Japanisch", "Japanese"}, true},
    {"korean", {"Koreanisch", "Korean"}, true},
    {"tchinese", {"Chinesisch (traditionell)", "Chinese (traditional)"}, true},
    {"portuguese", {"Portugiesisch", "Portuguese"}, true},
    {"1..9", {"Xbox-Sprachnummer 1 bis 9", "Xbox language number 1 to 9"}, true},
};
constexpr ConfigChoice kKeyModeChoices[] = {
    {"fixed", {"Fester Projektschlüssel", "Fixed project key"}},
    {"xbox", {"Schlüssel einer echten Xbox", "Key of a real Xbox"}},
};
constexpr ConfigChoice kLogLevelChoices[] = {
    {"error", {"Nur Fehler", "Errors only"}},
    {"warn", {"Fehler und Warnungen", "Errors and warnings"}},
    {"info", {"Informationen (Standard)", "Information (default)"}},
    {"debug", {"Ausführlich (Debug)", "Detailed (debug)"}},
    {"trace", {"Alles (sehr ausführlich)", "Everything (very verbose)"}},
    {"warning", {"Fehler und Warnungen", "Errors and warnings"}, true},
};
constexpr ConfigChoice kD3dTrapChoices[] = {
    {"abort", {"Abbrechen (Standard)", "Abort (default)"}},
    {"log", {"Nur protokollieren und weiterlaufen", "Log only and continue"}},
};
constexpr ConfigChoice kBackendChoices[] = {
    {"d3d11", {"Direct3D 11", "Direct3D 11"}},
    {"null", {"Kein Bild (nur für Tests)", "No output (testing only)"}},
};
constexpr ConfigChoice kDebugLayerChoices[] = {
    {"auto", {"Automatisch (nur Debug-Builds)", "Automatic (debug builds only)"}},
    {"on", {"An", "On"}},
    {"off", {"Aus", "Off"}},
};
constexpr ConfigChoice kAdapterChoices[] = {
    {"hardware", {"Grafikkarte", "Graphics card"}},
    {"warp", {"Software (WARP, sehr langsam)", "Software (WARP, very slow)"}},
};
constexpr ConfigChoice kScalingChoices[] = {
    {"linear", {"Weich (linear)", "Smooth (linear)"}},
    {"point", {"Scharf (nächster Pixel)", "Sharp (nearest pixel)"}},
    {"integer", {"Ganzzahlig (pixelgenau, mit Rand)", "Integer (pixel-exact, with border)"}},
};
constexpr ConfigChoice kPacingChoices[] = {
    {"auto", {"Automatisch (empfohlen)", "Automatic (recommended)"}},
    {"vsync", {"Immer an den Bildschirm gekoppelt", "Always coupled to the display"}},
    {"timer", {"Zeitgeber", "Timer"}},
    {"off", {"Ungebremst (nur für Tests)", "Unthrottled (testing only)"}},
};
// M3c (renderer agent): [render] screenshot_size (gfx/gfx_config.cpp).
constexpr ConfigChoice kScreenshotSizeChoices[] = {
    {"output", {"Wie angezeigt", "As shown"}},
    {"internal", {"Intern (Supersampling-Größe)", "Internal (supersampling size)"}},
};
// M5 (tooling agent): [audio] capture_format (audio/capture_session.cpp).
constexpr ConfigChoice kCaptureFormatChoices[] = {
    {"float", {"32-Bit-Gleitkomma (exakt)", "32-bit floating point (exact)"}},
    {"pcm16", {"16-Bit-PCM", "16-bit PCM"}},
};
// M5 (codec/3D/reverb agent): [audio] speakers (audio/audio_engine.cpp ParseSpeakers: auto|stereo|2.0|5.1) and
// [audio] positional (audio/spatial_model.h ParsePositionalMode).
constexpr ConfigChoice kSpeakerChoices[] = {
    {"auto", {"Automatisch (wie das Ausgabegerät)", "Automatic (like the output device)"}},
    {"stereo", {"Stereo", "Stereo"}},
    {"5.1", {"5.1-Surround", "5.1 surround"}},
    {"2.0", {"Stereo", "Stereo"}, true},
};
// M5 (integration): [audio] device_test (audio/audio_engine.cpp EngineConfigFromSettings).
constexpr ConfigChoice kDeviceTestChoices[] = {
    {"off", {"Aus", "Off"}},
    {"absent", {"Kein Ausgabegerät", "No output device"}},
    {"loss", {"Gerät alle 20 s verlieren", "Lose the device every 20 s"}},
};
constexpr ConfigChoice kPositionalChoices[] = {
    {"corrected", {"Richtig herum (empfohlen)", "Correct orientation (recommended)"}},
    {"xbox", {"Wie auf der Xbox (vorne und hinten vertauscht)", "As on the Xbox (front and back swapped)"}},
    {"off", {"Aus (alle Geräusche von vorne)", "Off (every sound from the front)"}},
};
constexpr ConfigChoice kMsaaChoices[] = {
    {"off", {"Aus", "Off"}},
    {"2", {"2×", "2×"}},
    {"4", {"4×", "4×"}},
    {"8", {"8×", "8×"}},
};
constexpr ConfigChoice kAnisotropyChoices[] = {
    {"off", {"Aus", "Off"}},
    {"2", {"2×", "2×"}},
    {"4", {"4×", "4×"}},
    {"8", {"8×", "8×"}},
    {"16", {"16×", "16×"}},
};
// Suggestions for [render] resolution (type resolution: any valid size is accepted; the launcher adds the
// monitor's modes from --query-gpu).
constexpr ConfigChoice kResolutionSuggestions[] = {
    {"auto", {"Automatisch (Fenster bzw. Bildschirm)", "Automatic (window or screen)"}},
    {"640x480", {"640×480 (wie auf der Xbox)", "640×480 (as on the Xbox)"}},
    {"1280x960", {"1280×960 (2×)", "1280×960 (2×)"}},
    {"1920x1440", {"1920×1440 (3×)", "1920×1440 (3×)"}},
    {"2560x1920", {"2560×1920 (4×)", "2560×1920 (4×)"}},
    {"2880x2160", {"2880×2160 (4:3 in 4K)", "2880×2160 (4:3 in 4K)"}},
};
// xapi/game_timing.cpp ParseTickRate (unit test "timing: ParseTickRate accepts exactly the choices ...").
constexpr ConfigChoice kTickRateChoices[] = {
    {"60", {"60 Hz (Standard)", "60 Hz (default)"}},
    {"50", {"50 Hz (PAL-Original)", "50 Hz (original PAL)"}},
};
// M6: [debug] movies (video/xmv_hle.cpp).
constexpr ConfigChoice kDebugMoviesChoices[] = {
    {"play", {"Abspielen (Standard)", "Play (default)"}},
    {"skip", {"Sofort beenden (wie vor M6)", "End at once (as before M6)"}},
};
// M6: [game] skip_videos (video/movie_skip.cpp).
constexpr ConfigChoice kSkipVideosChoices[] = {
    {"always", {"Immer (Standard)", "Always (default)"}},
    {"original", {"Wie im Original", "As in the original"}},
};
constexpr ConfigChoice kFrameClockChoices[] = {
    {"lockstep", {"Bildsynchron (empfohlen)", "Frame-locked (recommended)"}},
    {"realtime", {"Echtzeit (wie das Original)", "Real time (like the original)"}},
};
// M3d (frame-clock/pacing agent): [render] frame_rate, gfx/gfx_config.cpp ParseFrameRate (unit test "pacer m3d:
// ParseFrameRate accepts exactly the schema choices"). 60 = the M3b behaviour; 120 and display choose the rate from
// the refresh (platform/pacer_policy.h FrameRateMode). 50 (the PAL logic rate, which an unset frame_rate follows) and
// the numbers 0..1000 stay accepted as developer values (0 = unthrottled).
constexpr ConfigChoice kFrameRateChoices[] = {
    // The labels the user asked for (ROADMAP M3d; M3d review finding 12).
    {"60", {"60", "60"}},
    {"120", {"120 (interpoliert)", "120 (interpolated)"}},
    {"display", {"Monitor (max. 120)", "Monitor (max. 120)"}},
    {"50", {"50 fps (PAL-Logik-Takt)", "50 fps (PAL logic rate)"}, true},
    {"0..1000", {"Feste Bildrate (Entwicklerwert)", "Fixed frame rate (developer value)"}, true},
};
// M3d (interpolation agent): gfx/interp_history.h ParseDebugMode.
constexpr ConfigChoice kInterpolationDebugChoices[] = {
    {"off", {"Aus", "Off"}},
    {"tint", {"Nicht interpolierte Objekte einfärben", "Tint objects that are not interpolated"}},
    {"trace", {"Zähler je Bild in eine CSV-Datei", "Per-frame counters to a CSV file"}},
};

// M4 (input agent): [input] pad_mode, input/input_config.h PadMode.
constexpr ConfigChoice kPadModeChoices[] = {
    {"single", {"Ein Spieler: alle Controller an Anschluss 1", "One player: all controllers on port 1"}},
    {"per_port", {"Ein Anschluss je Controller", "One port per controller"}},
};
// [freecam] toggles (M4b, gfx/freecam.cpp).
constexpr ConfigChoice kFreecamPadChoices[] = {
    {"l3r3", {"Beide Sticks drücken (L3+R3)", "Press both sticks (L3+R3)"}},
    {"off", {"Aus", "Off"}},
};
constexpr ConfigChoice kFreecamKeyChoices[] = {
    {"off", {"Aus", "Off"}}, {"f1", {"F1", "F1"}},   {"f2", {"F2", "F2"}},   {"f3", {"F3", "F3"}},
    {"f4", {"F4", "F4"}},    {"f5", {"F5", "F5"}},   {"f6", {"F6", "F6"}},   {"f7", {"F7", "F7"}},
    {"f8", {"F8", "F8"}},    {"f9", {"F9", "F9"}},   {"f10", {"F10", "F10"}}, {"f11", {"F11", "F11"}},
    {"f12", {"F12", "F12"}},
};
// Shared help texts of the [input] binding rows (src/INTERFACES.md 4.26).
constexpr ConfigText kPadBindingHelp = {
    "Tasten, Trigger oder Stickrichtungen des Xbox-Controllers, die diese Xbox-Taste auslösen, durch Komma getrennt "
    "(z. B. pad:a, pad:rt). Leer = nicht belegt.",
    "Buttons, triggers or stick directions of the Xbox controller that trigger this Xbox control, separated by commas "
    "(e.g. pad:a, pad:rt). Empty = not bound."};
constexpr ConfigText kKbmBindingHelp = {
    "Tasten und Mauseingaben, die diese Xbox-Taste auslösen, durch Komma getrennt (z. B. key:enter, mouse:left). Leer "
    "= nicht belegt.",
    "Keys and mouse inputs that trigger this Xbox control, separated by commas (e.g. key:enter, mouse:left). Empty = "
    "not bound."};

#ifdef NDEBUG
constexpr const char* kDebugBuildDefault = "0";
#else
constexpr const char* kDebugBuildDefault = "1";
#endif

// G1 (outlines, gfx/gfx_config.h ParseOutlineColor / ParseOutlineOpacity accept the same values): "original" or
// 6 hex digits (optional '#'), "original" or a number 0..1. Blanks around the value are ignored.
std::string_view TrimOutlineValue(std::string_view v) noexcept {
    while (!v.empty() && (v.front() == ' ' || v.front() == '	')) {
        v.remove_prefix(1);
    }
    while (!v.empty() && (v.back() == ' ' || v.back() == '	')) {
        v.remove_suffix(1);
    }
    return v;
}
bool IsOriginalWord(std::string_view v) noexcept {
    static constexpr char kWord[] = "original";
    if (v.size() != sizeof(kWord) - 1) {
        return false;
    }
    for (std::size_t i = 0; i < v.size(); ++i) {
        if ((v[i] | 0x20) != kWord[i]) {
            return false;
        }
    }
    return true;
}
bool ValidateOutlineColor(std::string_view value, std::string& error) noexcept {
    std::string_view v = TrimOutlineValue(value);
    if (IsOriginalWord(v)) {
        return true;
    }
    if (!v.empty() && v.front() == '#') {
        v.remove_prefix(1);
    }
    bool ok = v.size() == 6;
    for (std::size_t i = 0; ok && i < v.size(); ++i) {
        const char c = static_cast<char>(v[i] | 0x20);
        ok = (v[i] >= '0' && v[i] <= '9') || (c >= 'a' && c <= 'f');
    }
    if (!ok) {
        error = "expected original or a colour RRGGBB (6 hex digits, e.g. 000000 = black)";
    }
    return ok;
}
bool ValidateOutlineOpacity(std::string_view value, std::string& error) noexcept {
    const std::string_view v = TrimOutlineValue(value);
    if (IsOriginalWord(v)) {
        return true;
    }
    double d = 0.0;
    if (!ParseConfigFloat(v, d) || d < 0.0 || d > 1.0) {
        error = "expected original or a number from 0 to 1";
        return false;
    }
    return true;
}

constexpr std::uint32_t kAdv = kConfigAdvanced;
constexpr std::uint32_t kDev = kConfigAdvanced | kConfigDeveloper;
constexpr std::uint32_t kDyn = kConfigDynamicDefault;
constexpr std::uint32_t kHint = kConfigRangeIsHint;

// ---- the key table -----------------------------------------------------------------------------------------
// Grouped by section (schema order of kSections). ui.order is unique per tab.
constexpr ConfigKeyDef kKeys[] = {
    // [game] (M3b, xapi/game_timing.h)
    {.section = "game", .name = "tick_rate", .type = T::Choice, .defaultValue = "60", .choices = kTickRateChoices,
     .label = {"Logik-Takt", "Game logic rate"},
     .help = {"Wie oft das Spiel pro Sekunde rechnet. 60 Hz läuft flüssiger und so schnell wie die NTSC-Fassung; "
              "50 Hz entspricht dem Tempo der europäischen Xbox-Fassung.",
              "How often the game updates per second. 60 Hz is smoother and runs at the speed of the NTSC release; "
              "50 Hz matches the speed of the European Xbox release."},
     .tab = Tab::Game, .order = 20, .widget = W::Combo},
    {.section = "game", .name = "frame_clock", .type = T::Choice, .defaultValue = "lockstep",
     .choices = kFrameClockChoices, .label = {"Zeitquelle der Spiellogik", "Game logic time source"},
     .help = {"Woher die Spiellogik ihre Zeit nimmt. „Bildsynchron“ rechnet genau einen Schritt pro angezeigtem "
              "Bild und vermeidet so Ruckler; „Echtzeit“ liest die Uhr wie das Original.",
              "Where the game logic takes its time from. Frame-locked computes exactly one step per displayed frame "
              "and so avoids stutter; real time reads the clock like the original."},
     .tab = Tab::Advanced, .order = 200, .widget = W::Combo, .flags = kAdv},
    // M6 (video/movie_skip.cpp): skip condition of the cut scenes (FUN_000D3120 probe hook).
    {.section = "game", .name = "skip_videos", .type = T::Choice, .defaultValue = "always",
     .choices = kSkipVideosChoices, .label = {"Filme überspringen", "Skip movies"},
     .help = {"Ob sich Filme und Zwischensequenzen mit A oder START überspringen lassen. „Immer“ erlaubt das bei "
              "jedem Film sofort; „Wie im Original“ sperrt die ersten beiden Filme von Level 1-1 beim ersten Ansehen "
              "und jede Zwischensequenz in ihren ersten 2,5 Sekunden.",
              "Whether movies and cut scenes can be skipped with A or START. \"Always\" allows it for every movie "
              "right away; \"As in the original\" locks the first two movies of level 1-1 on first viewing and "
              "every cut scene in its first 2.5 seconds."},
     .tab = Tab::Game, .order = 30, .widget = W::Combo},

    // [render] - window and output (M3, gfx/gfx_config.h)
    // M3c (integration): monitor selection, platform/window.h WindowOptions::monitor. Type string: the values are
    // the GDI device names of --query-gpu "outputs[].device"; the launcher shows them as a monitor list.
    {.section = "render", .name = "monitor", .type = T::String, .defaultValue = "primary",
     .label = {"Bildschirm", "Display"},
     .help = {"Auf welchem Bildschirm das Spiel startet, im Fenster und im Vollbild. „primary“ ist der "
              "Hauptbildschirm von Windows, sonst der Gerätename aus der Bildschirmliste (z. B. \\\\.\\DISPLAY2). "
              "Ist dieser Bildschirm nicht angeschlossen, startet das Spiel auf dem Hauptbildschirm.",
              "The display the game starts on, in a window and in fullscreen. \"primary\" is the Windows main "
              "display, otherwise the device name from the display list (e.g. \\\\.\\DISPLAY2). If that display is "
              "not connected, the game starts on the main display."},
     .tab = Tab::Graphics, .order = 5, .widget = W::Monitor},
    {.section = "render", .name = "fullscreen", .type = T::Bool, .defaultValue = "0",
     .label = {"Randloses Vollbild", "Borderless fullscreen"},
     .help = {"Spielt in einem randlosen Fenster über den ganzen Bildschirm. Im Spiel wechselt "
              "Alt+Eingabetaste zwischen Fenster und Vollbild.",
              "Plays in a borderless window that covers the whole screen. In the game, Alt+Enter switches "
              "between window and fullscreen."},
     .tab = Tab::Graphics, .order = 10, .widget = W::Check},
    {.section = "render", .name = "window_scale", .type = T::Int, .defaultValue = "2", .ranged = true, .min = 1,
     .max = 8, .step = 1, .label = {"Fenstergröße", "Window size"},
     .help = {"Größe des Fensters beim Start als Vielfaches von 640×480, bezogen auf die Skalierung von "
              "Windows (2 = 1280×960 bei 100 %, 2880×2160 Pixel bei 225 %). Ein zu großes Fenster wird auf den "
              "Bildschirm verkleinert.",
              "Size of the window at startup as a multiple of 640×480, relative to the Windows display scaling "
              "(2 = 1280×960 at 100 %, 2880×2160 pixels at 225 %). A window that is too large is shrunk to fit "
              "the screen."},
     .tab = Tab::Graphics, .order = 20, .widget = W::Spin, .flags = kHint},
    // [render] - M3c render resolution and anti-aliasing (shared contract item 5; renderer agent)
    {.section = "render", .name = "resolution", .type = T::Resolution, .defaultValue = "auto",
     .choices = kResolutionSuggestions, .ranged = true, .min = kConfigResolutionMin, .max = kConfigResolutionMax,
     .step = 1, .label = {"Renderauflösung", "Render resolution"},
     .help = {"Auflösung, in der das Spiel gezeichnet wird. „Automatisch“ nimmt die größte 4:3-Fläche im Fenster "
              "bzw. auf dem Bildschirm, der Rest bleibt schwarz. 640×480 entspricht der Xbox.",
              "Resolution the game is rendered at. Automatic uses the largest 4:3 area of the window or screen; "
              "the rest stays black. 640×480 matches the Xbox."},
     .tab = Tab::Graphics, .order = 30, .widget = W::Resolution},
    {.section = "render", .name = "render_scale", .type = T::Float, .defaultValue = "1.0", .ranged = true,
     .min = 1.0, .max = 2.0, .step = 0.25, .label = {"Supersampling (Render-Skala)", "Supersampling (render scale)"},
     .help = {"Zeichnet intern um diesen Faktor größer und rechnet das Bild danach herunter. Das glättet alle "
              "Kanten, kostet aber viel Grafikleistung und Grafikspeicher. 1,0 = aus. 2,0 rechnet exakt herunter "
              "(je 2×2 Pixel); Zwischenwerte werden gefiltert und zeichnen Schrift und 2D-Elemente etwas weicher. "
              "Lässt sich mit MSAA kombinieren.",
              "Renders internally larger by this factor and scales the picture down afterwards. This smooths "
              "every edge but costs a lot of GPU power and video memory. 1.0 = off. 2.0 scales down exactly (2×2 "
              "pixels each); values in between are filtered and draw text and 2D elements slightly softer. Can be "
              "combined with MSAA."},
     .tab = Tab::Graphics, .order = 40, .widget = W::DSpin},
    {.section = "render", .name = "msaa", .type = T::Choice, .defaultValue = "4", .choices = kMsaaChoices,
     .label = {"Kantenglättung (MSAA)", "Anti-aliasing (MSAA)"},
     .help = {"Glättet die Kanten von Figuren und Levels per Multisampling. Höhere Stufen sehen besser aus und "
              "kosten mehr Leistung. Kann die Grafikkarte eine Stufe nicht, wird automatisch die nächstkleinere "
              "genommen.",
              "Smooths the edges of characters and levels with multisampling. Higher levels look better and cost "
              "more performance. If the graphics card does not support a level, the next lower one is used "
              "automatically."},
     .tab = Tab::Graphics, .order = 50, .widget = W::Combo},
    {.section = "render", .name = "alpha_to_coverage", .type = T::Bool, .defaultValue = "on",
     .label = {"Kantenglättung für Gitter und Pflanzen", "Smooth grates and plants"},
     .help = {"Glättet mit MSAA auch Kanten innerhalb von Texturen, etwa bei Gittern, Zäunen und Pflanzen "
              "(Alpha-to-Coverage). Wirkt nur, wenn MSAA eingeschaltet ist.",
              "With MSAA, also smooths edges inside textures such as grates, fences and plants (alpha to "
              "coverage). Only has an effect when MSAA is on."},
     .tab = Tab::Graphics, .order = 60, .widget = W::Check},
    {.section = "render", .name = "anisotropy", .type = T::Choice, .defaultValue = "16", .choices = kAnisotropyChoices,
     .label = {"Anisotrope Filterung", "Anisotropic filtering"},
     .help = {"Hält Texturen auf schrägen Flächen wie Böden und Straßen auch in der Entfernung scharf. Kostet kaum "
              "Leistung.",
              "Keeps textures on angled surfaces such as floors and roads sharp in the distance. Costs very little "
              "performance."},
     .tab = Tab::Graphics, .order = 70, .widget = W::Combo},
    // Review M3c finding 3: resolve and downsample on linear light (gfx_config.h, rhi DeviceDesc::linearResolve).
    {.section = "render", .name = "linear_aa", .type = T::Bool, .defaultValue = "off",
     .label = {"Helligkeitsrichtige Kantenglättung", "Gamma-correct anti-aliasing"},
     .help = {"Mittelt die Kantenpixel von MSAA und Supersampling in linearem Licht statt wie die Xbox im "
              "Gamma-Raum. Helle, dünne Linien wie Blitze und Umrisse wirken dadurch gleichmäßiger und etwas "
              "kräftiger, dunkle Linien auf hellem Grund etwas feiner. Wirkt nur mit MSAA oder einer Render-Skala "
              "über 1,0. Aus = wie das Original.",
              "Averages the edge pixels of MSAA and supersampling in linear light instead of in gamma space like "
              "the Xbox. Thin bright lines such as lightning and outlines look more even and slightly stronger, "
              "dark lines on a bright background slightly finer. Only has an effect with MSAA or a render scale "
              "above 1.0. Off = like the original."},
     .tab = Tab::Graphics, .order = 75, .widget = W::Check},
    // G1 (character outlines, gfx/gfx_config.h OutlineSettings; analysis/loader/g1_outlines.md 7). Defaults = the
    // original look (no outline code runs).
    {.section = "render", .name = "outline_color", .type = T::String, .defaultValue = "original",
     .label = {"Konturfarbe", "Outline colour"},
     .help = {"Farbe der Konturlinien um Figuren und um den Kopf im Lebensanzeiger. „original“ = wie auf der Xbox "
              "(schwarz). Sonst eine Farbe als RRGGBB in Hexadezimal, z. B. 000000 = schwarz, FFFFFF = weiß, "
              "FF0000 = rot.",
              "Colour of the outlines around characters and around the head in the health display. \"original\" = "
              "like on the Xbox (black). Otherwise a colour as RRGGBB in hexadecimal, e.g. 000000 = black, "
              "FFFFFF = white, FF0000 = red."},
     .tab = Tab::Graphics, .order = 76, .widget = W::Color, .validate = &ValidateOutlineColor},
    {.section = "render", .name = "outline_opacity", .type = T::String, .defaultValue = "original",
     .label = {"Konturdeckkraft", "Outline opacity"},
     .help = {"Wie deckend die Konturlinien sind, von 0 (unsichtbar) bis 1 (voll deckend). „original“ = wie auf "
              "der Xbox (Figuren 0,75).",
              "How opaque the outlines are, from 0 (invisible) to 1 (fully opaque). \"original\" = like on the "
              "Xbox (characters 0.75)."},
     .tab = Tab::Graphics, .order = 77, .widget = W::Fraction, .validate = &ValidateOutlineOpacity},
    {.section = "render", .name = "outline_thickness", .type = T::Float, .defaultValue = "1.0", .ranged = true,
     .min = 0.0, .max = 2.5, .step = 0.25, .label = {"Konturstärke", "Outline thickness"},
     .help = {"Faktor auf die Dicke der Konturlinien. 1,0 = wie auf der Xbox, 2,0 = doppelt so dick, 0 = keine "
              "Konturlinien. Empfohlen bis etwa 2: Die Konturen sind aufgeblasene Kopien der Figuren, bei größeren "
              "Werten scheinen sie an Nase und Augen durch. Wie im Original werden die Linien in der Entfernung "
              "dünner und verschwinden weit weg ganz.",
              "Factor on the thickness of the outlines. 1.0 = like on the Xbox, 2.0 = twice as thick, 0 = no "
              "outlines. Recommended up to about 2: the outlines are inflated copies of the characters, with larger "
              "values they show through at nose and eyes. As in the original, the lines get thinner with distance "
              "and disappear far away."},
     .tab = Tab::Graphics, .order = 78, .widget = W::DSpin},
    {.section = "render", .name = "scaling", .type = T::Choice, .defaultValue = "linear", .choices = kScalingChoices,
     .label = {"Bildskalierung", "Image scaling"},
     .help = {"Wie das fertige Bild auf die Fenstergröße gebracht wird, wenn die Renderauflösung kleiner ist. "
              "„Ganzzahlig“ vergrößert nur um ganze Faktoren und lässt einen schwarzen Rand.",
              "How the finished picture is fitted to the window when the render resolution is smaller. Integer "
              "only scales by whole factors and leaves a black border."},
     .tab = Tab::Graphics, .order = 80, .widget = W::Combo},
    {.section = "render", .name = "vsync", .type = T::Bool, .defaultValue = "1", .label = {"VSync", "VSync"},
     .help = {"Gibt die Bilder im Takt des Bildschirms aus und verhindert Bildrisse. Aus: etwas weniger "
              "Verzögerung, aber mögliche Bildrisse (außer mit VRR wie G-SYNC oder FreeSync).",
              "Shows the frames in step with the display and prevents tearing. Off: slightly less latency, but "
              "possible tearing (except with VRR such as G-SYNC or FreeSync)."},
     .tab = Tab::Graphics, .order = 90, .widget = W::Check},
    {.section = "render", .name = "pacing", .type = T::Choice, .defaultValue = "auto", .choices = kPacingChoices,
     .label = {"Bildtakt", "Frame pacing"},
     .help = {"Wie die Bilder zeitlich verteilt werden. „Automatisch“ koppelt an den Bildschirm (bei 120 Hz jedes "
              "zweite Bild) oder nutzt einen Zeitgeber mit VRR.",
              "How the frames are spaced in time. Automatic couples to the display (every second refresh at "
              "120 Hz) or uses a timer with VRR."},
     .tab = Tab::Graphics, .order = 100, .widget = W::Combo, .flags = kAdv},
    // M3d (frame-clock/pacing agent): a choice since M3d (M3b/M3c: an int developer key). Unset = the logic rate
    // (dynamic default, review M3c finding 18): 60 with [game] tick_rate = 60, 50 with 50.
    {.section = "render", .name = "frame_rate", .type = T::Choice, .defaultValue = "60", .choices = kFrameRateChoices,
     .label = {"Bildrate", "Frame rate"},
     .help = {"Wie viele Bilder pro Sekunde das Spiel zeigt. Die Spiellogik bleibt beim Logik-Takt (Reiter "
              "„Spiel“). „120“ und „Monitor“ zeigen auf Bildschirmen mit mehr als 60 Hz bis zu 120 Bilder pro "
              "Sekunde und berechnen die Bilder zwischen zwei Logikschritten. „Monitor“ nimmt die höchste "
              "gleichmäßige Rate (120 bei 120 oder 240 Hz, 72 bei 144 Hz ohne VRR), „120“ bleibt dort bei 120 mit "
              "unterschiedlich langen Bildern. An einem Bildschirm mit 60 Hz oder weniger (auch 59,94 Hz) zeigen alle "
              "drei ein Bild pro Bildwechsel ohne Zwischenbilder.",
              "How many frames per second the game shows. The game logic keeps its logic rate (tab \"Game\"). "
              "\"120\" and \"Monitor\" show up to 120 frames per second on displays above 60 Hz and compute the "
              "frames between two logic steps. \"Monitor\" takes the highest even rate (120 at 120 or 240 Hz, 72 at "
              "144 Hz without VRR), \"120\" stays at 120 there with frames of different length. On a display with "
              "60 Hz or less (59.94 Hz too) all three show one frame per refresh without in-between frames."},
     .tab = Tab::Graphics, .order = 110, .widget = W::Combo, .flags = kDyn},
    // M3d (frame-clock/pacing agent): just-in-time frame start, platform/frame_pacer.h (M3c review finding 6).
    {.section = "render", .name = "low_latency", .type = T::Bool, .defaultValue = "on",
     .label = {"Geringe Verzögerung", "Low latency"},
     .help = {"Beginnt jedes Bild erst kurz bevor der Bildschirm es zeigen kann, statt direkt nach dem vorherigen. "
              "Das Spiel liest die Eingaben dadurch später und reagiert schneller (bei 60 Bildern pro Sekunde auf "
              "einem 120-Hz-Bildschirm etwa 12 ms). Wird die Zeit knapp, vergrößert das Spiel den Abstand selbst.",
              "Starts every frame only shortly before the display can show it instead of right after the previous "
              "one. The game reads the input later and reacts faster (about 12 ms at 60 frames per second on a "
              "120 Hz display). If the time gets short, the game widens the gap by itself."},
     .tab = Tab::Graphics, .order = 105, .widget = W::Check, .flags = kAdv},
    {.section = "render", .name = "screenshot_dir", .type = T::PathDir,
     .label = {"Screenshot-Ordner", "Screenshot folder"},
     .help = {"Hier werden PNG-Screenshots gespeichert. Leer = Ordner „screenshots“ in den Benutzerdaten.",
              "PNG screenshots are saved here. Empty = folder \"screenshots\" in the user data."},
     .tab = Tab::Paths, .order = 30, .widget = W::Dir, .flags = kDyn},
    // M3c (renderer agent): size of the screenshots (gfx/gfx_config.h ScreenshotSize).
    {.section = "render", .name = "screenshot_size", .type = T::Choice, .defaultValue = "output",
     .choices = kScreenshotSizeChoices, .label = {"Screenshot-Größe", "Screenshot size"},
     .help = {"„Wie angezeigt“ speichert das Bild in der Renderauflösung. „Intern“ speichert es in der "
              "Supersampling-Größe vor dem Herunterrechnen (nur mit Render-Skala über 1,0 größer).",
              "\"As shown\" saves the picture at the render resolution. \"Internal\" saves it at the supersampling "
              "size before it is scaled down (only larger with a render scale above 1.0)."},
     .tab = Tab::Advanced, .order = 100, .widget = W::Combo, .flags = kAdv},
    {.section = "render", .name = "backend", .type = T::Choice, .defaultValue = "d3d11", .choices = kBackendChoices,
     .label = {"Grafikschnittstelle", "Graphics API"},
     .help = {"Direct3D 11 zeigt das Spiel an. „Kein Bild“ läuft ohne Fenster und ist nur für automatische Tests "
              "gedacht.",
              "Direct3D 11 shows the game. \"No output\" runs without a window and is meant for automated tests "
              "only."},
     .tab = Tab::Advanced, .order = 110, .widget = W::Combo, .flags = kAdv},
    {.section = "render", .name = "adapter", .type = T::Choice, .defaultValue = "hardware", .choices = kAdapterChoices,
     .label = {"Grafikadapter", "Graphics adapter"},
     .help = {"„Software“ zeichnet ohne Grafikkarte auf dem Prozessor. Nur für Tests.",
              "Software renders on the processor without a graphics card. For testing only."},
     .tab = Tab::Advanced, .order = 120, .widget = W::Combo, .flags = kAdv},
    {.section = "render", .name = "debug_layer", .type = T::Choice, .defaultValue = "auto",
     .choices = kDebugLayerChoices, .label = {"D3D11-Debug-Layer", "D3D11 debug layer"},
     .help = {"Prüft jeden Direct3D-Aufruf und schreibt Fehler ins Protokoll. Macht das Spiel langsamer und "
              "braucht die optionalen Windows-Grafiktools.",
              "Checks every Direct3D call and writes errors to the log. Slows the game down and needs the optional "
              "Windows graphics tools."},
     .tab = Tab::Advanced, .order = 130, .widget = W::Combo, .flags = kAdv},
    {.section = "render", .name = "d3d_traps", .type = T::Choice, .defaultValue = "abort", .choices = kD3dTrapChoices,
     .label = {"Fehlende D3D-Funktionen", "Missing D3D functions"},
     .help = {"Was passiert, wenn das Spiel eine noch nicht nachgebaute Direct3D-Funktion aufruft: abbrechen oder "
              "nur protokollieren und weiterlaufen (kann zu Grafikfehlern führen).",
              "What happens when the game calls a Direct3D function that is not recreated yet: abort, or only log "
              "it and continue (may cause graphics glitches)."},
     .tab = Tab::Advanced, .order = 140, .widget = W::Combo, .flags = kAdv},
    {.section = "render", .name = "load_textures", .type = T::Bool, .defaultValue = "1",
     .label = {"Texturdateien laden", "Load texture files"},
     .help = {"Für Entwickler: Aus ersetzt Texturen, die aus eigenen Dateien kommen, durch eine Ersatztextur.",
              "For developers: off replaces textures that come from their own files with a placeholder texture."},
     .tab = Tab::Advanced, .order = 500, .widget = W::Check, .flags = kDev},
    {.section = "render", .name = "screenshot_frames", .type = T::String,
     .label = {"Automatische Screenshots", "Automatic screenshots"},
     .help = {"Für Entwickler: kommagetrennte Bildnummern, bei denen automatisch ein Screenshot entsteht.",
              "For developers: comma-separated frame numbers at which a screenshot is taken automatically."},
     .tab = Tab::Advanced, .order = 510, .widget = W::Text, .flags = kDev},
    {.section = "render", .name = "trace_frames", .type = T::String,
     .label = {"Zeichenaufrufe protokollieren", "Log draw calls"},
     .help = {"Für Entwickler: kommagetrennte Bildnummern, deren Zeichenaufrufe einzeln protokolliert werden.",
              "For developers: comma-separated frame numbers whose draw calls are logged one by one."},
     .tab = Tab::Advanced, .order = 520, .widget = W::Text, .flags = kDev},
    {.section = "render", .name = "validate", .type = T::Bool, .defaultValue = "0",
     .label = {"Datenprüfung ohne Bild", "Validation without output"},
     .help = {"Für Entwickler: Mit der Grafikschnittstelle „Kein Bild“ werden Geometrie und Texturen trotzdem "
              "ausgewertet und gezählt.",
              "For developers: with the graphics API \"No output\", geometry and textures are still evaluated and "
              "counted."},
     .tab = Tab::Advanced, .order = 530, .widget = W::Check, .flags = kDev},
    {.section = "render", .name = "pacing_trace", .type = T::PathFile,
     .label = {"Bildtakt-Messdatei (CSV)", "Frame pacing trace (CSV)"},
     .help = {"Für Entwickler: schreibt eine CSV-Datei mit einer Zeile pro Bild (Zeitmessung des Bildtakts).",
              "For developers: writes a CSV file with one line per frame (frame pacing measurements)."},
     .tab = Tab::Advanced, .order = 540, .widget = W::File, .flags = kDev, .fileFilter = "*.csv"},
    // [render] - M3d interpolation between logic ticks (interpolation agent, gfx/interp_history.h)
    {.section = "render", .name = "interpolation", .type = T::Bool, .defaultValue = "on",
     .label = {"Zwischenbilder berechnen", "Compute in-between frames"},
     .help = {"Bei mehr als 60 Bildern pro Sekunde werden Kamera, Figuren und Objekte zwischen zwei Schritten der "
              "Spiellogik interpoliert. Die Bewegung wird flüssiger und läuft dafür einen halben Logik-Schritt "
              "(8 ms) hinterher. Aus: Jedes Bild der Spiellogik wird zweimal gezeigt. Bei 60 Bildern pro Sekunde "
              "ohne Wirkung.",
              "With more than 60 frames per second, the camera, characters and objects are interpolated between two "
              "game logic steps. Motion becomes smoother and in return lags half a logic step (8 ms) behind. Off: "
              "every game logic picture is shown twice. No effect at 60 frames per second."},
     .tab = Tab::Graphics, .order = 112, .widget = W::Check, .flags = kAdv},
    {.section = "render", .name = "interpolation_debug", .type = T::Choice, .defaultValue = "off",
     .choices = kInterpolationDebugChoices, .label = {"Zwischenbilder prüfen", "Inspect in-between frames"},
     .help = {"Für Entwickler: „Einfärben“ zeigt Objekte, die nicht interpoliert werden (rot: Sprung oder neues "
              "Objekt, gelb: neu sichtbar, cyan: bildgetrieben, magenta: Kontextwechsel). „CSV“ schreibt je Bild eine "
              "Zeile mit den Zählern nach logs\\interp_trace.csv in den Benutzerdaten, für die Bilder aus "
              "„trace_frames“ zusätzlich je Objekt eine Zeile nach logs\\interp_draws.csv.",
              "For developers: tint shows objects that are not interpolated (red: jump or new object, yellow: newly "
              "visible, cyan: frame-driven, magenta: context switch). CSV writes one line of counters per frame to "
              "logs\\interp_trace.csv in the user data and, for the frames of trace_frames, one line per object to "
              "logs\\interp_draws.csv."},
     .tab = Tab::Advanced, .order = 548, .widget = W::Combo, .flags = kDev},
    // M3d (interpolation agent): stage 1b of analysis/loader/m3d_interpolation.md 3.5 (gfx/interp_history.h).
    {.section = "render", .name = "interpolation_hierarchy", .type = T::Bool, .defaultValue = "on",
     .label = {"Zwischenbilder entlang der Szenenhierarchie", "In-between frames along the scene hierarchy"},
     .help = {"Für Entwickler: Teile, die an bewegten Knoten hängen, folgen im Zwischenbild ihrer Kreisbahn (jede "
              "Ebene der Hierarchie wird für sich interpoliert). Aus: nur die Weltlage jedes Objekts wird "
              "interpoliert, Bögen werden dann zu Sehnen verkürzt.",
              "For developers: parts that hang on moving nodes follow their circular path in the in-between frame "
              "(every level of the hierarchy is interpolated on its own). Off: only the world placement of every "
              "object is interpolated, arcs are then shortened to chords."},
     .tab = Tab::Advanced, .order = 549, .widget = W::Check, .flags = kDev},

    // [input] (M4 input agent, input/input_config.h; src/INTERFACES.md 4.26 = the contract with the launcher's
    // "Steuerung" tab). Bindings target Xbox controls (the game's action table is fixed, m4_input.md 5.1/8.6).
    {.section = "input", .name = "pad_mode", .type = T::Choice, .defaultValue = "single", .choices = kPadModeChoices,
     .label = {"Controller-Anschlüsse", "Controller ports"},
     .help = {"„Ein Spieler“ legt alle Controller, Tastatur und Maus auf Anschluss 1; das Spiel hört im Level nur auf "
              "einen Controller. „Ein Anschluss je Controller“ gibt jedem Controller einen eigenen Anschluss.",
              "\"One player\" puts every controller, keyboard and mouse on port 1; in a level the game listens to one "
              "controller only. \"One port per controller\" gives every controller its own port."},
     .tab = Tab::Controls, .order = 10, .widget = W::Combo},
    {.section = "input", .name = "rumble", .type = T::Bool, .defaultValue = "1", .label = {"Vibration", "Rumble"},
     .help = {"Lässt den Controller vibrieren, wenn das Spiel es verlangt. Die Option „Vibration“ im Spiel muss "
              "ebenfalls an sein.",
              "Makes the controller vibrate when the game asks for it. The game's own \"Vibration\" option must be on "
              "as well."},
     .tab = Tab::Controls, .order = 20, .widget = W::Check},
    {.section = "input", .name = "rumble_strength", .type = T::Int, .defaultValue = "100", .ranged = true, .min = 0,
     .max = 100, .step = 5, .label = {"Stärke der Vibration", "Rumble strength"},
     .help = {"Stärke der Vibration in Prozent der Werte des Spiels.",
              "Rumble strength in percent of the game's values."},
     .tab = Tab::Controls, .order = 30, .widget = W::Slider, .unit = "%"},
    {.section = "input", .name = "pad_deadzone", .type = T::Int, .defaultValue = "0", .ranged = true, .min = 0,
     .max = 50, .step = 1, .label = {"Tote Zone der Sticks", "Stick dead zone"},
     .help = {"Kleine Stickausschläge bis zu diesem Anteil werden ignoriert, etwa bei einem Stick, der von selbst "
              "leicht ausschlägt. Das Spiel hat zusätzlich eine eigene, große tote Zone.",
              "Small stick deflections up to this share are ignored, e.g. for a stick that drifts slightly on its own. "
              "The game also has a large dead zone of its own."},
     .tab = Tab::Controls, .order = 40, .widget = W::Slider, .unit = "%"},
    {.section = "input", .name = "mouse_sensitivity", .type = T::Float, .defaultValue = "1.0", .ranged = true,
     .min = 0.1, .max = 10.0, .step = 0.1, .label = {"Mausempfindlichkeit", "Mouse sensitivity"},
     .help = {"Wie stark die Maus die Kamera dreht. Die Maus wirkt wie der rechte Stick, die größte "
              "Drehgeschwindigkeit legt das Spiel fest.",
              "How strongly the mouse turns the camera. The mouse acts like the right stick; the game sets the "
              "highest turning speed."},
     .tab = Tab::Controls, .order = 50, .widget = W::Slider, .unit = "×"},
    {.section = "input", .name = "mouse_capture", .type = T::Bool, .defaultValue = "1",
     .label = {"Maus im Fenster fangen", "Capture the mouse"},
     .help = {"Ein Klick ins Spielfenster fängt die Maus: Der Mauszeiger verschwindet und bleibt im Fenster, bis das "
              "Fenster den Fokus verliert (z. B. Alt+Tab). Im Vollbild geschieht das automatisch. Aus: Die Maus wirkt, "
              "solange das Fenster aktiv ist.",
              "A click into the game window captures the mouse: the cursor disappears and stays inside the window "
              "until the window loses the focus (e.g. Alt+Tab). In fullscreen this happens automatically. Off: the "
              "mouse works while the window is active."},
     .tab = Tab::Controls, .order = 60, .widget = W::Check},
    {.section = "input", .name = "menu_escape", .type = T::Bool, .defaultValue = "1",
     .label = {"START-Taste der Tastatur geht in Menüs zurück", "Keyboard START key goes back in menus"},
     .help = {"Im Hauptmenü und seinen Untermenüs wirken die Tasten, die unter „Tastatur und Maus“ auf START gelegt sind "
              "(Standard: Esc), wie BACK und gehen zurück, statt zu bestätigen. Im Spiel bleiben sie die Pausetaste. "
              "Controller sind nicht betroffen.",
              "In the main menu and its submenus the keys bound to START under \"Keyboard and mouse\" (default: Esc) "
              "act as BACK and go back instead of confirming. In the game they stay the pause key. Controllers are "
              "not affected."},
     .tab = Tab::Controls, .order = 70, .widget = W::Check},
    {.section = "input", .name = "background_input", .type = T::Bool, .defaultValue = "0",
     .label = {"Controller auch im Hintergrund", "Controllers in the background"},
     .help = {"Controller wirken auch, wenn das Spielfenster nicht im Vordergrund ist. Tastatur und Maus wirken nur im "
              "Vordergrund.",
              "Controllers also work while the game window is not in the foreground. Keyboard and mouse only work in "
              "the foreground."},
     .tab = Tab::Controls, .order = 80, .widget = W::Check, .flags = kAdv},
    {.section = "input", .name = "pad_a", .type = T::String, .defaultValue = "pad:a",
     .label = {"A – Bestätigen / Angriff 1", "A – Confirm / Attack 1"},
     .help = kPadBindingHelp, .tab = Tab::Controls, .order = 100, .widget = W::Binding,
     .validate = &inp::ValidatePadBindingValue},
    {.section = "input", .name = "pad_b", .type = T::String, .defaultValue = "pad:b",
     .label = {"B – Zurück / Springen", "B – Back / Jump"},
     .help = kPadBindingHelp, .tab = Tab::Controls, .order = 102, .widget = W::Binding,
     .validate = &inp::ValidatePadBindingValue},
    {.section = "input", .name = "pad_x", .type = T::String, .defaultValue = "pad:x",
     .label = {"X – Angriff 2", "X – Attack 2"},
     .help = kPadBindingHelp, .tab = Tab::Controls, .order = 104, .widget = W::Binding,
     .validate = &inp::ValidatePadBindingValue},
    {.section = "input", .name = "pad_y", .type = T::String, .defaultValue = "pad:y",
     .label = {"Y – Benutzen", "Y – Use"},
     .help = kPadBindingHelp, .tab = Tab::Controls, .order = 106, .widget = W::Binding,
     .validate = &inp::ValidatePadBindingValue},
    {.section = "input", .name = "pad_white", .type = T::String, .defaultValue = "pad:lb",
     .label = {"Weiß – Waffe zurück (Fry)", "White – Previous weapon (Fry)"},
     .help = kPadBindingHelp, .tab = Tab::Controls, .order = 108, .widget = W::Binding,
     .validate = &inp::ValidatePadBindingValue},
    {.section = "input", .name = "pad_black", .type = T::String, .defaultValue = "pad:rb",
     .label = {"Schwarz – Waffe vor (Fry)", "Black – Next weapon (Fry)"},
     .help = kPadBindingHelp, .tab = Tab::Controls, .order = 110, .widget = W::Binding,
     .validate = &inp::ValidatePadBindingValue},
    {.section = "input", .name = "pad_lt", .type = T::String, .defaultValue = "pad:lt",
     .label = {"Linker Trigger – Kamera / Zielwechsel", "Left trigger – Camera / switch target"},
     .help = kPadBindingHelp, .tab = Tab::Controls, .order = 112, .widget = W::Binding,
     .validate = &inp::ValidatePadBindingValue},
    {.section = "input", .name = "pad_rt", .type = T::String, .defaultValue = "pad:rt",
     .label = {"Rechter Trigger – Ziel erfassen (halten)", "Right trigger – Lock on target (hold)"},
     .help = kPadBindingHelp, .tab = Tab::Controls, .order = 114, .widget = W::Binding,
     .validate = &inp::ValidatePadBindingValue},
    {.section = "input", .name = "pad_start", .type = T::String, .defaultValue = "pad:start",
     .label = {"START – Pause / Bestätigen", "START – Pause / Confirm"},
     .help = kPadBindingHelp, .tab = Tab::Controls, .order = 116, .widget = W::Binding,
     .validate = &inp::ValidatePadBindingValue},
    {.section = "input", .name = "pad_back", .type = T::String, .defaultValue = "pad:back",
     .label = {"BACK – Zurück in Menüs", "BACK – Back in menus"},
     .help = kPadBindingHelp, .tab = Tab::Controls, .order = 118, .widget = W::Binding,
     .validate = &inp::ValidatePadBindingValue},
    {.section = "input", .name = "pad_dpad_up", .type = T::String, .defaultValue = "pad:dpad_up",
     .label = {"Steuerkreuz hoch – Menüauswahl / Laufen", "D-pad up – Menu selection / Walk"},
     .help = kPadBindingHelp, .tab = Tab::Controls, .order = 120, .widget = W::Binding,
     .validate = &inp::ValidatePadBindingValue},
    {.section = "input", .name = "pad_dpad_down", .type = T::String, .defaultValue = "pad:dpad_down",
     .label = {"Steuerkreuz runter – Menüauswahl / Laufen", "D-pad down – Menu selection / Walk"},
     .help = kPadBindingHelp, .tab = Tab::Controls, .order = 122, .widget = W::Binding,
     .validate = &inp::ValidatePadBindingValue},
    {.section = "input", .name = "pad_dpad_left", .type = T::String, .defaultValue = "pad:dpad_left",
     .label = {"Steuerkreuz links – Menüauswahl / Laufen", "D-pad left – Menu selection / Walk"},
     .help = kPadBindingHelp, .tab = Tab::Controls, .order = 124, .widget = W::Binding,
     .validate = &inp::ValidatePadBindingValue},
    {.section = "input", .name = "pad_dpad_right", .type = T::String, .defaultValue = "pad:dpad_right",
     .label = {"Steuerkreuz rechts – Menüauswahl / Laufen", "D-pad right – Menu selection / Walk"},
     .help = kPadBindingHelp, .tab = Tab::Controls, .order = 126, .widget = W::Binding,
     .validate = &inp::ValidatePadBindingValue},
    {.section = "input", .name = "pad_ls_click", .type = T::String, .defaultValue = "pad:ls_click",
     .label = {"Linker Stick drücken – ohne Funktion", "Left stick click – No function"},
     .help = kPadBindingHelp, .tab = Tab::Controls, .order = 128, .widget = W::Binding,
     .validate = &inp::ValidatePadBindingValue},
    {.section = "input", .name = "pad_rs_click", .type = T::String, .defaultValue = "pad:rs_click",
     .label = {"Rechter Stick drücken – Ego-Ansicht", "Right stick click – First-person view"},
     .help = kPadBindingHelp, .tab = Tab::Controls, .order = 130, .widget = W::Binding,
     .validate = &inp::ValidatePadBindingValue},
    {.section = "input", .name = "pad_ls_up", .type = T::String, .defaultValue = "pad:ly+",
     .label = {"Linker Stick hoch – Vorwärts laufen / Menü", "Left stick up – Walk forward / Menu"},
     .help = kPadBindingHelp, .tab = Tab::Controls, .order = 132, .widget = W::Binding,
     .validate = &inp::ValidatePadBindingValue},
    {.section = "input", .name = "pad_ls_down", .type = T::String, .defaultValue = "pad:ly-",
     .label = {"Linker Stick runter – Rückwärts laufen / Menü", "Left stick down – Walk backward / Menu"},
     .help = kPadBindingHelp, .tab = Tab::Controls, .order = 134, .widget = W::Binding,
     .validate = &inp::ValidatePadBindingValue},
    {.section = "input", .name = "pad_ls_left", .type = T::String, .defaultValue = "pad:lx-",
     .label = {"Linker Stick links – Nach links / Menü", "Left stick left – Move left / Menu"},
     .help = kPadBindingHelp, .tab = Tab::Controls, .order = 136, .widget = W::Binding,
     .validate = &inp::ValidatePadBindingValue},
    {.section = "input", .name = "pad_ls_right", .type = T::String, .defaultValue = "pad:lx+",
     .label = {"Linker Stick rechts – Nach rechts / Menü", "Left stick right – Move right / Menu"},
     .help = kPadBindingHelp, .tab = Tab::Controls, .order = 138, .widget = W::Binding,
     .validate = &inp::ValidatePadBindingValue},
    {.section = "input", .name = "pad_rs_up", .type = T::String, .defaultValue = "pad:ry+",
     .label = {"Rechter Stick hoch – Kamera nach oben", "Right stick up – Camera up"},
     .help = kPadBindingHelp, .tab = Tab::Controls, .order = 140, .widget = W::Binding,
     .validate = &inp::ValidatePadBindingValue},
    {.section = "input", .name = "pad_rs_down", .type = T::String, .defaultValue = "pad:ry-",
     .label = {"Rechter Stick runter – Kamera nach unten", "Right stick down – Camera down"},
     .help = kPadBindingHelp, .tab = Tab::Controls, .order = 142, .widget = W::Binding,
     .validate = &inp::ValidatePadBindingValue},
    {.section = "input", .name = "pad_rs_left", .type = T::String, .defaultValue = "pad:rx-",
     .label = {"Rechter Stick links – Kamera nach links", "Right stick left – Camera left"},
     .help = kPadBindingHelp, .tab = Tab::Controls, .order = 144, .widget = W::Binding,
     .validate = &inp::ValidatePadBindingValue},
    {.section = "input", .name = "pad_rs_right", .type = T::String, .defaultValue = "pad:rx+",
     .label = {"Rechter Stick rechts – Kamera nach rechts", "Right stick right – Camera right"},
     .help = kPadBindingHelp, .tab = Tab::Controls, .order = 146, .widget = W::Binding,
     .validate = &inp::ValidatePadBindingValue},
    {.section = "input", .name = "kbm_a", .type = T::String, .defaultValue = "key:enter, mouse:left",
     .label = {"A – Bestätigen / Angriff 1", "A – Confirm / Attack 1"},
     .help = kKbmBindingHelp, .tab = Tab::Controls, .order = 101, .widget = W::Binding,
     .validate = &inp::ValidateKbmBindingValue},
    {.section = "input", .name = "kbm_b", .type = T::String, .defaultValue = "key:space",
     .label = {"B – Zurück / Springen", "B – Back / Jump"},
     .help = kKbmBindingHelp, .tab = Tab::Controls, .order = 103, .widget = W::Binding,
     .validate = &inp::ValidateKbmBindingValue},
    {.section = "input", .name = "kbm_x", .type = T::String, .defaultValue = "key:f, mouse:right",
     .label = {"X – Angriff 2", "X – Attack 2"},
     .help = kKbmBindingHelp, .tab = Tab::Controls, .order = 105, .widget = W::Binding,
     .validate = &inp::ValidateKbmBindingValue},
    {.section = "input", .name = "kbm_y", .type = T::String, .defaultValue = "key:e",
     .label = {"Y – Benutzen", "Y – Use"},
     .help = kKbmBindingHelp, .tab = Tab::Controls, .order = 107, .widget = W::Binding,
     .validate = &inp::ValidateKbmBindingValue},
    {.section = "input", .name = "kbm_white", .type = T::String, .defaultValue = "key:1, mouse:wheel_down",
     .label = {"Weiß – Waffe zurück (Fry)", "White – Previous weapon (Fry)"},
     .help = kKbmBindingHelp, .tab = Tab::Controls, .order = 109, .widget = W::Binding,
     .validate = &inp::ValidateKbmBindingValue},
    {.section = "input", .name = "kbm_black", .type = T::String, .defaultValue = "key:2, mouse:wheel_up",
     .label = {"Schwarz – Waffe vor (Fry)", "Black – Next weapon (Fry)"},
     .help = kKbmBindingHelp, .tab = Tab::Controls, .order = 111, .widget = W::Binding,
     .validate = &inp::ValidateKbmBindingValue},
    {.section = "input", .name = "kbm_lt", .type = T::String, .defaultValue = "key:q",
     .label = {"Linker Trigger – Kamera / Zielwechsel", "Left trigger – Camera / switch target"},
     .help = kKbmBindingHelp, .tab = Tab::Controls, .order = 113, .widget = W::Binding,
     .validate = &inp::ValidateKbmBindingValue},
    {.section = "input", .name = "kbm_rt", .type = T::String, .defaultValue = "key:lshift",
     .label = {"Rechter Trigger – Ziel erfassen (halten)", "Right trigger – Lock on target (hold)"},
     .help = kKbmBindingHelp, .tab = Tab::Controls, .order = 115, .widget = W::Binding,
     .validate = &inp::ValidateKbmBindingValue},
    {.section = "input", .name = "kbm_start", .type = T::String, .defaultValue = "key:escape",
     .label = {"START – Pause / Bestätigen", "START – Pause / Confirm"},
     .help = kKbmBindingHelp, .tab = Tab::Controls, .order = 117, .widget = W::Binding,
     .validate = &inp::ValidateKbmBindingValue},
    {.section = "input", .name = "kbm_back", .type = T::String, .defaultValue = "key:backspace",
     .label = {"BACK – Zurück in Menüs", "BACK – Back in menus"},
     .help = kKbmBindingHelp, .tab = Tab::Controls, .order = 119, .widget = W::Binding,
     .validate = &inp::ValidateKbmBindingValue},
    {.section = "input", .name = "kbm_dpad_up", .type = T::String, .defaultValue = "key:up",
     .label = {"Steuerkreuz hoch – Menüauswahl / Laufen", "D-pad up – Menu selection / Walk"},
     .help = kKbmBindingHelp, .tab = Tab::Controls, .order = 121, .widget = W::Binding,
     .validate = &inp::ValidateKbmBindingValue},
    {.section = "input", .name = "kbm_dpad_down", .type = T::String, .defaultValue = "key:down",
     .label = {"Steuerkreuz runter – Menüauswahl / Laufen", "D-pad down – Menu selection / Walk"},
     .help = kKbmBindingHelp, .tab = Tab::Controls, .order = 123, .widget = W::Binding,
     .validate = &inp::ValidateKbmBindingValue},
    {.section = "input", .name = "kbm_dpad_left", .type = T::String, .defaultValue = "key:left",
     .label = {"Steuerkreuz links – Menüauswahl / Laufen", "D-pad left – Menu selection / Walk"},
     .help = kKbmBindingHelp, .tab = Tab::Controls, .order = 125, .widget = W::Binding,
     .validate = &inp::ValidateKbmBindingValue},
    {.section = "input", .name = "kbm_dpad_right", .type = T::String, .defaultValue = "key:right",
     .label = {"Steuerkreuz rechts – Menüauswahl / Laufen", "D-pad right – Menu selection / Walk"},
     .help = kKbmBindingHelp, .tab = Tab::Controls, .order = 127, .widget = W::Binding,
     .validate = &inp::ValidateKbmBindingValue},
    {.section = "input", .name = "kbm_ls_click", .type = T::String, .defaultValue = "key:c",
     .label = {"Linker Stick drücken – ohne Funktion", "Left stick click – No function"},
     .help = kKbmBindingHelp, .tab = Tab::Controls, .order = 129, .widget = W::Binding,
     .validate = &inp::ValidateKbmBindingValue},
    {.section = "input", .name = "kbm_rs_click", .type = T::String, .defaultValue = "key:v, mouse:middle",
     .label = {"Rechter Stick drücken – Ego-Ansicht", "Right stick click – First-person view"},
     .help = kKbmBindingHelp, .tab = Tab::Controls, .order = 131, .widget = W::Binding,
     .validate = &inp::ValidateKbmBindingValue},
    {.section = "input", .name = "kbm_ls_up", .type = T::String, .defaultValue = "key:w",
     .label = {"Linker Stick hoch – Vorwärts laufen / Menü", "Left stick up – Walk forward / Menu"},
     .help = kKbmBindingHelp, .tab = Tab::Controls, .order = 133, .widget = W::Binding,
     .validate = &inp::ValidateKbmBindingValue},
    {.section = "input", .name = "kbm_ls_down", .type = T::String, .defaultValue = "key:s",
     .label = {"Linker Stick runter – Rückwärts laufen / Menü", "Left stick down – Walk backward / Menu"},
     .help = kKbmBindingHelp, .tab = Tab::Controls, .order = 135, .widget = W::Binding,
     .validate = &inp::ValidateKbmBindingValue},
    {.section = "input", .name = "kbm_ls_left", .type = T::String, .defaultValue = "key:a",
     .label = {"Linker Stick links – Nach links / Menü", "Left stick left – Move left / Menu"},
     .help = kKbmBindingHelp, .tab = Tab::Controls, .order = 137, .widget = W::Binding,
     .validate = &inp::ValidateKbmBindingValue},
    {.section = "input", .name = "kbm_ls_right", .type = T::String, .defaultValue = "key:d",
     .label = {"Linker Stick rechts – Nach rechts / Menü", "Left stick right – Move right / Menu"},
     .help = kKbmBindingHelp, .tab = Tab::Controls, .order = 139, .widget = W::Binding,
     .validate = &inp::ValidateKbmBindingValue},
    {.section = "input", .name = "kbm_rs_up", .type = T::String, .defaultValue = "mouse:move_up",
     .label = {"Rechter Stick hoch – Kamera nach oben", "Right stick up – Camera up"},
     .help = kKbmBindingHelp, .tab = Tab::Controls, .order = 141, .widget = W::Binding,
     .validate = &inp::ValidateKbmBindingValue},
    {.section = "input", .name = "kbm_rs_down", .type = T::String, .defaultValue = "mouse:move_down",
     .label = {"Rechter Stick runter – Kamera nach unten", "Right stick down – Camera down"},
     .help = kKbmBindingHelp, .tab = Tab::Controls, .order = 143, .widget = W::Binding,
     .validate = &inp::ValidateKbmBindingValue},
    {.section = "input", .name = "kbm_rs_left", .type = T::String, .defaultValue = "mouse:move_left",
     .label = {"Rechter Stick links – Kamera nach links", "Right stick left – Camera left"},
     .help = kKbmBindingHelp, .tab = Tab::Controls, .order = 145, .widget = W::Binding,
     .validate = &inp::ValidateKbmBindingValue},
    {.section = "input", .name = "kbm_rs_right", .type = T::String, .defaultValue = "mouse:move_right",
     .label = {"Rechter Stick rechts – Kamera nach rechts", "Right stick right – Camera right"},
     .help = kKbmBindingHelp, .tab = Tab::Controls, .order = 147, .widget = W::Binding,
     .validate = &inp::ValidateKbmBindingValue},

    // [paths]
    {.section = "paths", .name = "iso", .type = T::PathFile, .label = {"Spielabbild (ISO)", "Game image (ISO)"},
     .help = {"Abbild (ISO) Ihrer eigenen Futurama-Disc für die Xbox, europäische Verkaufsversion. Erkannt werden "
              "XISO- und vollständige Redump-Abbilder.",
              "Image (ISO) of your own Futurama disc for the Xbox, European retail release. XISO and full redump "
              "images are recognised."},
     .tab = Tab::Paths, .order = 10, .widget = W::File, .fileFilter = "*.iso *.xiso *.bin"},
    {.section = "paths", .name = "userdata", .type = T::PathDir,
     .label = {"Benutzerdaten-Ordner", "User data folder"},
     .help = {"Hier liegen Spielstände, Protokolle, Screenshots und der Zwischenspeicher. Leer = Standardordner "
              "(%LOCALAPPDATA%\\FuturamaPC, im portablen Modus der Ordner von futurama.exe).",
              "Holds save games, logs, screenshots and the cache. Empty = default folder "
              "(%LOCALAPPDATA%\\FuturamaPC, in portable mode the folder of futurama.exe)."},
     .tab = Tab::Paths, .order = 20, .widget = W::Dir, .flags = kDyn},
    {.section = "paths", .name = "xbe", .type = T::PathFile,
     .label = {"Entpackte default.xbe", "Extracted default.xbe"},
     .help = {"Für Entwickler: lädt das Spielprogramm aus einer entpackten Datei statt aus dem Spielabbild. Es muss "
              "die europäische Verkaufsversion sein. Leer = aus dem Abbild.",
              "For developers: loads the game program from an extracted file instead of the disc image. It must be "
              "the European retail version. Empty = from the image."},
     .tab = Tab::Paths, .order = 80, .widget = W::File, .flags = kDev, .fileFilter = "*.xbe"},
    {.section = "paths", .name = "disc_dir", .type = T::PathDir,
     .label = {"Entpackter Disc-Ordner", "Extracted disc folder"},
     .help = {"Für Entwickler: Laufwerk D: aus einem Ordner mit entpackten Disc-Dateien, wenn kein Abbild "
              "eingestellt ist.",
              "For developers: drive D: from a folder with extracted disc files when no image is set."},
     .tab = Tab::Paths, .order = 90, .widget = W::Dir, .flags = kDev},

    // [settings] (kernel/kernel_settings.h: Xbox EEPROM / system settings)
    {.section = "settings", .name = "language", .type = T::Choice, .defaultValue = "english",
     .choices = kLanguageChoices, .label = {"Spielsprache", "Game language"},
     .help = {"Sprache des Spiels. Sie wird dem Spiel als Systemsprache der Xbox gemeldet.",
              "Language of the game. It is reported to the game as the Xbox system language."},
     .tab = Tab::Game, .order = 10, .widget = W::Combo},
    {.section = "settings", .name = "game_region", .type = T::Int, .defaultValue = "4",
     .label = {"Spielregion", "Game region"},
     .help = {"Für Entwickler: gemeldete Xbox-Spielregion (1 Nordamerika, 2 Japan, 4 Europa und Rest der Welt).",
              "For developers: reported Xbox game region (1 North America, 2 Japan, 4 Europe and rest of world)."},
     .tab = Tab::Advanced, .order = 600, .widget = W::Spin, .flags = kDev},
    {.section = "settings", .name = "av_region", .type = T::Int, .defaultValue = "0x00800300",
     .label = {"Videoregion", "Video region"},
     .help = {"Für Entwickler: gemeldete Xbox-Videoregion (0x00800300 = PAL-I). Das Spiel fragt sie nicht ab.",
              "For developers: reported Xbox video region (0x00800300 = PAL-I). The game does not query it."},
     .tab = Tab::Advanced, .order = 610, .widget = W::Spin, .flags = kDev},
    {.section = "settings", .name = "video_flags", .type = T::Int, .defaultValue = "0",
     .label = {"Video-Flags", "Video flags"},
     .help = {"Für Entwickler: gemeldete Xbox-Videoeinstellungen (Breitbild, 480p, ...). Das Spiel fragt sie "
              "nicht ab.",
              "For developers: reported Xbox video settings (widescreen, 480p, ...). The game does not query them."},
     .tab = Tab::Advanced, .order = 620, .widget = W::Spin, .flags = kDev},
    {.section = "settings", .name = "audio_flags", .type = T::Int, .defaultValue = "0",
     .label = {"Audio-Flags", "Audio flags"},
     .help = {"Für Entwickler: gemeldete Xbox-Audioeinstellungen (0 = Stereo).",
              "For developers: reported Xbox audio settings (0 = stereo)."},
     .tab = Tab::Advanced, .order = 630, .widget = W::Spin, .flags = kDev},
    {.section = "settings", .name = "av_pack", .type = T::Int, .defaultValue = "6",
     .label = {"AV-Kabel", "AV pack"},
     .help = {"Für Entwickler: gemeldeter Xbox-AV-Kabeltyp (6 = Standard-AV-Kabel).",
              "For developers: reported Xbox AV pack type (6 = standard AV cable)."},
     .tab = Tab::Advanced, .order = 640, .widget = W::Spin, .flags = kDev},

    // [saves] (design 4.5, kernel/kernel_settings.h)
    {.section = "saves", .name = "key_mode", .type = T::Choice, .defaultValue = "fixed", .choices = kKeyModeChoices,
     .label = {"Signaturschlüssel der Spielstände", "Save game signature key"},
     .help = {"Für Entwickler: womit Spielstände signiert werden. „Echte Xbox“ braucht den XboxCERTKey einer "
              "eigenen Konsole.",
              "For developers: what save games are signed with. \"Real Xbox\" needs the XboxCERTKey of your own "
              "console."},
     .tab = Tab::Advanced, .order = 700, .widget = W::Combo, .flags = kDev},
    {.section = "saves", .name = "signature_key", .type = T::String,
     .label = {"Eigener Signaturschlüssel", "Custom signature key"},
     .help = {"Für Entwickler: 32 Hex-Ziffern; ersetzt bei „Fester Projektschlüssel“ den eingebauten Schlüssel.",
              "For developers: 32 hex digits; replaces the built-in key with \"Fixed project key\"."},
     .tab = Tab::Advanced, .order = 710, .widget = W::Text, .flags = kDev},
    {.section = "saves", .name = "xbox_cert_key", .type = T::String, .label = {"XboxCERTKey", "XboxCERTKey"},
     .help = {"Für Entwickler: 32 Hex-Ziffern der eigenen Konsole; nötig für „Schlüssel einer echten Xbox“.",
              "For developers: 32 hex digits of your own console; needed for \"Key of a real Xbox\"."},
     .tab = Tab::Advanced, .order = 720, .widget = W::Text, .flags = kDev},

    // [audio] engine (M5 DSOUND core agent, audio/audio_engine.cpp EngineConfigFromSettings; src/INTERFACES.md 4.29)
    {.section = "audio", .name = "enabled", .type = T::Bool, .defaultValue = "1",
     .label = {"Tonausgabe", "Sound output"},
     .help = {"Aus = das Spiel läuft ohne Ton (die Tonmischung läuft trotzdem, Musik und Geräusche verhalten sich "
              "gleich). Ohne Ausgabegerät (z. B. Fernseher aus) läuft das Spiel ebenfalls stumm weiter und schaltet "
              "den Ton ein, sobald ein Gerät da ist.",
              "Off = the game runs without sound (the sound mix still runs, music and sounds behave the same). "
              "Without an output device (e.g. the TV is off) the game also continues silently and switches the "
              "sound on as soon as a device is available."},
     .tab = Tab::Audio, .order = 10, .widget = W::Check},
    {.section = "audio", .name = "device", .type = T::String,
     .label = {"Ausgabegerät", "Output device"},
     .help = {"Windows-Gerät für den Ton. Leer = das Standardgerät von Windows (wechselt Windows das "
              "Standardgerät, wechselt das Spiel mit). Ist das gewählte Gerät nicht verfügbar, nimmt das Spiel das "
              "Standardgerät und wechselt zurück, sobald das gewählte Gerät wieder da ist.",
              "Windows device for the sound. Empty = the Windows default device (when Windows changes the default "
              "device, the game follows). If the chosen device is not available, the game uses the default device "
              "and switches back as soon as the chosen device is there again."},
     .tab = Tab::Audio, .order = 20, .widget = W::AudioDevice},
    {.section = "audio", .name = "master_volume", .type = T::Int, .defaultValue = "100", .ranged = true, .min = 0,
     .max = 100, .step = 5, .label = {"Gesamtlautstärke", "Master volume"},
     .help = {"Lautstärke des ganzen Spiels. Musik, Geräusche und Sprache stellt man wie auf der Xbox im "
              "Optionsmenü des Spiels ein.",
              "Volume of the whole game. Music, sounds and speech are set in the game's options menu, as on the "
              "Xbox."},
     .tab = Tab::Audio, .order = 30, .widget = W::Slider, .unit = "%"},
    {.section = "audio", .name = "limiter", .type = T::Bool, .defaultValue = "1",
     .label = {"Übersteuerungsschutz", "Clipping protection"},
     .help = {"Begrenzt Spitzen der Gesamtmischung, wenn viele laute Geräusche zusammenkommen (das Material des "
              "Spiels ist sehr laut abgemischt). Aus = keine Begrenzung, kann knacken.",
              "Limits peaks of the final mix when many loud sounds come together (the game's material is mastered "
              "very loud). Off = no limiting, may crackle."},
     .tab = Tab::Audio, .order = 60, .widget = W::Check, .flags = kAdv},

    // [audio] (M2, audio/dsound_stubs.h; without effect since M5 - kept so that older INI files stay valid)
    {.section = "audio", .name = "dummy_streams", .type = T::Bool, .defaultValue = "0",
     .label = {"Stumme Audio-Streams (veraltet)", "Silent audio streams (obsolete)"},
     .help = {"Für Entwickler: ohne Wirkung seit der Tonausgabe (M5); wird nur noch akzeptiert.",
              "For developers: without effect since sound output exists (M5); only still accepted."},
     .tab = Tab::Advanced, .order = 800, .widget = W::Check, .flags = kDev},
    // [audio] capture (M5 tooling agent, audio/capture_session.h; src/INTERFACES.md 4.29)
    {.section = "audio", .name = "capture_wav", .type = T::PathFile,
     .label = {"Tonmitschnitt (WAV-Datei)", "Audio capture (WAV file)"},
     .help = {"Für Entwickler und automatische Tests: schreibt die fertige Tonmischung zusätzlich unverändert in "
              "diese WAV-Datei (Abtastrate und Kanäle der Ausgabe) und protokolliert beim Beenden die Pegel je "
              "Kategorie. Ein relativer Pfad gilt ab dem Ordner der INI-Datei. Leer = aus.",
              "For developers and automated tests: also writes the final sound mix unchanged into this WAV file "
              "(sample rate and channels of the output) and logs the levels per category at exit. A relative path "
              "starts at the folder of the INI file. Empty = off."},
     .tab = Tab::Advanced, .order = 810, .widget = W::File, .flags = kDev, .fileFilter = "*.wav"},
    {.section = "audio", .name = "capture_format", .type = T::Choice, .defaultValue = "float",
     .choices = kCaptureFormatChoices, .label = {"Format des Tonmitschnitts", "Audio capture format"},
     .help = {"Für Entwickler: Sampleformat der Mitschnitt-Dateien. Gleitkomma ist eine exakte Kopie der Mischung, "
              "16 Bit ist halb so groß und begrenzt Spitzen über 0 dBFS.",
              "For developers: sample format of the capture files. Floating point is an exact copy of the mix, "
              "16 bit is half the size and clips peaks above 0 dBFS."},
     .tab = Tab::Advanced, .order = 820, .widget = W::Combo, .flags = kDev},
    {.section = "audio", .name = "capture_stems", .type = T::Bool, .defaultValue = "0",
     .label = {"Teilmischungen mitschneiden", "Capture the submixes"},
     .help = {"Für Entwickler: schreibt neben dem Tonmitschnitt je Teilmischung (Musik, Sprache, Effekte, Film, "
              "Hall) eine eigene WAV-Datei, z. B. mitschnitt.music.wav. Nur mit Tonmitschnitt wirksam.",
              "For developers: next to the audio capture, writes one WAV file per submix (music, speech, effects, "
              "movie, reverb), e.g. capture.music.wav. Only effective with an audio capture."},
     .tab = Tab::Advanced, .order = 830, .widget = W::Check, .flags = kDev},
    {.section = "audio", .name = "event_log", .type = T::Bool, .defaultValue = "0",
     .label = {"Ton-Ereignisliste", "Sound event list"},
     .help = {"Für Entwickler: schreibt neben dem Tonmitschnitt eine Ereignisliste (.events.tsv) mit Zeitpunkt in "
              "Samples, Logiktick, Tonobjekt, Name und Kategorie jedes DSOUND-Ereignisses. Nur mit Tonmitschnitt "
              "wirksam.",
              "For developers: next to the audio capture, writes an event list (.events.tsv) with the time in "
              "samples, logic tick, sound object, name and category of every DSOUND event. Only effective with an "
              "audio capture."},
     .tab = Tab::Advanced, .order = 840, .widget = W::Check, .flags = kDev},
    // [audio] device test (M5 integration, audio/audio_engine.cpp)
    {.section = "audio", .name = "device_test", .type = T::Choice, .defaultValue = "off",
     .choices = kDeviceTestChoices, .label = {"Test ohne Ausgabegerät", "Test without output device"},
     .help = {"Für Entwickler und automatische Tests: „Kein Ausgabegerät“ verhält sich, als hätte Windows kein "
              "aktives Ausgabegerät (das Spiel mischt stumm weiter); „Gerät alle 20 s verlieren“ meldet 20 s nach "
              "jedem Gerätestart einen Geräteverlust, das Spiel wechselt auf die stumme Mischung und nach etwa 3 s "
              "zurück.",
              "For developers and automated tests: \"No output device\" behaves as if Windows had no active output "
              "device (the game keeps mixing silently); \"Lose the device every 20 s\" reports a device loss 20 s "
              "after every device start, the game switches to the silent mix and back after about 3 s."},
     .tab = Tab::Advanced, .order = 845, .widget = W::Combo, .flags = kDev},
    // [audio] output layout, reverb, 3D (M5 codec/3D/reverb agent; read by audio/audio_engine.cpp (speakers, reverb)
    // and audio/spatial_x3d.cpp (positional, trim_3d); src/INTERFACES.md 4.29)
    {.section = "audio", .name = "speakers", .type = T::Choice, .defaultValue = "auto", .choices = kSpeakerChoices,
     .label = {"Lautsprecher", "Speakers"},
     .help = {"Kanäle der Tonausgabe. „Automatisch“ übernimmt die Lautsprecher-Einstellung von Windows für das "
              "Ausgabegerät. Bei 5.1 kommen Geräusche hinter der Spielfigur aus den hinteren Lautsprechern.",
              "Channels of the sound output. \"Automatic\" follows the Windows speaker setup of the output device. "
              "With 5.1, sounds behind the character come from the rear speakers."},
     .tab = Tab::Audio, .order = 25, .widget = W::Combo},
    {.section = "audio", .name = "reverb", .type = T::Bool, .defaultValue = "1",
     .label = {"Raumhall", "Room reverb"},
     .help = {"Der dezente Raumhall des Spiels auf Geräuschen im Raum (wie auf der Xbox). Aus = trockener Klang.",
              "The game's subtle room reverb on sounds in the scene (as on the Xbox). Off = dry sound."},
     .tab = Tab::Audio, .order = 55, .widget = W::Check},
    {.section = "audio", .name = "positional", .type = T::Choice, .defaultValue = "corrected",
     .choices = kPositionalChoices, .label = {"Räumlicher Klang", "Positional sound"},
     .help = {"Richtung von Geräuschen im Raum. Das Spiel übergibt der Xbox die Blickrichtung verkehrt herum, "
              "deshalb klingen dort Geräusche vor der Kamera wie von hinten (links und rechts stimmen). „Richtig "
              "herum“ korrigiert das, „Wie auf der Xbox“ behält es bei, „Aus“ spielt alle Geräusche von vorne "
              "(Lautstärke nach Entfernung bleibt).",
              "Direction of sounds in the scene. The game hands the Xbox the view direction reversed, so there "
              "sounds in front of the camera seem to come from behind (left and right are right). \"Correct "
              "orientation\" fixes that, \"As on the Xbox\" keeps it, \"Off\" plays every sound from the front "
              "(the volume by distance stays)."},
     .tab = Tab::Audio, .order = 65, .widget = W::Combo},
    {.section = "audio", .name = "trim_3d", .type = T::Float, .defaultValue = "0", .ranged = true, .min = -20.0,
     .max = 20.0, .step = 0.5, .label = {"Pegel räumlicher Geräusche (dB)", "Level of positional sounds (dB)"},
     .help = {"Für Entwickler: hebt oder senkt alle räumlichen Geräusche (und ihren Hall) gegenüber Musik, Sprache "
              "und Menüklängen, zum Abgleich mit der Xbox. 0 = Pegel wie von DSOUND berechnet.",
              "For developers: raises or lowers all positional sounds (and their reverb) against music, speech and "
              "menu sounds, to match the Xbox. 0 = levels as computed by DSOUND."},
     .tab = Tab::Advanced, .order = 855, .widget = W::DSpin, .flags = kDev, .unit = "dB"},

    // [freecam] (M4b, gfx/freecam.cpp; docs/ROADMAP.md "M4b")
    {.section = "freecam", .name = "enabled", .type = T::Bool, .defaultValue = "1",
     .label = {"Freie Kamera verfügbar", "Free camera available"},
     .help = {"Eine frei bewegliche Kamera für Screenshots und zum Umsehen, im Hauptmenü, in Levels und "
              "Zwischensequenzen. Während sie aktiv ist, erhält das Spiel keine Eingaben. Gamepad: linker Stick "
              "bewegen, rechter Stick umsehen, RT/LT hoch/runter, A schneller, X langsamer, Steuerkreuz hoch/runter "
              "zoomen, links/rechts Tempo, Y HUD aus/ein, B Wände von außen, BACK zurück zur Spielkamera. Tastatur/Maus: WASD bewegen, "
              "E/Q hoch/runter, Maus umsehen, Umschalt schneller, Strg langsamer, Mausrad zoomen, +/- Tempo, H HUD "
              "aus/ein, B Wände von außen, R zurück zur Spielkamera.",
              "A freely movable camera for screenshots and looking around, in the main menu, in levels and cut "
              "scenes. While it is active the game receives no input. Gamepad: left stick move, right stick look, "
              "RT/LT up/down, A faster, X slower, D-pad up/down zoom, left/right speed, Y HUD off/on, B walls from outside, BACK back to the "
              "game camera. Keyboard/mouse: WASD move, E/Q up/down, mouse look, Shift faster, Ctrl slower, mouse "
              "wheel zoom, +/- speed, H HUD off/on, B walls from outside, R back to the game camera."},
     .tab = Tab::Controls, .order = 300, .widget = W::Check},
    {.section = "freecam", .name = "toggle_pad", .type = T::Choice, .defaultValue = "l3r3",
     .choices = kFreecamPadChoices, .label = {"Ein/aus am Gamepad", "On/off on the gamepad"},
     .help = {"Schaltet die freie Kamera ein und aus. Ein einzelner Stick-Klick erreicht das Spiel weiterhin (bis "
              "zu 4 Ticks verzögert, damit die Kombination nicht als Ego-Ansicht ankommt).",
              "Turns the free camera on and off. A single stick click still reaches the game (delayed by up to 4 "
              "ticks so that the chord does not arrive as the first-person view)."},
     .tab = Tab::Controls, .order = 310, .widget = W::Combo},
    {.section = "freecam", .name = "toggle_key", .type = T::Choice, .defaultValue = "f6",
     .choices = kFreecamKeyChoices, .label = {"Ein/aus per Taste", "On/off key"},
     .help = {"Taste, die die freie Kamera ein- und ausschaltet.", "Key that turns the free camera on and off."},
     .tab = Tab::Controls, .order = 320, .widget = W::Combo},
    {.section = "freecam", .name = "move_speed", .type = T::Float, .defaultValue = "40", .ranged = true,
     .min = 5.0, .max = 1000.0, .step = 5.0, .label = {"Tempo", "Speed"},
     .help = {"Bewegungstempo der freien Kamera in Spieleinheiten pro Sekunde (Stick ganz ausgelenkt, im Level; im "
              "kleineren Hauptmenü entsprechend langsamer). Während des Flugs mit Steuerkreuz links/rechts bzw. "
              "+/- verstellbar.",
              "Movement speed of the free camera in game units per second (stick fully deflected, in a level; "
              "slower in the smaller main menu). Adjustable in flight with D-pad left/right or +/-."},
     .tab = Tab::Controls, .order = 330, .widget = W::DSpin},
    {.section = "freecam", .name = "look_speed", .type = T::Float, .defaultValue = "120", .ranged = true,
     .min = 10.0, .max = 720.0, .step = 10.0, .label = {"Drehtempo", "Turn speed"},
     .help = {"Drehtempo mit dem rechten Stick in Grad pro Sekunde.", "Turn speed with the right stick in degrees "
              "per second."},
     .tab = Tab::Controls, .order = 340, .widget = W::DSpin, .unit = "°/s"},
    {.section = "freecam", .name = "mouse_sensitivity", .type = T::Float, .defaultValue = "1", .ranged = true,
     .min = 0.1, .max = 10.0, .step = 0.1, .label = {"Mausempfindlichkeit", "Mouse sensitivity"},
     .help = {"Faktor für das Umsehen mit der Maus (1 = 0,08 Grad je Mauszählschritt).",
              "Factor for looking around with the mouse (1 = 0.08 degrees per mouse count)."},
     .tab = Tab::Controls, .order = 350, .widget = W::DSpin, .unit = "×"},
    {.section = "freecam", .name = "back_faces", .type = T::Bool, .defaultValue = "1",
     .label = {"Wände von außen zeigen", "Show walls from outside"},
     .help = {"Die Levels sind nur von innen gebaut: Von außen gesehen fehlen Wände, dort erscheint schwarze Leere. "
              "An = auch die Rückseiten zeichnen, so sind Gebäude von außen sichtbar (mit ihrer Innentextur). Im "
              "Flug mit B umschaltbar. Wirkt nur bei aktiver freier Kamera.",
              "The levels are only built from inside: seen from outside, walls are missing and black void shows. "
              "On = also draw the back faces, so buildings are visible from outside (with their inside texture). "
              "Toggle in flight with B. Only affects the active free camera."},
     .tab = Tab::Controls, .order = 355, .widget = W::Check},
    {.section = "freecam", .name = "invert_y", .type = T::Bool, .defaultValue = "0",
     .label = {"Vertikal invertiert", "Invert vertical"},
     .help = {"Kehrt das Auf- und Abschauen um (Stick und Maus).", "Inverts looking up and down (stick and mouse)."},
     .tab = Tab::Controls, .order = 360, .widget = W::Check},

    // [log] (core/log.h)
    {.section = "log", .name = "level", .type = T::Choice, .defaultValue = "info", .choices = kLogLevelChoices,
     .label = {"Protokollstufe", "Log level"},
     .help = {"Wie ausführlich die Datei futurama.log im Protokollordner geschrieben wird.",
              "How much is written to the file futurama.log in the log folder."},
     .tab = Tab::Advanced, .order = 10, .widget = W::Combo, .flags = kAdv},
    {.section = "log", .name = "console", .type = T::Bool, .defaultValue = kDebugBuildDefault,
     .label = {"Protokoll auf der Konsole", "Log to the console"},
     .help = {"Gibt das Protokoll zusätzlich auf der Konsole aus, wenn das Spiel aus einer Konsole gestartet "
              "wurde. Standard: an in Debug-Builds.",
              "Also prints the log to the console when the game was started from one. Default: on in debug "
              "builds."},
     .tab = Tab::Advanced, .order = 20, .widget = W::Check, .flags = kAdv | kDyn},
    {.section = "log", .name = "debugger", .type = T::Bool, .defaultValue = kDebugBuildDefault,
     .label = {"Protokoll an den Debugger", "Log to the debugger"},
     .help = {"Sendet das Protokoll zusätzlich an einen angehängten Debugger. Standard: an in Debug-Builds.",
              "Also sends the log to an attached debugger. Default: on in debug builds."},
     .tab = Tab::Advanced, .order = 30, .widget = W::Check, .flags = kAdv | kDyn},

    // [debug] (core, design 5.4)
    {.section = "debug", .name = "trace", .type = T::String, .label = {"Trace-Kanäle", "Trace channels"},
     .help = {"Kommagetrennte Kanäle, die alles protokollieren: core, krnl, vfs, xapi, gfx, snd, vid, inp, hook, "
              "game oder all. Erzeugt sehr große Protokolle.",
              "Comma-separated channels that log everything: core, krnl, vfs, xapi, gfx, snd, vid, inp, hook, game "
              "or all. Produces very large logs."},
     .tab = Tab::Advanced, .order = 40, .widget = W::Text, .flags = kAdv},
    {.section = "debug", .name = "selftest", .type = T::Bool, .defaultValue = "0",
     .label = {"Selbsttests beim Start", "Self-tests at startup"},
     .help = {"Prüft beim Start die Ladeschritte und schreibt die Ergebnisse ins Protokoll. Der Start dauert etwas "
              "länger.",
              "Checks the loader steps at startup and writes the results to the log. The start takes a little "
              "longer."},
     .tab = Tab::Advanced, .order = 50, .widget = W::Check, .flags = kAdv},
    {.section = "debug", .name = "msgbox", .type = T::Bool, .defaultValue = "0",
     .label = {"Fehler im Meldungsfenster", "Errors in a message box"},
     .help = {"Zeigt schwere Fehler zusätzlich in einem Fenster an. Standard: nur, wenn das Spiel ohne Konsole "
              "läuft.",
              "Also shows fatal errors in a message box. Default: only when the game runs without a console."},
     .tab = Tab::Advanced, .order = 60, .widget = W::Check, .flags = kAdv | kDyn},
    {.section = "debug", .name = "minidump", .type = T::Bool, .defaultValue = "1",
     .label = {"Minidump bei Absturz", "Minidump on crash"},
     .help = {"Schreibt bei einem Absturz eine Minidump-Datei in den Protokollordner. Hilfreich für Fehlerberichte.",
              "Writes a minidump file to the log folder on a crash. Helpful for bug reports."},
     .tab = Tab::Advanced, .order = 70, .widget = W::Check, .flags = kAdv},
    {.section = "debug", .name = "timeout", .type = T::Int, .defaultValue = "0", .ranged = true, .min = 0,
     .max = 86400, .step = 1, .label = {"Zeitlimit (Sekunden)", "Time limit (seconds)"},
     .help = {"Beendet das Spiel nach so vielen Sekunden mit Rückgabecode 5 und schreibt alle Threads ins Protokoll. "
              "0 = aus.",
              "Ends the game after this many seconds with exit code 5 and writes all threads to the log. 0 = off."},
     .tab = Tab::Advanced, .order = 80, .widget = W::Spin, .flags = kAdv | kHint},
    {.section = "debug", .name = "stop_at", .type = T::String, .label = {"Anhalten bei", "Stop at"},
     .help = {"Für Entwickler: beendet das Programm mit Rückgabecode 0, sobald das Spiel diese Funktion erreicht "
              "(main oder eine 0x-Adresse).",
              "For developers: ends the program with exit code 0 as soon as the game reaches this function (main "
              "or a 0x address)."},
     .tab = Tab::Advanced, .order = 300, .widget = W::Text, .flags = kDev},
    {.section = "debug", .name = "frames", .type = T::Int, .defaultValue = "0", .ranged = true, .min = 0,
     .max = 4294967295.0, .step = 1, .label = {"Bildlimit", "Frame limit"},
     .help = {"Für Entwickler: beendet das Spiel nach so vielen angezeigten Bildern mit Rückgabecode 0. 0 = aus.",
              "For developers: ends the game with exit code 0 after this many displayed frames. 0 = off."},
     .tab = Tab::Advanced, .order = 310, .widget = W::Spin, .flags = kDev},
    {.section = "debug", .name = "mmio_trap", .type = T::Bool, .defaultValue = "1",
     .label = {"MMIO-Falle", "MMIO trap"},
     .help = {"Für Entwickler: sperrt den Xbox-Hardwarebereich ab 0xFD000000, damit Zugriffe darauf gemeldet "
              "werden.",
              "For developers: blocks the Xbox hardware range from 0xFD000000 so that accesses to it are reported."},
     .tab = Tab::Advanced, .order = 320, .widget = W::Check, .flags = kDev},
    {.section = "debug", .name = "crash_test", .type = T::Bool, .defaultValue = "0",
     .label = {"Absturztest", "Crash test"},
     .help = {"Für Entwickler: löst vor dem Spielstart absichtlich einen Absturz aus, um die Absturzberichte zu "
              "prüfen.",
              "For developers: deliberately crashes before the game starts to test the crash reports."},
     .tab = Tab::Advanced, .order = 330, .widget = W::Check, .flags = kDev},
    {.section = "debug", .name = "exit_hang_test", .type = T::Bool, .defaultValue = "0",
     .label = {"Test: hängendes Beenden", "Test: hanging exit"},
     .help = {"Für Entwickler: blockiert das Beenden, damit das Zeitlimit geprüft werden kann.",
              "For developers: blocks the exit so that the watchdog can be tested."},
     .tab = Tab::Advanced, .order = 340, .widget = W::Check, .flags = kDev},
    // [debug] input_script (M4 test automation, input/input_script.h)
    {.section = "debug", .name = "input_script", .type = T::PathFile,
     .label = {"Eingabeskript", "Input script"},
     .help = {"Für Entwickler und automatische Tests: spielt die Tasten aus dieser Textdatei als Controller an "
              "Anschluss 1 ab (Format: tools/input_scripts/README.md). Leer = aus.",
              "For developers and automated tests: plays the buttons from this text file as a controller on port 1 "
              "(format: tools/input_scripts/README.md). Empty = off."},
     .tab = Tab::Advanced, .order = 350, .widget = W::File, .flags = kDev, .fileFilter = "*.txt"},
    // [debug] movies (M6, video/xmv_hle.cpp): the M2-M5 stub behaviour for regression checks with frame timelines.
    {.section = "debug", .name = "movies", .type = T::Choice, .defaultValue = "play", .choices = kDebugMoviesChoices,
     .label = {"Filme", "Movies"},
     .help = {"Für Entwickler und automatische Tests: „skip“ beendet jeden Film sofort ohne Bild und Ton (wie vor "
              "M6), damit Prüfläufe mit festen Bildnummern gleich bleiben.",
              "For developers and automated tests: \"skip\" ends every movie at once without picture and sound (as "
              "before M6), so check runs with fixed frame numbers stay the same."},
     .tab = Tab::Advanced, .order = 360, .widget = W::Combo, .flags = kDev},

    // [kernel]
    {.section = "kernel", .name = "strict", .type = T::Bool, .defaultValue = "0",
     .label = {"Strenger Kernel-Modus", "Strict kernel mode"},
     .help = {"Bricht ab, sobald das Spiel eine noch nicht nachgebaute Xbox-Kernelfunktion aufruft. Nur zur "
              "Fehlersuche.",
              "Aborts as soon as the game calls an Xbox kernel function that is not recreated yet. For debugging "
              "only."},
     .tab = Tab::Advanced, .order = 90, .widget = W::Check, .flags = kAdv},
    {.section = "kernel", .name = "min_guest_stack", .type = T::Int, .defaultValue = "0x200000", .ranged = true,
     .min = 0x10000, .max = 0x4000000, .step = 0x10000,
     .label = {"Mindest-Stack der Spiel-Threads", "Minimum game thread stack"},
     .help = {"Für Entwickler: reservierter Stack je Spiel-Thread in Byte (Standard 0x200000 = 2 MiB).",
              "For developers: reserved stack per game thread in bytes (default 0x200000 = 2 MiB)."},
     .tab = Tab::Advanced, .order = 400, .widget = W::Spin, .flags = kDev},
    {.section = "kernel", .name = "fiber_stack", .type = T::Int, .defaultValue = "0x80000", .ranged = true,
     .min = 0x10000, .max = 0x4000000, .step = 0x10000, .label = {"Fiber-Stackgröße", "Fiber stack size"},
     .help = {"Für Entwickler: Mindestgröße der Fiber-Stacks in Byte (Standard 0x80000 = 512 KiB). Werte außerhalb "
              "von 0x10000 bis 0x4000000 werden begrenzt.",
              "For developers: minimum size of the fiber stacks in bytes (default 0x80000 = 512 KiB). Values "
              "outside 0x10000 to 0x4000000 are clamped."},
     .tab = Tab::Advanced, .order = 410, .widget = W::Spin, .flags = kDev | kHint},

    // [hooks] (design 3.2)
    {.section = "hooks", .name = "disable", .type = T::String, .label = {"Hooks abschalten", "Disable hooks"},
     .help = {"Für Entwickler: kommagetrennte Hook-Namen, Präfixe mit * oder Adressen. Statt des Ersatzes läuft "
              "dann der Originalcode.",
              "For developers: comma-separated hook names, prefixes with * or addresses. The original code runs "
              "instead of the replacement."},
     .tab = Tab::Advanced, .order = 420, .widget = W::Text, .flags = kDev},
};

// Every key's section is listed in kSections, and the keys of a section are contiguous (schema JSON groups them).
consteval bool SectionsConsistent() {
    for (std::size_t i = 0; i < std::size(kKeys); ++i) {
        bool known = false;
        for (const ConfigSectionDef& s : kSections) {
            known = known || std::string_view(s.name) == kKeys[i].section;
        }
        if (!known) {
            return false;
        }
        for (std::size_t j = i + 2; j < std::size(kKeys); ++j) {
            if (std::string_view(kKeys[i].section) == kKeys[j].section &&
                std::string_view(kKeys[j - 1].section) != kKeys[i].section) {
                return false;
            }
        }
    }
    return true;
}
static_assert(SectionsConsistent(), "config_schema: unknown section or a section split into two groups");

bool IEquals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); };
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

std::string_view TrimBlanks(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

// Parses a whole decimal number without sign (resolution components).
bool ParseDecimal(std::string_view s, std::uint32_t& out) noexcept {
    s = TrimBlanks(s);
    if (s.empty()) {
        return false;
    }
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out, 10);
    return ec == std::errc() && end == s.data() + s.size();
}

std::string FormatLimit(double v) {
    char buf[64];
    if (v == std::floor(v) && std::fabs(v) < 1e15) {
        const auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), static_cast<long long>(v));
        return ec == std::errc() ? std::string(buf, end) : std::string("?");
    }
    const auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), v);
    return ec == std::errc() ? std::string(buf, end) : std::string("?");
}

// '|'-joined accepted values per key (index = position in kKeys), built once.
const std::vector<std::string>& ChoiceLists() {
    static const std::vector<std::string> lists = [] {
        std::vector<std::string> out(std::size(kKeys));
        for (std::size_t i = 0; i < std::size(kKeys); ++i) {
            if (kKeys[i].type != ConfigKeyType::Choice) {
                continue;
            }
            for (int pass = 0; pass < 2; ++pass) {  // visible choices first, then hidden ones
                for (const ConfigChoice& c : kKeys[i].choices) {
                    if (c.hidden == (pass == 1)) {
                        out[i] += out[i].empty() ? "" : "|";
                        out[i] += c.value;
                    }
                }
            }
        }
        return out;
    }();
    return lists;
}

}  // namespace

std::span<const ConfigSectionDef> ConfigSections() noexcept {
    return kSections;
}

std::span<const ConfigKeyDef> ConfigKeys() noexcept {
    return kKeys;
}

const ConfigKeyDef* FindConfigKey(std::string_view section, std::string_view key) noexcept {
    for (const ConfigKeyDef& k : kKeys) {
        if (IEquals(key, k.name) && (section.empty() || IEquals(section, k.section))) {
            return &k;
        }
    }
    return nullptr;
}

bool IsConfigSection(std::string_view section) noexcept {
    for (const ConfigSectionDef& s : kSections) {
        if (IEquals(section, s.name)) {
            return true;
        }
    }
    return false;
}

const char* ConfigChoiceList(const ConfigKeyDef& key) noexcept {
    if (key.type != ConfigKeyType::Choice || &key < std::begin(kKeys) || &key >= std::end(kKeys)) {
        return nullptr;
    }
    try {
        return ChoiceLists()[static_cast<std::size_t>(&key - std::begin(kKeys))].c_str();
    } catch (...) {
        return nullptr;
    }
}

std::string_view ConfigKeyDefault(std::string_view section, std::string_view key) noexcept {
    const ConfigKeyDef* k = FindConfigKey(section, key);
    return k != nullptr && !section.empty() ? std::string_view(k->defaultValue) : std::string_view();
}

bool ValidateConfigValue(const ConfigKeyDef& key, std::string_view value, std::string& error) {
    const std::string where = "[" + std::string(key.section) + "] " + key.name;
    const std::string shown(value);
    const bool enforceRange = key.ranged && !key.Has(kConfigRangeIsHint);
    switch (key.type) {
    case ConfigKeyType::Bool: {
        bool b = false;
        if (!ParseConfigBool(value, b)) {
            error = "invalid boolean for " + where + ": '" + shown + "' (expected 0/1, true/false, yes/no, on/off)";
            return false;
        }
        return true;
    }
    case ConfigKeyType::Int: {
        std::uint32_t u = 0;
        if (!ParseConfigU32(value, u)) {
            error = "invalid number for " + where + ": '" + shown + "' (expected decimal or 0x hex, no unit)";
            return false;
        }
        if (enforceRange && (u < key.min || u > key.max)) {
            error = "value out of range for " + where + ": '" + shown + "' (expected " + FormatLimit(key.min) +
                    ".." + FormatLimit(key.max) + ")";
            return false;
        }
        return true;
    }
    case ConfigKeyType::Float: {
        double d = 0.0;
        if (!ParseConfigFloat(value, d)) {
            error = "invalid number for " + where + ": '" + shown + "' (expected a decimal number such as 1.5)";
            return false;
        }
        if (enforceRange && (d < key.min || d > key.max)) {
            error = "value out of range for " + where + ": '" + shown + "' (expected " + FormatLimit(key.min) +
                    ".." + FormatLimit(key.max) + ")";
            return false;
        }
        return true;
    }
    case ConfigKeyType::Choice: {
        const char* list = ConfigChoiceList(key);
        if (list == nullptr || !MatchesConfigChoice(value, list)) {
            error = "invalid value for " + where + ": '" + shown + "' (expected " + (list ? list : "?") + ")";
            return false;
        }
        return true;
    }
    case ConfigKeyType::Resolution: {
        ConfigResolution r;
        if (!ParseConfigResolution(value, r)) {
            error = "invalid resolution for " + where + ": '" + shown + "' (expected auto or <width>x<height>, " +
                    "each " + std::to_string(kConfigResolutionMin) + ".." + std::to_string(kConfigResolutionMax) +
                    ", e.g. 1920x1440)";
            return false;
        }
        return true;
    }
    case ConfigKeyType::String:
    case ConfigKeyType::PathFile:
    case ConfigKeyType::PathDir:
        if (key.validate != nullptr) {  // M4: e.g. the [input] binding lists
            std::string detail;
            if (!key.validate(value, detail)) {
                error = "invalid value for " + where + ": '" + shown + "' (" + detail + ")";
                return false;
            }
        }
        return true;
    }
    return true;
}

const char* ConfigKeyTypeName(ConfigKeyType type) noexcept {
    switch (type) {
    case ConfigKeyType::Bool:
        return "bool";
    case ConfigKeyType::Int:
        return "int";
    case ConfigKeyType::Float:
        return "float";
    case ConfigKeyType::Choice:
        return "choice";
    case ConfigKeyType::String:
        return "string";
    case ConfigKeyType::PathFile:
        return "path_file";
    case ConfigKeyType::PathDir:
        return "path_dir";
    case ConfigKeyType::Resolution:
        return "resolution";
    }
    return "string";
}

const char* ConfigUiTabName(ConfigUiTab tab) noexcept {
    switch (tab) {
    case ConfigUiTab::Game:
        return "game";
    case ConfigUiTab::Graphics:
        return "graphics";
    case ConfigUiTab::Paths:
        return "paths";
    case ConfigUiTab::Advanced:
        return "advanced";
    case ConfigUiTab::Controls:
        return "controls";
    case ConfigUiTab::Audio:
        return "audio";
    }
    return "advanced";
}

const char* ConfigUiWidgetName(ConfigUiWidget widget) noexcept {
    switch (widget) {
    case ConfigUiWidget::Combo:
        return "combo";
    case ConfigUiWidget::Check:
        return "check";
    case ConfigUiWidget::Spin:
        return "spin";
    case ConfigUiWidget::DSpin:
        return "dspin";
    case ConfigUiWidget::Text:
        return "text";
    case ConfigUiWidget::File:
        return "file";
    case ConfigUiWidget::Dir:
        return "dir";
    case ConfigUiWidget::Resolution:
        return "resolution";
    case ConfigUiWidget::Monitor:
        return "monitor";
    case ConfigUiWidget::Binding:
        return "binding";
    case ConfigUiWidget::Slider:
        return "slider";
    case ConfigUiWidget::AudioDevice:
        return "audio_device";
    case ConfigUiWidget::Color:
        return "color";
    case ConfigUiWidget::Fraction:
        return "fraction";
    }
    return "text";
}

// ---- value parsers -----------------------------------------------------------------------------------------

bool ParseConfigBool(std::string_view text, bool& out) noexcept {
    const std::string_view v = TrimBlanks(text);
    for (const char* t : {"1", "true", "yes", "on"}) {
        if (IEquals(v, t)) {
            out = true;
            return true;
        }
    }
    for (const char* f : {"0", "false", "no", "off"}) {
        if (IEquals(v, f)) {
            out = false;
            return true;
        }
    }
    return false;
}

bool ParseConfigU32(std::string_view text, std::uint32_t& out) noexcept {
    const std::string_view v = TrimBlanks(text);
    char buf[72];
    if (v.empty() || v.size() >= sizeof(buf)) {
        return false;
    }
    std::memcpy(buf, v.data(), v.size());
    buf[v.size()] = 0;
    char* end = nullptr;
    const unsigned long long n = std::strtoull(buf, &end, 0);  // decimal, 0x hex (and C octal, as before M3c)
    if (end == nullptr || *end != 0 || n > 0xFFFFFFFFull || buf[0] == '-') {
        return false;
    }
    out = static_cast<std::uint32_t>(n);
    return true;
}

bool ParseConfigFloat(std::string_view text, double& out) noexcept {
    const std::string_view v = TrimBlanks(text);
    if (v.empty() || v.front() == '+') {
        return false;
    }
    double d = 0.0;
    const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), d, std::chars_format::fixed);
    if (ec != std::errc() || end != v.data() + v.size() || !std::isfinite(d)) {
        return false;
    }
    out = d;
    return true;
}

bool MatchesConfigChoice(std::string_view value, std::string_view choices) noexcept {
    const std::string_view v = TrimBlanks(value);
    std::uint32_t number = 0;
    const bool isNumber = ParseConfigU32(v, number);
    std::size_t start = 0;
    while (start <= choices.size()) {
        std::size_t bar = choices.find('|', start);
        if (bar == std::string_view::npos) {
            bar = choices.size();
        }
        const std::string_view choice = choices.substr(start, bar - start);
        const std::size_t dots = choice.find("..");
        if (dots != std::string_view::npos) {
            std::uint32_t lo = 0;
            std::uint32_t hi = 0;
            if (isNumber && ParseConfigU32(choice.substr(0, dots), lo) && ParseConfigU32(choice.substr(dots + 2), hi) &&
                number >= lo && number <= hi) {
                return true;
            }
        } else if (IEquals(v, choice)) {
            return true;
        }
        start = bar + 1;
    }
    return false;
}

bool ParseConfigResolution(std::string_view text, ConfigResolution& out) noexcept {
    const std::string_view v = TrimBlanks(text);
    if (IEquals(v, "auto")) {
        out = ConfigResolution{};
        return true;
    }
    const std::size_t x = v.find_first_of("xX");
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    if (x == std::string_view::npos || !ParseDecimal(v.substr(0, x), w) || !ParseDecimal(v.substr(x + 1), h) ||
        w < kConfigResolutionMin || w > kConfigResolutionMax || h < kConfigResolutionMin ||
        h > kConfigResolutionMax) {
        return false;
    }
    out.isAuto = false;
    out.width = w;
    out.height = h;
    return true;
}

}  // namespace core
