// Unit tests of gfx/interp_history.h (M3d renderer-level interpolation, analysis/loader/m3d_interpolation.md 3, 5, 8):
// history bookkeeping (generations, keys, occurrences, signature, destruction, frame-driven keys, gaps), rigid and
// camera interpolation at 120 Hz "phase B", skinning through the substitute NiSkinInstance, the pass-through at
// 60 fps, the table hash, the debug tint and the FUT_INTERP_STUB developer source. Guest objects live in
// gfx::AllocGuestPages memory; no guest code runs (SetGuestCallsForTest(false)).
#include "doctest.h"

#include "core/config.h"
#include "core/guest.h"
#include "gfx/guest_memory.h"
#include "gfx/interp_history.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>

namespace {

using namespace gfx;
using namespace gfx::interp;
using ni::GuestPtr;

constexpr float kPi = 3.14159265358979f;

class GuestHeap {
public:
    explicit GuestHeap(std::size_t size = 1u << 20) : size_(size) {
        base_ = static_cast<std::uint8_t*>(AllocGuestPages(size));
        REQUIRE(base_ != nullptr);
    }
    ~GuestHeap() { FreeGuestPages(base_); }
    GuestHeap(const GuestHeap&) = delete;
    GuestHeap& operator=(const GuestHeap&) = delete;
    GuestPtr Alloc(std::size_t bytes) {
        used_ = (used_ + 15) & ~std::size_t{15};
        REQUIRE(used_ + bytes <= size_);
        std::uint8_t* p = base_ + used_;
        std::memset(p, 0, bytes);
        used_ += bytes;
        return static_cast<GuestPtr>(reinterpret_cast<std::uintptr_t>(p));
    }

private:
    std::uint8_t* base_ = nullptr;
    std::size_t size_ = 0;
    std::size_t used_ = 0;
};

template <class T>
T* P(GuestPtr address) {
    return reinterpret_cast<T*>(static_cast<std::uintptr_t>(address));
}

ni::NiTransform Rotation(float x, float y, float z, float angle, float tx = 0, float ty = 0, float tz = 0,
                         float scale = 1.0f) {
    const float n = std::sqrt(x * x + y * y + z * z);
    x /= n;
    y /= n;
    z /= n;
    const float c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;
    ni::NiTransform r{};
    r.rotate[0][0] = t * x * x + c;
    r.rotate[0][1] = t * x * y - s * z;
    r.rotate[0][2] = t * x * z + s * y;
    r.rotate[1][0] = t * x * y + s * z;
    r.rotate[1][1] = t * y * y + c;
    r.rotate[1][2] = t * y * z - s * x;
    r.rotate[2][0] = t * x * z - s * y;
    r.rotate[2][1] = t * y * z + s * x;
    r.rotate[2][2] = t * z * z + c;
    r.translate[0] = tx;
    r.translate[1] = ty;
    r.translate[2] = tz;
    r.scale = scale;
    return r;
}

// The frame values the History under test reads.
FrameInterp g_frame;
FrameInterp TestSource() noexcept {
    return g_frame;
}

constexpr GuestPtr kTriShapeVtable = 0x002A7CE0;

// An NiTriShape-like object: vtable, world bound radius 1 at +0x20, world transform at +0x70, data +0xC8.
struct Shape {
    GuestPtr object = 0;
    GuestPtr data = 0;
    GuestPtr propertyState = 0;
    GuestPtr skin = 0;
    ni::NiTransform& World() const { return *P<ni::NiTransform>(object + 0x70); }
    ni::GeometryInput Input() const {
        ni::GeometryInput in;
        in.data = data;
        in.skinInstance = skin;
        in.transform = object + 0x70;
        in.bound = object + 0x20;
        in.propertyState = propertyState;
        return in;
    }
};

Shape MakeShape(GuestHeap& heap, float radius = 1.0f) {
    Shape s;
    s.object = heap.Alloc(0x100);
    s.data = heap.Alloc(0x50);
    s.propertyState = heap.Alloc(0x40);
    *P<GuestPtr>(s.object) = kTriShapeVtable;
    *P<float>(s.object + 0x2C) = radius;
    s.World() = IdentityTransform();
    *P<GuestPtr>(s.object + 0xC8) = s.data;
    *P<GuestPtr>(s.object + 0xC0) = s.propertyState;
    return s;
}

// Drives a History frame by frame like GpuRenderer does.
struct Driver {
    History history;
    ni::ExtractArena arena;
    std::uint32_t frame = 1;

    explicit Driver(const Settings& settings = {}) : history(settings) {
        history.SetSourceForTest(&TestSource);
        history.SetGuestCallsForTest(false);
    }
    // The arena is reset when the next frame begins, so results stay readable after End().
    void Begin(std::uint64_t tick, float alpha, std::uint32_t flags = 0, bool enabled = true) {
        arena.Reset();
        g_frame = FrameInterp{};
        g_frame.tick = tick;
        g_frame.alpha = alpha;
        g_frame.flags = flags;
        g_frame.enabled = enabled;
    }
    GeometryResult Draw(const Shape& s) { return history.Geometry(ni::DrawKind::Shape, s.Input(), arena); }
    void End() { history.EndFrame(frame++, false); }
    // One frame with one draw.
    GeometryResult Frame(const Shape& s, std::uint64_t tick, float alpha, std::uint32_t flags = 0) {
        Begin(tick, alpha, flags);
        const GeometryResult r = Draw(s);
        End();
        return r;
    }
};

// The transform a draw result points at (interpolated arena copy or the guest's own).
ni::NiTransform Shown(const GeometryResult& r) {
    return *P<ni::NiTransform>(r.input.transform);
}

}  // namespace

TEST_CASE("interp history: 60 fps and interpolation off pass every camera and draw through unchanged") {
    GuestHeap heap;
    Shape s = MakeShape(heap);
    {
        Driver d;
        for (std::uint64_t tick = 1; tick <= 6; ++tick) {
            s.World().translate[0] = static_cast<float>(tick);
            d.Begin(tick, 1.0f, 0, /*enabled=*/false);
            const GeometryResult r = d.Draw(s);
            CHECK(r.input.transform == s.object + 0x70);
            CHECK(r.input.skinInstance == 0);
            CHECK(r.reason == SnapReason::Disabled);
            CHECK_FALSE(d.history.Recording());
            d.End();
        }
        CHECK(d.history.GetStats().recordedFrames == 0);
        CHECK(d.history.GetStats().totals.draws == 0);
    }
    {
        Settings off;
        off.enabled = false;
        Driver d(off);
        for (std::uint64_t tick = 1; tick <= 6; ++tick) {
            s.World().translate[0] = static_cast<float>(tick) * 3.0f;
            d.Begin(tick, tick % 2 == 0 ? 0.5f : 1.0f);
            CHECK(d.Draw(s).input.transform == s.object + 0x70);
            d.End();
        }
        CHECK(d.history.GetStats().recordedFrames == 0);
    }
}

TEST_CASE("interp history: 120 Hz phase B - tick frames show the middle, zero-step frames the tick state") {
    GuestHeap heap;
    Shape s = MakeShape(heap);
    Driver d;
    // Tick 1: first sight (the guest data as is), then the zero-step frame.
    GeometryResult r = d.Frame(s, 1, 0.5f);
    CHECK(r.reason == SnapReason::FirstSight);
    CHECK(r.input.transform == s.object + 0x70);
    r = d.Frame(s, 1, 1.0f);
    CHECK(r.reason == SnapReason::AlphaOne);
    CHECK(r.input.transform == s.object + 0x70);
    // Tick 2 moved the object by 0.2 along x and turned it 10 degrees: the tick frame shows half of it.
    s.World() = Rotation(0, 0, 1, 10.0f * kPi / 180.0f, 0.2f, 0.0f, 0.0f);
    d.Begin(2, 0.5f);
    r = d.Draw(s);
    CHECK(r.reason == SnapReason::None);
    CHECK(r.input.transform != s.object + 0x70);
    const ni::NiTransform mid = Shown(r);
    CHECK(mid.translate[0] == doctest::Approx(0.1f));
    CHECK(AngleDeg(mid.rotate, IdentityTransform().rotate) == doctest::Approx(5.0f).epsilon(1e-3));
    CHECK(d.history.Interpolating());
    d.End();
    // The zero-step frame shows tick 2 exactly (guest pointer).
    r = d.Frame(s, 2, 1.0f);
    CHECK(r.reason == SnapReason::AlphaOne);
    CHECK(r.input.transform == s.object + 0x70);
    // Tick 3 without motion: static, guest pointer (bit-identical).
    r = d.Frame(s, 3, 0.5f);
    CHECK(r.reason == SnapReason::Static);
    CHECK(r.input.transform == s.object + 0x70);
    // Alpha values close to 1 count as 1 (6.1: the clock's 0.999).
    s.World().translate[0] = 0.4f;
    r = d.Frame(s, 4, 0.9995f);
    CHECK(r.reason == SnapReason::AlphaOne);
    CHECK(r.input.transform == s.object + 0x70);
    const Stats& st = d.history.GetStats();
    CHECK(st.recordedFrames == 6);
    CHECK(st.interpolatingFrames == 3);
    CHECK(st.totals.interpolated == 1);
    CHECK(st.generations == 4);
}

TEST_CASE("interp history: snaps - jump, generation gap, identity, destruction, snap-all, inside the step loop") {
    GuestHeap heap;
    Shape s = MakeShape(heap, 0.19f);
    Driver d;
    d.Frame(s, 1, 0.5f);
    d.Frame(s, 1, 1.0f);
    s.World().translate[0] = 0.095f;
    CHECK(d.Frame(s, 2, 0.5f).reason == SnapReason::None);
    d.Frame(s, 2, 1.0f);
    // Keyframe loop wrap: 4.6 units after 0.095 at radius 0.19.
    s.World().translate[0] = 4.7f;
    GeometryResult r = d.Frame(s, 3, 0.5f);
    CHECK(r.reason == SnapReason::Translation);
    CHECK(r.input.transform == s.object + 0x70);
    d.Frame(s, 3, 1.0f);
    // Continuous motion after the wrap interpolates again.
    s.World().translate[0] = 4.8f;
    CHECK(d.Frame(s, 4, 0.5f).reason == SnapReason::None);
    // Catch-up of two ticks: no state of tick G-1 -> generation gap.
    s.World().translate[0] = 5.0f;
    CHECK(d.Frame(s, 6, 0.5f).reason == SnapReason::GenerationGap);
    s.World().translate[0] = 5.1f;
    CHECK(d.Frame(s, 7, 0.5f).reason == SnapReason::None);
    // Tick 8 drew nothing (the History never saw it): in tick 9 the last capture is two ticks old -> a snap.
    d.Begin(8, 0.5f);
    d.End();
    s.World().translate[0] = 5.2f;
    CHECK(d.Frame(s, 9, 0.5f).reason == SnapReason::GenerationGap);
    // Not drawn in tick 10 while another object was (culled): first sight in tick 11.
    Shape other = MakeShape(heap);
    d.Frame(other, 10, 0.5f);
    s.World().translate[0] = 5.25f;
    CHECK(d.Frame(s, 11, 0.5f).reason == SnapReason::FirstSight);
    s.World().translate[0] = 5.27f;
    CHECK(d.Frame(s, 12, 0.5f).reason == SnapReason::None);
    // Another object at the same address: other data pointer -> identity.
    s.World().translate[0] = 5.3f;
    s.data = heap.Alloc(0x50);
    CHECK(d.Frame(s, 13, 0.5f).reason == SnapReason::Identity);
    s.World().translate[0] = 5.4f;
    CHECK(d.Frame(s, 14, 0.5f).reason == SnapReason::None);
    // The object was destroyed (and its address reused) since the last capture.
    History::OnObjectDestroyed(s.object);
    s.World().translate[0] = 5.5f;
    CHECK(d.Frame(s, 15, 0.5f).reason == SnapReason::Destroyed);
    s.World().translate[0] = 5.6f;
    CHECK(d.Frame(s, 16, 0.5f).reason == SnapReason::None);  // the destruction is older than the capture now
    // snapAll of the clock (context switch): the tick state, history continues.
    s.World().translate[0] = 5.7f;
    r = d.Frame(s, 17, 1.0f, kFrameInterpSnapAll);
    CHECK(r.reason == SnapReason::SnapAll);
    CHECK(r.input.transform == s.object + 0x70);
    s.World().translate[0] = 5.8f;
    CHECK(d.Frame(s, 18, 0.5f).reason == SnapReason::None);
    // Draws inside the step loop are passed through and not recorded.
    s.World().translate[0] = 99.0f;
    r = d.Frame(s, 19, 0.5f, kFrameInterpInsideStep);
    CHECK(r.reason == SnapReason::Disabled);
    CHECK(r.input.transform == s.object + 0x70);
    s.World().translate[0] = 5.9f;
    // Tick 19 has no capture: the state of tick 18 is two generations back -> no history.
    r = d.Frame(s, 20, 0.5f);
    CHECK(IsSnap(r.reason));
    const Stats& st = d.history.GetStats();
    CHECK(st.totals.reasons[static_cast<std::size_t>(SnapReason::Translation)] == 1);
    CHECK(st.totals.reasons[static_cast<std::size_t>(SnapReason::Destroyed)] == 1);
    CHECK(st.generationGaps >= 2);
}

TEST_CASE("interp history: a key that changes within one tick generation is frame-driven") {
    GuestHeap heap;
    Shape s = MakeShape(heap);
    Driver d;
    d.Frame(s, 1, 0.5f);
    s.World().translate[0] = 0.01f;  // changed in the zero-step frame: animated per frame (g_frameDelta)
    d.Frame(s, 1, 1.0f);
    CHECK(d.history.GetStats().totals.frameDrivenChanges == 1);
    s.World().translate[0] = 0.02f;
    GeometryResult r = d.Frame(s, 2, 0.5f);
    CHECK(r.reason == SnapReason::FrameDriven);
    CHECK(r.input.transform == s.object + 0x70);
    // The hold lasts frameDrivenHoldFrames frames after the last change, then the key interpolates again.
    // Afterwards the key moves only with the ticks (unchanged in the zero-step frames).
    std::uint64_t tick = 2;
    const std::uint32_t until = 2 + SnapLimits{}.frameDrivenHoldFrames;  // the change was seen in frame 2
    while (d.frame + 1 <= until) {  // the tick frame after the next zero-step frame is still held
        d.Frame(s, tick, 1.0f);
        ++tick;
        s.World().translate[0] += 0.01f;
        CHECK(d.Frame(s, tick, 0.5f).reason == SnapReason::FrameDriven);
    }
    d.Frame(s, tick, 1.0f);
    ++tick;
    s.World().translate[0] += 0.01f;
    CHECK(d.Frame(s, tick, 0.5f).reason == SnapReason::None);
}

TEST_CASE("interp history: occurrences of one object in a frame have their own history") {
    GuestHeap heap;
    Shape s = MakeShape(heap);
    Driver d;
    auto twice = [&](std::uint64_t tick, float alpha, float x0, float x1, GeometryResult* r0, GeometryResult* r1) {
        d.Begin(tick, alpha);
        s.World().translate[0] = x0;
        *r0 = d.Draw(s);
        s.World().translate[0] = x1;
        *r1 = d.Draw(s);
        d.End();
    };
    GeometryResult a, b;
    twice(1, 0.5f, 0.0f, 10.0f, &a, &b);
    twice(1, 1.0f, 0.0f, 10.0f, &a, &b);
    CHECK(d.history.GetStats().totals.frameDrivenChanges == 0);  // occurrence 0 and 1 are separate keys
    twice(2, 0.5f, 1.0f, 11.0f, &a, &b);
    CHECK(a.reason == SnapReason::None);
    CHECK(b.reason == SnapReason::None);
    CHECK(Shown(a).translate[0] == doctest::Approx(0.5f));
    CHECK(Shown(b).translate[0] == doctest::Approx(10.5f));
}

TEST_CASE("interp history: camera interpolation, cuts and frame-driven cameras") {
    GuestHeap heap;
    const GuestPtr cam = heap.Alloc(0x260);
    auto* loc = P<float>(cam + ni::kNiCameraWorldLocation);
    auto* dir = P<float>(cam + ni::kNiCameraWorldDirection);
    auto* up = P<float>(cam + ni::kNiCameraWorldUp);
    auto* right = P<float>(cam + ni::kNiCameraWorldRight);
    auto* frustum = P<ni::NiFrustum>(cam + ni::kNiCameraFrustum);
    auto* port = P<ni::NiRectF>(cam + ni::kNiCameraPort);
    dir[0] = 1.0f;
    up[1] = 1.0f;
    right[2] = 1.0f;
    *frustum = ni::NiFrustum{-0.7714f, 0.7714f, 0.5786f, -0.5786f, 0.1f, 1000.0f};
    *port = ni::NiRectF{0.0f, 1.0f, 1.0f, 0.0f};
    ni::CameraInput in;
    in.location = loc;
    in.direction = dir;
    in.up = up;
    in.right = right;
    in.frustum = frustum;
    in.port = port;
    Driver d;
    auto frame = [&](std::uint64_t tick, float alpha) {
        d.Begin(tick, alpha);
        const ni::CameraInput out = d.history.Camera(cam, in);
        d.End();
        return out;
    };
    ni::CameraInput out = frame(1, 0.5f);
    CHECK(out.location == loc);  // first sight
    frame(1, 1.0f);
    // Starts moving with 0.4 units per tick (a start above 0.5 units would count as a cut: 3.7).
    loc[0] = 0.4f;
    out = frame(2, 0.5f);
    CHECK(out.location != loc);
    CHECK(out.location[0] == doctest::Approx(0.2f));
    CHECK(out.direction[0] == doctest::Approx(1.0f));
    CHECK(out.frustum->nearPlane == 0.1f);
    CHECK(out.port->right == 1.0f);
    out = frame(2, 1.0f);
    CHECK(out.location == loc);
    // A steady pan continues to interpolate (also faster than 0.5 units once moving); a jump of 20 units is a cut.
    loc[0] = 1.2f;
    out = frame(3, 0.5f);
    CHECK(out.location != loc);
    CHECK(out.location[0] == doctest::Approx(0.8f));
    frame(3, 1.0f);
    loc[0] = 22.0f;
    CHECK(frame(4, 0.5f).location == loc);
    frame(4, 1.0f);
    // Zoom by a factor 1.5: cut.
    frustum->left *= 1.5f;
    frustum->right *= 1.5f;
    CHECK(frame(5, 0.5f).location == loc);
    frame(5, 1.0f);
    // Unchanged camera: static (guest pointers).
    CHECK(frame(6, 0.5f).location == loc);
    // A camera that moves between the frames of one tick is frame-driven.
    loc[1] = 0.5f;
    frame(6, 1.0f);
    loc[1] = 1.0f;
    CHECK(frame(7, 0.5f).location == loc);
    const Stats& st = d.history.GetStats();
    CHECK(st.totals.cameraReasons[static_cast<std::size_t>(SnapReason::None)] == 2);
    CHECK(st.totals.cameraReasons[static_cast<std::size_t>(SnapReason::CameraCut)] == 2);
    CHECK(st.totals.cameraReasons[static_cast<std::size_t>(SnapReason::Static)] == 1);
    CHECK(st.totals.cameraReasons[static_cast<std::size_t>(SnapReason::FrameDriven)] == 1);
    // The camera keys the following draws: another camera means another history (first sight).
    GuestHeap objects;
    Shape s = MakeShape(objects);
    const GuestPtr cam2 = heap.Alloc(0x260);
    std::memcpy(P<void>(cam2), P<void>(cam), 0x260);
    ni::CameraInput in2 = in;
    in2.location = P<float>(cam2 + ni::kNiCameraWorldLocation);
    d.Begin(8, 0.5f);
    d.history.Camera(cam, in);
    d.Draw(s);
    d.End();
    d.Begin(8, 1.0f);
    d.history.Camera(cam, in);
    d.Draw(s);
    d.End();
    s.World().translate[0] = 0.1f;
    d.Begin(9, 0.5f);
    d.history.Camera(cam2, in2);
    CHECK(d.Draw(s).reason == SnapReason::FirstSight);
    d.End();
}

TEST_CASE("interp history: skinned draws interpolate relative bone poses through a substitute NiSkinInstance") {
    GuestHeap heap;
    // NiSkinData with 2 bones (+0x50), root and bones (world at +0x70), the bone list and the NiSkinInstance.
    const GuestPtr skinData = heap.Alloc(0x60);
    *P<std::uint32_t>(skinData + 0x50) = 2;
    const GuestPtr root = heap.Alloc(0xC0);
    const GuestPtr bone0 = heap.Alloc(0xC0);
    const GuestPtr bone1 = heap.Alloc(0xC0);
    const GuestPtr list = heap.Alloc(8);
    P<GuestPtr>(list)[0] = bone0;
    P<GuestPtr>(list)[1] = bone1;
    const GuestPtr skin = heap.Alloc(0x18);
    *P<GuestPtr>(skin) = 0x002A0000;  // a vtable (copied, never read by FUN_0014F710)
    *P<GuestPtr>(skin + 0x08) = skinData;
    *P<GuestPtr>(skin + 0x0C) = root;
    *P<GuestPtr>(skin + 0x10) = list;
    auto world = [](GuestPtr object) -> ni::NiTransform& { return *P<ni::NiTransform>(object + 0x70); };
    world(root) = Rotation(0, 1, 0, 0.3f, 10, 0, 5, 1.0f);
    world(bone0) = Compose(world(root), Rotation(1, 0, 0, 0.0f, 0, 1, 0));
    world(bone1) = Compose(world(root), Rotation(0, 0, 1, 0.0f, 0, 2, 0));
    Shape a = MakeShape(heap);
    a.skin = skin;
    Shape b = MakeShape(heap);  // a second shape of the same skin instance
    b.skin = skin;
    Driver d;
    auto frame = [&](std::uint64_t tick, float alpha, GeometryResult* ra, GeometryResult* rb) {
        d.Begin(tick, alpha);
        *ra = d.Draw(a);
        *rb = d.Draw(b);
        d.End();
    };
    GeometryResult ra, rb;
    frame(1, 0.5f, &ra, &rb);
    CHECK(ra.input.skinInstance == skin);
    frame(1, 1.0f, &ra, &rb);
    // Bone 1 turns 20 degrees about z relative to the root: the tick frame shows 10 degrees.
    const ni::NiTransform bone1Local = Rotation(0, 0, 1, 20.0f * kPi / 180.0f, 0, 2, 0);
    world(bone1) = Compose(world(root), bone1Local);
    frame(2, 0.5f, &ra, &rb);
    CHECK(ra.reason == SnapReason::None);
    REQUIRE(ra.input.skinInstance != skin);
    CHECK(rb.input.skinInstance == ra.input.skinInstance);  // one substitute per skin instance and frame
    CHECK(ra.input.transform == a.object + 0x70);           // the shape itself did not move
    const GuestPtr sub = ra.input.skinInstance;
    CHECK(*P<GuestPtr>(sub) == 0x002A0000u);
    CHECK(*P<GuestPtr>(sub + 0x08) == skinData);
    const GuestPtr subRoot = *P<GuestPtr>(sub + 0x0C);
    const GuestPtr subList = *P<GuestPtr>(sub + 0x10);
    CHECK(BitwiseEqual(world(subRoot), IdentityTransform()));
    // Bone 0 keeps root^-1 * bone (= its local pose), bone 1 is half way.
    const ni::NiTransform rel0 = world(P<GuestPtr>(subList)[0]);
    const ni::NiTransform rel1 = world(P<GuestPtr>(subList)[1]);
    CHECK(rel0.translate[1] == doctest::Approx(1.0f).epsilon(1e-5));
    CHECK(AngleDeg(rel0.rotate, IdentityTransform().rotate) < 1e-3f);
    CHECK(rel1.translate[1] == doctest::Approx(2.0f).epsilon(1e-5));
    CHECK(AngleDeg(rel1.rotate, IdentityTransform().rotate) == doctest::Approx(10.0f).epsilon(1e-3));
    // Substitute objects are guest-visible arena memory below 0x80000000.
    CHECK(sub < 0x80000000u);
    CHECK(d.history.GetStats().totals.skinsInterpolated == 2);  // both draws (totals updated by EndFrame)
    frame(2, 1.0f, &ra, &rb);
    CHECK(ra.input.skinInstance == skin);
    CHECK(d.history.GetStats().totals.skinsInterpolated == 2);
    // The whole character moves (root and bones together): relative poses unchanged -> the real instance; the
    // shape's own transform interpolates.
    const ni::NiTransform moved = Rotation(0, 1, 0, 0.5f, 11, 0, 5, 1.0f);
    world(root) = moved;
    world(bone0) = Compose(moved, Rotation(1, 0, 0, 0.0f, 0, 1, 0));
    world(bone1) = Compose(moved, bone1Local);
    a.World().translate[0] = 1.0f;
    frame(3, 0.5f, &ra, &rb);
    CHECK(ra.reason == SnapReason::None);
    CHECK(ra.input.transform != a.object + 0x70);
    // Relative poses equal up to rounding: either the real instance (bitwise equal) or a substitute that holds
    // the same poses.
    if (ra.input.skinInstance != skin) {
        const GuestPtr l = *P<GuestPtr>(ra.input.skinInstance + 0x10);
        CHECK(AngleDeg(world(P<GuestPtr>(l)[1]).rotate, bone1Local.rotate) < 1e-2f);
    }
}

TEST_CASE("interp history: the table hash spreads 16-byte aligned guest pointers (5.2)") {
    for (const GuestPtr stride : {0x10u, 0x40u, 0xD0u, 0x100u, 0x1000u}) {
        std::vector<GuestPtr> objects;
        for (GuestPtr i = 0; i < 2048; ++i) {
            objects.push_back(0x03400000u + i * stride);
        }
        CAPTURE(stride);
        const std::uint32_t longest = LongestProbeForTest(objects.data(), 2048, 0x0348E9C0u, 4096);
        CHECK(longest >= 1);
        CHECK(longest <= 16);
    }
}

TEST_CASE("interp history: the tables grow beyond the initial capacity without losing history") {
    GuestHeap heap(4u << 20);
    std::vector<Shape> shapes;
    for (int i = 0; i < 3000; ++i) {
        shapes.push_back(MakeShape(heap));
    }
    Driver d;
    for (std::uint64_t tick = 1; tick <= 2; ++tick) {
        for (const float alpha : {0.5f, 1.0f}) {
            d.Begin(tick, alpha);
            for (Shape& s : shapes) {
                s.World().translate[0] = static_cast<float>(tick) * 0.1f;
                d.Draw(s);
            }
            d.End();
        }
    }
    const Stats& st = d.history.GetStats();
    CHECK(st.tableOverflows == 0);
    CHECK(st.tablePeak == 3000);
    CHECK(st.tableCapacity >= 8192);
    CHECK(st.totals.interpolated == 3000);  // every key found its tick-1 state in tick 2
}

TEST_CASE("interp history: debug tint colors and the extra texture stage") {
    float rgba[4];
    CHECK_FALSE(TintColor(SnapReason::None, rgba));
    CHECK_FALSE(TintColor(SnapReason::Static, rgba));
    CHECK(TintColor(SnapReason::Translation, rgba));
    CHECK(rgba[0] == 1.0f);
    CHECK(TintColor(SnapReason::FirstSight, rgba));
    CHECK(rgba[1] == 1.0f);
    ni::DrawItem item;
    item.state.stages[0].colorOp = FF_TOP_MODULATE;
    item.state.stages[0].colorArg1 = FF_TA_TEXTURE;
    item.state.stages[0].colorArg2 = FF_TA_DIFFUSE;
    item.state.stageCount = 1;
    CHECK(ApplyTint(&item, rgba));
    CHECK(item.state.stageCount == 2);
    CHECK(item.state.stages[1].colorOp == FF_TOP_MODULATE);
    CHECK(item.state.stages[1].colorArg1 == FF_TA_CURRENT);
    CHECK(item.state.stages[1].colorArg2 == FF_TA_TFACTOR);
    CHECK(item.state.stages[2].colorOp == FF_TOP_DISABLE);
    CHECK(item.state.textureFactor.y == 1.0f);
    // A material that reads TFACTOR itself keeps its state.
    ni::DrawItem factor;
    factor.state.stages[0].colorOp = FF_TOP_MODULATE;
    factor.state.stages[0].colorArg1 = FF_TA_TFACTOR;
    CHECK_FALSE(ApplyTint(&factor, rgba));
    CHECK(factor.state.stages[1].colorOp == FF_TOP_DISABLE);
    // Four active stages: no room.
    ni::DrawItem full;
    for (std::uint32_t i = 0; i < ni::kMaxTextureStages; ++i) {
        full.state.stages[i].colorOp = FF_TOP_SELECTARG1;
    }
    CHECK_FALSE(ApplyTint(&full, rgba));
}

TEST_CASE("interp history: tint mode marks snapped draws in the tick frame and the frame after it") {
    GuestHeap heap;
    Shape s = MakeShape(heap, 0.1f);
    Settings tint;
    tint.debug = DebugMode::Tint;
    Driver d(tint);
    CHECK(d.Frame(s, 1, 0.5f).tint == SnapReason::FirstSight);
    CHECK(d.Frame(s, 1, 1.0f).tint == SnapReason::FirstSight);
    s.World().translate[0] = 0.01f;
    CHECK(d.Frame(s, 2, 0.5f).tint == SnapReason::None);
    CHECK(d.Frame(s, 2, 1.0f).tint == SnapReason::None);
    s.World().translate[0] = 50.0f;
    CHECK(d.Frame(s, 3, 0.5f).tint == SnapReason::Translation);
    CHECK(d.Frame(s, 3, 1.0f).tint == SnapReason::Translation);
}

TEST_CASE("interp history: the FUT_INTERP_STUB developer source follows the fixed steps of each frame") {
    // Phase B at 120 Hz with the lockstep clock: steps 1, 0, 1, 0, ... -> alpha 1/2, 1, 1/2, 1; the displayed time
    // G - 1 + alpha rises by exactly 1/2 per frame.
    FrameInterp f;
    f.tick = 100;
    const std::uint64_t steps[] = {101, 101, 102, 102, 103, 103};
    double shown = static_cast<double>(f.tick);
    for (const std::uint64_t s : steps) {
        f = StepsFrameInterp(f, s);
        CHECK(f.enabled);
        CHECK(f.flags == 0u);
        const double now = static_cast<double>(f.tick) - 1.0 + static_cast<double>(f.alpha);
        CHECK(now - shown == doctest::Approx(0.5));
        shown = now;
    }
    // A catch-up of two steps: snap-all; backwards: clock reset.
    f = StepsFrameInterp(f, 105);
    CHECK(f.flags == kFrameInterpSnapAll);
    CHECK(f.alpha == 1.0f);
    CHECK(f.stepsThisFrame == 2);
    f = StepsFrameInterp(f, 3);
    CHECK(f.flags == kFrameInterpClockReset);
    // Without a registered clock and without FUT_INTERP_STUB the stub disables interpolation.
    CHECK_FALSE(StubFrameInterp().enabled);
    CHECK(StubFrameInterp().alpha == 1.0f);
}

TEST_CASE("interp history: [render] interpolation and interpolation_debug") {
    DebugMode mode = DebugMode::Off;
    CHECK(ParseDebugMode(" Tint ", &mode));
    CHECK(mode == DebugMode::Tint);
    CHECK(ParseDebugMode("trace", &mode));
    CHECK(mode == DebugMode::Trace);
    CHECK(ParseDebugMode("off", &mode));
    CHECK(mode == DebugMode::Off);
    CHECK_FALSE(ParseDebugMode("on", &mode));
    CHECK(std::string(DebugModeName(DebugMode::Trace)) == "trace");
    core::Config cfg;
    cfg.userdata = "C:\\fut-userdata";
    Settings s = SettingsFromConfig(cfg);
    CHECK(s.enabled);    // schema default "on"
    CHECK(s.hierarchy);  // schema default "on" (stage 1b)
    CHECK(s.debug == DebugMode::Off);
    CHECK(s.tracePath.filename() == "interp_trace.csv");
    cfg.values["render.interpolation"] = "off";
    cfg.values["render.interpolation_debug"] = "tint";
    cfg.values["render.interpolation_hierarchy"] = "off";
    s = SettingsFromConfig(cfg);
    CHECK_FALSE(s.enabled);
    CHECK_FALSE(s.hierarchy);
    CHECK(s.debug == DebugMode::Tint);
}

namespace {

// A scene-graph node: vtable, parent +0x14, world bound radius +0x2C, world transform +0x70.
GuestPtr MakeNode(GuestHeap& heap, GuestPtr parent, float radius = 1.0f) {
    const GuestPtr node = heap.Alloc(0x100);
    *P<GuestPtr>(node) = 0x002A6000;  // a NiNode-like vtable
    *P<GuestPtr>(node + 0x14) = parent;
    *P<float>(node + 0x2C) = radius;
    *P<ni::NiTransform>(node + 0x70) = IdentityTransform();
    return node;
}

ni::NiTransform& WorldOf(GuestPtr object) {
    return *P<ni::NiTransform>(object + 0x70);
}

// Arm: a pivot node turning about z (its world rotation) with a shape hanging at (2, 0, 0) in the pivot's space.
struct Arm {
    GuestPtr root = 0;
    GuestPtr pivot = 0;
    Shape shape;
    void Pose(float degrees) {
        WorldOf(pivot) = Rotation(0, 0, 1, degrees * kPi / 180.0f, 0.0f, 0.0f, 0.0f);
        const ni::NiTransform local = Rotation(0, 0, 1, 0.0f, 2.0f, 0.0f, 0.0f);
        shape.World() = Compose(WorldOf(pivot), local);
    }
};

Arm MakeArm(GuestHeap& heap) {
    Arm a;
    a.root = MakeNode(heap, 0, 10.0f);
    a.pivot = MakeNode(heap, a.root, 3.0f);
    a.shape = MakeShape(heap, 1.0f);  // radius 1: steps up to 2 units per tick from rest interpolate (R2)
    *P<GuestPtr>(a.shape.object + 0x14) = a.pivot;
    return a;
}

// 120 Hz phase B: tick frame with alpha 1/2, then the zero-step frame.
GeometryResult TickPair(Driver& d, const Shape& s, std::uint64_t tick) {
    const GeometryResult r = d.Frame(s, tick, 0.5f);
    d.Frame(s, tick, 1.0f);
    return r;
}

}  // namespace

TEST_CASE("interp history: hierarchy - a part on a turning node follows the arc, world interpolation the chord") {
    GuestHeap heap;
    Arm arm = MakeArm(heap);
    Settings hierarchy;  // default: hierarchy on
    Settings world;
    world.hierarchy = false;
    for (const bool along : {true, false}) {
        CAPTURE(along);
        Driver d(along ? hierarchy : world);
        arm.Pose(0.0f);
        TickPair(d, arm.shape, 1);
        arm.Pose(15.0f);
        TickPair(d, arm.shape, 2);
        arm.Pose(55.0f);  // 40 degrees in one tick (after 15: no snap, R4 needs > 45 degrees)
        const GeometryResult r = TickPair(d, arm.shape, 3);
        REQUIRE(r.reason == SnapReason::None);
        const ni::NiTransform shown = Shown(r);
        const float radius = std::sqrt(shown.translate[0] * shown.translate[0] + shown.translate[1] * shown.translate[1]);
        // Same direction as the true pose at 35 degrees (the pivot's world rotation at the middle).
        const ni::NiTransform truth = Compose(Rotation(0, 0, 1, 35.0f * kPi / 180.0f),
                                              Rotation(0, 0, 1, 0.0f, 2.0f, 0.0f, 0.0f));
        CHECK(shown.translate[0] * truth.translate[1] - shown.translate[1] * truth.translate[0] ==
              doctest::Approx(0.0f).epsilon(1e-5));
        CHECK(shown.translate[0] * truth.translate[0] + shown.translate[1] * truth.translate[1] > 0.0f);
        if (along) {
            CHECK(radius == doctest::Approx(2.0f).epsilon(1e-5));  // on the arc
            CHECK(d.history.GetStats().totals.hierarchical == 2);
        } else {
            CHECK(radius == doctest::Approx(2.0f * std::cos(20.0f * kPi / 180.0f)).epsilon(1e-5));  // on the chord
            CHECK(d.history.GetStats().totals.hierarchical == 0);
        }
        // The rotation of the part is the same either way (slerp of the world rotation).
        CHECK(AngleDeg(shown.rotate, Rotation(0, 0, 1, 35.0f * kPi / 180.0f).rotate) < 1e-2f);
    }
}

TEST_CASE("interp history: hierarchy - without parent motion the result equals world interpolation") {
    GuestHeap heap;
    Shape a = MakeShape(heap);  // no parent: the drawn object is the topmost node
    Arm arm = MakeArm(heap);    // static pivot, the shape moves in the pivot's space
    arm.Pose(0.0f);
    Settings world;
    world.hierarchy = false;
    Driver dh, dw(world);
    for (std::uint64_t tick = 1; tick <= 4; ++tick) {
        a.World() = Rotation(1, 1, 0, 0.1f * static_cast<float>(tick), 0.3f * static_cast<float>(tick), 0.0f, 1.0f);
        arm.shape.World() = Compose(WorldOf(arm.pivot), Rotation(0, 1, 0, 0.2f * static_cast<float>(tick),
                                                                   static_cast<float>(tick) * 0.05f, 0.0f, 0.0f));
        for (const float alpha : {0.5f, 1.0f}) {
            dh.Begin(tick, alpha);
            dw.Begin(tick, alpha);
            const GeometryResult h0 = dh.Draw(a), w0 = dw.Draw(a);
            const GeometryResult h1 = dh.Draw(arm.shape), w1 = dw.Draw(arm.shape);
            if (tick >= 2 && alpha < 1.0f) {
                REQUIRE(h0.reason == SnapReason::None);
                REQUIRE(h1.reason == SnapReason::None);
                CHECK(BitwiseEqual(Shown(h0), Shown(w0)));  // root: the same blend of the same transforms
                const ni::NiTransform hs = Shown(h1), ws = Shown(w1);
                for (int c = 0; c < 3; ++c) {
                    CHECK(hs.translate[c] == doctest::Approx(ws.translate[c]).epsilon(1e-5));
                }
                CHECK(AngleDeg(hs.rotate, ws.rotate) < 1e-2f);
            }
            dh.End();
            dw.End();
        }
    }
}

TEST_CASE("interp history: hierarchy - new or re-parented nodes have no history") {
    GuestHeap heap;
    Arm arm = MakeArm(heap);
    Driver d;
    arm.Pose(0.0f);
    TickPair(d, arm.shape, 1);
    arm.Pose(10.0f);
    TickPair(d, arm.shape, 2);
    // The shape moves to another (static) parent: its node has no history for the chain -> world interpolation.
    const GuestPtr other = MakeNode(heap, arm.root);
    *P<GuestPtr>(arm.shape.object + 0x14) = other;
    arm.Pose(20.0f);
    const std::uint64_t before = d.history.GetStats().totals.hierarchyFallbacks;
    GeometryResult r = TickPair(d, arm.shape, 3);
    CHECK(r.reason == SnapReason::None);
    CHECK(d.history.GetStats().totals.hierarchyFallbacks == before + 1);
    const ni::NiTransform shown = Shown(r);
    const float radius = std::sqrt(shown.translate[0] * shown.translate[0] + shown.translate[1] * shown.translate[1]);
    CHECK(radius == doctest::Approx(2.0f * std::cos(5.0f * kPi / 180.0f)).epsilon(1e-5));  // world: the chord
    // From the next tick on the new chain has history again.
    arm.Pose(30.0f);
    WorldOf(other) = WorldOf(arm.pivot);
    r = TickPair(d, arm.shape, 4);
    CHECK(r.reason == SnapReason::None);
    CHECK(d.history.GetStats().totals.hierarchical >= 2);
}

TEST_CASE("interp history: hierarchy - node motion is known for the growth rule after a snapped tick") {
    // Measured in the frontend fly-in: a letter node turns 23.3 deg per tick, then 155.6 (the draw snaps: R4), then
    // 72 deg per tick. 72 deg must interpolate (72 < 4 x 155.6), also for the node rule of the chain.
    GuestHeap heap;
    Arm arm = MakeArm(heap);
    *P<float>(arm.shape.object + 0x2C) = 1.0f;  // radius 1: a start from rest with 0.8 units per tick interpolates
    Driver d;
    float a = 0.0f;
    std::uint64_t tick = 1;
    arm.Pose(a);
    TickPair(d, arm.shape, tick++);
    for (int i = 0; i < 4; ++i) {
        a += 23.3f;
        arm.Pose(a);
        CHECK(TickPair(d, arm.shape, tick++).reason == SnapReason::None);
    }
    a += 155.6f;
    arm.Pose(a);
    CHECK(IsSnap(TickPair(d, arm.shape, tick++).reason));  // R2 and R4
    const std::uint64_t nodeSnaps = d.history.GetStats().totals.nodeSnaps;
    for (int i = 0; i < 2; ++i) {
        a += 72.0f;
        arm.Pose(a);
        const GeometryResult r = TickPair(d, arm.shape, tick++);
        CHECK(r.reason == SnapReason::None);
        // On the arc at the middle of the 72 degree turn (hierarchy), not on its chord.
        const ni::NiTransform shown = Shown(r);
        const ni::NiTransform truth = Compose(Rotation(0, 0, 1, (a - 36.0f) * kPi / 180.0f),
                                              Rotation(0, 0, 1, 0.0f, 2.0f, 0.0f, 0.0f));
        CHECK(shown.translate[0] == doctest::Approx(truth.translate[0]).epsilon(1e-4));
        CHECK(shown.translate[1] == doctest::Approx(truth.translate[1]).epsilon(1e-4));
    }
    CHECK(d.history.GetStats().totals.nodeSnaps == nodeSnaps);
}

// ---- M3d review findings 1, 3, 4, 6, 10 ----------------------------------------------------------------------------

TEST_CASE("interp history: the reversal point of a fast small oscillation is no jump (review finding 1, S10)") {
    // Frontend object 0x03341160 (run review_m3d_interp\trace120): r 0.0385, 0.12-0.15 units per tick; at its turning
    // point one tick moves 0.002 units. The tick after it must interpolate (it snapped as "translation" and held:
    // displayed steps 0 / full tick / 0). Both with the parent chain (decides) and in world space (leaf rule).
    const float ys[] = {-0.573514f, -0.4356814f, -0.3024931f, -0.1755233f, -0.05622045f,
                        -0.05820518f, -0.1767823f, -0.2982394f, -0.4220499f, -0.5476451f};
    for (const bool hierarchy : {true, false}) {
        CAPTURE(hierarchy);
        GuestHeap heap;
        const GuestPtr node = MakeNode(heap, 0, 0.0385f);
        Shape s = MakeShape(heap, 0.0385f);
        *P<GuestPtr>(s.object + 0x14) = node;
        Settings settings;
        settings.hierarchy = hierarchy;
        Driver d(settings);
        for (std::size_t i = 0; i < std::size(ys); ++i) {
            CAPTURE(i);
            WorldOf(node).translate[1] = ys[i];
            s.World().translate[1] = ys[i];
            const GeometryResult r = TickPair(d, s, 100 + i);
            if (i >= 1) {
                REQUIRE(r.reason == SnapReason::None);
                CHECK(Shown(r).translate[1] == doctest::Approx(0.5f * (ys[i - 1] + ys[i])).epsilon(1e-5));
            }
        }
        CHECK(d.history.GetStats().totals.reasons[static_cast<std::size_t>(SnapReason::Translation)] == 0);
    }
}

TEST_CASE("interp history: a fast object seen for the first time interpolates in its second tick (finding 1, S1)") {
    GuestHeap heap;
    Shape s = MakeShape(heap, 0.0385f);
    Driver d;
    for (std::uint64_t tick = 1; tick <= 4; ++tick) {
        CAPTURE(tick);
        s.World().translate[0] = 0.13f * static_cast<float>(tick);  // 3.4 radii per tick, constant speed
        const GeometryResult r = TickPair(d, s, tick);
        if (tick == 1) {
            CHECK(r.reason == SnapReason::FirstSight);
        } else {
            CHECK(r.reason == SnapReason::None);
            CHECK(Shown(r).translate[0] == doctest::Approx(0.13f * (static_cast<float>(tick) - 0.5f)));
        }
    }
    // A jump right after the first sight (unknown previous motion) above 16 radii still snaps.
    Shape t = MakeShape(heap, 0.1f);
    TickPair(d, t, 5);
    t.World().translate[0] = 1.7f;  // 17 radii
    CHECK(TickPair(d, t, 6).reason == SnapReason::Translation);
    MotionReference unknown;
    CHECK(MotionSnapRule(1.5f, 0.0f, 0.1f, unknown) == SnapReason::None);
    CHECK(MotionSnapRule(1.7f, 0.0f, 0.1f, unknown) == SnapReason::Translation);
    CHECK(MotionSnapRule(0.0f, 95.0f, 0.1f, unknown) == SnapReason::Rotation);
}

TEST_CASE("interp history: a rigid assembly starting from rest stays together (finding 1, S2)") {
    // A body (r 1) and a hand (r 0.03) on one node that starts moving with 0.1 units per tick: in world space the hand
    // jumps 3.3 radii from rest (R2), but its local transform does not move - the chain decides and both interpolate.
    GuestHeap heap;
    const GuestPtr root = MakeNode(heap, 0, 10.0f);
    const GuestPtr node = MakeNode(heap, root, 2.0f);
    Shape body = MakeShape(heap, 1.0f);
    Shape hand = MakeShape(heap, 0.03f);
    *P<GuestPtr>(body.object + 0x14) = node;
    *P<GuestPtr>(hand.object + 0x14) = node;
    const ni::NiTransform handLocal = Rotation(0, 0, 1, 0.0f, 0.5f, 0.0f, 0.0f);
    Driver d;
    float x = 0.0f;
    for (std::uint64_t tick = 1; tick <= 5; ++tick) {
        CAPTURE(tick);
        if (tick >= 3) {
            x += 0.1f;
        }
        WorldOf(node) = Rotation(0, 0, 1, 0.0f, x, 0.0f, 0.0f);
        body.World() = WorldOf(node);
        hand.World() = Compose(WorldOf(node), handLocal);
        d.Begin(tick, 0.5f);
        const GeometryResult rb = d.Draw(body);
        const GeometryResult rh = d.Draw(hand);
        d.End();
        d.Begin(tick, 1.0f);
        d.Draw(body);
        d.Draw(hand);
        d.End();
        if (tick >= 3) {
            CHECK(rb.reason == SnapReason::None);
            CHECK(rh.reason == SnapReason::None);
            CHECK(Shown(rh).translate[0] - Shown(rb).translate[0] == doctest::Approx(0.5f).epsilon(1e-5));
            CHECK(Shown(rb).translate[0] == doctest::Approx(x - 0.05f).epsilon(1e-5));
        }
    }
    CHECK(d.history.GetStats().totals.chainDecisions >= 1);
    // A jump of the node itself (20 units: 10 radii) still snaps the whole assembly, together.
    x += 20.0f;
    WorldOf(node) = Rotation(0, 0, 1, 0.0f, x, 0.0f, 0.0f);
    body.World() = WorldOf(node);
    hand.World() = Compose(WorldOf(node), handLocal);
    d.Begin(6, 0.5f);
    const GeometryResult rb = d.Draw(body);
    const GeometryResult rh = d.Draw(hand);
    d.End();
    CHECK(rb.reason == SnapReason::Translation);
    CHECK(rh.reason == SnapReason::Translation);
    CHECK(Shown(rb).translate[0] == doctest::Approx(x));
    CHECK(Shown(rh).translate[0] - Shown(rb).translate[0] == doctest::Approx(0.5f).epsilon(1e-5));
}

TEST_CASE("interp history: pointer reuse inside the zero-step frame is a new object, not frame-driven (finding 3)") {
    GuestHeap heap;
    Shape s = MakeShape(heap, 1.0f);
    Driver d;
    std::uint64_t tick = 1;
    for (; tick <= 3; ++tick) {
        s.World().translate[0] = 0.05f * static_cast<float>(tick);
        TickPair(d, s, tick);
    }
    // Tick 4: in its zero-step frame the object is destroyed and a new one with the same signature (shared data and
    // property state) is placed at the same address elsewhere.
    s.World().translate[0] = 0.2f;
    d.Frame(s, tick, 0.5f);
    History::OnObjectDestroyed(s.object);
    s.World().translate[0] = 3.0f;
    d.Frame(s, tick, 1.0f);
    CHECK(d.history.GetStats().totals.recaptureDestroyed == 1);
    CHECK(d.history.GetStats().totals.frameDrivenChanges == 0);
    int interpolated = 0, other = 0;
    for (++tick; tick <= 64; ++tick) {
        s.World().translate[0] += 0.05f;
        const GeometryResult r = TickPair(d, s, tick);
        (r.reason == SnapReason::None ? interpolated : other) += 1;
    }
    CHECK(interpolated == 60);  // from the first tick after the reuse on (no second "destroyed" snap either)
    CHECK(other == 0);
    // Destroyed and re-created before the draw of a tick frame: one "destroyed" snap, then history again.
    History::OnObjectDestroyed(s.object);
    s.World().translate[0] += 2.0f;
    CHECK(TickPair(d, s, tick++).reason == SnapReason::Destroyed);
    s.World().translate[0] += 0.05f;
    CHECK(TickPair(d, s, tick++).reason == SnapReason::None);
}

TEST_CASE("interp history: a camera rebuilt at the same address has no history (finding 4, S11)") {
    GuestHeap heap;
    const GuestPtr cam = heap.Alloc(0x260);
    auto* loc = P<float>(cam + ni::kNiCameraWorldLocation);
    P<float>(cam + ni::kNiCameraWorldDirection)[0] = 1.0f;
    P<float>(cam + ni::kNiCameraWorldUp)[1] = 1.0f;
    P<float>(cam + ni::kNiCameraWorldRight)[2] = 1.0f;
    *P<ni::NiFrustum>(cam + ni::kNiCameraFrustum) = ni::NiFrustum{-0.7714f, 0.7714f, 0.5786f, -0.5786f, 0.1f, 1000.0f};
    *P<ni::NiRectF>(cam + ni::kNiCameraPort) = ni::NiRectF{0.0f, 1.0f, 1.0f, 0.0f};
    ni::CameraInput in;
    in.location = loc;
    in.direction = P<float>(cam + ni::kNiCameraWorldDirection);
    in.up = P<float>(cam + ni::kNiCameraWorldUp);
    in.right = P<float>(cam + ni::kNiCameraWorldRight);
    in.frustum = P<ni::NiFrustum>(cam + ni::kNiCameraFrustum);
    in.port = P<ni::NiRectF>(cam + ni::kNiCameraPort);
    Driver d;
    for (std::uint64_t tick = 1; tick <= 3; ++tick) {
        loc[0] = 0.2f * static_cast<float>(tick);
        for (const float alpha : {0.5f, 1.0f}) {
            d.Begin(tick, alpha);
            d.history.Camera(cam, in);
            d.End();
        }
    }
    History::OnObjectDestroyed(cam);  // the shot camera is deleted, a new one is built at the same address
    loc[0] = 0.8f;
    loc[2] = 0.45f;  // 0.47 units away: below the cut threshold
    d.Begin(4, 0.5f);
    ni::CameraInput out = d.history.Camera(cam, in);
    CHECK(out.location == loc);  // the new shot as is, no blend with the old one
    d.End();
    d.Begin(4, 1.0f);
    d.history.Camera(cam, in);
    d.End();
    // The new camera moves on: interpolated from its next tick (the destruction in its first frame is not its own).
    loc[0] = 1.0f;
    d.Begin(5, 0.5f);
    out = d.history.Camera(cam, in);
    CHECK(out.location != loc);
    CHECK(out.location[0] == doctest::Approx(0.9f));
    d.End();
    const Stats& st = d.history.GetStats();
    CHECK(st.totals.cameraReasons[static_cast<std::size_t>(SnapReason::Destroyed)] == 1);
}

TEST_CASE("interp history: a catch-up or the 6 dt cap interpolates over the step pair (finding 10)") {
    // The displayed time G - 1 + alpha: 120 Hz phase B, then a frame with 2 steps (alpha 1/2: G - 1/2 = interp(S[G-2],
    // S[G], 3/4)), then the cap (6 steps, alpha 0, then the alignment frame with alpha 1/2 and no step).
    GuestHeap heap;
    Shape s = MakeShape(heap, 1.0f);
    Driver d;
    const auto position = [](std::uint64_t tick) { return 0.01f * static_cast<float>(tick); };  // 0.01 units per tick
    const auto frame = [&](std::uint64_t tick, float alpha, std::uint32_t pairTicks) {
        s.World().translate[0] = position(tick);
        d.Begin(tick, alpha);
        g_frame.pairTicks = pairTicks;
        const GeometryResult r = d.Draw(s);
        d.End();
        return r;
    };
    for (std::uint64_t tick = 1; tick <= 4; ++tick) {
        frame(tick, 0.5f, 1);
        frame(tick, 1.0f, 1);
    }
    // Catch-up: ticks 5 and 6 in one frame.
    GeometryResult r = frame(6, 0.5f, 2);
    CHECK(r.reason == SnapReason::None);
    CHECK(Shown(r).translate[0] == doctest::Approx(0.01f * 5.5f));  // G - 1/2, not S[G] then S[G] again
    r = frame(6, 1.0f, 2);
    CHECK(Shown(r).translate[0] == doctest::Approx(position(6)));
    r = frame(7, 0.5f, 1);
    CHECK(Shown(r).translate[0] == doctest::Approx(0.01f * 6.5f));
    frame(7, 1.0f, 1);
    // The cap: 6 steps with alpha 0 (displayed time G - 1), the alignment frame with alpha 1/2, then the pattern.
    r = frame(13, 0.0f, 6);
    CHECK(r.reason == SnapReason::None);
    CHECK(Shown(r).translate[0] == doctest::Approx(0.01f * 12.0f));
    r = frame(13, 0.5f, 6);
    CHECK(Shown(r).translate[0] == doctest::Approx(0.01f * 12.5f));
    r = frame(13, 1.0f, 6);
    CHECK(Shown(r).translate[0] == doctest::Approx(position(13)));
    r = frame(14, 0.5f, 1);
    CHECK(Shown(r).translate[0] == doctest::Approx(0.01f * 13.5f));
    // More than kMaxPairTicks, or a jump that is not the pair of the frame: a generation gap (snap).
    CHECK(frame(16, 0.5f, 1).reason == SnapReason::GenerationGap);
    CHECK(frame(24, 0.5f, 8).reason == SnapReason::GenerationGap);
    const Stats& st = d.history.GetStats();
    CHECK(st.pairSpans == 2);
}

TEST_CASE("interp history: the readable-page cache of the hierarchy is dropped when guest memory is released (6)") {
    GuestHeap heap;
    Arm arm = MakeArm(heap);
    Driver d;
    arm.Pose(0.0f);
    TickPair(d, arm.shape, 1);
    arm.Pose(5.0f);
    TickPair(d, arm.shape, 2);
    const std::uint64_t before = d.history.GetStats().readableCacheResets;
    guest::NoteMemoryReleased();
    arm.Pose(10.0f);
    CHECK(TickPair(d, arm.shape, 3).reason == SnapReason::None);
    CHECK(d.history.GetStats().readableCacheResets == before + 1);
    CHECK(d.history.GetStats().totals.hierarchical >= 2);
}
