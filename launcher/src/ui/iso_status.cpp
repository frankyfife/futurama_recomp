#include "ui/iso_status.h"

#include "core/game_cli.h"
#include "core/l10n.h"
#include "core/validation.h"
#include "ui/app_style.h"

#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>

namespace fl::ui {
namespace {

QString Colored(Severity severity, const QString& html) {
    return QStringLiteral("<span style=\"color:%1\">%2</span>").arg(ColorCss(SeverityColor(severity)), html);
}

// Plain glyphs without an emoji presentation (U+2714/U+2716 come out as colour emoji with some fonts).
QString Verdict(Severity severity, const QString& glyph, const QString& text) {
    return Colored(severity, QStringLiteral("<b>") + glyph + QLatin1Char(' ') + text.toHtmlEscaped() +
                                 QStringLiteral("</b>"));
}

QString FormatSize(qint64 bytes) {
    return QLocale().formattedDataSize(bytes, 2, QLocale::DataSizeTraditionalFormat);
}

}  // namespace

QString IsoVerdictHtml(const IsoCheckResult& r) {
    switch (r.verdict()) {
    case IsoVerdict::Supported:
        return Verdict(Severity::Ok, QString(QChar(0x2713)), T("wiz.iso.supported"));
    case IsoVerdict::UnsupportedVersion:
        return Verdict(Severity::Warning, QString(QChar(0x26A0)),
                       T("wiz.iso.unsupported").arg(r.versionDisplayName()));
    case IsoVerdict::NotXboxImage:
        return Verdict(Severity::Error, QString(QChar(0x2717)), T("wiz.iso.not_xbox"));
    case IsoVerdict::CheckFailed:
        return Verdict(Severity::Warning, QString(QChar(0x26A0)), T("wiz.iso.check_failed"));
    }
    return QString();
}

QStringList IsoDetailLines(const IsoCheckResult& r, bool wizard) {
    QStringList lines;
    if (!r.message.isEmpty() && r.verdict() != IsoVerdict::Supported) {
        lines.append(r.message.text());  // the host's own explanation (for a supported image the verdict says it)
    }
    if (r.verdict() == IsoVerdict::Supported || r.verdict() == IsoVerdict::UnsupportedVersion) {
        lines.append(T("wiz.iso.detail_version")
                         .arg(r.title.isEmpty() ? QStringLiteral("?") : r.title, r.versionDisplayName(),
                              r.layoutDisplayName(), FormatSize(r.size)));
        if (wizard && !r.xbeSha1.isEmpty()) {
            lines.append(T("wiz.iso.detail_sha1").arg(r.xbeSha1));
        }
        if (r.verdict() == IsoVerdict::UnsupportedVersion) {
            // Review M3c finding 9: the host stops such an image with exit code 2 (not "may not start").
            lines.append(wizard ? T("wiz.iso.unsupported_hint") : T("iso.unsupported_hint"));
        }
    } else if (r.verdict() == IsoVerdict::NotXboxImage) {
        if (r.message.isEmpty()) {
            lines.append(T("wiz.iso.reason").arg(r.reason.isEmpty() ? T("cli.no_detail") : r.reason));
        }
    } else {
        lines.append(r.runError);
        lines.append(T("wiz.iso.check_failed_hint"));
    }
    return lines;
}

IsoStatusBox::IsoStatusBox(AppContext& ctx, QWidget* parent) : QGroupBox(T("iso.box"), parent), ctx_(ctx) {
    setObjectName(QStringLiteral("isoStatus"));
    auto* grid = new QGridLayout(this);
    grid->setColumnStretch(0, 1);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("isoStatusText"));
    status_->setTextFormat(Qt::RichText);
    status_->setWordWrap(true);
    details_ = new QLabel(this);
    details_->setWordWrap(true);
    details_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QPalette p = details_->palette();
    p.setColor(QPalette::WindowText, MutedTextColor(details_));
    details_->setPalette(p);
    change_ = new QPushButton(this);
    change_->setObjectName(QStringLiteral("isoChange"));
    grid->addWidget(status_, 0, 0);
    grid->addWidget(change_, 0, 1, Qt::AlignRight | Qt::AlignTop);
    grid->addWidget(details_, 1, 0, 1, 2);
    if (ctx_.paths.gameFound()) {
        job_ = new IsoCheckJob(ctx_.paths.gameExe, ctx_.paths.cacheDir(), this);
        connect(job_, &IsoCheckJob::finished, this, [this](const IsoCheckResult& result, const QString& iso) {
            const QString key = FileKey(iso);
            if (key == runningKey_) {
                result_ = result;
                checkedKey_ = key;
                runningKey_.clear();
            }
            refresh();
        });
    }
    connect(change_, &QPushButton::clicked, this, [this] { browse(); });
    refresh();
}

IsoStatusBox::~IsoStatusBox() {
    if (job_ != nullptr) {
        job_->cancel();
    }
}

QString IsoStatusBox::configuredPath() const {
    const KeyDef* iso = ctx_.model.schema().find(QStringLiteral("paths"), QStringLiteral("iso"));
    if (iso == nullptr) {
        return QString();
    }
    const QString value = ctx_.model.value(*iso).trimmed();
    return value.isEmpty() ? QString() : QDir::toNativeSeparators(ResolvePath(value, ctx_.model.iniDir()));
}

QString IsoStatusBox::FileKey(const QString& path) {
    const QFileInfo info(path);
    if (path.isEmpty() || !info.isFile()) {
        return QString();
    }
    return QDir::toNativeSeparators(info.absoluteFilePath()).toLower() + QLatin1Char('|') +
           QString::number(info.size()) + QLatin1Char('|') +
           QString::number(info.lastModified().toMSecsSinceEpoch());
}

void IsoStatusBox::refresh() {
    const QString key = FileKey(configuredPath());
    if (!key.isEmpty() && key != checkedKey_ && key != runningKey_ && job_ != nullptr) {
        runningKey_ = key;
        job_->start(configuredPath());
    }
    showState();
}

void IsoStatusBox::checkNow() {
    const QString path = configuredPath();
    const QString key = FileKey(path);
    if (key.isEmpty() || key == checkedKey_ || !ctx_.paths.gameFound()) {
        showState();
        return;
    }
    if (job_ != nullptr) {
        job_->cancel();
    }
    const GameCli cli(ctx_.paths.gameExe, ctx_.paths.cacheDir());
    result_ = GameCli::ToIsoResult(cli.checkIso(path));
    checkedKey_ = key;
    runningKey_.clear();
    showState();
}

std::optional<IsoCheckResult> IsoStatusBox::currentResult() {
    const QString key = FileKey(configuredPath());
    if (key.isEmpty() || !ctx_.paths.gameFound()) {
        return std::nullopt;
    }
    if (key != checkedKey_) {
        checkNow();
    }
    if (key != checkedKey_) {
        return std::nullopt;
    }
    return result_;
}

void IsoStatusBox::showState() {
    setTitle(T("iso.box"));
    const QString path = configuredPath();
    const QString key = FileKey(path);
    change_->setText(path.isEmpty() ? T("iso.choose") : T("iso.change"));
    change_->setToolTip(T("iso.change.tip"));
    if (path.isEmpty()) {
        status_->setText(Verdict(Severity::Warning, QString(QChar(0x26A0)), T("tab.iso_missing")));
        details_->setText(T("iso.none_hint"));
        return;
    }
    if (key.isEmpty()) {
        status_->setText(Verdict(Severity::Error, QString(QChar(0x2717)), T("wiz.iso.not_found")));
        details_->setText(path);
        return;
    }
    if (key != checkedKey_) {
        status_->setText(ctx_.paths.gameFound() ? T("wiz.iso.checking").toHtmlEscaped()
                                                : T("iso.no_game").toHtmlEscaped());
        details_->setText(path);
        return;
    }
    QStringList lines;
    switch (result_.verdict()) {
    case IsoVerdict::Supported:
        status_->setText(Verdict(Severity::Ok, QString(QChar(0x2713)),
                                 T("iso.box.supported")
                                     .arg(result_.title.isEmpty() ? QStringLiteral("Futurama") : result_.title,
                                          result_.versionDisplayName())));
        lines.append(T("iso.box.detail").arg(result_.layoutDisplayName(), FormatSize(result_.size), path));
        break;
    case IsoVerdict::UnsupportedVersion:
    case IsoVerdict::NotXboxImage:
        status_->setText(IsoVerdictHtml(result_));
        lines = IsoDetailLines(result_, false);
        lines.append(path);
        break;
    case IsoVerdict::CheckFailed:
        status_->setText(IsoVerdictHtml(result_));
        lines.append(result_.runError);
        lines.append(path);
        break;
    }
    details_->setText(lines.join(QLatin1Char('\n')));
}

void IsoStatusBox::browse() {
    const QString current = configuredPath();
    QString start = current.isEmpty() ? QString() : QFileInfo(current).absolutePath();
    if (start.isEmpty() || !QFileInfo(start).isDir()) {
        start = QDir::homePath();
    }
    const QString file = QFileDialog::getOpenFileName(this, T("wiz.iso.dialog"), start, T("editor.iso_filter"));
    if (!file.isEmpty()) {
        emit isoChosen(QDir::toNativeSeparators(file));
    }
}

}  // namespace fl::ui
