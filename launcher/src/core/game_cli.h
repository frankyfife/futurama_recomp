// Calls of the machine-readable command line of futurama.exe (shared contract items 1-3):
//   --print-config-schema=<file.json>            schema, exit 0, the game does not start
//   --check-iso=<path> --result=<file.json>      ISO check, exit 0 if supported
//   --query-gpu=<file.json>                      adapters, outputs, MSAA levels, anisotropy, tearing
// Every call writes its JSON into a fresh temporary directory (below `tempParent`, normally the
// launcher cache next to the INI) and is removed afterwards. Arguments go through QProcess, which quotes
// them for CreateProcess, so paths with blanks, parentheses and commas arrive unchanged in the game.
#pragma once

#include "core/iso_check.h"

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <memory>

class QTemporaryDir;

namespace fl {

struct CliResult {
    bool ran = false;  // started and finished in time
    int exitCode = -1;
    bool crashed = false;
    QString error;    // why there is no usable result (localized), empty on success
    QByteArray json;  // the result file (may be present with a non-zero exit code)
    QString output;   // tail of stdout + stderr

    bool hasJson() const { return !json.isEmpty(); }
};

class GameCli {
public:
    explicit GameCli(QString exePath, QString tempParent = QString());

    CliResult printConfigSchema(int timeoutMs = 30000) const;
    CliResult queryGpu(int timeoutMs = 30000) const;
    CliResult checkIso(const QString& isoPath, int timeoutMs = 180000) const;
    const QString& exePath() const { return exe_; }

    // Turns a check-iso call into an IsoCheckResult (runError set if the call failed).
    static IsoCheckResult ToIsoResult(const CliResult& cli);
    // Argument list of a check-iso call (tests).
    static QStringList CheckIsoArguments(const QString& isoPath, const QString& resultFile);

private:
    CliResult run(const QStringList& args, const QString& resultFile, QTemporaryDir& dir,
                  int timeoutMs) const;
    std::unique_ptr<QTemporaryDir> makeTempDir() const;

    QString exe_;
    QString tempParent_;
};

// Asynchronous --check-iso for the wizard (the UI stays responsive while the image is hashed).
class IsoCheckJob : public QObject {
    Q_OBJECT
public:
    IsoCheckJob(QString exePath, QString tempParent, QObject* parent = nullptr);
    ~IsoCheckJob() override;

    void start(const QString& isoPath);
    void cancel();
    bool isRunning() const;
    const QString& isoPath() const { return iso_; }

signals:
    void finished(const fl::IsoCheckResult& result, const QString& isoPath);

private:
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void onError(QProcess::ProcessError error);

    QString exe_;
    QString tempParent_;
    QString iso_;
    QString resultFile_;
    QProcess* process_ = nullptr;
    std::unique_ptr<QTemporaryDir> dir_;
};

}  // namespace fl
