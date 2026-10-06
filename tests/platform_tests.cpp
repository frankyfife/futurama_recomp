// Unit tests of platform/** (M3 backend agent): view layout, the main window with a D3D11 swap chain
// (WARP), present statistics and the 50 Hz pacer, PNG screenshots through WIC.
#include "doctest.h"

#include "gfx/rhi/d3d11/d3d11_device.h"
#include "gfx/rhi/rhi.h"
#include "platform/frame_pacer.h"
#include "platform/screenshot.h"
#include "platform/window.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path ExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

// Fresh temporary directory: %FUT_TEST_TMP% or <exe dir>\fut_tests_tmp (as tests/vfs_tests.cpp).
fs::path TempDir(const char* name) {
    wchar_t buf[1024];
    const DWORD n = GetEnvironmentVariableW(L"FUT_TEST_TMP", buf, 1024);
    const fs::path base = n > 0 && n < 1024 ? fs::path(buf) : ExeDir() / "fut_tests_tmp";
    const fs::path dir = (base / name).make_preferred();
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void PumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// The game's thread model: the main window belongs to a UI thread that pumps its messages (boot thread,
// HostMain stages 12/14) while another thread creates the swap chain and presents (guest main thread).
class UiThread {
public:
    explicit UiThread(const platform::WindowOptions& options) {
        thread_ = std::thread([this, options] {
            const char* error = nullptr;
            created_ = platform::CreateMainWindow(options, &error);
            ready_ = true;
            while (!quit_) {
                MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                PumpMessages();
            }
            platform::DestroyMainWindow();
            PumpMessages();
        });
        while (!ready_) {
            std::this_thread::yield();
        }
    }
    ~UiThread() {
        quit_ = true;
        thread_.join();
    }
    UiThread(const UiThread&) = delete;
    UiThread& operator=(const UiThread&) = delete;
    bool Created() const { return created_; }

private:
    std::thread thread_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> quit_{false};
    std::atomic<bool> created_{false};
};

}  // namespace

TEST_CASE("platform: 4:3 view layout inside the client area") {
    platform::ViewRect r = platform::FitAspectRect(1280, 960, 640, 480);
    CHECK(r.x == 0);
    CHECK(r.y == 0);
    CHECK(r.width == 1280);
    CHECK(r.height == 960);
    r = platform::FitAspectRect(1920, 1080, 640, 480);  // pillarbox
    CHECK(r.width == 1440);
    CHECK(r.height == 1080);
    CHECK(r.x == 240);
    CHECK(r.y == 0);
    r = platform::FitAspectRect(800, 1000, 640, 480);  // letterbox
    CHECK(r.width == 800);
    CHECK(r.height == 600);
    CHECK(r.x == 0);
    CHECK(r.y == 200);
    r = platform::FitAspectRect(0, 100, 640, 480);
    CHECK(r.width == 0);
    CHECK(r.height == 0);
}

TEST_CASE("platform: [render] monitor selects a monitor by its GDI device name (M3c)") {
    const HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    REQUIRE(GetMonitorInfoW(primary, &mi));
    char device[40] = {};
    WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, device, 39, nullptr, nullptr);
    // primary: default, empty and the word "primary" (any case, blanks around it).
    for (const char* name : {static_cast<const char*>(nullptr), "", "primary", "  Primary ", "PRIMARY"}) {
        const platform::MonitorChoice c = platform::SelectMonitor(name);
        CHECK(c.monitor == primary);
        CHECK(c.primary);
        CHECK(c.found);
        CHECK(std::string(c.device) == device);
    }
    // Every connected monitor by its device name (case-insensitive, as --query-gpu lists it).
    std::vector<std::pair<HMONITOR, std::string>> monitors;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            if (GetMonitorInfoW(m, &info)) {
                char name[40] = {};
                WideCharToMultiByte(CP_UTF8, 0, info.szDevice, -1, name, 39, nullptr, nullptr);
                reinterpret_cast<std::vector<std::pair<HMONITOR, std::string>>*>(lp)->emplace_back(m, name);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&monitors));
    REQUIRE(!monitors.empty());
    for (const auto& [monitor, name] : monitors) {
        std::string lower = name;
        for (char& ch : lower) {
            ch = static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
        }
        for (const std::string& query : {name, lower, " " + name + " "}) {
            const platform::MonitorChoice c = platform::SelectMonitor(query.c_str());
            CHECK(c.monitor == monitor);
            CHECK(c.found);
            CHECK(c.primary == (monitor == primary));
            CHECK(std::string(c.device) == name);
        }
    }
    // Not connected: the primary monitor, found = false.
    for (const char* name : {"\\\\.\\DISPLAY99", "LG TV", "\\\\.\\DISPLAY1x"}) {
        const platform::MonitorChoice c = platform::SelectMonitor(name);
        CHECK(c.monitor == primary);
        CHECK(c.primary);
        CHECK_FALSE(c.found);
        CHECK(std::string(c.device) == device);
    }
}

TEST_CASE("platform: present statistics") {
    platform::PresentStatsAccumulator steady;
    steady.SetSlowThreshold(1.5 / 50);
    for (int i = 0; i <= 150; ++i) {  // 3 s at 50 Hz with a little jitter
        steady.Add(i * 0.02 + ((i % 3) - 1) * 0.0005);
    }
    platform::PresentStatistics s = steady.Result();
    CHECK(s.frames == 151);
    CHECK(s.seconds == doctest::Approx(3.0).epsilon(0.001));
    CHECK(s.averageHz == doctest::Approx(50.0).epsilon(0.001));
    CHECK(s.slowestWindowHz == doctest::Approx(50.0).epsilon(0.03));
    CHECK(s.longestFrameMs == doctest::Approx(20.5).epsilon(0.01));
    CHECK(s.slowFrames == 0);

    platform::PresentStatsAccumulator stall;
    stall.SetSlowThreshold(1.5 / 50);
    double t = 0;
    for (int i = 0; i < 100; ++i, t += 0.02) {
        stall.Add(t);
    }
    t += 0.48;  // one 500 ms frame
    for (int i = 0; i < 100; ++i, t += 0.02) {
        stall.Add(t);
    }
    s = stall.Result();
    CHECK(s.frames == 200);
    CHECK(s.longestFrameMs == doctest::Approx(500.0).epsilon(0.01));
    CHECK(s.slowFrames == 1);
    CHECK(s.slowestWindowHz < 30.0);
    CHECK(s.slowestWindowHz > 20.0);
    CHECK(s.averageHz < 50.0);

    platform::PresentStatsAccumulator gap;  // a gap longer than the window
    gap.Add(0.0);
    gap.Add(0.5);
    gap.Add(2.5);
    gap.Add(2.52);
    s = gap.Result();
    CHECK(s.slowestWindowHz == doctest::Approx(0.5));

    platform::PresentStatsAccumulator empty;
    s = empty.Result();
    CHECK(s.frames == 0);
    CHECK(s.averageHz == 0.0);
    CHECK(s.slowestWindowHz == 0.0);
}

TEST_CASE("platform: the 50 Hz pacer holds the frame period") {
    platform::ResetFramePacer();
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 26; ++i) {  // the first call starts the schedule, 25 periods follow
        platform::PaceFrame(50);
        platform::NotePresentedFrame();
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(elapsed > 0.45);
    CHECK(elapsed < 1.5);
    const platform::PresentStatistics s = platform::GetPresentStatistics();
    CHECK(s.frames == 26);
    CHECK(s.pacingHz == 50);
    CHECK(s.averageHz > 40.0);
    CHECK(s.averageHz < 55.0);
    platform::PaceFrame(0);  // off: returns at once
    platform::LogPresentStatistics();
    platform::ResetFramePacer();
    CHECK(platform::GetPresentStatistics().frames == 0);
}

TEST_CASE("platform: frame work statistics (review M3 finding 3)") {
    platform::ResetFramePacer();
    platform::NoteFrameWorkDone();  // no frame started yet: no sample
    CHECK(platform::GetPresentStatistics().workFrames == 0);
    for (int i = 0; i < 3; ++i) {
        platform::PaceFrame(0);  // frame start
        Sleep(3);
        platform::NoteFrameWorkDone();
        platform::NoteFrameWorkDone();  // one sample per frame
    }
    const platform::PresentStatistics s = platform::GetPresentStatistics();
    CHECK(s.workFrames == 3);
    CHECK(s.workAverageMs >= 2.5);
    CHECK(s.workAverageMs < 100.0);
    CHECK(s.workP99Ms >= s.workAverageMs - 0.1);
    CHECK(s.workLongestMs >= s.workAverageMs);
    CHECK(s.workOverPeriod == 0);  // no pacing period (hz 0)
    platform::LogPresentStatistics();
    platform::ResetFramePacer();
    CHECK(platform::GetPresentStatistics().workFrames == 0);
}

TEST_CASE("platform: PNG screenshots through WIC") {
    const fs::path dir = TempDir("platform_png");
    gfx::rhi::Image image;
    image.format = gfx::rhi::Format::BGRA8_UNorm;
    image.width = 5;
    image.height = 3;
    image.rowPitch = 24;  // padded rows
    image.pixels.resize(24 * 3);
    for (std::size_t i = 0; i < image.pixels.size(); ++i) {
        image.pixels[i] = static_cast<std::uint8_t>(i * 37 + 11);
    }
    const fs::path path = platform::ScreenshotPath(dir / "sub", 42);
    CHECK(path.filename() == "frame_000042.png");
    std::string error;
    REQUIRE_MESSAGE(platform::WritePngFile(path, image, &error), error);
    gfx::rhi::Image back;
    REQUIRE_MESSAGE(platform::ReadImageFile(path, back, &error), error);
    CHECK(back.width == 5);
    CHECK(back.height == 3);
    for (std::uint32_t y = 0; y < 3; ++y) {
        for (std::uint32_t x = 0; x < 5; ++x) {
            const std::uint8_t* a = image.pixels.data() + y * 24 + x * 4;
            const std::uint8_t* b = back.pixels.data() + y * back.rowPitch + x * 4;
            CHECK(a[0] == b[0]);
            CHECK(a[1] == b[1]);
            CHECK(a[2] == b[2]);
            CHECK(b[3] == 0xFF);  // written opaque
        }
    }
    // RGBA8 input: channels swapped into the PNG.
    gfx::rhi::Image rgba;
    rgba.format = gfx::rhi::Format::RGBA8_UNorm;
    rgba.width = 1;
    rgba.height = 1;
    rgba.rowPitch = 4;
    rgba.pixels = {0x10, 0x20, 0x30, 0x00};
    REQUIRE(platform::WritePngFile(dir / "rgba.png", rgba, &error));
    REQUIRE(platform::ReadImageFile(dir / "rgba.png", back, &error));
    CHECK(back.pixels[0] == 0x30);  // B
    CHECK(back.pixels[2] == 0x10);  // R
    // Rejected input.
    gfx::rhi::Image bad = rgba;
    bad.format = gfx::rhi::Format::BC1_UNorm;
    CHECK_FALSE(platform::WritePngFile(dir / "bad.png", bad, &error));
    CHECK_FALSE(error.empty());
    bad = rgba;
    bad.pixels.clear();
    CHECK_FALSE(platform::WritePngFile(dir / "bad.png", bad, &error));
    CHECK_FALSE(fs::exists(dir / "bad.png"));
}

TEST_CASE("platform: main window with a D3D11 flip-model swap chain (WARP)") {
    REQUIRE(platform::MainWindow() == nullptr);
    platform::WindowOptions options;
    options.scale = 1;
    options.title = L"fut_tests window";
    const char* error = nullptr;
    REQUIRE_MESSAGE(platform::CreateMainWindow(options, &error), (error != nullptr ? error : ""));
    CHECK_FALSE(platform::CreateMainWindow(options, &error));  // only one
    const HWND view = platform::MainWindow();
    REQUIRE(view != nullptr);
    CHECK(GetParent(view) == platform::MainFrameWindow());
    std::uint32_t w = 0, h = 0;
    platform::MainWindowClientSize(&w, &h);
    CHECK(w >= 640);  // 640x480 * DPI / 96 (physical pixels)
    CHECK(w * 3 == h * 4);
    CHECK_FALSE(IsWindowVisible(platform::MainFrameWindow()));
    CHECK(platform::ConsumeHotkeys() == 0);
    platform::PostHotkeys(platform::kHotkeyScreenshot);
    CHECK(platform::ConsumeHotkeys() == platform::kHotkeyScreenshot);
    CHECK(platform::ConsumeHotkeys() == 0);
    // F12 in the frame sets the screenshot hotkey (auto-repeat ignored).
    SendMessageW(platform::MainFrameWindow(), WM_KEYDOWN, VK_F12, 1);
    SendMessageW(platform::MainFrameWindow(), WM_KEYDOWN, VK_F12, 1 | (1 << 30));
    CHECK(platform::ConsumeHotkeys() == platform::kHotkeyScreenshot);
    // A resized frame keeps the view at 4:3 (pillarbox).
    RECT wr{0, 0, 1000, 480};
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, 0);
    SetWindowPos(platform::MainFrameWindow(), nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    platform::MainWindowClientSize(&w, &h);
    CHECK(w * 3 == h * 4);
    // Alt+Enter toggles the borderless fullscreen window and back; the view keeps 4:3.
    const HWND frame = platform::MainFrameWindow();
    const LPARAM altEnter = 1 | (static_cast<LPARAM>(KF_ALTDOWN) << 16);
    SendMessageW(frame, WM_SYSKEYDOWN, VK_RETURN, altEnter);
    CHECK((GetWindowLongPtrW(frame, GWL_STYLE) & WS_POPUP) != 0);
    CHECK((GetWindowLongPtrW(frame, GWL_STYLE) & WS_CAPTION) == 0);
    platform::MainWindowClientSize(&w, &h);
    CHECK(w > 0);
    CHECK(static_cast<int>(w * 3) - static_cast<int>(h * 4) <= 4);
    CHECK(static_cast<int>(h * 4) - static_cast<int>(w * 3) <= 4);
    SendMessageW(frame, WM_SYSKEYDOWN, VK_RETURN, altEnter);
    CHECK((GetWindowLongPtrW(frame, GWL_STYLE) & WS_CAPTION) == WS_CAPTION);
    CHECK_FALSE(IsWindowVisible(frame));  // toggling does not show the hidden window
    {
        gfx::rhi::DeviceDesc desc;
        desc.backend = gfx::rhi::Backend::D3D11;
        desc.nativeWindow = view;
        desc.softwareAdapter = true;
        desc.debugLayer = true;
        std::string reason;
        auto device = gfx::rhi::CreateDevice(desc, &reason);
        REQUIRE_MESSAGE(device != nullptr, reason);
        CHECK_FALSE(device->Info().headless);
        CHECK(device->Info().backBufferWidth == 640);
        const float red[4] = {1, 0, 0, 1};
        for (int n = 0; n < 4; ++n) {
            device->Context().Clear(gfx::rhi::kClearColor | gfx::rhi::kClearDepth, red, 1.0f, 0, nullptr);
            gfx::rhi::Image image;
            REQUIRE(device->CaptureBackBuffer(image));
            CHECK(image.width == 640);
            CHECK(image.height == 480);
            CHECK(image.pixels[2] == 0xFF);  // R of pixel 0
            CHECK(image.pixels[0] == 0x00);
            const gfx::rhi::PresentResult r = device->Present(gfx::rhi::PresentDesc{0});
            CHECK((r == gfx::rhi::PresentResult::Ok || r == gfx::rhi::PresentResult::Occluded));
            PumpMessages();
        }
        // Screenshot helper (CaptureBackBuffer + PNG + log line).
        const fs::path dir = TempDir("platform_window");
        device->Context().Clear(gfx::rhi::kClearColor, red, 1.0f, 0, nullptr);
        CHECK(platform::SaveScreenshot(*device, dir, 7));
        CHECK(fs::exists(dir / "frame_000007.png"));
        std::vector<gfx::rhi::DebugMessage> messages;
        device->DrainDebugMessages(messages);
        for (const auto& m : messages) {
            MESSAGE("debug message #" << m.id << ": " << m.text);
        }
        CHECK(device->Stats().debugErrors == 0);
    }
    // Present pass (review M3 findings 2/8/11): every scaling mode, a resized window (ResizeBuffers in
    // Present), and the context's own bindings survive Present (pipeline, texture slot 0, viewport).
    for (gfx::rhi::PresentScaling scaling : {gfx::rhi::PresentScaling::Linear, gfx::rhi::PresentScaling::Point,
                                             gfx::rhi::PresentScaling::Integer}) {
        gfx::rhi::DeviceDesc desc;
        desc.backend = gfx::rhi::Backend::D3D11;
        desc.nativeWindow = view;
        desc.softwareAdapter = true;
        desc.debugLayer = true;
        desc.scaling = scaling;
        std::string reason;
        auto device = gfx::rhi::CreateDevice(desc, &reason);
        REQUIRE_MESSAGE(device != nullptr, reason);
        gfx::rhi::CommandContext& ctx = device->Context();
        const gfx::rhi::ShaderHandle vs = device->CreateShader(
            gfx::rhi::ShaderStage::Vertex, gfx::rhi::d3d11::BuiltinShaderCode(gfx::rhi::d3d11::BuiltinShader::DiagVS),
            "diag vs");
        const gfx::rhi::ShaderHandle ps = device->CreateShader(
            gfx::rhi::ShaderStage::Pixel,
            gfx::rhi::d3d11::BuiltinShaderCode(gfx::rhi::d3d11::BuiltinShader::DiagTexturePS), "diag texture ps");
        REQUIRE(vs);
        REQUIRE(ps);
        gfx::rhi::PipelineDesc pd;
        pd.vertexShader = vs;
        pd.pixelShader = ps;
        pd.layout = gfx::rhi::d3d11::DiagVertexLayout();
        pd.depthStencil.depthEnable = false;
        const gfx::rhi::PipelineHandle pipeline = device->GetPipeline(pd, "present pass test");
        REQUIRE(pipeline);
        const std::uint8_t texel[4] = {0x40, 0x80, 0xC0, 0xFF};  // B, G, R, A
        gfx::rhi::TextureDesc td;
        td.width = 1;
        td.height = 1;
        td.format = gfx::rhi::Format::BGRA8_UNorm;
        gfx::rhi::SubresourceData level;
        level.data = texel;
        const gfx::rhi::TextureHandle texture = device->CreateTexture(td, &level);
        REQUIRE(texture);
        // Clip-space quad over the whole target (DiagVS: position, color, uv), color 1.
        const float quad[6][9] = {{-1, 1, 0, 1, 1, 1, 1, 0, 0}, {1, 1, 0, 1, 1, 1, 1, 1, 0},  {-1, -1, 0, 1, 1, 1, 1, 0, 1},
                                  {-1, -1, 0, 1, 1, 1, 1, 0, 1}, {1, 1, 0, 1, 1, 1, 1, 1, 0}, {1, -1, 0, 1, 1, 1, 1, 1, 1}};
        gfx::rhi::BufferDesc vb;
        vb.size = sizeof(quad);
        vb.bind = gfx::rhi::kBindVertex;
        vb.initialData = quad;
        const gfx::rhi::BufferHandle vertices = device->CreateBuffer(vb);
        REQUIRE(vertices);
        ctx.SetPipeline(pipeline);
        ctx.SetVertexBuffer(0, vertices, 0, 36);
        ctx.SetTexture(0, texture);
        ctx.SetSampler(0, device->GetSampler(gfx::rhi::SamplerDesc{}));
        ctx.SetViewport(gfx::rhi::Viewport{0.0f, 0.0f, 320.0f, 240.0f, 0.0f, 1.0f});  // top-left quarter
        const float black[4] = {0, 0, 0, 1};
        for (int n = 0; n < 3; ++n) {
            if (n == 1) {  // a resized window: the next Present resizes the swap chain
                RECT sized{0, 0, 800 + 64 * static_cast<int>(scaling), 600};
                AdjustWindowRectEx(&sized, WS_OVERLAPPEDWINDOW, FALSE, 0);
                SetWindowPos(platform::MainFrameWindow(), nullptr, 0, 0, sized.right - sized.left,
                             sized.bottom - sized.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                PumpMessages();
            }
            ctx.Clear(gfx::rhi::kClearColor, black, 1.0f, 0, nullptr);
            ctx.Draw(6, 0);  // no re-binding after Present: the context restored its state
            gfx::rhi::Image image;
            REQUIRE(device->CaptureBackBuffer(image));
            CHECK(image.width == 640);
            const std::uint8_t* inside = image.pixels.data() + 100 * image.rowPitch + 100 * 4;
            const std::uint8_t* outside = image.pixels.data() + 400 * image.rowPitch + 500 * 4;
            CHECK_MESSAGE(inside[2] == 0xC0, "frame " << n << " scaling " << static_cast<int>(scaling));
            CHECK(inside[1] == 0x80);
            CHECK(outside[2] == 0x00);
            const gfx::rhi::PresentResult r = device->Present(gfx::rhi::PresentDesc{0});
            CHECK((r == gfx::rhi::PresentResult::Ok || r == gfx::rhi::PresentResult::Occluded));
            PumpMessages();
        }
        device->Destroy(texture);
        device->Destroy(vertices);
        std::vector<gfx::rhi::DebugMessage> messages;
        device->DrainDebugMessages(messages);
        for (const auto& m : messages) {
            MESSAGE("debug message #" << m.id << ": " << m.text);
        }
        CHECK(device->Stats().debugErrors == 0);
        CHECK(device->Stats().debugWarnings == 0);
    }
    // WM_CLOSE only requests the end (review M3 findings 5/10): the renderer ends the process after its next
    // frame on the guest main thread. DestroyMainWindow right after also removes the 3 s fallback timer.
    CHECK_FALSE(platform::CloseRequested());
    SendMessageW(platform::MainFrameWindow(), WM_CLOSE, 0, 0);
    CHECK(platform::CloseRequested());
    CHECK(platform::MainWindow() != nullptr);  // the window stays
    platform::DestroyMainWindow();
    PumpMessages();
    CHECK(platform::MainWindow() == nullptr);
    std::uint32_t w2 = 1, h2 = 1;
    platform::MainWindowClientSize(&w2, &h2);
    CHECK(w2 == 0);
    CHECK(h2 == 0);
}

namespace {

// Renders `frames` frames on this thread into the (shown) window of a UI thread; returns the device's debug
// errors. Each frame: clear to a color ramp, optional screenshot, Present, NotePresentedFrame, PaceFrame.
std::uint32_t RenderLoop(bool hardware, std::uint32_t frames, std::uint32_t syncInterval,
                         std::uint32_t paceHz, const fs::path& screenshots) {
    platform::WindowOptions options;
    options.scale = 1;
    options.title = L"fut_tests present loop";
    UiThread ui(options);
    REQUIRE(ui.Created());
    REQUIRE(platform::MainWindow() != nullptr);
    gfx::rhi::DeviceDesc desc;
    desc.backend = gfx::rhi::Backend::D3D11;
    desc.nativeWindow = platform::MainWindow();
    desc.softwareAdapter = !hardware;
    desc.debugLayer = true;
    std::string reason;
    auto device = gfx::rhi::CreateDevice(desc, &reason);
    REQUIRE_MESSAGE(device != nullptr, reason);
    const std::string layer = device->DebugLayer().enabled ? std::string("enabled") : device->DebugLayer().note;
    MESSAGE("adapter " << device->Info().adapterName << ", " << device->Info().apiVersion << ", debug layer " << layer);
    platform::ResetFramePacer();
    std::uint32_t occluded = 0;
    for (std::uint32_t frame = 1; frame <= frames; ++frame) {
        const float t = static_cast<float>(frame % 100) / 100.0f;
        const float color[4] = {t, 0.25f, 1.0f - t, 1.0f};
        device->Context().Clear(gfx::rhi::kClearColor | gfx::rhi::kClearDepth | gfx::rhi::kClearStencil, color, 1.0f,
                                0, nullptr);
        if (!screenshots.empty() && frame % 100 == 0) {
            CHECK(platform::SaveScreenshot(*device, screenshots, frame));
        }
        const gfx::rhi::PresentResult r = device->Present(gfx::rhi::PresentDesc{syncInterval});
        REQUIRE((r == gfx::rhi::PresentResult::Ok || r == gfx::rhi::PresentResult::Occluded));
        occluded += r == gfx::rhi::PresentResult::Occluded ? 1 : 0;
        platform::NotePresentedFrame();  // the first call shows the window (ShowWindowAsync to the UI thread)
        platform::PaceFrame(paceHz);
    }
    const platform::PresentStatistics s = platform::GetPresentStatistics();
    MESSAGE("present: " << s.frames << " frames in " << s.seconds << " s, average " << s.averageHz
                        << " Hz, slowest 1 s window " << s.slowestWindowHz << " Hz, longest " << s.longestFrameMs
                        << " ms, schedule resets " << s.scheduleResets << ", occluded presents " << occluded);
    platform::LogPresentStatistics();
    return device->Stats().debugErrors;
}

}  // namespace

TEST_CASE("platform: swap chain on a window of another thread (WARP, hidden)") {
    // The window stays hidden (no NotePresentedFrame), Present without vsync: Ok or Occluded.
    platform::WindowOptions options;
    options.scale = 1;
    UiThread ui(options);
    REQUIRE(ui.Created());
    gfx::rhi::DeviceDesc desc;
    desc.backend = gfx::rhi::Backend::D3D11;
    desc.nativeWindow = platform::MainWindow();
    desc.softwareAdapter = true;
    desc.debugLayer = true;
    std::string reason;
    auto device = gfx::rhi::CreateDevice(desc, &reason);
    REQUIRE_MESSAGE(device != nullptr, reason);
    const float green[4] = {0, 1, 0, 1};
    for (int frame = 0; frame < 10; ++frame) {
        device->Context().Clear(gfx::rhi::kClearColor, green, 1.0f, 0, nullptr);
        gfx::rhi::Image image;
        REQUIRE(device->CaptureBackBuffer(image));
        CHECK(image.pixels[1] == 0xFF);
        const gfx::rhi::PresentResult r = device->Present(gfx::rhi::PresentDesc{0});
        CHECK((r == gfx::rhi::PresentResult::Ok || r == gfx::rhi::PresentResult::Occluded));
    }
    CHECK(device->Stats().debugErrors == 0);
    CHECK(device->Stats().presents == 10);
}

// Manual (skipped by default; run with  fut_tests.exe -tc="*present loop*" --no-skip): a visible window on
// the hardware adapter, VSync + the 50 Hz pacer for 500 frames, screenshots every 100 frames under
// <exe dir>\fut_tests_tmp\platform_present_loop. Evidence for M3 exit criterion 3 at the backend level.
TEST_CASE("platform: present loop at 50 Hz with vsync in a visible window (hardware)" * doctest::skip()) {
    const fs::path dir = TempDir("platform_present_loop");
    const std::uint32_t errors = RenderLoop(true, 500, 1, 50, dir);
    CHECK(errors == 0);
    const platform::PresentStatistics s = platform::GetPresentStatistics();
    CHECK(s.frames == 500);
    CHECK(s.averageHz >= 49.5);
    CHECK(s.averageHz <= 50.5);
    CHECK(s.slowestWindowHz >= 45.0);
    CHECK(fs::exists(dir / "frame_000500.png"));
    platform::ResetFramePacer();
}
