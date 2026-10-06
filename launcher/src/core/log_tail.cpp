#include "core/log_tail.h"

#include <QFile>

namespace fl {
namespace {

QString DecodeLine(QByteArray line) {
    if (line.endsWith('\r')) {
        line.chop(1);
    }
    return QString::fromUtf8(line);
}

}  // namespace

QStringList ReadLastLines(const QString& path, int count, qint64 maxBytes) {
    QFile f(path);
    if (count <= 0 || !f.open(QIODevice::ReadOnly)) {
        return {};
    }
    const qint64 size = f.size();
    const qint64 start = size > maxBytes ? size - maxBytes : 0;
    f.seek(start);
    QByteArray data = f.read(size - start);
    if (start > 0) {
        const qsizetype nl = data.indexOf('\n');
        data = nl >= 0 ? data.mid(nl + 1) : QByteArray();  // drop the cut first line
    }
    QList<QByteArray> parts = data.split('\n');
    if (!parts.isEmpty() && parts.last().isEmpty()) {
        parts.removeLast();
    }
    QStringList out;
    const qsizetype first = parts.size() > count ? parts.size() - count : 0;
    for (qsizetype i = first; i < parts.size(); ++i) {
        out.append(DecodeLine(parts[i]));
    }
    return out;
}

void LineRing::append(const QByteArray& chunk) {
    partial_ += chunk;
    qsizetype nl = 0;
    while ((nl = partial_.indexOf('\n')) >= 0) {
        lines_.append(DecodeLine(partial_.left(nl)));
        partial_.remove(0, nl + 1);
        if (lines_.size() > capacity_) {
            lines_.removeFirst();
        }
    }
    if (partial_.size() > 64 * 1024) {  // a runaway line without '\n'
        lines_.append(DecodeLine(partial_));
        partial_.clear();
        if (lines_.size() > capacity_) {
            lines_.removeFirst();
        }
    }
}

QStringList LineRing::lines() const {
    QStringList out = lines_;
    if (!partial_.isEmpty()) {
        out.append(DecodeLine(partial_));
    }
    return out;
}

void LineRing::clear() {
    lines_.clear();
    partial_.clear();
}

}  // namespace fl
