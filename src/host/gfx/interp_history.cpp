// Renderer-level interpolation: history tables, cameras, rigid objects, skinning (see interp_history.h).
#include "gfx/interp_history.h"

#include "core/config.h"
#include "core/config_schema.h"
#include "core/guest.h"
#include "core/guest_call.h"
#include "core/hooks.h"
#include "core/log.h"

#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace gfx::interp {
namespace {

using ni::GuestPtr;

// Guest layouts used here (gfx/ni_guest_layouts.h, analysis 3.8).
constexpr std::uint32_t kAvParent = 0x14;          // NiAVObject parent NiNode* (NiAVObject::Update 0x101840 calls
                                                   // [this+0x14]->vf44; NiNode::~NiNode detaches its children)
constexpr std::uint32_t kAvWorld = 0x70;           // NiAVObject world NiTransform (slot argument of 48-53)
constexpr std::uint32_t kAvWorldBound = 0x20;      // NiAVObject world NiBound (radius at +0x2C)
constexpr std::uint32_t kAvSize = 0xC0;            // sizeof(NiAVObject)
constexpr std::uint32_t kBoundRadius = 0x0C;       // NiBound radius
constexpr std::uint32_t kSkinData = 0x08;          // NiSkinInstance -> NiSkinData*
constexpr std::uint32_t kSkinRoot = 0x0C;          // NiSkinInstance -> root parent NiAVObject*
constexpr std::uint32_t kSkinBones = 0x10;         // NiSkinInstance -> NiAVObject*[bones]
constexpr std::uint32_t kSkinDataBoneCount = 0x50; // NiSkinData bone count (u32, FUN_0014F710 0x14F786)
constexpr std::uint32_t kSubstituteObject = 0xC0;  // sizeof(NiAVObject): substitute root/bones (zeroed)
constexpr std::uint32_t kSubstituteInstance = 0x20;  // NiSkinInstance 0x18, padded to 16
constexpr GuestPtr kNiAVObjectDestructor = 0x00102CE0;  // NiAVObject::~NiAVObject (13 direct callers)
constexpr std::uint32_t kSkinSelfTests = 4;
constexpr std::uint32_t kSelfTestMaxVertices = 8192;
constexpr float kSelfTestTolerance = 1e-4f;
constexpr std::uint32_t kMaxCameras = 16;
constexpr std::uint32_t kMaxStateLogs = 16;
constexpr std::uint32_t kCostBuckets = 4096;  // per-frame cost histogram in microseconds (last = overflow)
constexpr std::uint32_t kGuestPages = 0x80000000u >> 12;  // readable-page bitmap of the hierarchy walk

bool Plausible(GuestPtr address) noexcept {
    return address >= 0x10000u && address < 0x80000000u;
}

template <class T>
T Read(GuestPtr address) noexcept {
    return guest::Read<T>(address);
}

// ---- destroyed objects (probe:NiAVObject::Destroy) -------------------------------------------------------------

// Fixed-capacity address -> frame set with a generation stamp per slot (stamp != set stamp = empty).
class AddressSet {
public:
    static constexpr std::uint32_t kBits = 14;
    static constexpr std::uint32_t kCapacity = 1u << kBits;  // load <= 1/2 -> 8192 destructions per generation

    bool Allocate() noexcept {
        if (!slots_.empty()) {
            return true;
        }
        try {
            slots_.assign(kCapacity, Slot{});
        } catch (...) {
            return false;
        }
        return true;
    }
    void Clear() noexcept {
        ++stamp_;
        if (stamp_ == 0) {  // wrapped: really clear
            for (Slot& s : slots_) {
                s = Slot{};
            }
            stamp_ = 1;
        }
        count_ = 0;
        overflow_ = false;
    }
    void Insert(GuestPtr address, std::uint32_t frame) noexcept {
        if (slots_.empty()) {
            overflow_ = true;
            return;
        }
        std::uint32_t i = Index(address);
        for (;;) {
            Slot& s = slots_[i];
            if (s.stamp != stamp_) {
                if (count_ + 1 > kCapacity / 2) {
                    overflow_ = true;
                    return;
                }
                s = Slot{address, frame, stamp_};
                ++count_;
                return;
            }
            if (s.address == address) {
                s.frame = std::max(s.frame, frame);
                return;
            }
            i = (i + 1) & (kCapacity - 1);
        }
    }
    // Frame of the latest recorded destruction of `address`; false if none.
    bool Find(GuestPtr address, std::uint32_t* frame) const noexcept {
        if (slots_.empty() || count_ == 0) {
            return false;
        }
        std::uint32_t i = Index(address);
        for (;;) {
            const Slot& s = slots_[i];
            if (s.stamp != stamp_) {
                return false;
            }
            if (s.address == address) {
                *frame = s.frame;
                return true;
            }
            i = (i + 1) & (kCapacity - 1);
        }
    }
    bool Overflow() const noexcept { return overflow_; }
    std::uint32_t Count() const noexcept { return count_; }

private:
    struct Slot {
        GuestPtr address = 0;
        std::uint32_t frame = 0;
        std::uint32_t stamp = 0;
    };
    static std::uint32_t Index(GuestPtr address) noexcept { return (address * 0x9E3779B1u) >> (32 - kBits); }
    std::vector<Slot> slots_;
    std::uint32_t stamp_ = 1;
    std::uint32_t count_ = 0;
    bool overflow_ = false;
};

// Destructions of the current and of the previous generation (a prev entry is captured in G-1, so a destruction
// before G-1 cannot concern it). Written by the probe on any thread.
struct DestroyedRegistry {
    SRWLOCK lock = SRWLOCK_INIT;
    AddressSet sets[2];
    std::uint32_t current = 0;
    std::atomic<bool> tracking{false};
    std::atomic<std::uint32_t> frame{0};    // History frame number (destruction time)
    std::atomic<std::uint32_t> entries{0};  // > 0: some set holds entries or overflowed
    std::atomic<std::uint64_t> recorded{0};

    bool Allocate() noexcept {
        AcquireSRWLockExclusive(&lock);
        const bool ok = sets[0].Allocate() && sets[1].Allocate();
        ReleaseSRWLockExclusive(&lock);
        return ok;
    }
    void Record(GuestPtr address) noexcept {
        AcquireSRWLockExclusive(&lock);
        sets[current].Insert(address, frame.load(std::memory_order_relaxed));
        entries.store(1, std::memory_order_relaxed);
        ReleaseSRWLockExclusive(&lock);
        recorded.fetch_add(1, std::memory_order_relaxed);
    }
    // New generation: the set of G-1 becomes the (empty) set of G+1.
    void Advance() noexcept {
        AcquireSRWLockExclusive(&lock);
        current ^= 1u;
        sets[current].Clear();
        const AddressSet& other = sets[current ^ 1u];
        entries.store(other.Count() != 0 || other.Overflow() ? 1u : 0u, std::memory_order_relaxed);
        ReleaseSRWLockExclusive(&lock);
    }
    void Reset() noexcept {
        AcquireSRWLockExclusive(&lock);
        sets[0].Clear();
        sets[1].Clear();
        entries.store(0, std::memory_order_relaxed);
        ReleaseSRWLockExclusive(&lock);
    }
    // True if `address` was destroyed in or after frame `since` (or a set overflowed: conservative).
    bool DestroyedSince(GuestPtr address, std::uint32_t since) noexcept {
        if (entries.load(std::memory_order_relaxed) == 0) {
            return false;
        }
        bool result = false;
        AcquireSRWLockShared(&lock);
        for (const AddressSet& s : sets) {
            std::uint32_t f = 0;
            if (s.Overflow() || (s.Find(address, &f) && f >= since)) {
                result = true;
                break;
            }
        }
        ReleaseSRWLockShared(&lock);
        return result;
    }
};

DestroyedRegistry& Registry() noexcept {
    static DestroyedRegistry registry;
    return registry;
}

// NiAVObject::~NiAVObject FUN_00102CE0 - thiscall (ecx = object), no stack args, plain ret 0x102D4B; the base
// destructor every NiAVObject destructor ends in (NiNode FUN_001066B0, NiGeometry FUN_001447B0, NiCamera
// FUN_000FFB60, ...). Prologue 6A FF 68 93 A4 17 00 = push -1; push 0x17A493 (SEH frame setup, no relative
// operand; the next instruction mov eax,fs:[0] stays in place - fs:[0] is never fs-patched). Declared __fastcall
// with an unused edx (same layout and purge as thiscall without stack args). Records the address while a History
// records (interpolation active), otherwise only the original runs.
std::atomic<guest::GuestAddr> g_destructorTrampoline{0};

std::uint32_t __fastcall NiAVObjectDestroyProbe(std::uint32_t self, std::uint32_t /*edx*/) noexcept {
    History::OnObjectDestroyed(self);
    guest::GuestAddr t = g_destructorTrampoline.load(std::memory_order_relaxed);
    if (t == 0) {
        t = core::HookTrampoline(kNiAVObjectDestructor);
        if (t == 0) {
            core::Fatal("probe:NiAVObject::Destroy at 0x%08X runs without an installed trampoline",
                        kNiAVObjectDestructor);
        }
        g_destructorTrampoline.store(t, std::memory_order_relaxed);
    }
    return core::CallGuestThiscall(t, self, {});
}

// ---- history tables ----------------------------------------------------------------------------------------------

enum EntryFlag : std::uint8_t {
    kMotionKnown = 1u << 0,    // step/angle hold the motion of the tick ending at this generation
    kAfterDestroy = 1u << 1,   // captured after a destruction of the same address in the capture frame (review
                               // finding 3: that destruction concerns the previous object, not this capture)
    kChainSnap = 1u << 2,      // node: this node or an ancestor of its chain jumped in this generation
};

// Per-tick motion reference of a key whose motion of the last tick is `known` (MotionSnapRule).
MotionReference Reference(bool known, float step, float angle) noexcept {
    MotionReference r;
    r.known = known;
    r.step = known ? step : 0.0f;
    r.angleDeg = known ? angle : 0.0f;
    return r;
}

// Entry::refAngleCenti (an angle per tick is at most 180 degrees).
std::uint16_t ToCenti(float degrees) noexcept {
    const float v = std::isfinite(degrees) ? std::clamp(degrees, 0.0f, 655.0f) : 0.0f;
    return static_cast<std::uint16_t>(std::lround(v * 100.0f));
}
float FromCenti(std::uint16_t centi) noexcept {
    return static_cast<float>(centi) / 100.0f;
}

// How a node's interpolated world (stage 1b) was formed.
enum class NodeKind : std::uint8_t {
    Current,    // no history in G-1 (or another node / parent): the current world
    Static,     // world unchanged since G-1: the current world
    World,      // topmost node with history: interpolated in world space
    Snapped,    // topmost node with history whose world jumped: the current world
    Local,      // parent(alpha) * interp(local)
    LocalSnap,  // parent(alpha) * current local (the local transform jumped or failed a matrix check)
};

// One key of one generation (5.2: 128 bytes).
struct Entry {
    GuestPtr object;              // NiTransform* - 0x70
    GuestPtr camera;              // NiCamera of the last SetCameraData
    std::uint32_t stamp;          // generation (0 = never used)
    std::uint32_t frame;          // frame of the last capture (occurrence counting, destroyed check)
    GuestPtr vtable;              // signature
    GuestPtr data;
    GuestPtr skin;
    GuestPtr propertyState;
    ni::NiTransform world;        // the captured world transform
    float step;                   // motion per tick of the tick G-1 -> G (kMotionKnown)
    float angle;
    std::uint32_t frameDrivenUntil;  // frame number until which the key counts as frame-driven
    std::uint32_t boneOffset;     // bone records in the table's pool
    std::uint16_t boneCount;
    std::uint8_t occurrence;
    std::uint8_t flags;           // EntryFlag
    std::uint8_t lastReason;      // decision of the last interpolating frame (debug tint in the frames between)
    std::uint8_t pad;
    std::uint16_t refAngleCenti;  // reference of the next tick's rule (kMotionKnown): max angle of the last two ticks
                                  // in 1/100 degree
    float rootScale;              // skinned: world scale of the skin's root (bone radius in root space)
    float refStep;                // reference of the next tick's rule: max step of the last two ticks
};
static_assert(sizeof(Entry) == 128, "history entry layout (analysis 5.2)");

// Relative pose root^-1 * bone of one bone of a skinned draw.
struct BoneRecord {
    ni::NiTransform relative;
    float step;     // per tick (known)
    float angle;
    std::uint32_t known;
    float refStep;  // max step of the last two ticks (reference of the next tick's rule)
};
static_assert(sizeof(BoneRecord) == 80);

// Stage 1b (3.5): one scene-graph node (an ancestor of a drawn object, or the drawn object itself) of one generation,
// keyed (node, camera, 0). `shown` caches the interpolated world of the frame `shownFrame`.
struct NodeEntry {
    GuestPtr object;              // the NiAVObject
    GuestPtr camera;              // NiCamera of the last SetCameraData (billboard worlds depend on it)
    std::uint32_t stamp;          // generation (0 = never used)
    std::uint32_t frame;          // frame of the last capture
    GuestPtr parent;              // +0x14 at the capture
    GuestPtr vtable;
    ni::NiTransform world;        // +0x70 at the capture
    float step;                   // local motion per tick (relative to the parent) of the tick G-1 -> G (kMotionKnown)
    float angle;
    float radius;                 // world bound radius +0x2C at the capture
    std::uint32_t shownFrame;     // frame of `shown` (0 = none)
    std::uint8_t occurrence;      // always 0 (table key)
    std::uint8_t flags;           // EntryFlag (kMotionKnown, kChainSnap)
    std::uint8_t shownKind;       // NodeKind of `shown`
    std::uint8_t snapReason;      // SnapReason of the jump of this node or the nearest ancestor (kChainSnap)
    std::uint32_t motionGen;      // generation whose motion step/angle (and shownKind) were evaluated
    float refStep;                // max step / angle of the last two ticks (reference of the next tick's rule)
    float refAngle;
    ni::NiTransform shown;        // interpolated world of the frame shownFrame
};
static_assert(sizeof(NodeEntry) == 184);

// Open-addressing table of one generation (5.2). E has the key fields object/camera/occurrence and the stamp.
template <class E>
class BasicTable {
public:
    bool Allocate(std::uint32_t capacity) noexcept {
        try {
            slots_.assign(capacity, E{});
            bones.reserve(256);
        } catch (...) {
            slots_.clear();
            return false;
        }
        SetCapacity(capacity);
        return true;
    }
    bool Ready() const noexcept { return !slots_.empty(); }
    // A new generation: every slot is stale (no clearing needed - stamps grow monotonically).
    void Reset(std::uint32_t generation) noexcept {
        if (generation <= gen_) {  // cannot happen with the monotonic History counter; be safe
            for (E& e : slots_) {
                e.stamp = 0;
            }
        }
        gen_ = generation;
        count_ = 0;
        bones.clear();
    }
    std::uint32_t Generation() const noexcept { return gen_; }
    std::uint32_t Count() const noexcept { return count_; }
    // Calls f(entry) for every entry of the current generation.
    template <class F>
    void ForEach(F&& f) const {
        for (const E& e : slots_) {
            if (e.stamp == gen_ && gen_ != 0) {
                f(e);
            }
        }
    }
    std::uint32_t Capacity() const noexcept { return static_cast<std::uint32_t>(slots_.size()); }

    E* Find(GuestPtr object, GuestPtr camera, std::uint32_t occurrence) noexcept {
        if (slots_.empty() || gen_ == 0) {
            return nullptr;
        }
        std::uint32_t i = Hash(object, camera, occurrence);
        for (;;) {
            E& e = slots_[i];
            if (e.stamp != gen_) {
                return nullptr;
            }
            if (e.object == object && e.camera == camera && e.occurrence == occurrence) {
                return &e;
            }
            i = (i + 1) & mask_;
        }
    }
    // Inserts a key that is not present (Find returned nullptr). nullptr when the table cannot grow. Pointers
    // returned earlier by Find/Insert of THIS table are invalid afterwards (the table may grow).
    E* Insert(GuestPtr object, GuestPtr camera, std::uint32_t occurrence) noexcept {
        if (slots_.empty()) {
            return nullptr;
        }
        if (count_ + 1 > Capacity() / 2 && !Grow()) {
            return nullptr;
        }
        E* e = Place(object, camera, occurrence);
        ++count_;
        return e;
    }
    // Slots a lookup of the key visits (tests).
    std::uint32_t ProbeLength(GuestPtr object, GuestPtr camera, std::uint32_t occurrence) const noexcept {
        if (slots_.empty() || gen_ == 0) {
            return 0;
        }
        std::uint32_t i = Hash(object, camera, occurrence);
        for (std::uint32_t n = 1; n <= Capacity(); ++n) {
            const E& e = slots_[i];
            if (e.stamp != gen_ || (e.object == object && e.camera == camera && e.occurrence == occurrence)) {
                return n;
            }
            i = (i + 1) & mask_;
        }
        return Capacity();
    }
    // Makes the table at least `capacity` large (stale contents; used before Reset).
    void EnsureCapacity(std::uint32_t capacity) noexcept {
        if (Capacity() < capacity) {
            try {
                slots_.assign(capacity, E{});
                SetCapacity(capacity);
                count_ = 0;
            } catch (...) {
            }
        }
    }

    std::vector<BoneRecord> bones;  // bone records of this generation (object tables)

private:
    static std::uint32_t Mix(GuestPtr object, GuestPtr camera, std::uint32_t occurrence) noexcept {
        return (object ^ (camera * 0x85EBCA77u) ^ occurrence) * 0x9E3779B1u;  // Fibonacci; use the HIGH bits
    }
    std::uint32_t Hash(GuestPtr object, GuestPtr camera, std::uint32_t occurrence) const noexcept {
        return Mix(object, camera, occurrence) >> shift_;
    }
    void SetCapacity(std::uint32_t capacity) noexcept {
        mask_ = capacity - 1;
        std::uint32_t bits = 0;
        while ((1u << bits) < capacity) {
            ++bits;
        }
        shift_ = 32 - bits;
    }
    E* Place(GuestPtr object, GuestPtr camera, std::uint32_t occurrence) noexcept {
        std::uint32_t i = Hash(object, camera, occurrence);
        while (slots_[i].stamp == gen_) {
            i = (i + 1) & mask_;
        }
        E& e = slots_[i];
        std::memset(&e, 0, sizeof(E));
        e.object = object;
        e.camera = camera;
        e.occurrence = static_cast<std::uint8_t>(occurrence);
        e.stamp = gen_;
        return &e;
    }
    bool Grow() noexcept {
        const std::uint32_t capacity = Capacity() * 2;
        if (capacity > kMaxTableCapacity) {
            return false;
        }
        std::vector<E> old;
        try {
            old.swap(slots_);
            slots_.assign(capacity, E{});
        } catch (...) {
            slots_.swap(old);
            return false;
        }
        SetCapacity(capacity);
        for (const E& e : old) {
            if (e.stamp == gen_) {
                E* n = Place(e.object, e.camera, e.occurrence);
                *n = e;
            }
        }
        return true;
    }

    std::vector<E> slots_;
    std::uint32_t mask_ = 0;
    std::uint32_t shift_ = 32;
    std::uint32_t gen_ = 0;
    std::uint32_t count_ = 0;
};
using Table = BasicTable<Entry>;
using NodeTable = BasicTable<NodeEntry>;

// One camera key with the states of the last two generations.
struct CameraEntry {
    GuestPtr camera = 0;
    std::uint32_t occurrence = 0;
    std::uint32_t curGen = 0;   // 0 = unused
    std::uint32_t prevGen = 0;  // 0 = none
    std::uint32_t frame = 0;
    CameraState cur, prev;
    float step = 0.0f, angle = 0.0f;  // motion prevGen -> curGen (known)
    bool known = false;
    float beforeStep = 0.0f, beforeAngle = 0.0f;  // the tick before
    bool beforeKnown = false;
    std::uint32_t frameDrivenUntil = 0;
    // Destructions of the camera's address from this frame on concern this entry (review finding 4); frame + 1 after a
    // restart caused by a destruction in that frame.
    std::uint32_t destroyedCheckFrom = 0;
};

// One draw of a [render] trace_frames frame (debug = trace, interp_draws.csv).
struct DrawDetail {
    std::uint32_t index;
    GuestPtr object, camera, parent;
    std::uint8_t occurrence, reason, hierarchy;
    float radius, prevStep, prevAngle, step, angle;
    float cur[3], prev[3], shown[3];
    float lerp[3];  // world-space interpolation (= shown unless the hierarchy formed `shown`)
};

// Signature check (3.2).
struct Signature {
    GuestPtr vtable, data, skin, propertyState;
};
bool SameSignature(const Entry& e, const Signature& s) noexcept {
    return e.vtable == s.vtable && e.data == s.data && e.skin == s.skin && e.propertyState == s.propertyState;
}

std::size_t Index(SnapReason r) noexcept {
    return static_cast<std::size_t>(r);
}

std::uint64_t Ticks() noexcept {
    return __rdtsc();
}

std::int64_t Qpc() noexcept {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

std::int64_t Qpf() noexcept {
    LARGE_INTEGER v;
    QueryPerformanceFrequency(&v);
    return v.QuadPart;
}

std::string_view Trim(std::string_view v) noexcept {
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) {
        v.remove_prefix(1);
    }
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) {
        v.remove_suffix(1);
    }
    return v;
}

bool EqualsNoCase(std::string_view a, const char* b) noexcept {
    const std::size_t n = std::strlen(b);
    if (a.size() != n) {
        return false;
    }
    for (std::size_t i = 0; i < n; ++i) {
        const char c = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
        if (c != b[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace

// ---- settings -----------------------------------------------------------------------------------------------------

bool ParseDebugMode(std::string_view text, DebugMode* mode) noexcept {
    text = Trim(text);
    DebugMode m;
    if (EqualsNoCase(text, "off")) {
        m = DebugMode::Off;
    } else if (EqualsNoCase(text, "tint")) {
        m = DebugMode::Tint;
    } else if (EqualsNoCase(text, "trace")) {
        m = DebugMode::Trace;
    } else {
        return false;
    }
    if (mode != nullptr) {
        *mode = m;
    }
    return true;
}

const char* DebugModeName(DebugMode mode) noexcept {
    switch (mode) {
    case DebugMode::Off:
        return "off";
    case DebugMode::Tint:
        return "tint";
    case DebugMode::Trace:
        return "trace";
    }
    return "?";
}

Settings SettingsFromConfig(const core::Config& config) {
    Settings s;
    bool enabledDefault = true;
    core::ParseConfigBool(core::ConfigKeyDefault("render", "interpolation"), enabledDefault);
    s.enabled = config.GetBool("render", "interpolation", enabledDefault);
    bool hierarchyDefault = true;
    core::ParseConfigBool(core::ConfigKeyDefault("render", "interpolation_hierarchy"), hierarchyDefault);
    s.hierarchy = config.GetBool("render", "interpolation_hierarchy", hierarchyDefault);
    if (config.Has("render", "interpolation_debug")) {
        const std::string text = config.GetString("render", "interpolation_debug");
        if (!ParseDebugMode(text, &s.debug)) {
            FUT_LOGW(Gfx, "[render] interpolation_debug = '%s' is not off|tint|trace - using off", text.c_str());
            s.debug = DebugMode::Off;
        }
    }
    s.tracePath = config.LogDirectory() / "interp_trace.csv";
    s.drawTracePath = config.LogDirectory() / "interp_draws.csv";
    return s;
}

std::uint32_t LongestProbeForTest(const ni::GuestPtr* objects, std::uint32_t count, ni::GuestPtr camera,
                                  std::uint32_t capacity) noexcept {
    Table table;
    if (objects == nullptr || capacity == 0 || (capacity & (capacity - 1)) != 0 || count > capacity / 2 ||
        !table.Allocate(capacity)) {
        return 0;
    }
    table.Reset(1);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (table.Find(objects[i], camera, 0) == nullptr && table.Insert(objects[i], camera, 0) == nullptr) {
            return 0;
        }
    }
    std::uint32_t longest = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        longest = std::max(longest, table.ProbeLength(objects[i], camera, 0));
    }
    return longest;
}

// ---- tint ---------------------------------------------------------------------------------------------------------

bool TintColor(SnapReason reason, float rgba[4]) noexcept {
    float r = 1.0f, g = 0.3f, b = 0.3f;  // red: motion / identity / destroyed / mirror / scale / gap / bones
    switch (reason) {
    case SnapReason::None:
    case SnapReason::Disabled:
    case SnapReason::AlphaOne:
    case SnapReason::Static:
    case SnapReason::kCount:
        return false;
    case SnapReason::FirstSight:  // yellow
        g = 1.0f;
        b = 0.2f;
        break;
    case SnapReason::FrameDriven:  // cyan
        r = 0.2f;
        g = 1.0f;
        b = 1.0f;
        break;
    case SnapReason::SnapAll:  // magenta
        b = 1.0f;
        break;
    case SnapReason::GenerationGap:
    case SnapReason::Identity:
    case SnapReason::Destroyed:
    case SnapReason::Translation:
    case SnapReason::Rotation:
    case SnapReason::Scale:
    case SnapReason::Mirror:
    case SnapReason::NotOrthonormal:
    case SnapReason::CameraCut:
    case SnapReason::BoneCount:
        break;
    }
    rgba[0] = r;
    rgba[1] = g;
    rgba[2] = b;
    rgba[3] = 1.0f;
    return true;
}

bool ApplyTint(ni::DrawItem* item, const float rgba[4]) noexcept {
    if (item == nullptr) {
        return false;
    }
    ni::RenderStateDesc& s = item->state;
    std::uint32_t active = 0;
    while (active < ni::kMaxTextureStages && s.stages[active].colorOp != FF_TOP_DISABLE) {
        const ni::TextureStageDesc& st = s.stages[active];
        for (const std::uint8_t arg : {st.colorArg0, st.colorArg1, st.colorArg2, st.alphaArg0, st.alphaArg1,
                                       st.alphaArg2}) {
            if ((arg & FF_TA_SELECTMASK) == FF_TA_TFACTOR) {
                return false;  // the material uses the texture factor itself
            }
        }
        ++active;
    }
    if (active >= ni::kMaxTextureStages) {
        return false;
    }
    ni::TextureStageDesc tint;
    tint.colorOp = FF_TOP_MODULATE;
    tint.colorArg1 = FF_TA_CURRENT;
    tint.colorArg2 = FF_TA_TFACTOR;
    tint.alphaOp = FF_TOP_SELECTARG1;
    tint.alphaArg1 = FF_TA_CURRENT;
    tint.texCoordIndex = active;
    s.stages[active] = tint;
    if (active + 1 < ni::kMaxTextureStages) {
        s.stages[active + 1].colorOp = FF_TOP_DISABLE;
        s.stages[active + 1].alphaOp = FF_TOP_DISABLE;
    }
    s.stageCount = active + 1;
    s.textureFactor = Float4{rgba[0], rgba[1], rgba[2], rgba[3]};
    return true;
}

// ---- History ------------------------------------------------------------------------------------------------------

struct History::Impl {
    Settings settings;
    SnapLimits limits;
    FrameInterpSource testSource = nullptr;
    bool guestCalls = true;
    bool ready = false;  // tables allocated

    // Frame state.
    std::uint32_t frame = 1;         // number of the frame being drawn
    std::uint32_t latchedFrame = 0;  // frame of the last Latch (0 = none in this frame)
    FrameInterp fi;
    bool recording = false;
    bool interpolating = false;
    bool snapAll = false;
    float alpha = 1.0f;
    bool haveTick = false;
    std::uint64_t lastTick = 0;
    std::uint32_t gen = 0;  // generation of the cur table (internal, monotonic; +2 on gaps)
    // Ticks between the states of the prev and the cur table (1; 2..kMaxPairTicks after a catch-up or the 6 dt cap,
    // review finding 10). Motions are compared per tick (step / span).
    std::uint32_t span = 1;
    float perTick = 1.0f;  // 1 / span
    std::uint32_t releaseEpoch = 0;  // guest::MemoryReleaseEpoch() the readable-page bitmap is valid for
    Table tables[2];
    NodeTable nodeTables[2];  // stage 1b (swapped together with `tables`)
    bool nodesReady = false;
    std::uint32_t curIndex = 0;
    bool prevValid = false;
    std::vector<std::uint32_t> readablePages;  // stage 1b: bitmap of guest pages verified readable
    GuestPtr camera = 0;
    std::array<CameraEntry, kMaxCameras> cameras{};
    CameraState cameraOut;
    std::vector<std::pair<GuestPtr, GuestPtr>> substitutes;  // real -> substitute skin instance (this frame)
    std::array<ni::NiTransform, kMaxBones> poses{};

    // Statistics.
    Stats stats;
    FrameCounters fc;
    bool frameRecorded = false;
    bool frameInterpolated = false;
    std::uint64_t frameCost = 0;
    std::vector<std::uint32_t> costHistogram;  // microseconds per recorded frame
    std::int64_t qpcStart = 0;
    std::uint64_t tscStart = 0;
    double tscPerMicrosecond = 0.0;
    bool lastRecording = false;
    std::uint32_t stateLogs = 0;
    FILE* trace = nullptr;
    bool traceTried = false;
    bool detail = false;               // this frame is listed in [render] trace_frames
    std::vector<DrawDetail> details;   // debug = trace: the draws of a detail frame
    FILE* drawTrace = nullptr;
    bool drawTraceTried = false;
    FILE* nodeTrace = nullptr;              // debug = trace, hierarchy: the node worlds of each traced tick
    std::uint32_t generationFrame = 0;      // first frame of the cur generation

    Table& Cur() noexcept { return tables[curIndex]; }
    Table& Prev() noexcept { return tables[curIndex ^ 1u]; }
    NodeTable& CurNodes() noexcept { return nodeTables[curIndex]; }
    NodeTable& PrevNodes() noexcept { return nodeTables[curIndex ^ 1u]; }
    bool Hierarchy() const noexcept { return settings.hierarchy && nodesReady; }

    // What the parent chain says about a drawn object (EvaluateChain).
    struct ChainResult {
        bool shown = false;          // *out holds the interpolated world from the chain (interpolating frames)
        bool decides = false;        // the drawn object has a parent with history: the chain decides its motion
        SnapReason snap = SnapReason::None;  // the jump of a node of the chain (decides: the draw's snap reason)
    };

    void Latch() noexcept;
    void Advance(bool consecutive, std::uint32_t pairSpan) noexcept;
    void NoteState(bool nowRecording) noexcept;
    ni::CameraInput Camera(GuestPtr cameraObject, const ni::CameraInput& in) noexcept;
    void Geometry(const ni::GeometryInput& in, ni::ExtractArena& arena, GeometryResult* result) noexcept;
    float CaptureBones(Entry& e, GuestPtr skin, bool recapture, const Entry* prev, bool* changed) noexcept;
    GuestPtr InterpolateSkin(const Entry& e, const Entry& p, GuestPtr skin, float radius,
                             ni::ExtractArena& arena) noexcept;
    GuestPtr BuildSubstitute(GuestPtr real, std::uint32_t count, ni::ExtractArena& arena) noexcept;
    void SkinSelfTest(const ni::GeometryInput& in, const Entry& e, ni::ExtractArena& arena) noexcept;
    bool NodeReadable(GuestPtr node) noexcept;
    void CaptureChain(GuestPtr object, const ni::NiTransform& world, float radius) noexcept;
    ChainResult EvaluateChain(GuestPtr object, ni::NiTransform* out) noexcept;
    void EndFrame(std::uint32_t displayed, bool logFrame) noexcept;
    void WriteTrace(std::uint32_t displayed) noexcept;
    void WriteDrawTrace(std::uint32_t displayed) noexcept;
    void WriteNodeTrace(std::uint32_t displayed) noexcept;
    double Microseconds(std::uint64_t ticks) noexcept;
    void Count(SnapReason reason) noexcept { ++fc.reasons[Index(reason)]; }
};

void History::Impl::NoteState(bool nowRecording) noexcept {
    if (nowRecording == lastRecording) {
        return;
    }
    lastRecording = nowRecording;
    if (stateLogs >= kMaxStateLogs) {
        return;
    }
    ++stateLogs;
    if (nowRecording) {
        FUT_LOGI(Gfx, "interpolation: active from frame %u (frame clock source %s, tick %llu, alpha %.3f, flags 0x%X)%s",
                 frame, FrameInterpSourceName(), static_cast<unsigned long long>(fi.tick),
                 static_cast<double>(fi.alpha), fi.flags, stateLogs == kMaxStateLogs ? " - further changes not logged" : "");
    } else {
        FUT_LOGI(Gfx, "interpolation: inactive from frame %u (%s)%s", frame,
                 !fi.enabled ? "the frame clock produces no intermediate frames" : "drawing inside the step loop",
                 stateLogs == kMaxStateLogs ? " - further changes not logged" : "");
    }
}

void History::Impl::Latch() noexcept {
    fi = testSource != nullptr ? testSource() : CurrentFrameInterp();
    latchedFrame = frame;
    const bool active = settings.enabled && fi.enabled && ready;
    Registry().tracking.store(active, std::memory_order_relaxed);
    recording = active && (fi.flags & kFrameInterpInsideStep) == 0;
    if ((fi.flags & kFrameInterpInsideStep) == 0) {
        NoteState(recording);
    }
    if (!recording) {
        interpolating = false;
        snapAll = false;
        alpha = 1.0f;
        return;
    }
    if (!haveTick || fi.tick != lastTick) {
        // One tick, or k = pairTicks ticks at once (catch-up, 6 dt cap): the prev table holds S[G-k].
        const std::uint64_t delta = haveTick && fi.tick > lastTick ? fi.tick - lastTick : 0;
        const bool pair = delta == 1 || (delta >= 2 && delta <= kMaxPairTicks && fi.pairTicks == delta);
        Advance(pair, pair ? static_cast<std::uint32_t>(delta) : 1u);
        haveTick = true;
        lastTick = fi.tick;
    }
    snapAll = (fi.flags & (kFrameInterpSnapAll | kFrameInterpClockReset)) != 0;
    const float a = std::isnan(fi.alpha) ? 1.0f : std::clamp(fi.alpha, 0.0f, 1.0f);
    // Over a pair of k ticks the displayed time G - 1 + a is the weight (k - 1 + a) / k of S[G] against S[G-k].
    alpha = span > 1 ? (static_cast<float>(span - 1) + a) / static_cast<float>(span) : a;
    interpolating = alpha < 1.0f - kAlphaOneEpsilon && !snapAll;
    frameRecorded = true;
    frameInterpolated = frameInterpolated || interpolating;
}

void History::Impl::Advance(bool consecutive, std::uint32_t pairSpan) noexcept {
    ++stats.generations;
    if (!consecutive) {
        ++stats.generationGaps;
    } else if (pairSpan > 1) {
        ++stats.pairSpans;
    }
    span = consecutive && pairSpan >= 1 ? pairSpan : 1u;
    perTick = 1.0f / static_cast<float>(span);
    stats.tablePeak = std::max(stats.tablePeak, Cur().Count());
    stats.bonePoolPeak = std::max(stats.bonePoolPeak, static_cast<std::uint32_t>(Cur().bones.size()));
    stats.nodePeak = std::max(stats.nodePeak, CurNodes().Count());
    const std::uint32_t step = consecutive ? 1u : 2u;
    if (gen > std::numeric_limits<std::uint32_t>::max() - 4) {  // practically unreachable (2^32 ticks)
        gen = 0;
        for (int i = 0; i < 2; ++i) {
            tables[i].Reset(0);
            nodeTables[i].Reset(0);
        }
    }
    gen += step;
    generationFrame = frame;
    curIndex ^= 1u;
    Cur().EnsureCapacity(Prev().Capacity());
    Cur().Reset(gen);
    prevValid = Prev().Generation() != 0 && Prev().Generation() + 1 == gen;
    stats.tableCapacity = std::max(Cur().Capacity(), Prev().Capacity());
    if (nodesReady) {
        CurNodes().EnsureCapacity(PrevNodes().Capacity());
        CurNodes().Reset(gen);
        stats.nodeCapacity = std::max(CurNodes().Capacity(), PrevNodes().Capacity());
    }
    Registry().Advance();
}

ni::CameraInput History::Impl::Camera(GuestPtr cameraObject, const ni::CameraInput& in) noexcept {
    const std::uint64_t t0 = Ticks();
    Latch();
    camera = cameraObject;
    if (!recording || in.location == nullptr || in.direction == nullptr || in.up == nullptr || in.right == nullptr ||
        in.frustum == nullptr || in.port == nullptr) {
        return in;
    }
    ++fc.cameras;
    CameraState s;
    std::memcpy(s.location, in.location, sizeof(s.location));
    std::memcpy(s.direction, in.direction, sizeof(s.direction));
    std::memcpy(s.up, in.up, sizeof(s.up));
    std::memcpy(s.right, in.right, sizeof(s.right));
    s.frustum = *in.frustum;
    s.port = *in.port;
    if (detail && settings.debug == DebugMode::Trace) {
        FUT_LOGI(Gfx,
                 "interp camera frame %u: 0x%08X location %.6g %.6g %.6g, direction %.6g %.6g %.6g, up %.6g %.6g %.6g, "
                 "right %.6g %.6g %.6g, frustum %.6g %.6g %.6g %.6g %.6g %.6g",
                 frame, cameraObject, static_cast<double>(s.location[0]), static_cast<double>(s.location[1]),
                 static_cast<double>(s.location[2]), static_cast<double>(s.direction[0]),
                 static_cast<double>(s.direction[1]), static_cast<double>(s.direction[2]), static_cast<double>(s.up[0]),
                 static_cast<double>(s.up[1]), static_cast<double>(s.up[2]), static_cast<double>(s.right[0]),
                 static_cast<double>(s.right[1]), static_cast<double>(s.right[2]),
                 static_cast<double>(s.frustum.left), static_cast<double>(s.frustum.right),
                 static_cast<double>(s.frustum.top), static_cast<double>(s.frustum.bottom),
                 static_cast<double>(s.frustum.nearPlane), static_cast<double>(s.frustum.farPlane));
    }

    // Key (camera, occurrence): the n-th SetCameraData of this camera in this frame.
    std::uint32_t occurrence = 0;
    CameraEntry* e = nullptr;
    for (;;) {
        e = nullptr;
        for (CameraEntry& c : cameras) {
            if (c.curGen != 0 && c.camera == cameraObject && c.occurrence == occurrence) {
                e = &c;
                break;
            }
        }
        if (e == nullptr || e->frame != frame || occurrence + 1 >= kMaxOccurrences) {
            break;
        }
        ++occurrence;
    }
    if (e != nullptr && e->frame == frame) {
        ++fc.cameraReasons[Index(SnapReason::Disabled)];  // too many occurrences: as given
        frameCost += Ticks() - t0;
        return in;
    }
    bool restartedAfterDestroy = false;
    if (e != nullptr && Registry().DestroyedSince(cameraObject, e->destroyedCheckFrom)) {
        // The NiCamera at this address was destroyed since the last capture (a new shot built at the same address,
        // review finding 4): another camera - no history.
        *e = CameraEntry{};
        e->camera = cameraObject;
        e->occurrence = occurrence;
        restartedAfterDestroy = true;
    }
    if (e == nullptr) {  // free or oldest slot
        e = &cameras[0];
        for (CameraEntry& c : cameras) {
            if (c.curGen == 0) {
                e = &c;
                break;
            }
            if (c.curGen < e->curGen || (c.curGen == e->curGen && c.frame < e->frame)) {
                e = &c;
            }
        }
        *e = CameraEntry{};
        e->camera = cameraObject;
        e->occurrence = occurrence;
        restartedAfterDestroy = Registry().DestroyedSince(cameraObject, frame);
    }
    if (restartedAfterDestroy || e->destroyedCheckFrom <= frame) {
        // A destruction recorded in this frame happened before this capture: it concerns the previous camera.
        e->destroyedCheckFrom = restartedAfterDestroy ? frame + 1 : frame;
    }
    const bool recapture = e->curGen == gen;
    bool changed = false;
    if (recapture) {
        changed = !BitwiseEqual(e->cur, s);
        if (changed) {
            e->frameDrivenUntil = frame + limits.frameDrivenHoldFrames;
            ++fc.frameDrivenChanges;
        }
        e->cur = s;
    } else {
        e->beforeStep = e->step;
        e->beforeAngle = e->angle;
        e->beforeKnown = e->known;
        e->prev = e->cur;
        e->prevGen = e->curGen;
        e->cur = s;
        e->curGen = gen;
        e->known = false;
    }
    e->frame = frame;
    const bool havePrev = e->prevGen != 0 && e->prevGen + 1 == gen;
    const bool same = havePrev && BitwiseEqual(e->prev, e->cur);
    CameraMotion m;
    if (havePrev) {
        if (same) {
            e->step = 0.0f;
            e->angle = 0.0f;
            e->known = true;
        } else if (!recapture || changed || interpolating) {
            m = AnalyzeCamera(e->prev, e->cur, limits);
            e->step = m.step * perTick;  // per tick (a catch-up pair spans several ticks)
            e->angle = m.angleDeg * perTick;
            e->known = m.check != SnapReason::Mirror && m.check != SnapReason::NotOrthonormal;
        }
    }
    SnapReason reason = SnapReason::None;
    if (snapAll) {
        reason = SnapReason::SnapAll;
    } else if (!interpolating) {
        reason = SnapReason::AlphaOne;
    } else if (restartedAfterDestroy) {
        reason = SnapReason::Destroyed;
    } else if (!havePrev) {
        reason = e->prevGen != 0 ? SnapReason::GenerationGap : SnapReason::FirstSight;
    } else if (e->frameDrivenUntil >= frame) {
        reason = SnapReason::FrameDriven;
    } else if (same) {
        reason = SnapReason::Static;
    } else if (m.check != SnapReason::None) {
        reason = m.check;
    } else {
        reason = CameraCutRule(m.step * perTick, m.angleDeg * perTick, e->beforeKnown ? e->beforeStep : 0.0f,
                               e->beforeKnown ? e->beforeAngle : 0.0f, limits);
    }
    ++fc.cameraReasons[Index(reason)];
    ni::CameraInput out = in;
    if (reason == SnapReason::None) {
        BlendCameras(e->prev, e->cur, m, alpha, &cameraOut);
        out.location = cameraOut.location;
        out.direction = cameraOut.direction;
        out.up = cameraOut.up;
        out.right = cameraOut.right;
        out.frustum = &cameraOut.frustum;
        out.port = &cameraOut.port;
    }
    frameCost += Ticks() - t0;
    return out;
}

// Relative bone poses root^-1 * bone of a skinned draw into the cur table's pool; returns the root's world scale (0
// if the skin cannot be read). `changed` = a recapture found other poses than the earlier capture of this generation.
float History::Impl::CaptureBones(Entry& e, GuestPtr skin, bool recapture, const Entry* prev, bool* changed) noexcept {
    *changed = false;
    const GuestPtr skinData = Read<GuestPtr>(skin + kSkinData);
    const GuestPtr root = Read<GuestPtr>(skin + kSkinRoot);
    const GuestPtr boneList = Read<GuestPtr>(skin + kSkinBones);
    if (!Plausible(skinData) || !Plausible(root) || !Plausible(boneList)) {
        e.boneCount = 0;
        return 0.0f;
    }
    const std::uint32_t count = Read<std::uint32_t>(skinData + kSkinDataBoneCount);
    if (count == 0 || count > kMaxBones) {
        e.boneCount = 0;
        return 0.0f;
    }
    const ni::NiTransform rootWorld = Read<ni::NiTransform>(root + kAvWorld);
    if (!(rootWorld.scale != 0.0f) || !std::isfinite(rootWorld.scale)) {
        e.boneCount = 0;
        return 0.0f;
    }
    const ni::NiTransform inverse = Inverse(rootWorld);
    Table& cur = Cur();
    const bool reuse = recapture && e.boneCount == count && e.boneOffset + count <= cur.bones.size();
    if (!reuse) {
        try {
            e.boneOffset = static_cast<std::uint32_t>(cur.bones.size());
            cur.bones.resize(cur.bones.size() + count);
        } catch (...) {
            e.boneCount = 0;
            return 0.0f;
        }
        e.boneCount = static_cast<std::uint16_t>(count);
    }
    const BoneRecord* before = nullptr;
    if (prev != nullptr && prev->boneCount == count) {
        const Table& p = Prev();
        if (prev->boneOffset + count <= p.bones.size()) {
            before = &p.bones[prev->boneOffset];
        }
    }
    BoneRecord* records = &cur.bones[e.boneOffset];
    for (std::uint32_t i = 0; i < count; ++i) {
        const GuestPtr bone = Read<GuestPtr>(boneList + 4 * i);
        if (!Plausible(bone)) {
            e.boneCount = 0;
            return 0.0f;
        }
        const ni::NiTransform relative = Compose(inverse, Read<ni::NiTransform>(bone + kAvWorld));
        BoneRecord& r = records[i];
        const bool same = reuse && BitwiseEqual(r.relative, relative);
        if (reuse && !same) {
            *changed = true;
        }
        if (same && !interpolating) {
            continue;  // motion unchanged
        }
        r.relative = relative;
        r.known = 0;
        r.refStep = 0.0f;
        if (before != nullptr) {
            if (BitwiseEqual(before[i].relative, relative)) {
                r.step = 0.0f;
                r.angle = 0.0f;
                r.known = 1;
            } else {
                const MotionInfo m = AnalyzeMotion(before[i].relative, relative, limits);
                r.step = m.step * perTick;
                r.angle = m.angleDeg * perTick;
                r.known = m.check == SnapReason::None || m.check == SnapReason::Scale ? 1u : 0u;
            }
            r.refStep = before[i].known != 0 ? std::max(r.step, before[i].step) : r.step;
        }
    }
    e.rootScale = rootWorld.scale;
    return rootWorld.scale;
}

// Substitute NiSkinInstance in the arena (3.8): +0x08 the real NiSkinData, +0x0C a root with identity world, +0x10
// the list of substitute bones with `poses[i]` at +0x70. FUN_0014F710 then computes RootToSkin * pose_i * bone data
// - the original formula with root^-1 * bone replaced by the interpolated relative pose.
GuestPtr History::Impl::BuildSubstitute(GuestPtr real, std::uint32_t count, ni::ExtractArena& arena) noexcept {
    const std::uint32_t listBytes = (4 * count + 15u) & ~15u;
    const std::size_t size = kSubstituteInstance + kSubstituteObject + listBytes +
                             static_cast<std::size_t>(count) * kSubstituteObject;
    auto* base = static_cast<std::uint8_t*>(arena.Allocate(size, 16));
    if (base == nullptr) {
        ++stats.arenaFailures;
        return 0;
    }
    std::memset(base, 0, size);
    std::uint8_t* instance = base;
    std::uint8_t* root = instance + kSubstituteInstance;
    std::uint8_t* list = root + kSubstituteObject;
    std::uint8_t* bones = list + listBytes;
    const std::uint32_t header[6] = {Read<std::uint32_t>(real), 1u, Read<GuestPtr>(real + kSkinData), guest::Addr(root),
                                     guest::Addr(list), 0u};
    std::memcpy(instance, header, sizeof(header));
    const ni::NiTransform identity = IdentityTransform();
    std::memcpy(root + kAvWorld, &identity, sizeof(identity));
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint8_t* bone = bones + static_cast<std::size_t>(i) * kSubstituteObject;
        const GuestPtr address = guest::Addr(bone);
        std::memcpy(list + 4 * i, &address, 4);
        std::memcpy(bone + kAvWorld, &poses[i], sizeof(ni::NiTransform));
    }
    return guest::Addr(instance);
}

GuestPtr History::Impl::InterpolateSkin(const Entry& e, const Entry& p, GuestPtr skin, float radius,
                                        ni::ExtractArena& arena) noexcept {
    for (const auto& s : substitutes) {
        if (s.first == skin) {
            return s.second;  // a second draw of the same skin instance in this frame
        }
    }
    const std::uint32_t count = e.boneCount;
    if (count == 0 || p.boneCount != count) {
        return 0;
    }
    const Table& cur = Cur();
    const Table& prev = Prev();
    if (e.boneOffset + count > cur.bones.size() || p.boneOffset + count > prev.bones.size()) {
        return 0;
    }
    const BoneRecord* c = &cur.bones[e.boneOffset];
    const BoneRecord* b = &prev.bones[p.boneOffset];
    bool any = false;
    for (std::uint32_t i = 0; i < count; ++i) {
        poses[i] = c[i].relative;
        if (BitwiseEqual(b[i].relative, c[i].relative)) {
            continue;
        }
        const MotionInfo m = AnalyzeMotion(b[i].relative, c[i].relative, limits);
        SnapReason r = m.check;
        if (r == SnapReason::None) {
            r = MotionSnapRule(m.step * perTick, m.angleDeg * perTick, radius,
                               Reference(b[i].known != 0, b[i].refStep, b[i].angle), limits);
        }
        if (r != SnapReason::None) {
            ++fc.boneSnaps;
            continue;
        }
        BlendTransforms(b[i].relative, c[i].relative, m, alpha, &poses[i]);
        ++fc.bonesInterpolated;
        any = true;
    }
    if (!any) {
        return 0;  // no bone moved relative to the root: the real instance gives the same pose
    }
    const GuestPtr substitute = BuildSubstitute(skin, count, arena);
    if (substitute != 0) {
        try {
            substitutes.emplace_back(skin, substitute);
        } catch (...) {
        }
    }
    return substitute;
}

// Self-test (3.8): the substitute with the CURRENT relative poses must deform like the real instance (difference
// only from the other association of the transform products).
void History::Impl::SkinSelfTest(const ni::GeometryInput& in, const Entry& e, ni::ExtractArena& arena) noexcept {
    if (!guestCalls || stats.skinSelfTests >= kSkinSelfTests || e.boneCount == 0 || !Plausible(in.data)) {
        return;
    }
    const auto& gd = *guest::Ptr<const ni::NiGeometryData>(in.data);
    const std::uint32_t count = gd.vertexCount;
    if (count == 0 || count > kSelfTestMaxVertices || !Plausible(gd.vertices)) {
        return;
    }
    const Table& cur = Cur();
    if (e.boneOffset + e.boneCount > cur.bones.size()) {
        return;
    }
    for (std::uint32_t i = 0; i < e.boneCount; ++i) {
        poses[i] = cur.bones[e.boneOffset + i].relative;
    }
    const bool hasNormals = Plausible(gd.normals);
    const std::uint32_t stride = hasNormals ? 24u : 12u;
    auto* a = static_cast<float*>(arena.Allocate(static_cast<std::size_t>(count) * stride + 16));
    auto* b = static_cast<float*>(arena.Allocate(static_cast<std::size_t>(count) * stride + 16));
    const GuestPtr substitute = BuildSubstitute(in.skinInstance, e.boneCount, arena);
    if (a == nullptr || b == nullptr || substitute == 0) {
        return;
    }
    core::CallGuestThiscall(ni::kNiSkinInstanceDeform, in.skinInstance,
                            {gd.vertices, hasNormals ? gd.normals : 0u, count, guest::Addr(a),
                             hasNormals ? guest::Addr(a + 3) : 0u, stride});
    core::CallGuestThiscall(ni::kNiSkinInstanceDeform, substitute,
                            {gd.vertices, hasNormals ? gd.normals : 0u, count, guest::Addr(b),
                             hasNormals ? guest::Addr(b + 3) : 0u, stride});
    float worst = 0.0f;
    const std::size_t floats = static_cast<std::size_t>(count) * (stride / 4);
    for (std::size_t i = 0; i < floats; ++i) {
        const float d = std::fabs(a[i] - b[i]) / std::max(1.0f, std::fabs(a[i]));
        worst = std::isfinite(d) ? std::max(worst, d) : 1.0f;
    }
    ++stats.skinSelfTests;
    stats.skinSelfTestMaxError = std::max(stats.skinSelfTestMaxError, worst);
    const bool pass = worst <= kSelfTestTolerance;
    if (!pass) {
        ++stats.skinSelfTestFailures;
    }
    if (pass) {
        FUT_LOGI(Gfx, "interpolation: skin self-test %u: substitute NiSkinInstance matches FUN_0014F710 on skin 0x%08X "
                 "(%u bones, %u vertices, max relative difference %.2e)", stats.skinSelfTests, in.skinInstance,
                 e.boneCount, count, static_cast<double>(worst));
    } else {
        FUT_LOGW(Gfx, "interpolation: skin self-test %u FAILED: substitute NiSkinInstance differs from FUN_0014F710 on "
                 "skin 0x%08X (%u bones, %u vertices, max relative difference %.2e > %.0e)", stats.skinSelfTests,
                 in.skinInstance, e.boneCount, count, static_cast<double>(worst),
                 static_cast<double>(kSelfTestTolerance));
    }
}

// ---- stage 1b: hierarchy -----------------------------------------------------------------------------------------

// The parents of a drawn object are live NiNodes (NiNode::~NiNode FUN_001066B0 detaches its children, so a surviving
// child has parent 0), but the walk may leave the subtree the guest just traversed. Each guest page is checked once
// with VirtualQuery (guest::IsReadable) and remembered until the HLE memory manager releases or decommits guest memory
// (guest::MemoryReleaseEpoch, review finding 6): guest::Read has no exception handling, so a page released after its
// check must not stay "readable".
bool History::Impl::NodeReadable(GuestPtr node) noexcept {
    if (!Plausible(node) || node > 0x80000000u - kAvSize) {
        return false;
    }
    if (readablePages.empty()) {
        try {
            readablePages.assign(kGuestPages / 32, 0u);
        } catch (...) {
            return false;
        }
        releaseEpoch = guest::MemoryReleaseEpoch();
    }
    if (const std::uint32_t epoch = guest::MemoryReleaseEpoch(); epoch != releaseEpoch) {
        std::fill(readablePages.begin(), readablePages.end(), 0u);
        releaseEpoch = epoch;
        ++stats.readableCacheResets;
    }
    for (std::uint32_t page = node >> 12; page <= (node + kAvSize - 1) >> 12; ++page) {
        std::uint32_t& word = readablePages[page >> 5];
        const std::uint32_t bit = 1u << (page & 31u);
        if ((word & bit) != 0) {
            continue;
        }
        if (!guest::IsReadable(page << 12, 0x1000)) {
            return false;
        }
        word |= bit;
    }
    return true;
}

// Records the drawn object and its ancestors in the cur node table, once per generation: a walk stops at a node this
// generation already recorded (with its ancestors). A node that changes later within the generation (animated per
// frame) is not re-read - its drawn descendants are frame-driven and drawn as given. The drawn object's world is the
// slot argument (already read).
void History::Impl::CaptureChain(GuestPtr object, const ni::NiTransform& world, float radius) noexcept {
    NodeTable& nodes = CurNodes();
    GuestPtr node = object;
    for (std::uint32_t depth = 0; depth <= kMaxHierarchyDepth && node != 0; ++depth) {
        if (nodes.Find(node, camera, 0) != nullptr) {
            break;  // this node and its ancestors are recorded for this generation
        }
        if (depth != 0 && !NodeReadable(node)) {
            ++stats.nodeOverflows;
            break;
        }
        NodeEntry* n = nodes.Insert(node, camera, 0);  // may grow the table (no other node pointer is held here)
        if (n == nullptr) {
            ++stats.nodeOverflows;
            break;
        }
        n->flags = 0;
        n->step = 0.0f;
        n->angle = 0.0f;
        n->shownFrame = 0;
        n->motionGen = 0;
        n->frame = frame;
        n->vtable = Read<GuestPtr>(node);
        const GuestPtr parent = Read<GuestPtr>(node + kAvParent);
        n->parent = Plausible(parent) ? parent : 0u;
        if (depth == 0) {
            n->world = world;
            n->radius = radius;
        } else {
            n->world = Read<ni::NiTransform>(node + kAvWorld);
            n->radius = Read<float>(node + kAvWorldBound + kBoundRadius);
        }
        if (!std::isfinite(n->radius)) {
            n->radius = 0.0f;
        }
        node = n->parent;
    }
}

// Evaluates the parent chain of a recorded draw top-down (3.5). Per node: the motion of this generation relative to
// the parent (once per generation, `motionGen` - the growth rule of the next tick needs it even when no draw of the
// chain interpolates) and, in interpolating frames, the interpolated world W(alpha) = W_parent(alpha) *
// interp(L_prev, L_cur, alpha) with L = W_parent^-1 * W (once per frame, `shownFrame`); the topmost node with history
// of both ticks interpolates in world space. The jump rule runs per node with the node's own radius and local motion
// (review finding 1: a small part of a rigid assembly that starts moving, or the reversal point of an oscillation, is
// no jump of the part - only a node whose own local transform jumps is). A node that jumped keeps its current local
// transform; kChainSnap marks it and its descendants for this generation.
// Result: `decides` when the drawn object has a parent with history (its motion is judged by the chain, not by the
// world-space rule of the draw), `snap` = the jump of the chain if any, `shown` (*out = the drawn object's
// interpolated world) in interpolating frames when the chain decides.
History::Impl::ChainResult History::Impl::EvaluateChain(GuestPtr object, ni::NiTransform* out) noexcept {
    ChainResult result;
    NodeTable& cur = CurNodes();
    NodeTable* prev = PrevNodes().Generation() != 0 && PrevNodes().Generation() + 1 == gen ? &PrevNodes() : nullptr;
    const bool blend = interpolating;
    const auto done = [&](const NodeEntry* n) { return blend ? n->shownFrame == frame : n->motionGen == gen; };
    NodeEntry* chain[kMaxHierarchyDepth + 1];
    const NodeEntry* history[kMaxHierarchyDepth + 1];  // valid G-1 entry of chain[i] once evaluated (nullptr: none)
    bool evaluated[kMaxHierarchyDepth + 1];
    std::uint32_t count = 0;
    for (NodeEntry* n = cur.Find(object, camera, 0); n != nullptr && count <= kMaxHierarchyDepth;) {
        evaluated[count] = false;
        chain[count++] = n;
        if (done(n) || n->parent == 0) {
            break;
        }
        n = cur.Find(n->parent, camera, 0);  // nullptr: not recorded (depth limit, unreadable) -> a root
    }
    if (count == 0) {
        return result;
    }
    for (std::uint32_t i = count; i-- > 0;) {
        NodeEntry* c = chain[i];
        if (done(c)) {
            continue;
        }
        ++fc.nodesEvaluated;
        const NodeEntry* p = prev != nullptr ? prev->Find(c->object, camera, 0) : nullptr;
        if (p != nullptr &&
            (p->vtable != c->vtable || p->parent != c->parent || Registry().DestroyedSince(c->object, p->frame))) {
            p = nullptr;  // another node at this address or re-parented: no history
        }
        history[i] = p;
        evaluated[i] = true;
        // The parent's states of both ticks (evaluated before this node: the chain runs top-down). A parent without
        // history makes this node the topmost one.
        const NodeEntry* parentCur = i + 1 < count ? chain[i + 1] : nullptr;
        const NodeEntry* parentPrev = nullptr;
        if (parentCur != nullptr && prev != nullptr && parentCur->motionGen == gen &&
            parentCur->shownKind != static_cast<std::uint8_t>(NodeKind::Current)) {
            if (evaluated[i + 1]) {
                parentPrev = history[i + 1];
            } else {
                parentPrev = prev->Find(parentCur->object, camera, 0);
                if (parentPrev != nullptr && parentPrev->vtable != parentCur->vtable) {
                    parentPrev = nullptr;
                }
            }
        }
        const bool knownBefore = p != nullptr && (p->flags & kMotionKnown) != 0;
        c->flags &= static_cast<std::uint8_t>(~(kMotionKnown | kChainSnap));
        c->snapReason = static_cast<std::uint8_t>(SnapReason::None);
        NodeKind kind;
        if (p == nullptr) {
            kind = NodeKind::Current;
            c->shown = c->world;
        } else if (BitwiseEqual(p->world, c->world)) {
            kind = NodeKind::Static;
            c->shown = c->world;
            c->step = 0.0f;
            c->angle = 0.0f;
            c->refStep = knownBefore ? p->step : 0.0f;
            c->refAngle = knownBefore ? p->angle : 0.0f;
            c->flags |= kMotionKnown;
        } else {
            const bool local = parentPrev != nullptr;
            const ni::NiTransform from = local ? Compose(Inverse(parentPrev->world), p->world) : p->world;
            const ni::NiTransform to = local ? Compose(Inverse(parentCur->world), c->world) : c->world;
            const MotionInfo m = AnalyzeMotion(from, to, limits);
            c->step = m.step * perTick;
            c->angle = m.angleDeg * perTick;
            c->refStep = knownBefore ? std::max(c->step, p->step) : c->step;
            c->refAngle = knownBefore ? std::max(c->angle, p->angle) : c->angle;
            if (m.check == SnapReason::None || m.check == SnapReason::Scale) {
                c->flags |= kMotionKnown;
            }
            SnapReason r = m.check;
            if (r == SnapReason::None) {
                // Rules R2/R4 with the node's bound radius (in the parent's space for a local transform) against the
                // larger of the last two known motions.
                const float scale = local ? std::fabs(parentCur->world.scale) : 1.0f;
                const float radius = scale > 0.0f ? c->radius / scale : c->radius;
                r = MotionSnapRule(c->step, c->angle, std::isfinite(radius) ? radius : 0.0f,
                                   Reference(knownBefore, knownBefore ? p->refStep : 0.0f,
                                             knownBefore ? p->refAngle : 0.0f),
                                   limits);
            }
            if (!local) {
                kind = r == SnapReason::None ? NodeKind::World : NodeKind::Snapped;
            } else {
                kind = r == SnapReason::None ? NodeKind::Local : NodeKind::LocalSnap;
            }
            if (r != SnapReason::None) {
                c->flags |= kChainSnap;
                c->snapReason = static_cast<std::uint8_t>(r);
            } else if (local && (parentCur->flags & kChainSnap) != 0) {
                c->flags |= kChainSnap;  // an ancestor jumped: this node moves with it
                c->snapReason = parentCur->snapReason;
            }
            if (blend) {
                if (r != SnapReason::None) {
                    ++fc.nodeSnaps;
                }
                if (kind == NodeKind::World) {
                    BlendTransforms(from, to, m, alpha, &c->shown);
                } else if (kind == NodeKind::Snapped) {
                    c->shown = c->world;
                } else if (kind == NodeKind::Local) {
                    ni::NiTransform blended;
                    BlendTransforms(from, to, m, alpha, &blended);
                    c->shown = Compose(parentCur->shown, blended);
                } else {
                    c->shown = Compose(parentCur->shown, to);
                }
            }
        }
        c->shownKind = static_cast<std::uint8_t>(kind);
        c->motionGen = gen;
        if (blend) {
            c->shownFrame = frame;
        }
    }
    const NodeEntry* leaf = chain[0];
    const auto kind = static_cast<NodeKind>(leaf->shownKind);
    result.decides = kind == NodeKind::Local || kind == NodeKind::LocalSnap;
    if (result.decides && (leaf->flags & kChainSnap) != 0) {
        result.snap = static_cast<SnapReason>(leaf->snapReason);
    }
    if (blend && result.decides) {
        *out = leaf->shown;
        result.shown = true;
    }
    return result;
}

void History::Impl::Geometry(const ni::GeometryInput& in, ni::ExtractArena& arena, GeometryResult* result) noexcept {
    ++fc.draws;
    if (!Plausible(in.transform) || in.transform < 0x10000u + kAvWorld) {
        result->reason = SnapReason::Disabled;
        Count(SnapReason::Disabled);
        return;
    }
    const GuestPtr object = in.transform - kAvWorld;
    const ni::NiTransform world = Read<ni::NiTransform>(in.transform);
    const Signature sig{Read<GuestPtr>(object), in.data, in.skinInstance, in.propertyState};
    const auto destroyedSince = [&](const Entry& entry) {
        // A destruction recorded in the capture frame before the capture concerns the previous object (kAfterDestroy).
        return Registry().DestroyedSince(object, entry.frame + ((entry.flags & kAfterDestroy) != 0 ? 1u : 0u));
    };

    // Key (object, camera, occurrence) of the cur table.
    Table& cur = Cur();
    std::uint32_t occurrence = 0;
    Entry* e = nullptr;
    for (; occurrence < kMaxOccurrences; ++occurrence) {
        e = cur.Find(object, camera, occurrence);
        if (e == nullptr || e->frame != frame) {
            break;
        }
    }
    if (occurrence == kMaxOccurrences) {
        ++stats.occurrenceOverflows;
        result->reason = SnapReason::Disabled;
        Count(SnapReason::Disabled);
        return;
    }
    bool recapture = e != nullptr;
    bool changed = false;
    if (recapture && !SameSignature(*e, sig)) {
        recapture = false;  // another object at the same address within the generation: start over
    } else if (recapture && destroyedSince(*e)) {
        // Destroyed and re-created with the same signature within the generation (a clone sharing data and property
        // state, review finding 3): another object as well, not a frame-driven change of this one.
        recapture = false;
        ++fc.recaptureDestroyed;
    } else if (recapture) {
        changed = !BitwiseEqual(e->world, world);
        e->flags &= static_cast<std::uint8_t>(~kAfterDestroy);  // the capture moves to this (later) frame
    }
    if (e == nullptr) {
        e = cur.Insert(object, camera, occurrence);
        if (e == nullptr) {
            ++stats.tableOverflows;
            result->reason = SnapReason::Disabled;
            Count(SnapReason::Disabled);
            return;
        }
    }
    const Entry* p = prevValid ? Prev().Find(object, camera, occurrence) : nullptr;
    const bool pSame = p != nullptr && SameSignature(*p, sig);
    const bool pDestroyed = pSame && destroyedSince(*p);  // the G-1 capture belongs to a destroyed object
    const bool pValid = pSame && !pDestroyed;
    const bool pKnown = pValid && (p->flags & kMotionKnown) != 0;
    if (!recapture) {
        e->vtable = sig.vtable;
        e->data = sig.data;
        e->skin = sig.skin;
        e->propertyState = sig.propertyState;
        e->flags = Registry().DestroyedSince(object, frame) ? kAfterDestroy : 0u;
        e->step = 0.0f;
        e->angle = 0.0f;
        e->refStep = 0.0f;
        e->refAngleCenti = 0;
        e->boneCount = 0;
        e->boneOffset = 0;
        e->frameDrivenUntil = pValid ? p->frameDrivenUntil : 0;
        e->lastReason = pValid ? p->lastReason : static_cast<std::uint8_t>(SnapReason::None);
    }
    e->world = world;
    e->frame = frame;
    if (changed) {
        e->frameDrivenUntil = frame + limits.frameDrivenHoldFrames;
        ++fc.frameDrivenChanges;
    }

    // Motion of the tick G-1 -> G (per tick: a catch-up pair spans `span` ticks): the decision now and the reference
    // of the next tick's rule (the larger of the last two known motions).
    MotionInfo motion;
    bool analyzed = false;
    bool same = false;
    if (pValid) {
        same = BitwiseEqual(p->world, world);
        if (same) {
            e->step = 0.0f;
            e->angle = 0.0f;
            e->refStep = pKnown ? p->step : 0.0f;
            e->refAngleCenti = ToCenti(pKnown ? p->angle : 0.0f);
            e->flags |= kMotionKnown;
        } else if (!recapture || changed || interpolating) {
            motion = AnalyzeMotion(p->world, world, limits);
            analyzed = true;
            e->step = motion.step * perTick;
            e->angle = motion.angleDeg * perTick;
            e->refStep = pKnown ? std::max(e->step, p->step) : e->step;
            e->refAngleCenti = ToCenti(pKnown ? std::max(e->angle, p->angle) : e->angle);
            if (motion.check == SnapReason::None || motion.check == SnapReason::Scale) {
                e->flags |= kMotionKnown;
            } else {
                e->flags &= static_cast<std::uint8_t>(~kMotionKnown);
            }
        }
    }

    // Skinned draws: relative bone poses (every recorded frame - frame-driven detection, history of the next tick).
    float rootScale = 0.0f;
    if (in.skinInstance != 0 && Plausible(in.skinInstance)) {
        ++fc.skinnedDraws;
        bool bonesChanged = false;
        rootScale = CaptureBones(*e, in.skinInstance, recapture, pValid ? p : nullptr, &bonesChanged);
        if (bonesChanged) {
            e->frameDrivenUntil = frame + limits.frameDrivenHoldFrames;
            if (!changed) {
                ++fc.frameDrivenChanges;
            }
        }
        if (stats.skinSelfTests < kSkinSelfTests) {
            SkinSelfTest(in, *e, arena);
        }
    }

    // Stage 1b: the parent chain of this generation (first occurrence only - further occurrences of one object in a
    // frame are drawn with other transforms and interpolate in world space).
    float boundRadius = Plausible(in.bound) ? Read<float>(in.bound + kBoundRadius) : 0.0f;
    if (!std::isfinite(boundRadius)) {
        boundRadius = 0.0f;
    }
    const bool chainKey = Hierarchy() && occurrence == 0;
    if (chainKey) {
        CaptureChain(object, world, boundRadius);
    }

    SnapReason reason;
    bool motionRule = false;  // the jump rule R2/R4 made the decision (the parent chain may judge it instead)
    if (snapAll) {
        reason = SnapReason::SnapAll;
    } else if (!interpolating) {
        reason = SnapReason::AlphaOne;
    } else if (p == nullptr) {
        reason = !prevValid && Prev().Find(object, camera, occurrence) != nullptr ? SnapReason::GenerationGap
                                                                                  : SnapReason::FirstSight;
    } else if (pDestroyed) {
        reason = SnapReason::Destroyed;
    } else if (!pValid) {
        reason = SnapReason::Identity;
    } else if (e->frameDrivenUntil >= frame) {
        reason = SnapReason::FrameDriven;
    } else if (same) {
        reason = SnapReason::Static;
    } else {
        if (!analyzed) {
            motion = AnalyzeMotion(p->world, world, limits);
            analyzed = true;
        }
        if (motion.check != SnapReason::None) {
            reason = motion.check;
        } else {
            reason = MotionSnapRule(motion.step * perTick, motion.angleDeg * perTick, boundRadius,
                                    Reference(pKnown, p->refStep, FromCenti(p->refAngleCenti)), limits);
            motionRule = true;
        }
    }

    // The node motion of the chain (also when this draw does not interpolate: the reference of the next tick) and,
    // when the drawn object has a parent with history, the chain's verdict on the jump (review finding 1).
    ChainResult chain;
    ni::NiTransform chainWorld = world;
    if (chainKey) {
        chain = EvaluateChain(object, &chainWorld);
    }
    const bool useChain = motionRule && chain.decides && chain.shown;
    if (useChain && chain.snap != reason) {
        ++fc.chainDecisions;  // the world-space rule of the leaf would have decided otherwise
    }
    if (useChain) {
        reason = chain.snap;
    }

    bool interpolated = false;
    std::uint8_t hierarchy = 0;  // detail trace: 1 = along the parent chain, 2 = world (no node history)
    float lerpTranslate[3] = {};
    bool haveLerp = false;
    if (reason == SnapReason::None || useChain) {
        auto* t = arena.AllocateArray<ni::NiTransform>(1);
        if (t == nullptr) {
            ++stats.arenaFailures;
            if (reason == SnapReason::None) {
                reason = SnapReason::Disabled;
            }
        } else {
            if (useChain) {
                // The chain's world: interpolated, or - after a jump in the chain - the jumped node's current local
                // transform under its interpolated parents with the nodes below it still interpolated.
                *t = chainWorld;
                hierarchy = 1;
                if (reason == SnapReason::None) {
                    ++fc.hierarchical;
                }
                if (detail) {
                    ni::NiTransform lerp;
                    BlendTransforms(p->world, world, motion, alpha, &lerp);
                    std::memcpy(lerpTranslate, lerp.translate, sizeof(lerpTranslate));
                    haveLerp = true;
                }
            } else {
                BlendTransforms(p->world, world, motion, alpha, t);
                if (chainKey) {
                    hierarchy = 2;
                    ++fc.hierarchyFallbacks;
                }
            }
            result->input.transform = guest::Addr(t);
            interpolated = reason == SnapReason::None;
        }
    }
    if (in.skinInstance != 0 && pValid && (reason == SnapReason::None || reason == SnapReason::Static)) {
        if (e->boneCount == 0 || p->boneCount != e->boneCount) {
            ++fc.skinsWithoutHistory;  // the skin keeps the tick pose (the draw's own decision stands)
        } else {
            const float scale = rootScale != 0.0f ? std::fabs(rootScale) : 1.0f;
            const float radius = Plausible(in.bound) ? Read<float>(in.bound + kBoundRadius) / scale : 0.0f;
            const GuestPtr substitute = InterpolateSkin(*e, *p, in.skinInstance, std::isfinite(radius) ? radius : 0.0f,
                                                        arena);
            if (substitute != 0) {
                result->input.skinInstance = substitute;
                interpolated = true;
                ++fc.skinsInterpolated;
            }
        }
    }
    if (interpolated) {
        reason = SnapReason::None;
        ++fc.interpolated;
    }
    if (interpolating || snapAll) {
        e->lastReason = static_cast<std::uint8_t>(reason);
    }
    result->reason = reason;
    Count(reason);
    if (settings.debug == DebugMode::Tint) {
        const auto shown = interpolating || snapAll ? reason : static_cast<SnapReason>(e->lastReason);
        result->tint = IsSnap(shown) ? shown : SnapReason::None;
    }
    if (detail && settings.debug == DebugMode::Trace) {
        DrawDetail dd{};
        dd.index = static_cast<std::uint32_t>(fc.draws);
        dd.object = object;
        dd.camera = camera;
        dd.parent = Read<GuestPtr>(object + kAvParent);
        dd.occurrence = static_cast<std::uint8_t>(occurrence);
        dd.reason = static_cast<std::uint8_t>(reason);
        dd.hierarchy = hierarchy;
        dd.radius = boundRadius;
        dd.prevStep = pKnown ? p->step : -1.0f;
        dd.prevAngle = pKnown ? p->angle : -1.0f;
        dd.step = (e->flags & kMotionKnown) != 0 ? e->step : -1.0f;
        dd.angle = (e->flags & kMotionKnown) != 0 ? e->angle : -1.0f;
        const ni::NiTransform shownWorld = Read<ni::NiTransform>(result->input.transform);
        for (int c = 0; c < 3; ++c) {
            dd.cur[c] = world.translate[c];
            dd.prev[c] = pValid ? p->world.translate[c] : world.translate[c];
            dd.shown[c] = shownWorld.translate[c];
            dd.lerp[c] = haveLerp ? lerpTranslate[c] : shownWorld.translate[c];
        }
        try {
            details.push_back(dd);
        } catch (...) {
        }
    }
}

double History::Impl::Microseconds(std::uint64_t ticks) noexcept {
    return tscPerMicrosecond > 0.0 ? static_cast<double>(ticks) / tscPerMicrosecond : 0.0;
}

void History::Impl::WriteTrace(std::uint32_t displayed) noexcept {
    if (settings.debug != DebugMode::Trace) {
        return;
    }
    if (!traceTried) {
        traceTried = true;
        if (!settings.tracePath.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(settings.tracePath.parent_path(), ec);
            trace = _wfopen(settings.tracePath.c_str(), L"w");
        }
        if (trace == nullptr) {
            FUT_LOGW(Gfx, "interpolation: trace file %s could not be opened - no trace",
                     settings.tracePath.string().c_str());
            return;
        }
        FUT_LOGI(Gfx, "interpolation: trace -> %s", settings.tracePath.string().c_str());
        std::fputs("frame,tick,steps,alpha,flags,recording,interpolating,draws,interpolated", trace);
        for (std::size_t r = 0; r < kSnapReasonCount; ++r) {
            // "reason_" keeps the names unique (SnapReason::None is called "interpolated" like the column before).
            std::fprintf(trace, ",reason_%s", SnapReasonName(static_cast<SnapReason>(r)));
        }
        std::fputs(",cameras,camera_interpolated,camera_cut,camera_first_sight,skinned,substitute_skins,bones,bone_snaps,"
                   "frame_driven_changes,hierarchical,hierarchy_fallbacks,nodes,node_snaps,cost_us\n", trace);
    }
    if (trace == nullptr) {
        return;
    }
    std::fprintf(trace, "%u,%llu,%u,%.4f,%u,%d,%d,%llu,%llu", displayed, static_cast<unsigned long long>(fi.tick),
                 fi.stepsThisFrame, static_cast<double>(frameRecorded ? alpha : fi.alpha), fi.flags, frameRecorded ? 1 : 0,
                 frameInterpolated ? 1 : 0, static_cast<unsigned long long>(fc.draws),
                 static_cast<unsigned long long>(fc.interpolated));
    for (std::size_t r = 0; r < kSnapReasonCount; ++r) {
        std::fprintf(trace, ",%llu", static_cast<unsigned long long>(fc.reasons[r]));
    }
    std::fprintf(trace, ",%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.1f\n",
                 static_cast<unsigned long long>(fc.cameras),
                 static_cast<unsigned long long>(fc.cameraReasons[Index(SnapReason::None)]),
                 static_cast<unsigned long long>(fc.cameraReasons[Index(SnapReason::CameraCut)]),
                 static_cast<unsigned long long>(fc.cameraReasons[Index(SnapReason::FirstSight)]),
                 static_cast<unsigned long long>(fc.skinnedDraws), static_cast<unsigned long long>(fc.skinsInterpolated),
                 static_cast<unsigned long long>(fc.bonesInterpolated), static_cast<unsigned long long>(fc.boneSnaps),
                 static_cast<unsigned long long>(fc.frameDrivenChanges), static_cast<unsigned long long>(fc.hierarchical),
                 static_cast<unsigned long long>(fc.hierarchyFallbacks),
                 static_cast<unsigned long long>(fc.nodesEvaluated), static_cast<unsigned long long>(fc.nodeSnaps),
                 Microseconds(frameCost));
}

void History::Impl::WriteDrawTrace(std::uint32_t displayed) noexcept {
    if (details.empty()) {
        return;
    }
    if (!drawTraceTried) {
        drawTraceTried = true;
        if (!settings.drawTracePath.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(settings.drawTracePath.parent_path(), ec);
            drawTrace = _wfopen(settings.drawTracePath.c_str(), L"w");
        }
        if (drawTrace == nullptr) {
            FUT_LOGW(Gfx, "interpolation: draw trace file %s could not be opened - no draw trace",
                     settings.drawTracePath.string().c_str());
            return;
        }
        FUT_LOGI(Gfx, "interpolation: draw trace of the trace_frames frames -> %s",
                 settings.drawTracePath.string().c_str());
        std::fputs("frame,tick,alpha,draw,object,camera,parent,occurrence,reason,hierarchy,radius,prev_step,prev_angle,"
                   "step,angle,prev_x,prev_y,prev_z,cur_x,cur_y,cur_z,shown_x,shown_y,shown_z,lerp_x,lerp_y,lerp_z\n",
                   drawTrace);
    }
    if (drawTrace == nullptr) {
        return;
    }
    for (const DrawDetail& d : details) {
        std::fprintf(drawTrace,
                     "%u,%llu,%.4f,%u,0x%08X,0x%08X,0x%08X,%u,%s,%u,%.6g,%.6g,%.6g,%.6g,%.6g,%.7g,%.7g,%.7g,%.7g,%.7g,"
                     "%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g\n",
                     displayed, static_cast<unsigned long long>(fi.tick), static_cast<double>(alpha), d.index, d.object,
                     d.camera, d.parent, d.occurrence, SnapReasonName(static_cast<SnapReason>(d.reason)), d.hierarchy,
                     static_cast<double>(d.radius), static_cast<double>(d.prevStep), static_cast<double>(d.prevAngle),
                     static_cast<double>(d.step), static_cast<double>(d.angle), static_cast<double>(d.prev[0]),
                     static_cast<double>(d.prev[1]), static_cast<double>(d.prev[2]), static_cast<double>(d.cur[0]),
                     static_cast<double>(d.cur[1]), static_cast<double>(d.cur[2]), static_cast<double>(d.shown[0]),
                     static_cast<double>(d.shown[1]), static_cast<double>(d.shown[2]), static_cast<double>(d.lerp[0]),
                     static_cast<double>(d.lerp[1]), static_cast<double>(d.lerp[2]));
    }
    std::fflush(drawTrace);
}

// Debug = trace with the hierarchy: the recorded nodes of every traced tick (first frame of a generation) with their
// world transform and world bound center - input of the offline two-tick midpoint test (interp(S[G-2], S[G], 1/2)
// against the real S[G-1]) of hierarchical against world interpolation.
void History::Impl::WriteNodeTrace(std::uint32_t displayed) noexcept {
    if (!detail || !Hierarchy() || !frameRecorded || generationFrame != displayed || drawTrace == nullptr) {
        return;
    }
    if (nodeTrace == nullptr) {
        const std::filesystem::path path = settings.drawTracePath.parent_path() / "interp_nodes.csv";
        nodeTrace = _wfopen(path.c_str(), L"w");
        if (nodeTrace == nullptr) {
            return;
        }
        std::fputs("frame,tick,node,camera,parent,r00,r01,r02,r10,r11,r12,r20,r21,r22,tx,ty,tz,scale,cx,cy,cz,radius\n",
                   nodeTrace);
    }
    CurNodes().ForEach([&](const NodeEntry& n) {
        if (n.frame != displayed) {
            return;
        }
        const ni::NiBound bound = Read<ni::NiBound>(n.object + kAvWorldBound);
        const ni::NiTransform& w = n.world;
        std::fprintf(nodeTrace,
                     "%u,%llu,0x%08X,0x%08X,0x%08X,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,"
                     "%.9g,%.9g,%.9g,%.9g\n",
                     displayed, static_cast<unsigned long long>(fi.tick), n.object, n.camera, n.parent,
                     static_cast<double>(w.rotate[0][0]), static_cast<double>(w.rotate[0][1]),
                     static_cast<double>(w.rotate[0][2]), static_cast<double>(w.rotate[1][0]),
                     static_cast<double>(w.rotate[1][1]), static_cast<double>(w.rotate[1][2]),
                     static_cast<double>(w.rotate[2][0]), static_cast<double>(w.rotate[2][1]),
                     static_cast<double>(w.rotate[2][2]), static_cast<double>(w.translate[0]),
                     static_cast<double>(w.translate[1]), static_cast<double>(w.translate[2]),
                     static_cast<double>(w.scale), static_cast<double>(bound.center[0]),
                     static_cast<double>(bound.center[1]), static_cast<double>(bound.center[2]),
                     static_cast<double>(bound.radius));
    });
    std::fflush(nodeTrace);
}

void History::Impl::EndFrame(std::uint32_t displayed, bool logFrame) noexcept {
    if (settings.enabled) {
        // TSC calibration against QPC (the per-draw cost is measured with rdtsc).
        const std::int64_t qpc = Qpc();
        const std::uint64_t tsc = Ticks();
        if (qpcStart == 0) {
            qpcStart = qpc;
            tscStart = tsc;
        } else if (qpc - qpcStart > Qpf() / 10) {
            const double us = static_cast<double>(qpc - qpcStart) * 1e6 / static_cast<double>(Qpf());
            tscPerMicrosecond = static_cast<double>(tsc - tscStart) / us;
        }
        if (frameRecorded) {
            ++stats.recordedFrames;
            if (frameInterpolated) {
                ++stats.interpolatingFrames;
            }
            FrameCounters& t = stats.totals;
            t.draws += fc.draws;
            t.interpolated += fc.interpolated;
            t.cameras += fc.cameras;
            t.skinnedDraws += fc.skinnedDraws;
            t.skinsInterpolated += fc.skinsInterpolated;
            t.bonesInterpolated += fc.bonesInterpolated;
            t.boneSnaps += fc.boneSnaps;
            t.skinsWithoutHistory += fc.skinsWithoutHistory;
            t.frameDrivenChanges += fc.frameDrivenChanges;
            t.hierarchical += fc.hierarchical;
            t.hierarchyFallbacks += fc.hierarchyFallbacks;
            t.nodesEvaluated += fc.nodesEvaluated;
            t.nodeSnaps += fc.nodeSnaps;
            t.chainDecisions += fc.chainDecisions;
            t.recaptureDestroyed += fc.recaptureDestroyed;
            for (std::size_t r = 0; r < kSnapReasonCount; ++r) {
                t.reasons[r] += fc.reasons[r];
                t.cameraReasons[r] += fc.cameraReasons[r];
            }
            stats.costTicks += frameCost;
            stats.costMaxTicks = std::max(stats.costMaxTicks, frameCost);
            if (tscPerMicrosecond > 0.0 && !costHistogram.empty()) {
                const double us = Microseconds(frameCost);
                const auto bucket = static_cast<std::size_t>(std::min(us, static_cast<double>(kCostBuckets - 1)));
                ++costHistogram[bucket];
            }
            stats.tablePeak = std::max(stats.tablePeak, Cur().Count());
            stats.bonePoolPeak = std::max(stats.bonePoolPeak, static_cast<std::uint32_t>(Cur().bones.size()));
            stats.tableCapacity = std::max(stats.tableCapacity, Cur().Capacity());
            stats.nodePeak = std::max(stats.nodePeak, CurNodes().Count());
            if (logFrame) {
                char reasons[512];
                std::size_t used = 0;
                reasons[0] = 0;
                for (std::size_t r = 0; r < kSnapReasonCount; ++r) {
                    const auto reason = static_cast<SnapReason>(r);
                    if (!IsSnap(reason) || fc.reasons[r] == 0) {
                        continue;
                    }
                    const int w = std::snprintf(reasons + used, sizeof(reasons) - used, "%s%s %llu", used != 0 ? ", " : "",
                                                SnapReasonName(reason), static_cast<unsigned long long>(fc.reasons[r]));
                    if (w < 0 || static_cast<std::size_t>(w) >= sizeof(reasons) - used) {
                        break;
                    }
                    used += static_cast<std::size_t>(w);
                }
                std::uint64_t snapped = 0;
                for (std::size_t r = 0; r < kSnapReasonCount; ++r) {
                    snapped += IsSnap(static_cast<SnapReason>(r)) ? fc.reasons[r] : 0;
                }
                const char* cameraState = "none";
                for (std::size_t r = 0; r < kSnapReasonCount; ++r) {
                    if (fc.cameraReasons[r] != 0) {
                        cameraState = SnapReasonName(static_cast<SnapReason>(r));
                        break;
                    }
                }
                FUT_LOGI(Gfx,
                         "interp frame %u: tick %llu alpha %.3f%s, %llu draws (%llu interpolated [%llu along the "
                         "hierarchy, %llu nodes], %llu static, %llu alpha one, %llu snapped%s%s), %llu skinned (%llu "
                         "substitute skins, %llu bones), camera %s, %.1f us",
                         displayed, static_cast<unsigned long long>(fi.tick), static_cast<double>(alpha),
                         snapAll ? " snap-all" : "", static_cast<unsigned long long>(fc.draws),
                         static_cast<unsigned long long>(fc.interpolated),
                         static_cast<unsigned long long>(fc.hierarchical),
                         static_cast<unsigned long long>(fc.nodesEvaluated),
                         static_cast<unsigned long long>(fc.reasons[Index(SnapReason::Static)]),
                         static_cast<unsigned long long>(fc.reasons[Index(SnapReason::AlphaOne)]),
                         static_cast<unsigned long long>(snapped), used != 0 ? ": " : "", reasons,
                         static_cast<unsigned long long>(fc.skinnedDraws),
                         static_cast<unsigned long long>(fc.skinsInterpolated),
                         static_cast<unsigned long long>(fc.bonesInterpolated), cameraState, Microseconds(frameCost));
            }
        }
        WriteTrace(displayed);
        WriteDrawTrace(displayed);
        WriteNodeTrace(displayed);
        ++stats.frames;
    }
    details.clear();
    detail = false;
    substitutes.clear();
    fc = FrameCounters{};
    frameCost = 0;
    frameRecorded = false;
    frameInterpolated = false;
    frame = displayed + 1;
    latchedFrame = 0;
    Registry().frame.store(frame, std::memory_order_relaxed);
}

// ---- History (public) ---------------------------------------------------------------------------------------------

History::History(const Settings& settings, const SnapLimits& limits) noexcept {
    try {
        impl_ = std::make_unique<Impl>();
    } catch (...) {
        return;
    }
    Impl& d = *impl_;
    d.settings = settings;
    d.limits = limits;
    if (settings.enabled) {
        d.ready = d.tables[0].Allocate(kInitialTableCapacity) && d.tables[1].Allocate(kInitialTableCapacity) &&
                  Registry().Allocate();
        if (d.ready && settings.hierarchy) {
            d.nodesReady = d.nodeTables[0].Allocate(kInitialTableCapacity) &&
                           d.nodeTables[1].Allocate(kInitialTableCapacity);
            if (!d.nodesReady) {
                FUT_LOGW(Gfx, "interpolation: out of memory for the node tables - no hierarchical interpolation");
            }
            d.stats.nodeCapacity = d.nodesReady ? kInitialTableCapacity : 0u;
        }
        try {
            d.substitutes.reserve(32);
            d.costHistogram.assign(kCostBuckets, 0);
        } catch (...) {
        }
        if (!d.ready) {
            FUT_LOGW(Gfx, "interpolation: out of memory for the history tables - interpolation off");
        }
        d.stats.tableCapacity = kInitialTableCapacity;
    }
    Registry().Reset();
}

History::~History() {
    if (impl_ != nullptr && impl_->trace != nullptr) {
        std::fclose(impl_->trace);
    }
    if (impl_ != nullptr && impl_->drawTrace != nullptr) {
        std::fclose(impl_->drawTrace);
    }
    if (impl_ != nullptr && impl_->nodeTrace != nullptr) {
        std::fclose(impl_->nodeTrace);
    }
    Registry().tracking.store(false);
}

void History::SetSourceForTest(FrameInterpSource source) noexcept {
    if (impl_ != nullptr) {
        impl_->testSource = source;
    }
}

void History::SetGuestCallsForTest(bool allow) noexcept {
    if (impl_ != nullptr) {
        impl_->guestCalls = allow;
    }
}

ni::CameraInput History::Camera(ni::GuestPtr cameraObject, const ni::CameraInput& in) noexcept {
    if (impl_ == nullptr || !impl_->settings.enabled) {
        return in;
    }
    return impl_->Camera(cameraObject, in);
}

GeometryResult History::Geometry(ni::DrawKind kind, const ni::GeometryInput& in, ni::ExtractArena& arena) noexcept {
    GeometryResult result;
    result.input = in;
    if (impl_ == nullptr || !impl_->settings.enabled) {
        return result;
    }
    Impl& d = *impl_;
    if (d.latchedFrame != d.frame) {
        const std::uint64_t t0 = Ticks();
        d.Latch();
        d.frameCost += Ticks() - t0;
    }
    if (!d.recording || kind == ni::DrawKind::ScreenPolygon || kind == ni::DrawKind::ScreenTexture) {
        return result;
    }
    const std::uint64_t t0 = Ticks();
    d.Geometry(in, arena, &result);
    d.frameCost += Ticks() - t0;
    return result;
}

void History::BeginFrame(std::uint32_t frame, bool detail) noexcept {
    if (impl_ != nullptr && impl_->settings.enabled) {
        impl_->detail = detail && impl_->settings.debug == DebugMode::Trace;
        static_cast<void>(frame);  // EndFrame numbers the frames (frame = last displayed + 1)
    }
}

void History::EndFrame(std::uint32_t frame, bool logFrame) noexcept {
    if (impl_ != nullptr) {
        impl_->EndFrame(frame, logFrame);
    }
}

void History::OnObjectDestroyed(ni::GuestPtr object) noexcept {
    DestroyedRegistry& r = Registry();
    if (!r.tracking.load(std::memory_order_relaxed)) {
        return;
    }
    r.Record(object);
}

const Settings& History::GetSettings() const noexcept {
    static const Settings kOff = [] {
        Settings s;
        s.enabled = false;
        s.hierarchy = false;
        return s;
    }();
    return impl_ != nullptr ? impl_->settings : kOff;
}

bool History::Recording() const noexcept {
    return impl_ != nullptr && impl_->recording;
}

bool History::Interpolating() const noexcept {
    return impl_ != nullptr && impl_->interpolating;
}

const FrameInterp& History::Frame() const noexcept {
    static const FrameInterp kNone{};
    return impl_ != nullptr ? impl_->fi : kNone;
}

const Stats& History::GetStats() const noexcept {
    static const Stats kNone{};
    return impl_ != nullptr ? impl_->stats : kNone;
}

const FrameCounters& History::CurrentFrameCounters() const noexcept {
    static const FrameCounters kNone{};
    return impl_ != nullptr ? impl_->fc : kNone;
}

void History::LogConfiguration() const noexcept {
    if (impl_ == nullptr) {
        return;
    }
    const Impl& d = *impl_;
    if (!d.settings.enabled) {
        FUT_LOGI(Gfx, "interpolation: off ([render] interpolation = off) - frames above the tick rate repeat the last "
                 "tick state");
        return;
    }
    FUT_LOGI(Gfx, "interpolation: on ([render] interpolation_hierarchy = %s, interpolation_debug = %s; frame clock "
             "source %s; active while the clock produces intermediate frames, display latency 1/2 tick at 120 Hz)%s",
             d.settings.hierarchy ? (d.nodesReady ? "on" : "on, but no node tables") : "off",
             DebugModeName(d.settings.debug), FrameInterpSourceName(),
             d.ready ? "" : " - history tables unavailable, nothing is interpolated");
}

void History::LogStatistics() const noexcept {
    if (impl_ == nullptr || !impl_->settings.enabled) {
        return;
    }
    Impl& d = *impl_;
    const Stats& s = d.stats;
    const FrameCounters& t = s.totals;
    auto n = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
    auto r = [&](SnapReason reason) { return n(t.reasons[Index(reason)]); };
    auto c = [&](SnapReason reason) { return n(t.cameraReasons[Index(reason)]); };
    std::uint64_t snapped = 0;
    for (std::size_t i = 0; i < kSnapReasonCount; ++i) {
        snapped += IsSnap(static_cast<SnapReason>(i)) ? t.reasons[i] : 0;
    }
    // Cost percentiles over the recorded frames.
    double p99 = 0.0;
    std::uint64_t histogramTotal = 0;
    for (const std::uint32_t v : d.costHistogram) {
        histogramTotal += v;
    }
    if (histogramTotal != 0) {
        const std::uint64_t target = histogramTotal - histogramTotal / 100;
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < d.costHistogram.size(); ++i) {
            seen += d.costHistogram[i];
            if (seen >= target) {
                p99 = static_cast<double>(i + 1);
                break;
            }
        }
    }
    const double average = s.recordedFrames != 0 ? d.Microseconds(s.costTicks) / static_cast<double>(s.recordedFrames)
                                                 : 0.0;
    FUT_LOGI(Gfx,
             "interpolation: %llu frames (%llu recorded, %llu with alpha < 1); draws %llu: interpolated %llu, static %llu, "
             "alpha one %llu, first sight %llu, snapped %llu (translation %llu, rotation %llu, scale %llu, identity %llu, "
             "destroyed %llu, generation gap %llu, frame-driven %llu, snap-all %llu, not orthonormal %llu, mirror %llu), "
             "not recorded %llu; skins without bone history %llu; recaptures after a destruction %llu",
             n(s.frames), n(s.recordedFrames), n(s.interpolatingFrames), n(t.draws), n(t.interpolated),
             r(SnapReason::Static), r(SnapReason::AlphaOne), r(SnapReason::FirstSight),
             n(snapped - t.reasons[Index(SnapReason::FirstSight)]), r(SnapReason::Translation), r(SnapReason::Rotation),
             r(SnapReason::Scale), r(SnapReason::Identity), r(SnapReason::Destroyed), r(SnapReason::GenerationGap),
             r(SnapReason::FrameDriven), r(SnapReason::SnapAll), r(SnapReason::NotOrthonormal), r(SnapReason::Mirror),
             r(SnapReason::Disabled), n(t.skinsWithoutHistory), n(t.recaptureDestroyed));
    FUT_LOGI(Gfx,
             "interpolation: cameras %llu (interpolated %llu, static %llu, alpha one %llu, first sight %llu, cuts %llu, "
             "generation gap %llu, frame-driven %llu, snap-all %llu, destroyed %llu); skinned draws %llu (substitute "
             "skins %llu, bones interpolated %llu, bone snaps %llu); frame-driven changes %llu",
             n(t.cameras), c(SnapReason::None), c(SnapReason::Static), c(SnapReason::AlphaOne), c(SnapReason::FirstSight),
             c(SnapReason::CameraCut), c(SnapReason::GenerationGap), c(SnapReason::FrameDriven), c(SnapReason::SnapAll),
             c(SnapReason::Destroyed), n(t.skinnedDraws), n(t.skinsInterpolated), n(t.bonesInterpolated), n(t.boneSnaps),
             n(t.frameDrivenChanges));
    FUT_LOGI(Gfx,
             "interpolation: %llu generations (%llu gaps, %llu catch-up pairs interpolated); table peak %u entries "
             "(capacity %u), bone pool peak %u records "
             "(%u KiB); %llu destructions recorded; overflows: table %llu, occurrences %llu, arena %llu; skin self-test "
             "%u run, %u failed (max relative difference %.2e); cost average %.1f us, p99 %.0f us, max %.1f us per "
             "recorded frame",
             n(s.generations), n(s.generationGaps), n(s.pairSpans), s.tablePeak, s.tableCapacity, s.bonePoolPeak,
             static_cast<unsigned>(s.bonePoolPeak * sizeof(BoneRecord) / 1024), n(Registry().recorded.load()),
             n(s.tableOverflows), n(s.occurrenceOverflows), n(s.arenaFailures), s.skinSelfTests, s.skinSelfTestFailures,
             static_cast<double>(s.skinSelfTestMaxError), average, p99, d.Microseconds(s.costMaxTicks));
    if (d.settings.hierarchy) {
        FUT_LOGI(Gfx,
                 "interpolation: hierarchy: %llu draws along the parent chain, %llu without node history (world); %llu "
                 "node evaluations, %llu node snaps; %llu jump verdicts of the chain against the leaf rule; node table "
                 "peak %u entries (capacity %u), %llu nodes not recorded, readable-page cache reset %llu times",
                 n(t.hierarchical), n(t.hierarchyFallbacks), n(t.nodesEvaluated), n(t.nodeSnaps), n(t.chainDecisions),
                 s.nodePeak, s.nodeCapacity, n(s.nodeOverflows), n(s.readableCacheResets));
    }
}

}  // namespace gfx::interp

FUT_PROBE(gfx::interp::kNiAVObjectDestructor, &gfx::interp::NiAVObjectDestroyProbe, Thiscall,
          "probe:NiAVObject::Destroy", "6A FF 68 93 A4 17 00", 7);
