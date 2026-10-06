// Refresh rate of a monitor (pacer_display.h).
#include "platform/pacer_display.h"

#include <cstring>
#include <cwchar>
#include <vector>

namespace platform {
namespace {

void ToUtf8(const wchar_t* text, char* out, std::size_t size) noexcept {
    out[0] = 0;
    if (text == nullptr || text[0] == 0) {
        return;
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, out, static_cast<int>(size), nullptr, nullptr);
    if (n <= 0) {
        out[0] = 0;
    }
    out[size - 1] = 0;
}

// QueryDisplayConfig path of the monitor with GDI name `device`: signal rate of its target mode.
bool QueryConfigTiming(const wchar_t* device, DisplayTiming* t) noexcept {
    try {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths;
        std::vector<DISPLAYCONFIG_MODE_INFO> modes;
        LONG rc = ERROR_INSUFFICIENT_BUFFER;
        for (int attempt = 0; attempt < 4 && rc == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
            UINT32 pathCount = 0, modeCount = 0;
            if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) {
                return false;
            }
            paths.resize(pathCount);
            modes.resize(modeCount);
            rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
            paths.resize(pathCount);
            modes.resize(modeCount);
        }
        if (rc != ERROR_SUCCESS) {
            return false;
        }
        for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
            source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            source.header.size = sizeof(source);
            source.header.adapterId = path.sourceInfo.adapterId;
            source.header.id = path.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
                std::wcscmp(source.viewGdiDeviceName, device) != 0) {
                continue;
            }
            const UINT32 index = path.targetInfo.modeInfoIdx;
            DISPLAYCONFIG_RATIONAL rate{};
            if (index != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && index < modes.size() &&
                modes[index].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) {
                rate = modes[index].targetMode.targetVideoSignalInfo.vSyncFreq;
                t->source = "qdc signal";
            }
            if (rate.Numerator == 0 || rate.Denominator == 0) {
                rate = path.targetInfo.refreshRate;
                t->source = "qdc path";
            }
            DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
            target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            target.header.size = sizeof(target);
            target.header.adapterId = path.targetInfo.adapterId;
            target.header.id = path.targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS) {
                ToUtf8(target.monitorFriendlyDeviceName, t->name, sizeof(t->name));
            }
            if (rate.Numerator == 0 || rate.Denominator == 0) {
                t->source = "none";
                return false;
            }
            t->numerator = rate.Numerator;
            t->denominator = rate.Denominator;
            t->hz = static_cast<double>(rate.Numerator) / static_cast<double>(rate.Denominator);
            t->valid = true;
            return true;
        }
        return false;
    } catch (...) {
        return false;  // std::vector allocation failure
    }
}

}  // namespace

DisplayTiming QueryDisplayTiming(void* monitor) noexcept {
    DisplayTiming t;
    HMONITOR m = static_cast<HMONITOR>(monitor);
    if (m == nullptr) {
        m = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    }
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (m == nullptr || !GetMonitorInfoW(m, &mi)) {
        return t;
    }
    ToUtf8(mi.szDevice, t.device, sizeof(t.device));
    if (QueryConfigTiming(mi.szDevice, &t)) {
        return t;
    }
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    // dmDisplayFrequency 0 or 1 = "the hardware default", no usable value.
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) {
        t.numerator = dm.dmDisplayFrequency;
        t.denominator = 1;
        t.hz = static_cast<double>(dm.dmDisplayFrequency);
        t.source = "gdi";
        t.valid = true;
    }
    return t;
}

}  // namespace platform
