// Display-coupled frame pacing: pure decision logic and arithmetic (pacer_policy.h).
#include "platform/pacer_policy.h"

#include <algorithm>
#include <cmath>

namespace platform::pacing {

// ---- names ----------------------------------------------------------------------------------------------------

const char* ModeName(Mode mode) noexcept {
    switch (mode) {
    case Mode::Auto:
        return "auto";
    case Mode::Vsync:
        return "vsync";
    case Mode::Timer:
        return "timer";
    case Mode::Off:
        return "off";
    }
    return "?";
}

bool ParseMode(std::string_view text, Mode* mode) noexcept {
    char lower[8] = {};
    if (text.size() >= sizeof(lower)) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        lower[i] = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }
    const std::string_view v(lower, text.size());
    Mode m;
    if (v == "auto") {
        m = Mode::Auto;
    } else if (v == "vsync") {
        m = Mode::Vsync;
    } else if (v == "timer") {
        m = Mode::Timer;
    } else if (v == "off") {
        m = Mode::Off;
    } else {
        return false;
    }
    if (mode != nullptr) {
        *mode = m;
    }
    return true;
}

const char* PathName(Path path) noexcept {
    switch (path) {
    case Path::None:
        return "none";
    case Path::VblankLocked:
        return "vblank";
    case Path::DisplayRate:
        return "display-rate";
    case Path::TearingTimer:
        return "tearing-timer";
    case Path::Timer:
        return "timer";
    case Path::Free:
        return "free";
    case Path::Unthrottled:
        return "unthrottled";
    case Path::Headless:
        return "headless";
    case Path::Count:
        break;
    }
    return "?";
}

bool IsVblankPath(Path path) noexcept {
    return path == Path::VblankLocked || path == Path::DisplayRate;
}

bool IsTimerPath(Path path) noexcept {
    return path == Path::TearingTimer || path == Path::Timer || path == Path::Free || path == Path::Headless;
}

const char* VrrStateName(VrrState state) noexcept {
    switch (state) {
    case VrrState::Unknown:
        return "not probed";
    case VrrState::Probing:
        return "probing";
    case VrrState::Confirmed:
        return "confirmed";
    case VrrState::Rejected:
        return "rejected";
    }
    return "?";
}

// ---- refresh rate --------------------------------------------------------------------------------------------

RefreshClass ClassifyRefresh(double refreshHz, double targetHz) noexcept {
    RefreshClass r;
    if (!(refreshHz > 0.0) || !(targetHz > 0.0)) {
        return r;
    }
    r.known = true;
    const double k = std::floor(refreshHz / targetHz + 0.5);
    if (k < 1.0) {
        return r;  // refresh below half the target
    }
    r.k = k > 1000.0 ? 1000u : static_cast<std::uint32_t>(k);
    r.integer = std::fabs(refreshHz / k - targetHz) <= kIntegerTolerance * targetHz;
    return r;
}

// ---- plan selection ------------------------------------------------------------------------------------------

Plan SelectPlan(const PlanInput& in) noexcept {
    Plan p;
    const double target = in.targetHz > 0.0 ? in.targetHz : 60.0;
    p.periodSeconds = 1.0 / target;
    const Environment& e = in.env;
    if (!e.swapChain) {
        p.path = in.mode == Mode::Off ? Path::Unthrottled : Path::Headless;
        p.presentToSwapChain = false;
        p.reason = "no swap chain";
        return p;
    }
    const bool hidden = e.minimized || !e.displayOn || e.sessionLocked;
    p.presentToSwapChain = !hidden;
    const char* hiddenReason = e.minimized ? "window minimized" : !e.displayOn ? "display off" : "session locked";
    if (in.mode == Mode::Off) {
        p.path = Path::Unthrottled;
        p.allowTearing = in.tearingSupported && !in.vsync;
        p.reason = "[render] pacing = off";
        return p;
    }
    if (in.mode == Mode::Timer) {
        p.path = Path::Timer;
        p.allowTearing = in.tearingSupported && !in.vsync;
        p.reason = "[render] pacing = timer";
        return p;
    }
    if (in.mode == Mode::Auto && !in.vsync) {
        p.path = Path::Timer;
        p.allowTearing = in.tearingSupported;
        p.reason = "[render] vsync = 0";
        return p;
    }
    // Nothing reaches the screen: no vblank to couple to (Mode::Vsync included).
    if (hidden) {
        p.path = Path::Free;
        p.reason = hiddenReason;
        return p;
    }
    if (in.mode == Mode::Auto && (!e.foreground || e.displayLimited)) {
        p.path = Path::Free;
        p.reason = e.displayLimited ? "the display holds the pace (vblank waits too long)" : "window not in the foreground";
        return p;
    }
    const RefreshClass rc = ClassifyRefresh(in.refreshHz, target);
    if (rc.integer && rc.k >= 1 && rc.k <= kMaxSyncInterval) {
        p.path = Path::VblankLocked;
        p.syncInterval = rc.k;
        p.k = rc.k;
        p.reason = rc.k == 1 ? "refresh = target rate" : "refresh = integer multiple of the target rate";
        return p;
    }
    const auto displayRate = [&](const char* reason) {
        p.path = Path::DisplayRate;
        p.syncInterval = 1;
        p.k = 1;
        p.periodSeconds = in.refreshHz > 0.0 ? 1.0 / in.refreshHz : 1.0 / target;
        p.reason = reason;
    };
    if (in.mode == Mode::Vsync) {
        displayRate(rc.known ? "[render] pacing = vsync, refresh not a multiple of the target rate"
                             : "[render] pacing = vsync, refresh unknown");
        return p;
    }
    if (in.tearingSupported && in.vrr != VrrState::Rejected) {
        p.path = Path::TearingTimer;
        p.allowTearing = true;
        p.reason = rc.known ? (rc.integer ? "refresh above 4x the target rate, tearing supported"
                                          : "refresh not a multiple of the target rate, tearing supported")
                            : "refresh unknown, tearing supported";
        return p;
    }
    displayRate(in.tearingSupported ? "VRR rejected: one frame per refresh" : "no tearing support: one frame per refresh");
    return p;
}

bool SamePlan(const Plan& a, const Plan& b) noexcept {
    return a.path == b.path && a.syncInterval == b.syncInterval && a.allowTearing == b.allowTearing &&
           a.presentToSwapChain == b.presentToSwapChain && a.k == b.k &&
           std::fabs(a.periodSeconds - b.periodSeconds) <= 1.0e-9;
}

bool PlanHysteresis::Accept(const Plan& current, const Plan& desired, bool immediate) noexcept {
    if (SamePlan(current, desired)) {
        pendingCount_ = 0;
        return false;
    }
    // Presentation resumes (the window was restored from minimized): at once, else the frozen last picture
    // would stay for holdFrames frames (review M3b finding 5).
    const bool resumesPresenting = !current.presentToSwapChain && desired.presentToSwapChain;
    if (immediate || resumesPresenting || current.path == Path::None || current.path != Path::Free ||
        desired.path == Path::Free) {
        pendingCount_ = 0;
        return true;
    }
    if (pendingCount_ != 0 && SamePlan(pending_, desired)) {
        ++pendingCount_;
    } else {
        pending_ = desired;
        pendingCount_ = 1;
    }
    if (pendingCount_ >= holdFrames_) {
        pendingCount_ = 0;
        return true;
    }
    return false;
}

// ---- timer arithmetic ------------------------------------------------------------------------------------------

FrameSchedule::Step FrameSchedule::Next(std::int64_t now, double periodTicks) noexcept {
    Step s;
    if (!(periodTicks > 0.0)) {
        started_ = false;
        s.waitUntil = now;
        s.started = true;
        return s;
    }
    if (!started_ || std::fabs(periodTicks - period_) > 1.0e-6) {
        started_ = true;
        anchor_ = now;
        index_ = 1;
        period_ = periodTicks;
        s.waitUntil = now;
        s.started = true;
        return s;
    }
    const std::int64_t deadline = anchor_ + std::llround(static_cast<double>(index_) * period_);
    if (now - deadline > std::llround(period_)) {
        ++resets_;
        anchor_ = now;
        index_ = 1;
        s.waitUntil = now;
        s.reset = true;
        return s;
    }
    s.waitUntil = deadline;
    s.late = now > deadline;
    // Keep the products exact: re-anchor on the (integer) deadline now and then.
    constexpr std::uint64_t kRebase = 1u << 20;
    if (index_ >= kRebase) {
        anchor_ = deadline;
        index_ = 1;
    } else {
        ++index_;
    }
    return s;
}

void FrameSchedule::Restart(std::int64_t anchor, double periodTicks) noexcept {
    if (!(periodTicks > 0.0)) {
        started_ = false;
        return;
    }
    started_ = true;
    anchor_ = anchor;
    index_ = 1;
    period_ = periodTicks;
}

std::int64_t RelativeDueTime100ns(std::int64_t ticks, std::int64_t qpf) noexcept {
    if (ticks <= 0 || qpf <= 0) {
        return 0;
    }
    const std::int64_t units = ticks / qpf * 10000000 + ticks % qpf * 10000000 / qpf;
    return -std::max<std::int64_t>(units, 1);
}

std::uint32_t VblankWaitTimeoutMs(double periodSeconds) noexcept {
    const double ms = std::ceil(2.0 * periodSeconds * 1000.0);
    if (!(ms >= 4.0)) {
        return 4;
    }
    return ms > 250.0 ? 250u : static_cast<std::uint32_t>(ms);
}

// ---- cadence guard ------------------------------------------------------------------------------------------------

int CadenceGuard::AddDuration(std::uint32_t refreshes, std::uint32_t expected, double visibleSeconds,
                              double refreshSeconds) noexcept {
    if (expected == 0 || stopped_) {
        return 0;
    }
    if (holdoff_ != 0) {
        --holdoff_;
        return 0;
    }
    ++judged_;
    ++samples_;
    bool irregular = refreshes != expected;
    if (irregular && visibleSeconds >= 0.0 && refreshSeconds > 0.0 &&
        std::fabs(visibleSeconds - static_cast<double>(expected) * refreshSeconds) <= 0.5 * refreshSeconds) {
        irregular = false;  // the panel followed the present (VRR): on time although not k fixed refreshes
        ++followed_;
    }
    irregular_ += irregular ? 1 : 0;
    if (irregular_ >= kIrregularLimit) {
        samples_ = irregular_ = 0;
        if (++streak_ > kMaxShifts) {
            stopped_ = true;  // shifting does not help (load, compositor): keep the grid
            return 0;
        }
        holdoff_ = kHoldoff;
        ++shifts_;
        return shifts_ % 2 == 1 ? 1 : -1;
    }
    if (samples_ >= kWindow) {
        samples_ = irregular_ = 0;
        streak_ = 0;  // a clean window: the last shift worked
    }
    return 0;
}

// ---- display-limited detector -------------------------------------------------------------------------------

bool DisplayLimitDetector::AddWait(double waitSeconds, double periodSeconds, bool timedOut) noexcept {
    const bool slow = timedOut || waitSeconds > 1.5 * periodSeconds;
    history_ = static_cast<std::uint8_t>((history_ << 1) | (slow ? 1u : 0u));
    count_ = count_ < 8 ? count_ + 1 : 8;
    std::uint32_t bits = 0;
    for (std::uint8_t h = history_; h != 0; h = static_cast<std::uint8_t>(h & (h - 1))) {
        ++bits;
    }
    if (bits >= 3) {
        ResetWindow();
        return true;
    }
    return false;
}

double DisplayLimitDetector::FireHoldSeconds(double nowSeconds) noexcept {
    if (hold_ <= 0.0 || nowSeconds - lastFire_ > hold_ + 10.0) {
        hold_ = 1.0;
    } else {
        hold_ = std::min(hold_ * 2.0, 8.0);
    }
    lastFire_ = nowSeconds;
    return hold_;
}

// ---- VRR detection -------------------------------------------------------------------------------------------

void VrrDetector::Start(double targetHz, double refreshHz, std::uint32_t warmupFrames) noexcept {
    state_ = VrrState::Probing;
    target_ = targetHz > 0.0 ? targetHz : 60.0;
    refresh_ = refreshHz > 0.0 ? refreshHz : 0.0;
    samples_ = match_ = shared_ = frames_ = 0;
    warmup_ = warmupFrames;
    reason_ = "probing";
}

void VrrDetector::AddInterval(double seconds) noexcept {
    if (state_ != VrrState::Probing || seconds < 0.0 || frames_ < warmup_) {
        return;
    }
    ++samples_;
    if (seconds < 0.00025) {
        ++shared_;
    } else {
        const bool nearTarget = std::fabs(seconds - 1.0 / target_) <= 0.0015;
        bool onGrid = false;
        if (refresh_ > 0.0) {
            const double period = 1.0 / refresh_;
            const double m = std::floor(seconds / period + 0.5);
            onGrid = m >= 1.0 && std::fabs(seconds - m * period) <= 0.0003;
        }
        if (nearTarget && !onGrid) {
            ++match_;
        }
    }
    if (samples_ >= kSamples) {
        Evaluate();
    }
}

void VrrDetector::AddFrame() noexcept {
    if (state_ != VrrState::Probing) {
        return;
    }
    ++frames_;
    if (frames_ >= warmup_ + kMaxFrames && samples_ < kSamples) {
        state_ = VrrState::Rejected;
        reason_ = "too few frame statistics (occluded window or another adapter)";
    }
}

void VrrDetector::Evaluate() noexcept {
    if (shared_ * 10 > samples_) {
        state_ = VrrState::Rejected;
        reason_ = "presents share vblanks (tearing without VRR)";
    } else if (match_ * 10 >= samples_ * 9) {
        state_ = VrrState::Confirmed;
        reason_ = "the display follows the presents";
    } else {
        state_ = VrrState::Rejected;
        reason_ = "visible intervals follow the refresh grid (fixed refresh or composed window)";
    }
}

// ---- statistics ---------------------------------------------------------------------------------------------

void IntervalHistogram::Add(double seconds, double periodSeconds) noexcept {
    const double ratio = periodSeconds > 0.0 ? seconds / periodSeconds : 1.0;
    std::size_t b = 0;
    while (b < kBuckets - 1 && ratio >= kEdges[b]) {
        ++b;
    }
    ++counts_[b];
    ++total_;
}

double IntervalHistogram::OnPeriodPercent() const noexcept {
    return total_ != 0 ? 100.0 * static_cast<double>(counts_[3]) / static_cast<double>(total_) : 0.0;
}

void IntervalHistogram::Reset() noexcept {
    std::fill(std::begin(counts_), std::end(counts_), 0u);
    total_ = 0;
}

void OnScreenTracker::NotePresent(std::uint32_t presentId, std::int64_t qpc, std::uint32_t plannedRefreshes,
                                  std::uint32_t nominalRefreshes) noexcept {
    ring_[next_] = Entry{presentId, qpc, plannedRefreshes, nominalRefreshes, true};
    next_ = (next_ + 1) % kRing;
    ++presents_;
}

void OnScreenTracker::Segment::Add(std::uint32_t refresh, std::int64_t qpc) noexcept {
    if (n == 0) {
        baseRefresh = refresh;
        baseQpc = qpc;
    }
    const double x = static_cast<double>(static_cast<std::uint32_t>(refresh - baseRefresh));
    const double y = static_cast<double>(qpc - baseQpc);
    ++n;
    const double dx = x - meanX;
    meanX += dx / static_cast<double>(n);
    meanY += (y - meanY) / static_cast<double>(n);
    sxx += dx * (x - meanX);
    sxy += dx * (y - meanY);
}

void OnScreenTracker::CloseSegment() noexcept {
    if (segment_.n >= 2) {
        pooledSxx_ += segment_.sxx;
        pooledSxy_ += segment_.sxy;
        ++fitSegments_;
    }
    segment_ = Segment{};
}

const OnScreenTracker::Entry* OnScreenTracker::Find(std::uint32_t id) const noexcept {
    for (std::size_t i = 0; i < kRing; ++i) {
        const Entry& e = ring_[(next_ + kRing - 1 - i) % kRing];  // newest first
        if (e.used && e.id == id) {
            return &e;
        }
    }
    return nullptr;
}

double OnScreenTracker::NoteStatistics(std::uint32_t presentCount, std::uint32_t syncRefreshCount,
                                       std::int64_t syncQpc) noexcept {
    if (haveLast_ && presentCount == lastCount_) {
        return -1.0;  // nothing new became visible
    }
    last_ = LastPair{};
    if (haveLast_ && static_cast<std::int32_t>(presentCount - lastCount_) < 0) {
        haveLast_ = false;  // counter went back (new swap chain): start over
        CloseSegment();
    }
    ++samples_;
    const Entry* e = Find(presentCount);
    if (e != nullptr && syncQpc >= e->qpc) {
        const double ms = static_cast<double>(syncQpc - e->qpc) * 1000.0 / qpf_;
        const auto bucket = static_cast<std::size_t>(ms * 10.0);
        ++latency_[bucket < kLatencyBuckets ? bucket : kLatencyBuckets - 1];
        ++latencySamples_;
    }
    const std::uint32_t expected = e == nullptr ? 0u : e->planned != 0 ? e->planned : e->nominal;
    double interval = -1.0;
    if (haveLast_) {
        const std::uint32_t d = presentCount - lastCount_;
        const std::uint32_t dr = syncRefreshCount - lastRefresh_;
        const std::int64_t dt = syncQpc - lastQpc_;
        // Refresh fit: a new segment when the counter stands still or goes back, after a gap of more than 1 s
        // (minimized, occluded) or when the count differs from what the elapsed time allows by half a refresh or
        // more. Measured on the 120 Hz machine: the counter sometimes lags its time stamp by one refresh for good
        // (4 times in 60 s, the counter-based rate then read 119.80 Hz over 15 s) or for a single sample; both
        // only end a segment instead of bending the fit.
        bool consistent = static_cast<std::int32_t>(dr) > 0 && dt > 0 && static_cast<double>(dt) <= qpf_;
        if (consistent && nominalHz_ > 0.0) {
            const double expectedRefreshes = static_cast<double>(dt) / qpf_ * nominalHz_;
            consistent = std::fabs(expectedRefreshes - static_cast<double>(dr)) < 0.5 + 0.002 * expectedRefreshes;
        }
        if (!consistent) {
            CloseSegment();
        }
        if (d == 1 && dt >= 0 && dr < 100000) {
            ++pairs_;
            ++durations_[dr < kDurationBuckets - 1 ? dr : kDurationBuckets - 1];
            if (e != nullptr && e->planned > 0) {
                ++planned_;
                if (dr == e->planned) {
                    ++onPlan_;
                } else if (dr > e->planned) {
                    ++late_;
                } else {
                    ++early_;
                }
            }
            last_.valid = true;
            last_.refreshes = dr;
            last_.expected = expected;
            interval = static_cast<double>(dt) / qpf_;
            last_.seconds = interval;
        } else if (d >= 2 && d < 1000 && dt >= 0 && dr < 100000) {
            ++jumps_;
            jumpPresents_ += d;
            if (expected != 0 && dr != d * expected) {
                ++jumpsIrregular_;
            }
        }
    }
    segment_.Add(syncRefreshCount, syncQpc);
    ++fitSamples_;
    haveLast_ = true;
    lastCount_ = presentCount;
    lastRefresh_ = syncRefreshCount;
    lastQpc_ = syncQpc;
    return interval;
}

void OnScreenTracker::Reset() noexcept {
    const double qpf = qpf_;
    const double nominal = nominalHz_;
    *this = OnScreenTracker{};
    qpf_ = qpf;
    nominalHz_ = nominal;
}

OnScreenTracker::Result OnScreenTracker::Get() const noexcept {
    Result r;
    r.presents = presents_;
    r.samples = samples_;
    r.pairs = pairs_;
    r.invalid = invalid_;
    std::copy(std::begin(durations_), std::end(durations_), std::begin(r.durations));
    r.planned = planned_;
    r.onPlan = onPlan_;
    r.late = late_;
    r.early = early_;
    r.jumps = jumps_;
    r.jumpPresents = jumpPresents_;
    r.jumpsIrregular = jumpsIrregular_;
    // Pooled slope (QPC ticks per refresh) of the closed segments and the open one.
    const double sxx = pooledSxx_ + (segment_.n >= 2 ? segment_.sxx : 0.0);
    const double sxy = pooledSxy_ + (segment_.n >= 2 ? segment_.sxy : 0.0);
    r.refreshSamples = fitSamples_;
    r.refreshSegments = fitSegments_ + (segment_.n >= 2 ? 1u : 0u);
    r.measuredHz = sxx > 0.0 && sxy > 0.0 ? qpf_ / (sxy / sxx) : 0.0;
    r.latencySamples = latencySamples_;
    if (latencySamples_ != 0) {
        const std::uint64_t median = (latencySamples_ + 1) / 2;
        const std::uint64_t p99 = (latencySamples_ * 99 + 99) / 100;
        std::uint64_t seen = 0;
        bool haveMedian = false;
        for (std::size_t b = 0; b < kLatencyBuckets; ++b) {
            seen += latency_[b];
            const double upper = static_cast<double>(b + 1) / 10.0;  // upper edge of the bucket
            if (!haveMedian && seen >= median) {
                r.latencyMedianMs = upper;
                haveMedian = true;
            }
            if (seen >= p99) {
                r.latencyP99Ms = upper;
                break;
            }
        }
    }
    return r;
}

}  // namespace platform::pacing
