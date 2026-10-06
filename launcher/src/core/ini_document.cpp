#include "core/ini_document.h"

#include "core/l10n.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStringDecoder>

namespace fl {
namespace {

// Host Trim(): ' ', '\t', '\r', '\n'.
bool IsBlank(QChar c) {
    return c == QLatin1Char(' ') || c == QLatin1Char('\t') || c == QLatin1Char('\r') ||
           c == QLatin1Char('\n');
}

bool IsSpaceOrTab(QChar c) {
    return c == QLatin1Char(' ') || c == QLatin1Char('\t');
}

QString AsciiLower(QStringView s) {
    QString out = s.toString();
    for (QChar& c : out) {
        if (c >= QLatin1Char('A') && c <= QLatin1Char('Z')) {
            c = QChar(c.unicode() + ('a' - 'A'));
        }
    }
    return out;
}

QStringView TrimView(QStringView s) {
    qsizetype b = 0;
    qsizetype e = s.size();
    while (b < e && IsBlank(s[b])) {
        ++b;
    }
    while (e > b && IsBlank(s[e - 1])) {
        --e;
    }
    return s.mid(b, e - b);
}

// Simulates the host on a line "k = <text>" and returns the value it reads.
QString HostReadsValue(const QString& text) {
    const QString line = QStringLiteral("k = ") + text;
    QStringView view = TrimView(line);
    bool quoted = false;
    for (qsizetype i = 0; i < view.size(); ++i) {
        const QChar c = view[i];
        if (c == QLatin1Char('"')) {
            quoted = !quoted;
        } else if (!quoted && (c == QLatin1Char(';') || c == QLatin1Char('#')) && i > 0 &&
                   IsSpaceOrTab(view[i - 1])) {
            view = TrimView(view.left(i));
            break;
        }
    }
    const qsizetype eq = view.indexOf(QLatin1Char('='));
    return eq < 0 ? QString() : IniDocument::DecodeValue(view.mid(eq + 1));
}

}  // namespace

QString IniDocument::DecodeValue(QStringView raw) {
    QStringView v = TrimView(raw);
    if (v.size() >= 2 && v.front() == QLatin1Char('"') && v.back() == QLatin1Char('"')) {
        v = v.mid(1, v.size() - 2);
    }
    return v.toString();
}

std::optional<QString> IniDocument::EncodeValue(const QString& value) {
    if (value.contains(QLatin1Char('\n')) || value.contains(QLatin1Char('\r'))) {
        return std::nullopt;
    }
    if (HostReadsValue(value) == value) {
        return value;
    }
    const QString quoted = QLatin1Char('"') + value + QLatin1Char('"');
    if (HostReadsValue(quoted) == value) {
        return quoted;
    }
    return std::nullopt;
}

QString IniDocument::NormalizeName(QStringView name) {
    return AsciiLower(TrimView(name));
}

IniDocument::Line IniDocument::ParseLine(const QString& text, const QString& currentSection) {
    Line line;
    line.text = text;
    line.section = currentSection;
    const qsizetype n = text.size();
    qsizetype b = 0;
    while (b < n && IsBlank(text[b])) {
        ++b;
    }
    qsizetype e = n;
    while (e > b && IsBlank(text[e - 1])) {
        --e;
    }
    if (b == e) {
        line.kind = Kind::Blank;
        return line;
    }
    if (text[b] == QLatin1Char(';') || text[b] == QLatin1Char('#')) {
        line.kind = Kind::Comment;
        return line;
    }
    // Inline comment (host StripInlineComment on the trimmed line).
    qsizetype contentEnd = e;
    bool quoted = false;
    for (qsizetype i = b; i < e; ++i) {
        const QChar c = text[i];
        if (c == QLatin1Char('"')) {
            quoted = !quoted;
        } else if (!quoted && (c == QLatin1Char(';') || c == QLatin1Char('#')) && i > b &&
                   IsSpaceOrTab(text[i - 1])) {
            contentEnd = i;
            while (contentEnd > b && IsBlank(text[contentEnd - 1])) {
                --contentEnd;
            }
            break;
        }
    }
    if (text[b] == QLatin1Char('[')) {
        if (text[contentEnd - 1] != QLatin1Char(']') || contentEnd - b < 2) {
            line.kind = Kind::Invalid;
            return line;
        }
        line.kind = Kind::Section;
        line.section = AsciiLower(TrimView(QStringView(text).mid(b + 1, contentEnd - b - 2)));
        return line;
    }
    const qsizetype eq = text.indexOf(QLatin1Char('='), b);
    if (eq < 0 || eq >= contentEnd) {
        line.kind = Kind::Invalid;
        return line;
    }
    line.kind = Kind::KeyValue;
    line.key = AsciiLower(TrimView(QStringView(text).mid(b, eq - b)));
    qsizetype vs = eq + 1;
    while (vs < contentEnd && IsBlank(text[vs])) {
        ++vs;
    }
    line.valueStart = vs;
    line.valueEnd = contentEnd;
    return line;
}

void IniDocument::reparse() {
    const bool keepUtf8Problem = badUtf8_;
    problems_.clear();
    if (keepUtf8Problem) {
        problems_.append({0, QStringLiteral("file is not valid UTF-8 (saving replaces the invalid bytes)"),
                          ProblemCode::InvalidUtf8});
    }
    QString section;
    for (qsizetype i = 0; i < lines_.size(); ++i) {
        Line parsed = ParseLine(lines_[i].text, section);
        if (parsed.kind == Kind::Section) {
            section = parsed.section;
        } else if (parsed.kind == Kind::Invalid) {
            const QStringView trimmed = TrimView(parsed.text);
            const bool header = trimmed.startsWith(QLatin1Char('['));
            problems_.append({static_cast<int>(i + 1),
                              header ? QStringLiteral("bad section header") : QStringLiteral("expected key = value"),
                              header ? ProblemCode::BadSectionHeader : ProblemCode::ExpectedKeyValue});
        } else if (parsed.kind == Kind::KeyValue && parsed.key.isEmpty()) {
            problems_.append({static_cast<int>(i + 1), QStringLiteral("empty key"), ProblemCode::EmptyKey});
        }
        lines_[i] = std::move(parsed);
    }
}

void IniDocument::parse(const QByteArray& input) {
    QByteArray bytes = input;
    badUtf8_ = false;
    bom_ = bytes.startsWith("\xEF\xBB\xBF");
    if (bom_) {
        bytes.remove(0, 3);
    }
    if (bytes.contains("\r\n")) {
        eol_ = QStringLiteral("\r\n");
    } else if (bytes.contains('\n')) {
        eol_ = QStringLiteral("\n");
    } else {
        eol_ = QStringLiteral("\r\n");
    }
    QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString text = decoder.decode(bytes);
    const bool badUtf8 = decoder.hasError();
    trailingNewline_ = text.isEmpty() || text.endsWith(QLatin1Char('\n'));
    QStringList raw = text.split(QLatin1Char('\n'));
    if (trailingNewline_ && !raw.isEmpty()) {
        raw.removeLast();
    }
    lines_.clear();
    lines_.reserve(raw.size());
    for (QString& l : raw) {
        if (l.endsWith(QLatin1Char('\r'))) {
            l.chop(1);
        }
        Line line;
        line.text = std::move(l);
        lines_.append(std::move(line));
    }
    reparse();
    badUtf8_ = badUtf8;
    if (badUtf8) {
        problems_.prepend({0, QStringLiteral("file is not valid UTF-8 (saving replaces the invalid bytes)"),
                           ProblemCode::InvalidUtf8});
    }
}

bool IniDocument::hasInvalidUtf8() const {
    return badUtf8_;
}

QString IniDocument::ProblemText(const Problem& problem) {
    switch (problem.code) {
    case ProblemCode::BadSectionHeader:
        return T("ini.problem.bad_section");
    case ProblemCode::ExpectedKeyValue:
        return T("ini.problem.expected_key_value");
    case ProblemCode::EmptyKey:
        return T("ini.problem.empty_key");
    case ProblemCode::InvalidUtf8:
        return T("ini.problem.invalid_utf8");
    }
    return problem.message;
}

QByteArray IniDocument::serialize() const {
    QString text;
    for (qsizetype i = 0; i < lines_.size(); ++i) {
        text += lines_[i].text;
        if (i + 1 < lines_.size() || trailingNewline_) {
            text += eol_;
        }
    }
    QByteArray out = text.toUtf8();
    if (bom_) {
        out.prepend("\xEF\xBB\xBF");
    }
    return out;
}

bool IniDocument::load(const QString& path, QString* error) {
    QFile file(path);
    if (!file.exists()) {
        parse(QByteArray());
        existed_ = false;
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = file.errorString();
        }
        return false;
    }
    parse(file.readAll());
    existed_ = true;
    return true;
}

bool IniDocument::save(const QString& path, QString* error) const {
    // Review M3c finding 13: localized messages with a hint instead of Qt's English error strings.
    const QFileInfo info(path);
    const QString native = QDir::toNativeSeparators(info.absoluteFilePath());
    if (!QDir().mkpath(info.absolutePath())) {
        if (error != nullptr) {
            *error = T("save.cannot_create_dir").arg(QDir::toNativeSeparators(info.absolutePath())) +
                     QLatin1Char(' ') + T("save.hint");
        }
        return false;
    }
    if (info.exists() && !info.isWritable()) {
        if (error != nullptr) {
            *error = T("save.read_only").arg(native) + QLatin1Char(' ') + T("save.hint");
        }
        return false;
    }
    QSaveFile file(path);
    auto fail = [&](const QString& detail) {
        if (error != nullptr) {
            *error = T("save.write_failed").arg(native, detail) + QLatin1Char(' ') + T("save.hint");
        }
        return false;
    };
    if (!file.open(QIODevice::WriteOnly)) {
        return fail(file.errorString());
    }
    const QByteArray data = serialize();
    if (file.write(data) != data.size()) {
        const QString detail = file.errorString();
        file.cancelWriting();
        return fail(detail);
    }
    if (!file.commit()) {
        return fail(file.errorString());
    }
    return true;
}

int IniDocument::findLast(const QString& section, const QString& key) const {
    for (qsizetype i = lines_.size() - 1; i >= 0; --i) {
        const Line& l = lines_[i];
        if (l.kind == Kind::KeyValue && l.section == section && l.key == key) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::optional<QString> IniDocument::value(const QString& section, const QString& key) const {
    const int i = findLast(NormalizeName(section), NormalizeName(key));
    if (i < 0) {
        return std::nullopt;
    }
    const Line& l = lines_[i];
    return DecodeValue(QStringView(l.text).mid(l.valueStart, l.valueEnd - l.valueStart));
}

std::optional<QString> IniDocument::effectiveValue(const QString& section, const QString& key) const {
    if (auto v = value(section, key)) {
        return v;
    }
    return value(QString(), key);
}

bool IniDocument::setValue(const QString& sectionName, const QString& keyName, const QString& value) {
    const std::optional<QString> encoded = EncodeValue(value);
    const QString section = NormalizeName(sectionName);
    const QString key = NormalizeName(keyName);
    if (!encoded || key.isEmpty()) {
        return false;
    }
    const int existing = findLast(section, key);
    if (existing >= 0) {
        const Line& l = lines_[existing];
        QString before = l.text.left(l.valueStart);
        QString after = l.text.mid(l.valueEnd);
        const qsizetype oldLen = l.valueEnd - l.valueStart;
        qsizetype blanks = 0;
        while (blanks < after.size() && after[blanks] == QLatin1Char(' ')) {
            ++blanks;
        }
        if (blanks > 0 && blanks < after.size()) {
            // Keep the inline comment's column if possible (at least one blank before it).
            const qsizetype wanted = std::max<qsizetype>(1, blanks + oldLen - encoded->size());
            after = QString(wanted, QLatin1Char(' ')) + after.mid(blanks);
        }
        if (oldLen == 0 && !encoded->isEmpty() && before.endsWith(QLatin1Char('=')) && before.size() >= 2 &&
            IsSpaceOrTab(before[before.size() - 2])) {
            before += QLatin1Char(' ');
        }
        const QString newText = before + *encoded + after;
        const Line check = ParseLine(newText, l.section);
        if (check.kind != Kind::KeyValue || check.key != key ||
            DecodeValue(QStringView(check.text).mid(check.valueStart, check.valueEnd - check.valueStart)) !=
                value) {
            return false;
        }
        lines_[existing].text = newText;
        reparse();
        return true;
    }

    // New key: after the last key line of the section's last block.
    int header = -1;
    bool sectionFound = section.isEmpty();
    for (qsizetype i = lines_.size() - 1; i >= 0 && !section.isEmpty(); --i) {
        if (lines_[i].kind == Kind::Section && lines_[i].section == section) {
            header = static_cast<int>(i);
            sectionFound = true;
            break;
        }
    }
    if (!sectionFound) {
        if (!lines_.isEmpty() && lines_.last().kind != Kind::Blank) {
            lines_.append(Line{});
        }
        Line h;
        h.text = QLatin1Char('[') + section + QLatin1Char(']');
        lines_.append(h);
        Line kv;
        kv.text = key + QStringLiteral(" = ") + *encoded;
        lines_.append(kv);
        reparse();
        return true;
    }
    qsizetype insertAt = header + 1;
    int lastKey = -1;
    for (qsizetype i = header + 1; i < lines_.size(); ++i) {
        if (lines_[i].kind == Kind::Section) {
            break;
        }
        // Review M3c finding 20: after the last real key - never behind a broken line such as "[render" (the
        // host reads that line as an error; a key behind it would look like part of it).
        if (lines_[i].kind == Kind::KeyValue) {
            lastKey = static_cast<int>(i);
        }
    }
    QString text = key + QStringLiteral(" = ") + *encoded;
    if (lastKey >= 0) {
        insertAt = lastKey + 1;
        const Line& neighbour = lines_[lastKey];
        if (neighbour.kind == Kind::KeyValue) {
            const qsizetype eq = neighbour.text.indexOf(QLatin1Char('='));
            qsizetype lead = 0;
            while (lead < eq && IsSpaceOrTab(neighbour.text[lead])) {
                ++lead;
            }
            const QString indent = neighbour.text.left(lead);
            if (eq > 0 && !IsSpaceOrTab(neighbour.text[eq - 1])) {
                text = indent + key + QLatin1Char('=') + *encoded;  // "key=value" style
            } else {
                // Align with the block only if the file aligns it ("key      = value", two or more blanks before
                // '=' on some line): then the new key's '=' goes to the block's column when the key fits. A block
                // written as "key = value" (e.g. by this launcher) stays that way.
                qsizetype width = 0;
                for (qsizetype i = header + 1; i <= lastKey; ++i) {
                    const Line& l = lines_[i];
                    if (l.kind != Kind::KeyValue) {
                        continue;
                    }
                    const qsizetype e = l.text.indexOf(QLatin1Char('='));
                    qsizetype ll = 0;
                    while (ll < e && IsSpaceOrTab(l.text[ll])) {
                        ++ll;
                    }
                    if (e >= 2 && IsSpaceOrTab(l.text[e - 1]) && IsSpaceOrTab(l.text[e - 2])) {
                        width = std::max(width, e - ll);
                    }
                }
                const QString keyText = width > 0 && key.size() + 1 <= width
                                            ? key.leftJustified(width, QLatin1Char(' '))
                                            : key + QLatin1Char(' ');
                text = indent + keyText + QStringLiteral("= ") + *encoded;
            }
        }
    }
    Line kv;
    kv.text = text;
    lines_.insert(insertAt, kv);
    reparse();
    return true;
}

bool IniDocument::remove(const QString& sectionName, const QString& keyName) {
    const QString section = NormalizeName(sectionName);
    const QString key = NormalizeName(keyName);
    bool removed = false;
    for (qsizetype i = lines_.size() - 1; i >= 0; --i) {
        if (lines_[i].kind == Kind::KeyValue && lines_[i].section == section && lines_[i].key == key) {
            lines_.removeAt(i);
            removed = true;
        }
    }
    if (removed) {
        reparse();
    }
    return removed;
}

void IniDocument::prependComments(const QStringList& comments) {
    QList<Line> head;
    for (const QString& c : comments) {
        Line l;
        l.text =
            (c.startsWith(QLatin1Char(';')) || c.startsWith(QLatin1Char('#'))) ? c : QStringLiteral("; ") + c;
        head.append(l);
    }
    if (!lines_.isEmpty()) {
        head.append(Line{});
    }
    lines_ = head + lines_;
    reparse();
}

QList<IniDocument::Entry> IniDocument::entries() const {
    QList<Entry> out;
    for (qsizetype i = 0; i < lines_.size(); ++i) {
        const Line& l = lines_[i];
        if (l.kind == Kind::KeyValue) {
            out.append({l.section, l.key,
                        DecodeValue(QStringView(l.text).mid(l.valueStart, l.valueEnd - l.valueStart)),
                        static_cast<int>(i + 1)});
        }
    }
    return out;
}

QStringList IniDocument::sectionNames() const {
    QStringList out;
    for (const Line& l : lines_) {
        if (l.kind == Kind::Section && !out.contains(l.section)) {
            out.append(l.section);
        }
    }
    return out;
}

}  // namespace fl
