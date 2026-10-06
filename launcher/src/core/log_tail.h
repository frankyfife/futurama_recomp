// Last lines of the game log and of the game's console output.
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace fl {

// Last `count` lines of a text file (UTF-8, CRLF or LF), reading at most `maxBytes` from its end.
// Empty if the file cannot be read.
QStringList ReadLastLines(const QString& path, int count, qint64 maxBytes = 512 * 1024);

// Keeps the last `capacity` complete lines of a byte stream (process stdout/stderr) plus the pending
// partial line.
class LineRing {
public:
    explicit LineRing(int capacity = 200) : capacity_(capacity) {}
    void append(const QByteArray& chunk);
    QStringList lines() const;  // complete lines + the partial last one
    void clear();

private:
    int capacity_;
    QStringList lines_;
    QByteArray partial_;
};

}  // namespace fl
