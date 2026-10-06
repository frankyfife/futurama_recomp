#include "core/iso_check.h"

#include "core/l10n.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace fl {

IsoVerdict IsoCheckResult::verdict() const {
    if (!runError.isEmpty()) {
        return IsoVerdict::CheckFailed;
    }
    if (!xdvdfs) {
        return IsoVerdict::NotXboxImage;
    }
    return supported ? IsoVerdict::Supported : IsoVerdict::UnsupportedVersion;
}

QString IsoCheckResult::versionDisplayName() const {
    if (version == QLatin1String("retail_eu")) {
        return T("iso.version.retail_eu");
    }
    if (version == QLatin1String("proto_ntsc")) {
        return T("iso.version.proto_ntsc");
    }
    return version.isEmpty() || version == QLatin1String("unknown") ? T("iso.version.unknown") : version;
}

QString IsoCheckResult::layoutDisplayName() const {
    if (layout == QLatin1String("xiso")) {
        return T("iso.layout.xiso");
    }
    if (layout == QLatin1String("redump")) {
        return T("iso.layout.redump");
    }
    return T("iso.layout.unknown");
}

bool ParseIsoCheckResult(const QByteArray& json, IsoCheckResult* out, QString* error) {
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (doc.isNull() || !doc.isObject()) {
        if (error != nullptr) {
            *error = QStringLiteral("check-iso result is not a JSON object: ") + parseError.errorString();
        }
        return false;
    }
    const QJsonObject o = doc.object();
    IsoCheckResult r;
    r.ok = o.value(QLatin1String("ok")).toBool(false);
    r.reason = o.value(QLatin1String("reason")).toString();
    r.xdvdfs = o.value(QLatin1String("xdvdfs")).toBool(false);
    r.layout = o.value(QLatin1String("layout")).toString();
    r.xbeSha1 = o.value(QLatin1String("xbe_sha1")).toString();
    r.version = o.value(QLatin1String("version")).toString();
    r.supported = o.value(QLatin1String("supported")).toBool(false);
    r.title = o.value(QLatin1String("title")).toString();
    r.size = static_cast<qint64>(o.value(QLatin1String("size")).toDouble());
    r.reasonCode = o.value(QLatin1String("reason_code")).toString();
    const QJsonValue message = o.value(QLatin1String("message"));
    if (message.isObject()) {
        r.message.de = message.toObject().value(QLatin1String("de")).toString();
        r.message.en = message.toObject().value(QLatin1String("en")).toString();
    } else if (message.isString()) {
        r.message.de = r.message.en = message.toString();
    }
    r.titleId = o.value(QLatin1String("title_id")).toString();
    *out = r;
    return true;
}

}  // namespace fl
