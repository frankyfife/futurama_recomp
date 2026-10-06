// GPU and display capabilities from `futurama.exe --query-gpu=<file.json>` (shared contract item 3):
//   {"adapters":[{"name":"..","vendor_id":..,"dedicated_mb":..}],
//    "outputs":[{"name":"..","device":"\\\\.\\DISPLAY1","primary":bool,"rect":[x,y,w,h],
//                "refresh":{"num":..,"den":..},"modes":[[w,h]...]}],
//    "msaa":[1,2,4,8],"max_anisotropy":16,"tearing":bool,"vrr_hint":".."}
#pragma once

#include "core/l10n.h"

#include <QByteArray>
#include <QList>
#include <QRect>
#include <QSize>
#include <QString>

namespace fl {

struct GpuAdapter {
    QString name;
    quint32 vendorId = 0;
    qint64 dedicatedMb = 0;
};

struct GpuOutput {
    QString name;    // friendly monitor name (or the device name)
    QString device;  // GDI device name, e.g. \\.\DISPLAY1
    bool primary = false;
    QRect rect;  // desktop coordinates in physical pixels
    qint64 refreshNum = 0;
    qint64 refreshDen = 0;
    QList<QSize> modes;  // distinct sizes, largest first
    int dpi = 0;         // effective DPI (optional "dpi" of query-gpu; 96 = 100 % scaling), 0 = unknown

    double refreshHz() const { return refreshDen > 0 ? static_cast<double>(refreshNum) / refreshDen : 0.0; }
    QSize nativeSize() const;  // rect size, else the largest mode
};

struct GpuInfo {
    bool valid = false;
    QList<GpuAdapter> adapters;
    QList<GpuOutput> outputs;
    QList<int> msaa;        // supported sample counts (1 = off)
    int maxAnisotropy = 0;  // 0 = unknown
    bool tearing = false;
    QString vrrHint;
    int defaultAdapter = 0;     // "default_adapter" (index into adapters)
    LocalizedText vrrHintText;  // "vrr": {"hint": {"de", "en"}} (optional)

    const GpuAdapter* adapter() const;  // the default adapter, else the first one

    int maxMsaa() const;  // largest supported count, 0 = unknown
    const GpuOutput* primaryOutput() const;
    // Output whose device or name equals `id` (case-insensitive), else nullptr.
    const GpuOutput* findOutput(const QString& id) const;
};

// Parses the query-gpu JSON. Returns false with `error` if it is not a JSON object.
bool ParseGpuInfo(const QByteArray& json, GpuInfo* out, QString* error);

}  // namespace fl
