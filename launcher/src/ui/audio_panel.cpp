#include "ui/audio_panel.h"

#include "core/l10n.h"
#include "ui/app_style.h"
#include "ui/settings_tab.h"

#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPalette>
#include <QPushButton>
#include <QVBoxLayout>

namespace fl::ui {
namespace {

void SetTextColor(QLabel* label, const QColor& color) {
    QPalette p = label->palette();
    p.setColor(QPalette::WindowText, color);
    label->setPalette(p);
}

}  // namespace

AudioPanel::AudioPanel(AppContext& ctx, QWidget* parent) : QWidget(parent), ctx_(ctx) {
    setObjectName(QStringLiteral("audioPanel"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* box = new QGroupBox(T("audio.group"), this);
    outer->addWidget(box);
    auto* grid = new QGridLayout(box);
    grid->setColumnStretch(1, 1);
    grid->setHorizontalSpacing(fontMetrics().height());

    auto* deviceLabel = new QLabel(T("audio.output"), box);
    deviceLabel->setProperty(SettingsTab::kFormLabelProperty, true);
    deviceInfo_ = new QLabel(box);
    deviceInfo_->setObjectName(QStringLiteral("audioDeviceInfo"));
    deviceInfo_->setWordWrap(true);
    deviceInfo_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    grid->addWidget(deviceLabel, 0, 0, Qt::AlignTop);
    grid->addWidget(deviceInfo_, 0, 1);
    deviceWarning_ = new QLabel(box);
    deviceWarning_->setObjectName(QStringLiteral("audioDeviceWarning"));
    deviceWarning_->setWordWrap(true);
    SetTextColor(deviceWarning_, SeverityColor(Severity::Warning));
    grid->addWidget(deviceWarning_, 1, 1);

    auto* testLabel = new QLabel(T("audio.test"), box);
    testLabel->setProperty(SettingsTab::kFormLabelProperty, true);
    auto* row = new QHBoxLayout();
    row->setSpacing(fontMetrics().height() / 2);
    testButton_ = new QPushButton(T("audio.test_button"), box);
    testButton_->setObjectName(QStringLiteral("audioTestButton"));
    testButton_->setToolTip(T("audio.test_button.tip"));
    refreshButton_ = new QPushButton(T("audio.refresh"), box);
    refreshButton_->setObjectName(QStringLiteral("audioRefreshButton"));
    refreshButton_->setToolTip(T("audio.refresh.tip"));
    row->addWidget(testButton_);
    row->addWidget(refreshButton_);
    row->addStretch(1);
    grid->addWidget(testLabel, 2, 0);
    grid->addLayout(row, 2, 1);
    status_ = new QLabel(box);
    status_->setObjectName(QStringLiteral("audioTestStatus"));
    status_->setWordWrap(true);
    SetTextColor(status_, MutedTextColor(status_));
    status_->setText(T("audio.test_hint"));
    grid->addWidget(status_, 3, 1);
    auto* note = new QLabel(T("audio.game_sliders"), box);
    note->setWordWrap(true);
    SetTextColor(note, MutedTextColor(note));
    grid->addWidget(note, 4, 0, 1, 2);

    player_ = new TonePlayer(this);
    connect(testButton_, &QPushButton::clicked, this, &AudioPanel::playTone);
    connect(refreshButton_, &QPushButton::clicked, this, &AudioPanel::refreshDevicesRequested);
    connect(player_, &TonePlayer::finished, this, &AudioPanel::onToneFinished);
    refresh();
}

QString AudioPanel::keyValue(const char* name) const {
    if (const KeyDef* key = ctx_.model.schema().find(QStringLiteral("audio"), QLatin1String(name))) {
        return ctx_.model.value(*key);
    }
    return QString();
}

ToneRequest AudioPanel::currentRequest() const {
    ToneRequest r;
    r.deviceId = keyValue("device").trimmed();
    r.speakers = SpeakersFromValue(keyValue("speakers"));
    const QString volume = keyValue("master_volume");
    r.volume = volume.isEmpty() ? 1.0 : VolumeFromValue(volume);
    const QString enabled = keyValue("enabled");
    if (enabled == QLatin1String("0") || enabled.compare(QLatin1String("off"), Qt::CaseInsensitive) == 0 ||
        enabled.compare(QLatin1String("false"), Qt::CaseInsensitive) == 0) {
        r.volume = 0.0;
    }
    return r;
}

void AudioPanel::refresh() {
    const AudioDeviceList& list = ctx_.audio;
    const QString id = keyValue("device").trimmed();
    const AudioDevice* dev = list.resolve(id);
    QString info;
    QStringList warnings;
    const AudioDevice* used = dev;  // the device the game will open
    if (dev != nullptr) {
        info = AudioDeviceText(*dev);
        if (dev->isDefault) {
            info += QStringLiteral(" – ") + T("audio.is_default");
        }
    } else if (!id.isEmpty()) {
        // Not connected: the game opens the Windows default device instead (XaBackend::Init).
        used = list.defaultDevice();
        info = used != nullptr ? T("audio.device_fallback").arg(AudioDeviceText(*used)) : T("audio.device_default");
        warnings.append(T("audio.warn_not_connected").arg(id));
    } else {
        info = T("audio.device_default");
    }
    // [audio] speakers asks for more channels than the device has: Windows mixes them down.
    const int speakers = SpeakersFromValue(keyValue("speakers"));
    if (used != nullptr && speakers > 0 && used->channels > 0 && speakers > used->channels) {
        warnings.append(T("audio.warn_downmix").arg(speakers).arg(used->channels));
    }
    if (list.devices.isEmpty()) {
        warnings = {list.enumerated ? T("audio.warn_no_device")
                                    : T("audio.warn_no_enumeration")
                                          .arg(list.error.isEmpty() ? QStringLiteral("?") : list.error)};
    }
    const QString warning = warnings.join(QLatin1Char(' '));
    if (list.simulated) {
        info += QStringLiteral(" ") + T("audio.simulated");
    }
    deviceInfo_->setText(info);
    deviceWarning_->setText(warning);
    deviceWarning_->setVisible(!warning.isEmpty());
    const bool canPlay = !list.devices.isEmpty() && !player_->busy();
    testButton_->setEnabled(canPlay);
    // "Search devices again" rebuilds the window (and this panel): not while a tone plays.
    refreshButton_->setEnabled(!player_->busy());
}

void AudioPanel::playTone() {
    const ToneRequest r = currentRequest();
    if (!player_->play(r)) {
        return;
    }
    testButton_->setEnabled(false);
    refreshButton_->setEnabled(false);
    SetTextColor(status_, MutedTextColor(status_));
    status_->setText(T("audio.test_playing"));
}

void AudioPanel::onToneFinished(const ToneResult& result) {
    if (result.ok) {
        SetTextColor(status_, SeverityColor(Severity::Ok));
        QString text = T("audio.test_done")
                           .arg(result.speakers.join(QStringLiteral(", ")))
                           .arg(result.channels)
                           .arg(QLocale().toString(result.sampleRate / 1000.0, 'g', 4));
        if (result.fellBack) {
            text += QLatin1Char(' ') + T("audio.test_fallback");
        }
        status_->setText(text);
    } else {
        SetTextColor(status_, SeverityColor(Severity::Error));
        status_->setText(T("audio.test_failed").arg(result.error));
    }
    refresh();
}

}  // namespace fl::ui
