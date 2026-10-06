#include "core/presets.h"

#include "core/validation.h"

#include <QSize>

#include <algorithm>
#include <cmath>

namespace fl {
namespace {

// platform::FitAspectRect for 4:3 (ui/screen_info.cpp FitAspect43; core cannot use the widgets library).
QSize Fit43(QSize size) {
    const qint64 w = size.width();
    const qint64 h = size.height();
    if (w <= 0 || h <= 0) {
        return QSize();
    }
    if (w * 3 <= h * 4) {
        return QSize(static_cast<int>(w), static_cast<int>(std::min<qint64>(h, (w * 3 + 2) / 4)));
    }
    return QSize(static_cast<int>(std::min<qint64>(w, (h * 4 + 1) / 3)), static_cast<int>(h));
}

// The non-numeric choice that means "off" (or the first choice).
QString OffChoice(const KeyDef& key) {
    for (const ChoiceDef& c : key.choices) {
        if (c.value.compare(QLatin1String("off"), Qt::CaseInsensitive) == 0 ||
            c.value == QLatin1String("0") || c.value == QLatin1String("1")) {
            return c.value;
        }
    }
    return key.choices.isEmpty() ? QString() : key.choices.first().value;
}

void Add(PresetValues& out, const ConfigSchema& schema, const char* fullName, const QString& value) {
    if (schema.find(QLatin1String(fullName)) != nullptr && !value.isNull()) {
        out.values.append({QLatin1String(fullName), value});
    }
}

QString ClampFloat(const KeyDef& key, double wanted) {
    double v = wanted;
    if (key.max) {
        v = std::min(v, *key.max);
    }
    if (key.min) {
        v = std::max(v, *key.min);
    }
    return FormatFloat(v);
}

QString BoolOrChoice(const KeyDef* key, bool on) {
    if (key == nullptr) {
        return QString();
    }
    if (key->type == KeyType::Choice) {
        for (const ChoiceDef& c : key->choices) {
            const auto b = ParseBoolText(c.value);
            if (b && *b == on) {
                return c.value;
            }
        }
        return QString();
    }
    return BoolText(on);
}

}  // namespace

qint64 BackBufferMemoryMb(QSize output, double scale, int samples, qint64* largestMb) {
    const double pixels = static_cast<double>(output.width()) * output.height();
    const double internal = pixels * scale * scale;
    const int s = std::max(samples, 1);
    const double perBuffer = internal * 4.0 * s;  // BGRA8 (and D24S8) x samples
    double total = perBuffer * 3.0;               // 2 back buffers + depth
    if (s > 1) {
        total += internal * 4.0;  // resolve texture
    }
    if (scale > 1.0) {
        total += pixels * 4.0;  // output image
    }
    constexpr double kMb = 1024.0 * 1024.0;
    if (largestMb != nullptr) {
        *largestMb = static_cast<qint64>(std::ceil(perBuffer / kMb));
    }
    return static_cast<qint64>(std::ceil(total / kMb));
}

bool FitsVideoMemory(QSize output, double scale, int samples, qint64 dedicatedMb) {
    if (dedicatedMb <= 0 || !output.isValid()) {
        return false;
    }
    qint64 largest = 0;
    const qint64 total = BackBufferMemoryMb(output, scale, samples, &largest);
    // D3D11_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_A_TERM (128) / _B_TERM (0.25): max(128, min(VRAM/4, 2048)).
    const qint64 perResource = std::max<qint64>(128, std::min<qint64>(dedicatedMb / 4, 2048));
    return largest <= perResource && total <= dedicatedMb / 2;
}

QSize PresetOutputSize(const GpuInfo& gpu) {
    QSize best;
    for (const GpuOutput& o : gpu.outputs) {
        const QSize s = Fit43(o.nativeSize());
        if (s.isValid() && (!best.isValid() || qint64(s.width()) * s.height() > qint64(best.width()) * best.height())) {
            best = s;
        }
    }
    return best.isValid() ? best : QSize(2880, 2160);
}

QString PresetValues::value(const QString& fullName) const {
    for (const auto& [k, v] : values) {
        if (k == fullName) {
            return v;
        }
    }
    return QString();
}

QString PresetId(QualityPreset preset) {
    switch (preset) {
    case QualityPreset::Classic:
        return QStringLiteral("classic");
    case QualityPreset::Standard:
        return QStringLiteral("standard");
    case QualityPreset::Maximum:
        return QStringLiteral("maximum");
    case QualityPreset::Custom:
        return QStringLiteral("custom");
    }
    return QStringLiteral("custom");
}

QString ClampNumericChoice(const KeyDef& key, int wanted, const QList<int>& allowed) {
    int best = 0;
    QString bestValue;
    for (const ChoiceDef& c : key.choices) {
        bool ok = false;
        const int n = c.value.toInt(&ok);
        if (!ok || n <= 1 || n > wanted) {
            continue;
        }
        if (!allowed.isEmpty() && !allowed.contains(n)) {
            continue;
        }
        if (n > best) {
            best = n;
            bestValue = c.value;
        }
    }
    return best > 0 ? bestValue : OffChoice(key);
}

PresetValues ComputePreset(QualityPreset preset, const ConfigSchema& schema, const GpuInfo& gpu) {
    PresetValues out;
    if (preset == QualityPreset::Custom) {
        return out;
    }
    const KeyDef* resolution = schema.find(QLatin1String(kKeyResolution));
    const KeyDef* scale = schema.find(QLatin1String(kKeyRenderScale));
    const KeyDef* msaa = schema.find(QLatin1String(kKeyMsaa));
    const KeyDef* a2c = schema.find(QLatin1String(kKeyAlphaToCoverage));
    const KeyDef* aniso = schema.find(QLatin1String(kKeyAnisotropy));
    QList<int> anisoAllowed;
    if (gpu.maxAnisotropy > 0) {
        for (int n = 2; n <= gpu.maxAnisotropy; n *= 2) {
            anisoAllowed.append(n);
        }
    }
    if (preset == QualityPreset::Classic) {
        Add(out, schema, kKeyResolution, resolution != nullptr ? QStringLiteral("640x480") : QString());
        Add(out, schema, kKeyRenderScale, scale != nullptr ? ClampFloat(*scale, 1.0) : QString());
        Add(out, schema, kKeyMsaa, msaa != nullptr ? OffChoice(*msaa) : QString());
        Add(out, schema, kKeyAnisotropy, aniso != nullptr ? OffChoice(*aniso) : QString());
        return out;
    }
    const bool maximum = preset == QualityPreset::Maximum;
    // Steps from the best down (finding 1 and 21): {render scale, MSAA}; the first that fits the video memory wins.
    struct Step {
        double scale;
        int msaa;
    };
    const QList<Step> steps = maximum ? QList<Step>{{2.0, 8}, {2.0, 4}, {1.0, 8}, {1.0, 4}, {1.0, 2}, {1.0, 1}}
                                      : QList<Step>{{1.0, 4}, {1.0, 2}, {1.0, 1}};
    const QSize size = PresetOutputSize(gpu);
    const GpuAdapter* adapter = gpu.adapter();
    const qint64 vram = adapter != nullptr ? adapter->dedicatedMb : 0;
    Step chosen = steps.last();
    bool limited = false;
    for (const Step& step : steps) {
        // The sample count the game will really use (the GPU's support, the schema's choices).
        const QString msaaText = msaa != nullptr ? ClampNumericChoice(*msaa, step.msaa, gpu.msaa) : QString();
        const int samples = std::max<int>(1, msaaText.toInt());
        const double s = scale != nullptr ? std::clamp(step.scale, scale->min.value_or(1.0), scale->max.value_or(2.0))
                                          : 1.0;
        if (vram <= 0) {
            if (s <= 1.0) {  // unknown memory: the game steps down itself if needed, but no supersampling here
                chosen = step;
                break;
            }
            limited = true;
            continue;
        }
        if (FitsVideoMemory(size, s, samples, vram)) {
            chosen = step;
            break;
        }
        limited = true;
    }
    Add(out, schema, kKeyResolution, resolution != nullptr ? QStringLiteral("auto") : QString());
    Add(out, schema, kKeyRenderScale, scale != nullptr ? ClampFloat(*scale, chosen.scale) : QString());
    const QString msaaValue = msaa != nullptr ? ClampNumericChoice(*msaa, chosen.msaa, gpu.msaa) : QString();
    Add(out, schema, kKeyMsaa, msaaValue);
    Add(out, schema, kKeyAlphaToCoverage, BoolOrChoice(a2c, true));
    Add(out, schema, kKeyAnisotropy,
        aniso != nullptr ? ClampNumericChoice(*aniso, 16, anisoAllowed) : QString());
    // Review M3c finding 3: Maximum averages the edges on linear light; Standard stays gamma like the Xbox.
    Add(out, schema, kKeyLinearAa, BoolOrChoice(schema.find(QLatin1String(kKeyLinearAa)), maximum));
    out.videoMemoryMb =
        BackBufferMemoryMb(size, scale != nullptr ? chosen.scale : 1.0, std::max(1, msaaValue.toInt()));
    out.limitedByMemory = limited;
    return out;
}

QualityPreset DetectPreset(const ConfigSchema& schema, const GpuInfo& gpu,
                           const std::function<QString(const KeyDef&)>& current) {
    for (QualityPreset preset : {QualityPreset::Classic, QualityPreset::Standard, QualityPreset::Maximum}) {
        const PresetValues values = ComputePreset(preset, schema, gpu);
        if (values.values.isEmpty()) {
            continue;
        }
        bool all = true;
        for (const auto& [name, v] : values.values) {
            const KeyDef* key = schema.find(name);
            if (key == nullptr || !ValuesEquivalent(*key, current(*key), v)) {
                all = false;
                break;
            }
        }
        if (all) {
            return preset;
        }
    }
    return QualityPreset::Custom;
}

}  // namespace fl
