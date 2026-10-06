// Result of `futurama.exe --check-iso=<path> --result=<file.json>` (shared contract item 2):
//   {"ok":bool,"reason":"..","xdvdfs":bool,"layout":"xiso|redump|unknown","xbe_sha1":"..",
//    "version":"retail_eu|proto_ntsc|unknown","supported":bool,"title":"..","size":<bytes>}
// The exe exits with 0 if the image is supported.
#pragma once

#include "core/l10n.h"

#include <QByteArray>
#include <QString>

namespace fl {

enum class IsoVerdict {
    Supported,           // Xbox image of a supported version (retail EU)
    UnsupportedVersion,  // Xbox image, but another version (prototype, US, unknown XBE)
    NotXboxImage,        // no XDVDFS file system / not readable
    CheckFailed,         // the check itself could not run (exe missing, crashed, no result)
};

struct IsoCheckResult {
    bool ok = false;
    QString reason;
    bool xdvdfs = false;
    QString layout;
    QString xbeSha1;
    QString version;
    bool supported = false;
    QString title;
    qint64 size = 0;
    // Optional fields of the host (tolerated when missing):
    QString reasonCode;     // "reason_code", e.g. "supported"
    LocalizedText message;  // "message": localized explanation for the player
    QString titleId;        // "title_id"
    // Filled by the caller (GameCli):
    int exitCode = -1;
    QString runError;  // non-empty if the check could not run

    IsoVerdict verdict() const;
    QString versionDisplayName() const;  // localized: "Retail Europa", "NTSC-Prototyp", ...
    QString layoutDisplayName() const;   // "XISO", "Redump (komplettes Abbild)", ...
};

bool ParseIsoCheckResult(const QByteArray& json, IsoCheckResult* out, QString* error);

}  // namespace fl
