// Unit tests of gfx/interp_history.h (M3d renderer-level interpolation, analysis/loader/m3d_interpolation.md 3, 5, 8):
// history bookkeeping (generations, keys, occurrences, signature, destruction, frame-driven keys, gaps), rigid and
// camera interpolation at 120 Hz "phase B", skinning through the substitute NiSkinInstance, the pass-through at
// 60 fps, the table hash, the debug tint and the FUT_INTERP_STUB developer source. Guest objects live in
// gfx::AllocGuestPages memory; no guest code runs (SetGuestCallsForTest(false)).
#include "doctest.h"

#include "core/config.h"
#include "gfx/guest_memory.h"
#include "gfx/interp_history.h"

#include <cmath>
#include <cstdint>
#include <cstring>
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
    CHECK(s.enabled);  // schema default "on"
    CHECK(s.debug == DebugMode::Off);
    CHECK(s.tracePath.filename() == "interp_trace.csv");
    cfg.values["render.interpolation"] = "off";
    cfg.values["render.interpolation_debug"] = "tint";
    s = SettingsFromConfig(cfg);
    CHECK_FALSE(s.enabled);
    CHECK(s.debug == DebugMode::Tint);
}
