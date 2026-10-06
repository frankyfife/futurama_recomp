#include "core/gpu_info.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace fl {

QSize GpuOutput::nativeSize() const {
    if (rect.isValid() && !rect.isEmpty()) {
        return rect.size();
    }
    return modes.isEmpty() ? QSize() : modes.first();
}

int GpuInfo::maxMsaa() const {
    int best = 0;
    for (int m : msaa) {
        best = std::max(best, m);
    }
    return best;
}

const GpuAdapter* GpuInfo::adapter() const {
    if (defaultAdapter >= 0 && defaultAdapter < adapters.size()) {
        return &adapters[defaultAdapter];
    }
    return adapters.isEmpty() ? nullptr : &adapters.first();
}

const GpuOutput* GpuInfo::primaryOutput() const {
    for (const GpuOutput& o : outputs) {
        if (o.primary) {
            return &o;
        }
    }
    return outputs.isEmpty() ? nullptr : &outputs.first();
}

const GpuOutput* GpuInfo::findOutput(const QString& id) const {
    const QString wanted = id.trimmed();
    if (wanted.isEmpty()) {
        return nullptr;
    }
    for (const GpuOutput& o : outputs) {
        if (o.device.compare(wanted, Qt::CaseInsensitive) == 0 ||
            o.name.compare(wanted, Qt::CaseInsensitive) == 0) {
            return &o;
        }
    }
    return nullptr;
}

bool ParseGpuInfo(const QByteArray& json, GpuInfo* out, QString* error) {
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (doc.isNull() || !doc.isObject()) {
        if (error != nullptr) {
            *error = QStringLiteral("query-gpu result is not a JSON object: ") + parseError.errorString();
        }
        return false;
    }
    const QJsonObject root = doc.object();
    GpuInfo info;
    for (const QJsonValue& v : root.value(QLatin1String("adapters")).toArray()) {
        const QJsonObject o = v.toObject();
        GpuAdapter a;
        a.name = o.value(QLatin1String("name")).toString();
        a.vendorId = static_cast<quint32>(o.value(QLatin1String("vendor_id")).toDouble());
        a.dedicatedMb = static_cast<qint64>(o.value(QLatin1String("dedicated_mb")).toDouble());
        info.adapters.append(a);
    }
    for (const QJsonValue& v : root.value(QLatin1String("outputs")).toArray()) {
        const QJsonObject o = v.toObject();
        GpuOutput output;
        output.name = o.value(QLatin1String("name")).toString();
        output.device = o.value(QLatin1String("device")).toString();
        output.primary = o.value(QLatin1String("primary")).toBool(false);
        output.dpi = o.value(QLatin1String("dpi")).toInt(0);
        const QJsonArray r = o.value(QLatin1String("rect")).toArray();
        if (r.size() == 4) {
            output.rect = QRect(r[0].toInt(), r[1].toInt(), r[2].toInt(), r[3].toInt());
        }
        const QJsonObject refresh = o.value(QLatin1String("refresh")).toObject();
        output.refreshNum = static_cast<qint64>(refresh.value(QLatin1String("num")).toDouble());
        output.refreshDen = static_cast<qint64>(refresh.value(QLatin1String("den")).toDouble());
        for (const QJsonValue& m : o.value(QLatin1String("modes")).toArray()) {
            const QJsonArray wh = m.toArray();
            if (wh.size() >= 2) {
                const QSize size(wh[0].toInt(), wh[1].toInt());
                if (size.width() > 0 && size.height() > 0 && !output.modes.contains(size)) {
                    output.modes.append(size);
                }
            }
        }
        std::sort(output.modes.begin(), output.modes.end(), [](QSize a, QSize b) {
            return a.width() != b.width() ? a.width() > b.width() : a.height() > b.height();
        });
        if (output.name.isEmpty()) {
            output.name = output.device;
        }
        info.outputs.append(output);
    }
    for (const QJsonValue& v : root.value(QLatin1String("msaa")).toArray()) {
        const int n = v.toInt();
        if (n >= 1 && !info.msaa.contains(n)) {
            info.msaa.append(n);
        }
    }
    std::sort(info.msaa.begin(), info.msaa.end());
    info.maxAnisotropy = root.value(QLatin1String("max_anisotropy")).toInt(0);
    info.tearing = root.value(QLatin1String("tearing")).toBool(false);
    info.vrrHint = root.value(QLatin1String("vrr_hint")).toString();
    info.defaultAdapter = root.value(QLatin1String("default_adapter")).toInt(0);
    const QJsonValue hint = root.value(QLatin1String("vrr")).toObject().value(QLatin1String("hint"));
    if (hint.isObject()) {
        info.vrrHintText.de = hint.toObject().value(QLatin1String("de")).toString();
        info.vrrHintText.en = hint.toObject().value(QLatin1String("en")).toString();
    } else if (hint.isString()) {
        info.vrrHintText.de = info.vrrHintText.en = hint.toString();
    }
    info.valid = true;
    *out = std::move(info);
    return true;
}

}  // namespace fl
