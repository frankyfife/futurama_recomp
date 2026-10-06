// Renderer-level interpolation (M3d, analysis/loader/m3d_interpolation.md 3-6): per-object transform history
// between the last two logic ticks, camera interpolation, rigid objects and skinning. Owned by the interpolation
// agent; GpuRenderer (gfx/nipc_gpu.cpp) is the only caller. tests/interp_history_tests.cpp.
//
// Time model (interp_frame.h): every frame the frame clock reports the tick G of the guest data and alpha; the
// renderer shows interp(S[G-1], S[G], alpha). alpha >= 1 - 1e-3 (and every frame while the clock does not produce
// intermediate frames, i.e. 60 fps) passes the guest pointers through untouched: zero visual change.
//
// Keys and history (3.2, 5.2):
//   - Draws of slots 48-51: key = (object = NiTransform* - 0x70, camera = the NiCamera of the last SetCameraData,
//     occurrence = how often this (object, camera) was drawn in the frame so far). Signature = vtable obj+0, data,
//     skin instance, property state; a different signature is another object (snap "identity").
//   - Two open-addressing tables (linear probing, Fibonacci hash on the HIGH bits - guest pointers are 16-byte
//     aligned): "cur" = generation G (being filled), "prev" = G-1. When the tick advances they swap and the new
//     cur is reset by its generation stamp (stamp != table generation = empty; no deletion, no tombstones). A key
//     interpolates only when its prev entry is from exactly G-1 (gaps: culling, catch-up, context switches snap).
//   - probe:NiAVObject::Destroy (FUN_00102CE0, the common base destructor) records destroyed addresses while
//     interpolation is active; a prev entry captured before the destruction has no history (snap "destroyed").
//   - Frame-driven objects (3.9): a key whose transform changes between two captures of the same generation
//     (animated with g_frameDelta in Render) is drawn as given for SnapLimits::frameDrivenHoldFrames frames.
// Rigid objects (3.3/3.4): translation lerp, rotation slerp, scale lerp; fast paths AlphaOne / Static pass the
// guest pointer; the snap rules R2/R4 + scale/mirror/orthonormality. The interpolated NiTransform goes into the
// ExtractArena (guest-visible, valid until DisplayFrame resets it) and replaces GeometryInput::transform -
// ni_extract stays unchanged (world matrix, world scale and NORMALIZENORMALS follow automatically).
// Cameras (3.7): location lerp, slerp of the rows (dir, up, right), frustum tangents lerp, near/far/port of the
// current tick; cut rule; the result is host data handed to ni::ExtractCamera instead of the guest pointers.
// Skinning (3.8): relative bone poses root^-1 * bone (host replicas of FUN_0014D880 / FUN_0010DFC0) are captured
// per skinned draw and interpolated bone by bone; the original deformation FUN_0014F710 then runs on a substitute
// NiSkinInstance in the arena (+0x08 real NiSkinData, +0x0C substitute root with identity world, +0x10 list of
// substitute bones whose world +0x70 is the interpolated relative pose). A self-test compares the substitute with
// the original instance at alpha 1 for the first skinned draws (log "interpolation: skin self-test ...").
// Particles: slot 52 is not drawn by the port (P2); see interp_math.h InterpolatePoints.
//
// Debug ([render] interpolation_debug): tint = snapped draws get a colored extra texture stage (red: motion /
// identity / destroyed / mirror / scale / gap, yellow: first sight, cyan: frame-driven, magenta: snap-all), trace =
// one CSV line per frame (<userdata>\interp_trace.csv). Frames listed in [render] trace_frames log
// "interp frame N: ..."; the exit reporter "gfx" logs "interpolation: ..." (GpuRenderer::LogStatistics).
//
// Threading: the guest main thread (slot handlers). OnObjectDestroyed may come from any thread (locked).
#pragma once

#include "gfx/interp_frame.h"
#include "gfx/interp_math.h"
#include "gfx/ni_extract.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

namespace core {
struct Config;
}

namespace gfx::interp {

enum class DebugMode : std::uint8_t { Off, Tint, Trace };

// off | tint | trace (case-insensitive, blanks ignored).
bool ParseDebugMode(std::string_view text, DebugMode* mode) noexcept;
const char* DebugModeName(DebugMode mode) noexcept;

struct Settings {
    bool enabled = true;               // [render] interpolation
    DebugMode debug = DebugMode::Off;  // [render] interpolation_debug
    std::filesystem::path tracePath;   // CSV of debug = trace (<userdata>\interp_trace.csv)
};
// [render] interpolation / interpolation_debug (invalid values: GFX/warn and the default). May throw bad_alloc.
Settings SettingsFromConfig(const core::Config& config);

// Largest bone count handled (the disc has at most 49); larger skins keep the tick pose (SnapReason::BoneCount).
inline constexpr std::uint32_t kMaxBones = 128;
// Occurrences of one (object, camera) per frame that get their own history.
inline constexpr std::uint32_t kMaxOccurrences = 8;
// Table capacity (entries, power of two) at start and at most (load <= 1/2).
inline constexpr std::uint32_t kInitialTableCapacity = 4096;
inline constexpr std::uint32_t kMaxTableCapacity = 1u << 17;

// Counters of one frame (also the totals in Stats).
struct FrameCounters {
    std::uint64_t draws = 0;         // draws seen while recording
    std::uint64_t interpolated = 0;  // drawn with an arena transform and/or a substitute skin
    std::uint64_t reasons[kSnapReasonCount] = {};  // per decision (None = interpolated)
    std::uint64_t cameras = 0;
    std::uint64_t cameraReasons[kSnapReasonCount] = {};
    std::uint64_t skinnedDraws = 0;
    std::uint64_t skinsInterpolated = 0;  // substitute NiSkinInstance used
    std::uint64_t bonesInterpolated = 0;
    std::uint64_t boneSnaps = 0;          // single bones that kept the tick pose (rules R2/R4/checks)
    std::uint64_t skinsWithoutHistory = 0;  // skinned draws whose bones could not be paired (count, unreadable)
    std::uint64_t frameDrivenChanges = 0; // captures that changed within one generation
};

struct Stats {
    std::uint64_t frames = 0;               // EndFrame calls
    std::uint64_t recordedFrames = 0;       // frames with recording active
    std::uint64_t interpolatingFrames = 0;  // ... with alpha < 1 (tick frames at 120 Hz)
    FrameCounters totals;
    std::uint64_t tableOverflows = 0;       // draws not recorded (table at kMaxTableCapacity)
    std::uint64_t occurrenceOverflows = 0;  // draws beyond kMaxOccurrences
    std::uint64_t arenaFailures = 0;        // arena allocation failed (drawn with the tick state)
    std::uint64_t destroyedRecorded = 0;    // NiAVObject destructions recorded
    std::uint64_t generations = 0;          // tick changes seen
    std::uint64_t generationGaps = 0;       // ... by more than one tick (or backwards)
    std::uint32_t tablePeak = 0;            // most entries in one generation
    std::uint32_t tableCapacity = 0;
    std::uint32_t bonePoolPeak = 0;         // most bone records in one generation
    std::uint32_t skinSelfTests = 0;        // substitute vs original FUN_0014F710 at alpha 1
    std::uint32_t skinSelfTestFailures = 0;
    float skinSelfTestMaxError = 0.0f;      // max |difference| / max(1, |vertex|)
    // Cost of Camera + Geometry + EndFrame per recorded frame (TSC, converted at log time).
    std::uint64_t costTicks = 0;
    std::uint64_t costMaxTicks = 0;
};

// What GpuRenderer extracts for one draw of slots 48-51.
struct GeometryResult {
    ni::GeometryInput input;                    // transform / skinInstance replaced when interpolated
    SnapReason reason = SnapReason::Disabled;   // decision of this draw
    SnapReason tint = SnapReason::None;         // debug tint: the snap to show (None = no tint)
};

class History {
public:
    explicit History(const Settings& settings = {}, const SnapLimits& limits = {}) noexcept;
    ~History();
    History(const History&) = delete;
    History& operator=(const History&) = delete;

    // Tests: frame values from `source` instead of CurrentFrameInterp(); no guest calls (skin self-test).
    void SetSourceForTest(FrameInterpSource source) noexcept;
    void SetGuestCallsForTest(bool allow) noexcept;

    // Slot 47 (before ni::ExtractCamera): `cameraObject` = guest address of the NiCamera (location - 0xA0); also
    // the camera of the following draws' keys. Re-reads the frame values (the InsideStep phase ends at Render).
    // Returns `in` itself or a camera pointing into this object (valid until the next call).
    ni::CameraInput Camera(ni::GuestPtr cameraObject, const ni::CameraInput& in) noexcept;
    // Slots 48-51 (before ni::Extract*, not for texture targets).
    GeometryResult Geometry(ni::DrawKind kind, const ni::GeometryInput& in, ni::ExtractArena& arena) noexcept;
    // DisplayFrame of the back buffer, before the arena reset: `frame` = number of the displayed frame (1-based),
    // `logFrame` = log "interp frame N: ..." ([render] trace_frames).
    void EndFrame(std::uint32_t frame, bool logFrame) noexcept;

    // probe:NiAVObject::Destroy (any thread). Recorded only while some History records.
    static void OnObjectDestroyed(ni::GuestPtr object) noexcept;

    const Settings& GetSettings() const noexcept;
    bool Recording() const noexcept;      // this frame captures history
    bool Interpolating() const noexcept;  // this frame shows interpolated states (alpha < 1)
    const FrameInterp& Frame() const noexcept;  // the values latched for this frame
    const Stats& GetStats() const noexcept;
    const FrameCounters& CurrentFrameCounters() const noexcept;

    // "interpolation: on|off ..." (GFX/info) at renderer creation.
    void LogConfiguration() const noexcept;
    // The exit line "interpolation: ..." (GFX/info).
    void LogStatistics() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Tests (5.2): inserts the keys (objects[i], camera, occurrence 0) into a table of `capacity` entries (power of
// two, no growth) and returns the longest linear-probing sequence of a lookup (1 = found in its home slot).
std::uint32_t LongestProbeForTest(const ni::GuestPtr* objects, std::uint32_t count, ni::GuestPtr camera,
                                  std::uint32_t capacity) noexcept;

// Debug tint color (RGBA 0..1) of a snap reason; false = no tint.
bool TintColor(SnapReason reason, float rgba[4]) noexcept;
// Debug tint: appends a stage that multiplies the result with the texture factor `rgba` (D3DTOP_MODULATE of
// CURRENT and TFACTOR) when a stage is free and no active stage reads TFACTOR. False if the item was left as is.
bool ApplyTint(ni::DrawItem* item, const float rgba[4]) noexcept;

}  // namespace gfx::interp
