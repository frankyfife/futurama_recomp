// Configuration schema: definition table + validation (see config_schema.h).
//
// Evidence for defaults and accepted values (the modules that read the keys):
//   [paths]    core/config.cpp (iso, xbe, userdata), vfs/vfs_init.cpp (disc_dir)
//   [debug]    core/config.cpp, core/crash.cpp (minidump, mmio_trap), platform/platform.cpp (timeout),
//              core/host_main.cpp (exit_hang_test), core/boot.cpp (stop_at, crash_test), gfx/gfx_config.cpp (frames)
//   [kernel]   kernel/kernel_settings.cpp (min_guest_stack 0x10000..0x4000000), kernel/mm_kernel_stack.cpp
//              (fiber_stack, clamped), kernel/kernel_api.cpp (strict)
//   [log]      core/log.cpp ParseLevel (error|warn|info|debug|trace, "warning" = warn)
//   [settings] kernel/kernel_settings.cpp kLanguages + KernelSettings defaults
//   [saves]    kernel/kernel_settings.cpp (key_mode, signature_key, xbox_cert_key)
//   [render]   gfx/gfx_config.cpp (M2/M3/M3b keys); M3c keys resolution, render_scale, msaa, alpha_to_coverage,
//              anisotropy (shared contract item 5, read by the renderer); M3d interpolation, interpolation_debug
//              (gfx/interp_history.cpp SettingsFromConfig)
//   [audio]    audio/dsound_stubs.cpp
//   [game]     xapi/game_timing.cpp (tick_rate, frame_clock)
#include "core/config_schema.h"

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
constexpr ConfigChoice kFrameClockChoices[] = {
    {"lockstep", {"Bildsynchron (empfohlen)", "Frame-locked (recommended)"}},
    {"realtime", {"Echtzeit (wie das Original)", "Real time (like the original)"}},
};
// M3d (frame-clock/pacing agent): [render] frame_rate, gfx/gfx_config.cpp ParseFrameRate (unit test "pacer m3d:
// ParseFrameRate accepts exactly the schema choices"). 60 = the M3b behaviour; 120 and display choose the rate from
// the refresh (platform/pacer_policy.h FrameRateMode). 50 (the PAL logic rate, which an unset frame_rate follows) and
// the numbers 0..1000 stay accepted as developer values (0 = unthrottled).
constexpr ConfigChoice kFrameRateChoices[] = {
    {"60", {"60 fps", "60 fps"}},
    {"120", {"120 fps (interpoliert)", "120 fps (interpolated)"}},
    {"display", {"Bildschirm (max. 120)", "Display (max. 120)"}},
    {"50", {"50 fps (PAL-Logik-Takt)", "50 fps (PAL logic rate)"}, true},
    {"0..1000", {"Feste Bildrate (Entwicklerwert)", "Fixed frame rate (developer value)"}, true},
};
// M3d (interpolation agent): gfx/interp_history.h ParseDebugMode.
constexpr ConfigChoice kInterpolationDebugChoices[] = {
    {"off", {"Aus", "Off"}},
    {"tint", {"Nicht interpolierte Objekte einfärben", "Tint objects that are not interpolated"}},
    {"trace", {"Zähler je Bild in eine CSV-Datei", "Per-frame counters to a CSV file"}},
};

#ifdef NDEBUG
constexpr const char* kDebugBuildDefault = "0";
#else
constexpr const char* kDebugBuildDefault = "1";
#endif

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
              "„Spiel“). „120 fps“ und „Bildschirm“ zeigen auf Bildschirmen mit mehr als 60 Hz bis zu 120 Bilder pro "
              "Sekunde und berechnen die Bilder zwischen zwei Logikschritten. „Bildschirm“ nimmt die höchste "
              "gleichmäßige Rate (120 bei 120 oder 240 Hz, 72 bei 144 Hz ohne VRR), „120 fps“ bleibt dort bei 120 mit "
              "unterschiedlich langen Bildern. An einem 60-Hz-Bildschirm zeigen alle drei 60 Bilder pro Sekunde.",
              "How many frames per second the game shows. The game logic keeps its logic rate (tab \"Game\"). "
              "\"120 fps\" and \"Display\" show up to 120 frames per second on displays above 60 Hz and compute the "
              "frames between two logic steps. \"Display\" takes the highest even rate (120 at 120 or 240 Hz, 72 at "
              "144 Hz without VRR), \"120 fps\" stays at 120 there with frames of different length. On a 60 Hz "
              "display all three show 60 frames per second."},
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
              "Zeile mit den Zählern nach logs\\interp_trace.csv in den Benutzerdaten.",
              "For developers: tint shows objects that are not interpolated (red: jump or new object, yellow: newly "
              "visible, cyan: frame-driven, magenta: context switch). CSV writes one line of counters per frame to "
              "logs\\interp_trace.csv in the user data."},
     .tab = Tab::Advanced, .order = 548, .widget = W::Combo, .flags = kDev},

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

    // [audio] (M2, audio/dsound_stubs.h)
    {.section = "audio", .name = "dummy_streams", .type = T::Bool, .defaultValue = "0",
     .label = {"Stumme Audio-Streams", "Silent audio streams"},
     .help = {"Für Entwickler: gibt dem Spiel stumme Ersatz-Streams statt gar keiner (bis die Tonausgabe kommt).",
              "For developers: gives the game silent stand-in streams instead of none (until sound output exists)."},
     .tab = Tab::Advanced, .order = 800, .widget = W::Check, .flags = kDev},

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
