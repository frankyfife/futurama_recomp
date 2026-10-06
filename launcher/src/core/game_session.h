// One game run: starts futurama.exe --config=<ini>, collects its console output (the host writes
// startup errors to stderr when it has one - with a pipe it also shows no message box) and, when the
// game exits, the exit code and the last lines of <userdata>\logs\futurama.log if this run wrote it.
#pragma once

#include "core/exit_codes.h"
#include "core/log_tail.h"

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

namespace fl {

inline constexpr int kLogTailLines = 30;

struct GameRunResult {
    bool started = false;
    QString startError;
    int exitCode = -1;
    bool crashed = false;
    QDateTime startTime;
    QDateTime endTime;
    QString exe;
    QStringList arguments;
    QString logFile;
    bool logFromThisRun = false;  // the log was written after the start
    QStringList logTail;          // last kLogTailLines lines of the log (only if logFromThisRun)
    QStringList outputTail;       // last lines of stdout/stderr

    ExitInfo describe() const { return DescribeExit(exitCode, crashed, started); }
    double durationSeconds() const;
    // Review M3c finding 7: the cause of a failed start for the result panel - the start error, else the host's
    // last "futurama: <message>" line (without the prefix and a "FATAL: "), never its "log file: <path>" or "no log
    // file" line; else the last other line. Empty for a normal end.
    QString cause() const;
    // {"exit_code", "category", "crashed", "started", "start_error", "title", "detail", "cause", "log_file",
    //  "log_from_this_run", "log_tail", "output_tail", "started_at", "duration_s", "exe", "arguments"}
    QJsonObject toJson() const;
};

class GameSession : public QObject {
    Q_OBJECT
public:
    explicit GameSession(QObject* parent = nullptr);
    ~GameSession() override;

    // Starts the game. Returns false if it could not start (finished() is still emitted).
    bool start(const QString& exe, const QString& iniPath, const QString& logFile);
    bool isRunning() const;
    // Blocks (event loop) until the game exits; returns the result.
    GameRunResult waitForFinished();
    const GameRunResult& result() const { return result_; }

    // The argument list for `iniPath` (tests).
    static QStringList Arguments(const QString& iniPath);
    // Fills logFromThisRun/logTail of `r` from r.logFile (also used for the screenshot sample).
    static void CollectLog(GameRunResult& r);

signals:
    void finished(const fl::GameRunResult& result);

private:
    void onReadyRead();
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void onError(QProcess::ProcessError error);

    QProcess* process_ = nullptr;
    LineRing output_{200};
    GameRunResult result_;
    bool done_ = true;
};

}  // namespace fl
