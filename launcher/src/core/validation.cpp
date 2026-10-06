#include "core/validation.h"

#include "core/ini_document.h"

#include <QDir>
#include <QFileInfo>
#include <QLocale>

#include <algorithm>
#include <cmath>

namespace fl {
namespace {

constexpr int kMinResolutionWidth = 320;
constexpr int kMinResolutionHeight = 240;
constexpr int kMaxResolution = 16384;

ValidationResult Make(Severity severity, IssueCode code, const QString& message) {
    return ValidationResult{severity, code, message};
}

QString NumberText(double v, KeyType type) {
    if (type == KeyType::Int) {
        return QLocale().toString(static_cast<qint64>(std::llround(v)));
    }
    return QLocale().toString(v, 'g', 6);
}

bool IsWholeMultiple(double value, double base, double step) {
    const double q = (value - base) / step;
    return std::fabs(q - std::round(q)) < 1e-6;
}

}  // namespace

std::optional<bool> ParseBoolText(QStringView text) {
    const QString v = text.trimmed().toString().toLower();
    if (v == QLatin1String("1") || v == QLatin1String("true") || v == QLatin1String("yes") ||
        v == QLatin1String("on")) {
        return true;
    }
    if (v == QLatin1String("0") || v == QLatin1String("false") || v == QLatin1String("no") ||
        v == QLatin1String("off")) {
        return false;
    }
    return std::nullopt;
}

std::optional<qint64> ParseIntText(QStringView text) {
    const QString v = text.trimmed().toString();
    if (v.isEmpty()) {
        return std::nullopt;
    }
    bool ok = false;
    const qlonglong n = v.toLongLong(&ok, 0);
    if (!ok) {
        return std::nullopt;
    }
    return static_cast<qint64>(n);
}

std::optional<double> ParseFloatText(QStringView text) {
    const QString v = text.trimmed().toString();
    if (v.isEmpty()) {
        return std::nullopt;
    }
    bool ok = false;
    const double d = QLocale::c().toDouble(v, &ok);
    if (!ok || !std::isfinite(d)) {
        return std::nullopt;
    }
    return d;
}

bool IsAutoResolution(QStringView text) {
    return text.trimmed().compare(QLatin1String("auto"), Qt::CaseInsensitive) == 0;
}

std::optional<QSize> ParseResolutionText(QStringView text) {
    const QStringView t = text.trimmed();
    if (IsAutoResolution(t)) {
        return QSize();
    }
    const qsizetype x = t.indexOf(QLatin1Char('x'), 0, Qt::CaseInsensitive);
    if (x <= 0 || x + 1 >= t.size()) {
        return std::nullopt;
    }
    const QStringView ws = t.left(x).trimmed();
    const QStringView hs = t.mid(x + 1).trimmed();
    auto digitsOnly = [](QStringView s) {
        return std::all_of(s.begin(), s.end(), [](QChar c) { return c.isDigit() && c.unicode() < 128; });
    };
    if (!digitsOnly(ws) || !digitsOnly(hs) || ws.size() > 6 || hs.size() > 6) {
        return std::nullopt;
    }
    return QSize(ws.toInt(), hs.toInt());
}

QString FormatResolution(QSize size) {
    return QString::number(size.width()) + QLatin1Char('x') + QString::number(size.height());
}

bool MatchesChoice(const KeyDef& key, const QString& value) {
    const QString v = value.trimmed();
    QList<ChoiceDef> all = key.choices;
    all += key.hiddenChoices;
    for (const ChoiceDef& c : all) {
        if (c.isRange()) {
            const qsizetype dots = c.value.indexOf(QLatin1String(".."));
            const auto lo = ParseIntText(QStringView(c.value).left(dots));
            const auto hi = ParseIntText(QStringView(c.value).mid(dots + 2));
            const auto n = ParseIntText(v);
            if (lo && hi && n && *n >= *lo && *n <= *hi) {
                return true;
            }
        } else if (c.value.compare(v, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

QString ResolvePath(const QString& path, const QString& baseDir) {
    if (path.trimmed().isEmpty()) {
        return QString();
    }
    QString p = QDir::fromNativeSeparators(path.trimmed());
    if (QDir::isRelativePath(p) && !baseDir.isEmpty()) {
        p = QDir(baseDir).filePath(p);
    }
    return QDir::toNativeSeparators(QDir::cleanPath(p));
}

QString CleanPathInput(const QString& text) {
    QString v = text.trimmed();
    if (v.size() >= 2 && v.front() == QLatin1Char('"') && v.back() == QLatin1Char('"')) {
        v = v.mid(1, v.size() - 2).trimmed();
    }
    return v;
}

ValidationResult ValidateValue(const KeyDef& key, const QString& value, const QString& baseDir) {
    if (!IniDocument::EncodeValue(value)) {
        return Make(Severity::Error, IssueCode::NotRepresentable, T("val.not_representable"));
    }
    const QString v = value.trimmed();
    switch (key.type) {
    case KeyType::Choice:
        if (!MatchesChoice(key, v)) {
            QStringList names;
            for (const ChoiceDef& c : key.choices) {
                names.append(c.value);
            }
            return Make(Severity::Error, IssueCode::NotAChoice,
                        T("val.not_a_choice").arg(v, names.join(QLatin1String(", "))));
        }
        return {};
    case KeyType::Bool:
        if (!ParseBoolText(v)) {
            return Make(Severity::Error, IssueCode::NotABool, T("val.not_a_bool").arg(v));
        }
        return {};
    case KeyType::Int:
    case KeyType::Float: {
        double number = 0.0;
        if (key.type == KeyType::Int) {
            const auto n = ParseIntText(v);
            if (!n) {
                return Make(Severity::Error, IssueCode::NotAnInteger, T("val.not_an_integer").arg(v));
            }
            number = static_cast<double>(*n);
        } else {
            const auto d = ParseFloatText(v);
            if (!d) {
                return Make(Severity::Error, IssueCode::NotANumber, T("val.not_a_number").arg(v));
            }
            number = *d;
        }
        // "range_enforced": false = the host accepts values outside the suggested range (warning only).
        const Severity rangeSeverity = key.rangeEnforced ? Severity::Error : Severity::Warning;
        const QString hint = key.rangeEnforced ? QString() : QLatin1Char(' ') + T("val.range_hint");
        if (key.min && number < *key.min - 1e-9) {
            return Make(rangeSeverity, IssueCode::BelowMin,
                        T("val.below_min").arg(NumberText(*key.min, key.type)) + hint);
        }
        if (key.max && number > *key.max + 1e-9) {
            return Make(rangeSeverity, IssueCode::AboveMax,
                        T("val.above_max").arg(NumberText(*key.max, key.type)) + hint);
        }
        if (key.step && !IsWholeMultiple(number, key.min.value_or(0.0), *key.step)) {
            // The host does not check steps: a warning, never a blocker.
            return Make(
                Severity::Warning, IssueCode::OffStep,
                T("val.off_step")
                        .arg(NumberText(*key.step, key.type), NumberText(key.min.value_or(0.0), key.type)) +
                    hint);
        }
        return {};
    }
    case KeyType::Resolution: {
        const auto size = ParseResolutionText(v);
        if (!size) {
            return Make(Severity::Error, IssueCode::BadResolution, T("val.bad_resolution").arg(v));
        }
        // The schema's min/max apply to each dimension; without them 320x240 .. 16384x16384.
        const int minW = key.min ? static_cast<int>(*key.min) : kMinResolutionWidth;
        const int minH = key.min ? static_cast<int>(*key.min) : kMinResolutionHeight;
        const int maxSide = key.max ? static_cast<int>(*key.max) : kMaxResolution;
        if (size->isValid() && (size->width() < minW || size->height() < minH || size->width() > maxSide ||
                                size->height() > maxSide)) {
            return Make(
                key.rangeEnforced ? Severity::Error : Severity::Warning, IssueCode::ResolutionOutOfRange,
                T("val.resolution_range")
                    .arg(FormatResolution(QSize(minW, minH)), FormatResolution(QSize(maxSide, maxSide))));
        }
        return {};
    }
    case KeyType::PathFile:
    case KeyType::PathDir: {
        if (v.isEmpty()) {
            return {};
        }
        if (v.startsWith(QLatin1Char('"')) || v.endsWith(QLatin1Char('"'))) {
            // The host would take the quotes as part of the path (an INI written by hand or an older launcher).
            return Make(Severity::Error, IssueCode::QuotedPath, T("val.quoted_path"));
        }
        const QFileInfo info(ResolvePath(v, baseDir));
        if (!info.exists()) {
            return key.type == KeyType::PathFile
                       ? Make(Severity::Warning, IssueCode::FileMissing, T("val.file_missing"))
                       : Make(Severity::Warning, IssueCode::DirMissing, T("val.dir_missing"));
        }
        if (key.type == KeyType::PathFile && info.isDir()) {
            return Make(Severity::Error, IssueCode::NotAFile, T("val.not_a_file"));
        }
        if (key.type == KeyType::PathDir && !info.isDir()) {
            return Make(Severity::Error, IssueCode::NotADirectory, T("val.not_a_dir"));
        }
        return {};
    }
    case KeyType::String:
        return {};
    }
    return {};
}

bool ValuesEquivalent(const KeyDef& key, const QString& a, const QString& b) {
    const QString x = a.trimmed();
    const QString y = b.trimmed();
    switch (key.type) {
    case KeyType::Bool: {
        const auto p = ParseBoolText(x);
        const auto q = ParseBoolText(y);
        return p && q ? *p == *q : x == y;
    }
    case KeyType::Int: {
        const auto p = ParseIntText(x);
        const auto q = ParseIntText(y);
        return p && q ? *p == *q : x == y;
    }
    case KeyType::Float: {
        const auto p = ParseFloatText(x);
        const auto q = ParseFloatText(y);
        return p && q ? std::fabs(*p - *q) < 1e-9 : x == y;
    }
    case KeyType::Resolution: {
        const auto p = ParseResolutionText(x);
        const auto q = ParseResolutionText(y);
        return p && q ? *p == *q : x.compare(y, Qt::CaseInsensitive) == 0;
    }
    case KeyType::Choice:
        return x.compare(y, Qt::CaseInsensitive) == 0;
    case KeyType::PathFile:
    case KeyType::PathDir:
        if (x.isEmpty() || y.isEmpty()) {
            return x.isEmpty() && y.isEmpty();
        }
        return QDir::cleanPath(QDir::fromNativeSeparators(x))
                   .compare(QDir::cleanPath(QDir::fromNativeSeparators(y)), Qt::CaseInsensitive) == 0;
    case KeyType::String:
        return a == b;
    }
    return a == b;
}

QString BoolText(bool value, const QString& existing) {
    const QString e = existing.trimmed().toLower();
    if (e == QLatin1String("true") || e == QLatin1String("false")) {
        return value ? QStringLiteral("true") : QStringLiteral("false");
    }
    if (e == QLatin1String("yes") || e == QLatin1String("no")) {
        return value ? QStringLiteral("yes") : QStringLiteral("no");
    }
    if (e == QLatin1String("on") || e == QLatin1String("off")) {
        return value ? QStringLiteral("on") : QStringLiteral("off");
    }
    return value ? QStringLiteral("1") : QStringLiteral("0");
}

}  // namespace fl
