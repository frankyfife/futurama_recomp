#include "ui/status_panel.h"

#include "core/l10n.h"
#include "ui/app_style.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace fl::ui {

StatusPanel::StatusPanel(QWidget* parent) : QFrame(parent) {
    setFrameShape(QFrame::StyledPanel);
    setAutoFillBackground(true);
    QPalette p = palette();
    p.setColor(QPalette::Window, p.color(QPalette::AlternateBase));
    setPalette(p);
    auto* layout = new QVBoxLayout(this);
    const int gap = fontMetrics().height() / 2;
    layout->setContentsMargins(gap * 2, gap, gap, gap * 2);
    layout->setSpacing(gap);

    auto* head = new QHBoxLayout();
    title_ = new QLabel(this);
    title_->setObjectName(QStringLiteral("statusTitle"));
    ScaleFont(title_, 1.25, true);
    title_->setWordWrap(true);
    close_ = new QToolButton(this);
    close_->setText(QStringLiteral("✕"));
    close_->setAutoRaise(true);
    close_->setToolTip(T("status.close.tip"));
    head->addWidget(title_, 1);
    head->addWidget(close_, 0, Qt::AlignTop);
    layout->addLayout(head);

    detail_ = new QLabel(this);
    detail_->setWordWrap(true);
    detail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(detail_);
    cause_ = new QLabel(this);
    cause_->setWordWrap(true);
    cause_->setTextFormat(Qt::RichText);
    cause_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(cause_);

    linesCaption_ = new QLabel(this);
    layout->addWidget(linesCaption_);
    lines_ = new QPlainTextEdit(this);
    lines_->setReadOnly(true);
    lines_->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setFamilies({QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas"), mono.family()});
    mono.setPointSizeF(font().pointSizeF() > 0 ? font().pointSizeF() : mono.pointSizeF());
    lines_->setFont(mono);
    lines_->setMinimumHeight(lines_->fontMetrics().lineSpacing() * 6);
    lines_->setMaximumHeight(lines_->fontMetrics().lineSpacing() * 10);
    layout->addWidget(lines_);

    auto* buttons = new QHBoxLayout();
    openLog_ = new QPushButton(T("status.open_log"), this);
    openFolder_ = new QPushButton(T("status.open_folder"), this);
    retry_ = new QPushButton(T("status.retry"), this);
    buttons->addWidget(openLog_);
    buttons->addWidget(openFolder_);
    buttons->addStretch(1);
    buttons->addWidget(retry_);
    layout->addLayout(buttons);

    connect(openLog_, &QPushButton::clicked, this, &StatusPanel::openLog);
    connect(openFolder_, &QPushButton::clicked, this, &StatusPanel::openFolder);
    connect(retry_, &QPushButton::clicked, this, &StatusPanel::retryRequested);
    connect(close_, &QToolButton::clicked, this, &StatusPanel::closeRequested);
}

void StatusPanel::showResult(const GameRunResult& result) {
    result_ = result;
    const ExitInfo info = result.describe();
    QString title = info.title;
    if (result.started) {
        title += QStringLiteral(" – ") + T("status.exit_code").arg(FormatExitCode(result.exitCode));
    }
    title_->setText(title);
    QPalette tp = title_->palette();
    tp.setColor(QPalette::WindowText, ToneColor(info.tone));
    title_->setPalette(tp);

    QString detail = info.detail;
    if (result.started && result.durationSeconds() > 0.0) {
        detail +=
            QLatin1Char(' ') + T("status.duration").arg(QLocale().toString(result.durationSeconds(), 'f', 1));
    }
    detail_->setText(detail);

    // Cause: the start error, else the host's "futurama: ..." line of a failed start (it prints startup errors to
    // stderr before the log file exists) - not the "log file: <path>" line after it (review M3c finding 7).
    const QString cause = result.cause();
    cause_->setVisible(!cause.isEmpty());
    cause_->setText(T("status.cause").arg(cause.toHtmlEscaped()));

    QStringList shown;
    if (result.logFromThisRun && !result.logTail.isEmpty()) {
        linesCaption_->setText(
            T("status.log_caption").arg(result.logTail.size()).arg(QFileInfo(result.logFile).fileName()));
        shown = result.logTail;
    } else if (!result.outputTail.isEmpty()) {
        linesCaption_->setText(T("status.output_caption"));
        shown = result.outputTail.mid(std::max<qsizetype>(0, result.outputTail.size() - kLogTailLines));
    } else {
        linesCaption_->setText(result.started ? T("status.no_log") : QString());
    }
    lines_->setPlainText(shown.join(QLatin1Char('\n')));
    lines_->setVisible(!shown.isEmpty());
    // The last line at the bottom, but the start of the lines (time stamp, thread, level) visible: a long last
    // line would scroll the view horizontally to its end (review M3c finding 8).
    lines_->moveCursor(QTextCursor::End);
    lines_->moveCursor(QTextCursor::StartOfLine);
    lines_->horizontalScrollBar()->setValue(0);
    // After a normal end "Erneut spielen", after a failure "Erneut versuchen" (finding 17).
    retry_->setText(info.category == ExitCategory::Ok || info.category == ExitCategory::GameEnded
                        ? T("status.play_again")
                        : T("status.retry"));
    openLog_->setEnabled(QFileInfo::exists(result.logFile));
    openFolder_->setEnabled(QFileInfo(result.logFile).absoluteDir().exists());
}

void StatusPanel::openLog() {
    if (QFileInfo::exists(result_.logFile)) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(result_.logFile));
    }
}

void StatusPanel::openFolder() {
    const QString dir = QFileInfo(result_.logFile).absolutePath();
    if (!dir.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    }
}

}  // namespace fl::ui
