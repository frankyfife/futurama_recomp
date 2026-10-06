#include "core/game_session.h"

#include "core/l10n.h"

#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>

namespace fl {

double GameRunResult::durationSeconds() const {
    if (!startTime.isValid() || !endTime.isValid()) {
        return 0.0;
    }
    return static_cast<double>(startTime.msecsTo(endTime)) / 1000.0;
}

QString GameRunResult::cause() const {
    if (!startError.isEmpty()) {
        return startError;
    }
    const ExitCategory category = describe().category;
    if (category == ExitCategory::Ok || category == ExitCategory::GameEnded) {
        return QString();
    }
    QString fallback;
    for (qsizetype i = outputTail.size() - 1; i >= 0; --i) {
        const QString line = outputTail[i].trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1String("log file:")) || line == QLatin1String("no log file")) {
            continue;
        }
        if (line.startsWith(QLatin1String("futurama: "))) {
            QString text = line.mid(10).trimmed();
            if (text.startsWith(QLatin1String("FATAL: "))) {
                text = text.mid(7);
            }
            return text;
        }
        if (fallback.isEmpty()) {
            fallback = line;
        }
    }
    return fallback;
}

QJsonObject GameRunResult::toJson() const {
    const ExitInfo info = describe();
    QJsonObject o;
    o.insert(QStringLiteral("started"), started);
    o.insert(QStringLiteral("start_error"), startError);
    o.insert(QStringLiteral("exit_code"), exitCode);
    o.insert(QStringLiteral("crashed"), crashed);
    o.insert(QStringLiteral("category"), ExitCategoryName(info.category));
    o.insert(QStringLiteral("title"), info.title);
    o.insert(QStringLiteral("detail"), info.detail);
    o.insert(QStringLiteral("cause"), cause());
    o.insert(QStringLiteral("exe"), exe);
    o.insert(QStringLiteral("arguments"), QJsonArray::fromStringList(arguments));
    o.insert(QStringLiteral("log_file"), logFile);
    o.insert(QStringLiteral("log_from_this_run"), logFromThisRun);
    o.insert(QStringLiteral("log_tail"), QJsonArray::fromStringList(logTail));
    o.insert(QStringLiteral("output_tail"), QJsonArray::fromStringList(outputTail));
    o.insert(QStringLiteral("started_at"), startTime.toString(Qt::ISODateWithMs));
    o.insert(QStringLiteral("duration_s"), durationSeconds());
    return o;
}

GameSession::GameSession(QObject* parent) : QObject(parent) {}

GameSession::~GameSession() {
    if (process_ != nullptr && process_->state() != QProcess::NotRunning) {
        // The game keeps running if the launcher goes away: QProcess would kill it in its destructor,
        // so the object is deliberately left alive (the process ends soon after anyway).
        process_->disconnect(this);
        process_->setParent(nullptr);
        process_ = nullptr;
    }
}

QStringList GameSession::Arguments(const QString& iniPath) {
    return {QStringLiteral("--config=") + QDir::toNativeSeparators(iniPath)};
}

bool GameSession::isRunning() const {
    return !done_;
}

void GameSession::CollectLog(GameRunResult& r) {
    r.logFromThisRun = false;
    r.logTail.clear();
    if (r.logFile.isEmpty()) {
        return;
    }
    const QFileInfo info(r.logFile);
    // 2 s slack: the file system time and the launcher clock may differ slightly.
    if (info.exists() && (!r.startTime.isValid() || info.lastModified() >= r.startTime.addSecs(-2))) {
        r.logFromThisRun = true;
        r.logTail = ReadLastLines(r.logFile, kLogTailLines);
    }
}

bool GameSession::start(const QString& exe, const QString& iniPath, const QString& logFile) {
    result_ = GameRunResult{};
    output_.clear();
    result_.exe = QDir::toNativeSeparators(exe);
    result_.arguments = Arguments(iniPath);
    result_.logFile = logFile;
    result_.startTime = QDateTime::currentDateTime();
    done_ = false;
    if (process_ != nullptr) {
        process_->disconnect(this);
        process_->deleteLater();
    }
    process_ = new QProcess(this);
    process_->setProgram(exe);
    process_->setArguments(result_.arguments);
    process_->setWorkingDirectory(QFileInfo(exe).absolutePath());
    process_->setProcessChannelMode(QProcess::MergedChannels);
    connect(process_, &QProcess::readyRead, this, &GameSession::onReadyRead);
    connect(process_, &QProcess::finished, this, &GameSession::onFinished);
    connect(process_, &QProcess::errorOccurred, this, &GameSession::onError);
    if (!QFileInfo(exe).isFile()) {
        result_.startError = T("cli.exe_missing").arg(QDir::toNativeSeparators(exe));
        result_.endTime = QDateTime::currentDateTime();
        done_ = true;
        emit finished(result_);
        return false;
    }
    process_->start(QIODevice::ReadOnly);
    return true;
}

void GameSession::onReadyRead() {
    if (process_ != nullptr) {
        output_.append(process_->readAll());
    }
}

void GameSession::onError(QProcess::ProcessError error) {
    if (error != QProcess::FailedToStart || done_) {
        return;
    }
    result_.startError = T("cli.start_failed").arg(result_.exe, process_->errorString());
    result_.endTime = QDateTime::currentDateTime();
    done_ = true;
    emit finished(result_);
}

void GameSession::onFinished(int exitCode, QProcess::ExitStatus status) {
    if (done_) {
        return;
    }
    onReadyRead();
    result_.started = true;
    result_.exitCode = exitCode;
    result_.crashed = status == QProcess::CrashExit;
    result_.endTime = QDateTime::currentDateTime();
    result_.outputTail = output_.lines();
    if (exitCode != kGameExitAlreadyRunning) {
        CollectLog(result_);  // exit code 6: the log belongs to the other, still running game
    }
    done_ = true;
    emit finished(result_);
}

GameRunResult GameSession::waitForFinished() {
    if (!done_) {
        QEventLoop loop;
        connect(this, &GameSession::finished, &loop, &QEventLoop::quit);
        if (!done_) {
            loop.exec();
        }
    }
    return result_;
}

}  // namespace fl
