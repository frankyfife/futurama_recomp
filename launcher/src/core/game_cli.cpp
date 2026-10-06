#include "core/game_cli.h"

#include "core/l10n.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

namespace fl {
namespace {

constexpr qsizetype kOutputTail = 4000;

QString Tail(const QByteArray& bytes) {
    QString text = QString::fromLocal8Bit(bytes).trimmed();
    if (text.size() > kOutputTail) {
        text = text.right(kOutputTail);
    }
    return text;
}

QByteArray ReadFileBytes(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return f.readAll();
}

void PrepareProcess(QProcess& process, const QString& exe) {
    process.setProgram(exe);
    process.setWorkingDirectory(QFileInfo(exe).absolutePath());
    process.setProcessChannelMode(QProcess::MergedChannels);
}

}  // namespace

GameCli::GameCli(QString exePath, QString tempParent)
    : exe_(std::move(exePath)), tempParent_(std::move(tempParent)) {}

std::unique_ptr<QTemporaryDir> GameCli::makeTempDir() const {
    if (!tempParent_.isEmpty() && QDir().mkpath(tempParent_)) {
        auto dir = std::make_unique<QTemporaryDir>(QDir(tempParent_).filePath(QStringLiteral("cli-XXXXXX")));
        if (dir->isValid()) {
            return dir;
        }
    }
    return std::make_unique<QTemporaryDir>();
}

CliResult GameCli::run(const QStringList& args, const QString& resultFile, QTemporaryDir& dir,
                       int timeoutMs) const {
    CliResult r;
    if (!dir.isValid()) {
        r.error = T("cli.no_temp_dir");
        return r;
    }
    if (!QFileInfo(exe_).isFile()) {
        r.error = T("cli.exe_missing").arg(QDir::toNativeSeparators(exe_));
        return r;
    }
    QProcess process;
    PrepareProcess(process, exe_);
    process.setArguments(args);
    process.start(QIODevice::ReadOnly);
    if (!process.waitForStarted(15000)) {
        r.error = T("cli.start_failed").arg(QDir::toNativeSeparators(exe_), process.errorString());
        return r;
    }
    process.closeWriteChannel();
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(5000);
        r.output = Tail(process.readAll());
        r.error = T("cli.timeout").arg(timeoutMs / 1000);
        return r;
    }
    r.ran = true;
    r.exitCode = process.exitCode();
    r.crashed = process.exitStatus() == QProcess::CrashExit;
    r.output = Tail(process.readAll());
    r.json = ReadFileBytes(resultFile);
    if (r.json.isEmpty()) {
        QString detail = r.output.section(QLatin1Char('\n'), -1).trimmed();
        if (detail.isEmpty()) {
            detail = T("cli.no_detail");
        }
        r.error = T("cli.no_result").arg(r.exitCode).arg(detail);
    }
    return r;
}

CliResult GameCli::printConfigSchema(int timeoutMs) const {
    auto dir = makeTempDir();
    const QString file = QDir::toNativeSeparators(dir->filePath(QStringLiteral("schema.json")));
    return run({QStringLiteral("--print-config-schema=") + file}, file, *dir, timeoutMs);
}

CliResult GameCli::queryGpu(int timeoutMs) const {
    auto dir = makeTempDir();
    const QString file = QDir::toNativeSeparators(dir->filePath(QStringLiteral("gpu.json")));
    return run({QStringLiteral("--query-gpu=") + file}, file, *dir, timeoutMs);
}

QStringList GameCli::CheckIsoArguments(const QString& isoPath, const QString& resultFile) {
    return {QStringLiteral("--check-iso=") + QDir::toNativeSeparators(isoPath),
            QStringLiteral("--result=") + QDir::toNativeSeparators(resultFile)};
}

CliResult GameCli::checkIso(const QString& isoPath, int timeoutMs) const {
    auto dir = makeTempDir();
    const QString file = QDir::toNativeSeparators(dir->filePath(QStringLiteral("iso.json")));
    return run(CheckIsoArguments(isoPath, file), file, *dir, timeoutMs);
}

IsoCheckResult GameCli::ToIsoResult(const CliResult& cli) {
    IsoCheckResult result;
    if (cli.hasJson()) {
        QString parseError;
        if (!ParseIsoCheckResult(cli.json, &result, &parseError)) {
            result.runError = parseError;
        }
    } else {
        result.runError = cli.error.isEmpty() ? T("cli.no_detail") : cli.error;
    }
    result.exitCode = cli.exitCode;
    return result;
}

// --- IsoCheckJob ---------------------------------------------------------------------------------

IsoCheckJob::IsoCheckJob(QString exePath, QString tempParent, QObject* parent)
    : QObject(parent), exe_(std::move(exePath)), tempParent_(std::move(tempParent)) {}

IsoCheckJob::~IsoCheckJob() {
    cancel();
}

bool IsoCheckJob::isRunning() const {
    return process_ != nullptr && process_->state() != QProcess::NotRunning;
}

void IsoCheckJob::cancel() {
    if (process_ != nullptr) {
        process_->disconnect(this);
        if (process_->state() != QProcess::NotRunning) {
            process_->kill();
            process_->waitForFinished(3000);
        }
        process_->deleteLater();
        process_ = nullptr;
    }
    dir_.reset();
}

void IsoCheckJob::start(const QString& isoPath) {
    cancel();
    iso_ = isoPath;
    if (!QFileInfo(exe_).isFile()) {
        IsoCheckResult r;
        r.runError = T("cli.exe_missing").arg(QDir::toNativeSeparators(exe_));
        emit finished(r, iso_);
        return;
    }
    if (!tempParent_.isEmpty() && QDir().mkpath(tempParent_)) {
        dir_ = std::make_unique<QTemporaryDir>(QDir(tempParent_).filePath(QStringLiteral("cli-XXXXXX")));
    }
    if (!dir_ || !dir_->isValid()) {
        dir_ = std::make_unique<QTemporaryDir>();
    }
    resultFile_ = QDir::toNativeSeparators(dir_->filePath(QStringLiteral("iso.json")));
    process_ = new QProcess(this);
    PrepareProcess(*process_, exe_);
    process_->setArguments(GameCli::CheckIsoArguments(isoPath, resultFile_));
    connect(process_, &QProcess::finished, this, &IsoCheckJob::onFinished);
    connect(process_, &QProcess::errorOccurred, this, &IsoCheckJob::onError);
    process_->start(QIODevice::ReadOnly);
}

void IsoCheckJob::onError(QProcess::ProcessError error) {
    if (error != QProcess::FailedToStart || process_ == nullptr) {
        return;  // crashes and the like arrive through finished()
    }
    IsoCheckResult r;
    r.runError = T("cli.start_failed").arg(QDir::toNativeSeparators(exe_), process_->errorString());
    process_->disconnect(this);
    process_->deleteLater();
    process_ = nullptr;
    dir_.reset();
    emit finished(r, iso_);
}

void IsoCheckJob::onFinished(int exitCode, QProcess::ExitStatus status) {
    if (process_ == nullptr) {
        return;
    }
    CliResult cli;
    cli.ran = true;
    cli.exitCode = exitCode;
    cli.crashed = status == QProcess::CrashExit;
    cli.output = Tail(process_->readAll());
    cli.json = ReadFileBytes(resultFile_);
    if (cli.json.isEmpty()) {
        QString detail = cli.output.section(QLatin1Char('\n'), -1).trimmed();
        cli.error = T("cli.no_result").arg(exitCode).arg(detail.isEmpty() ? T("cli.no_detail") : detail);
    }
    process_->disconnect(this);
    process_->deleteLater();
    process_ = nullptr;
    dir_.reset();
    emit finished(GameCli::ToIsoResult(cli), iso_);
}

}  // namespace fl
