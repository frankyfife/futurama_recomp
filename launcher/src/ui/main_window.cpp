#include "ui/main_window.h"

#include "core/l10n.h"
#include "core/bindings.h"
#include "core/pad_reader.h"
#include "core/presets.h"
#include "core/validation.h"
#include "ui/app_style.h"
#include "ui/audio_panel.h"
#include "ui/controls_panel.h"
#include "ui/pad_poller.h"
#include "ui/iso_status.h"
#include "ui/screen_info.h"
#include "ui/settings_tab.h"
#include "ui/status_panel.h"
#include "ui/wizard.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QDate>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace fl::ui {
namespace {

// Review M3c finding 13: "release, built 2026-10-06 14:03:12, msvc 19.44.35217" (the host's --print-config-schema
// "build") in the UI language; anything else is shown as it is.
QString FormatBuild(const QString& build) {
    static const QRegularExpression re(
        QStringLiteral("^(release|debug), built (\\d{4})-(\\d{2})-(\\d{2}) (\\d{2}:\\d{2})(?::\\d{2})?, (.+)$"));
    const QRegularExpressionMatch m = re.match(build.trimmed());
    if (!m.hasMatch()) {
        return build;
    }
    const QDate date(m.captured(2).toInt(), m.captured(3).toInt(), m.captured(4).toInt());
    QString compiler = m.captured(6);
    if (compiler.startsWith(QLatin1String("msvc"))) {
        compiler.replace(0, 4, QStringLiteral("MSVC"));
    }
    return T("adv.build")
        .arg(m.captured(1) == QLatin1String("debug") ? T("adv.build.debug") : T("adv.build.release"),
             date.toString(CurrentLanguage() == Language::German ? QStringLiteral("dd.MM.yyyy")
                                                                  : QStringLiteral("yyyy-MM-dd")),
             m.captured(5), compiler);
}

QString ProblemLine(const IniDocument::Problem& p) {
    return p.line > 0 ? T("main.ini_problem").arg(p.line).arg(IniDocument::ProblemText(p))
                      : T("main.ini_problem_file").arg(IniDocument::ProblemText(p));
}

QLabel* MutedLabel(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setWordWrap(true);
    QPalette p = l->palette();
    p.setColor(QPalette::WindowText, MutedTextColor(l));
    l->setPalette(p);
    return l;
}

QLabel* ColoredLabel(const QString& text, Severity severity, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setWordWrap(true);
    QPalette p = l->palette();
    p.setColor(QPalette::WindowText, SeverityColor(severity));
    l->setPalette(p);
    return l;
}

// Message box with localized buttons (the Qt installation has no qtbase translations).
int AskQuestion(QWidget* parent, QMessageBox::Icon icon, const QString& title, const QString& text,
                const QStringList& buttons, int defaultIndex) {
    QMessageBox box(icon, title, text, QMessageBox::NoButton, parent);
    QList<QPushButton*> created;
    for (const QString& b : buttons) {
        created.append(box.addButton(b, QMessageBox::ActionRole));
    }
    if (defaultIndex >= 0 && defaultIndex < created.size()) {
        box.setDefaultButton(created[defaultIndex]);
    }
    box.setEscapeButton(created.last());
    box.exec();
    return static_cast<int>(created.indexOf(qobject_cast<QPushButton*>(box.clickedButton())));
}

void Inform(QWidget* parent, QMessageBox::Icon icon, const QString& title, const QString& text) {
    AskQuestion(parent, icon, title, text, {T("common.ok")}, 0);
}

QString TabTitle(UiTab tab) {
    switch (tab) {
    case UiTab::Game:
        return T("tab.game");
    case UiTab::Graphics:
        return T("tab.graphics");
    case UiTab::Controls:
        return T("tab.controls");
    case UiTab::Audio:
        return T("tab.audio");
    case UiTab::Paths:
        return T("tab.paths");
    case UiTab::Advanced:
        return T("tab.advanced");
    }
    return QString();
}

}  // namespace

MainWindow::MainWindow(AppContext& ctx, StartupOptions options, QWidget* parent)
    : QMainWindow(parent), ctx_(ctx), options_(std::move(options)) {
    setWindowIcon(AppIcon());
    editorContext_.gpu = &ctx_.gpu;
    editorContext_.audio = &ctx_.audio;
    editorContext_.schema = &ctx_.model.schema();
    editorContext_.iniDir = [this] { return ctx_.model.iniDir(); };
    editorContext_.currentValue = [this](const KeyDef& key) { return ctx_.model.value(key); };
    editorContext_.placeholder = [this](const KeyDef& key) -> QString {
        if (key.fullName() == QLatin1String("paths.userdata")) {
            return ctx_.paths.defaultUserdata;
        }
        if (key.fullName() == QLatin1String("render.screenshot_dir")) {
            return ScreenshotDir(QString(), ctx_.userdata(), ctx_.paths.configDir);
        }
        return key.placeholder;
    };
    // Review M3c finding 18: what a key with a dynamic default is while it is not set - [render] frame_rate follows
    // [game] tick_rate (gfx/gfx_config.h).
    editorContext_.dynamicDefault = [this](const KeyDef& key) -> QString {
        if (key.fullName() == QLatin1String("render.frame_rate")) {
            if (const KeyDef* tick = ctx_.model.schema().find(QStringLiteral("game"), QStringLiteral("tick_rate"))) {
                return ctx_.model.value(*tick);
            }
        }
        return QString();
    };
    connect(&session_, &GameSession::finished, this, &MainWindow::onGameFinished, Qt::QueuedConnection);
    padPoller_ = new PadPoller(CreatePadReader(), this);
    buildUi();
    const int unit = fontMetrics().height();
    resize(unit * 64, unit * 50);
    const QByteArray geometry =
        QByteArray::fromBase64(ctx_.setting(QStringLiteral("window/geometry")).toLatin1());
    const bool restored = !geometry.isEmpty() && restoreGeometry(geometry);
    fitToScreen(!restored);
}

// Review M3c finding 12: the start size is 64 x 50 font heights (1024 x 800 logical pixels at 9 pt), which is taller
// than the work area of common notebooks (1920 x 1080 at 150 %: about 1280 x 690 logical pixels free). At most 90 %
// of the screen's available area; a window that would reach below the taskbar is moved up (or centered when it
// was not restored). The tabs scroll, so every setting stays reachable.
void MainWindow::fitToScreen(bool center) {
    const QScreen* s = screen();
    if (s == nullptr) {
        return;
    }
    const QRect avail = s->availableGeometry();
    const int frame = std::max(0, frameGeometry().height() - geometry().height());
    const QSize most(avail.width() * 9 / 10, avail.height() * 9 / 10 - frame);
    if (width() > most.width() || height() > most.height()) {
        resize(std::min(width(), most.width()), std::min(height(), std::max(most.height(), minimumSizeHint().height())));
    }
    if (center) {
        move(avail.center() - QPoint(width() / 2, (height() + frame) / 2));
    } else if (y() + height() + frame > avail.bottom() || x() + width() > avail.right()) {
        move(std::clamp(x(), avail.left(), std::max(avail.left(), avail.right() - width())),
             std::clamp(y(), avail.top(), std::max(avail.top(), avail.bottom() - height() - frame)));
    }
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi() {
    setWindowTitle(T("main.window_title"));
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    const int gap = fontMetrics().height() / 2;
    layout->setContentsMargins(gap * 2, gap * 2, gap * 2, gap * 2);
    layout->setSpacing(gap);
    layout->addWidget(buildHeader());

    statusPanel_ = new StatusPanel(central);
    statusPanel_->setObjectName(QStringLiteral("statusPanel"));
    statusPanel_->setVisible(false);
    layout->addWidget(statusPanel_);
    connect(statusPanel_, &StatusPanel::retryRequested, this,
            [this] { QTimer::singleShot(0, this, &MainWindow::startGame); });
    connect(statusPanel_, &StatusPanel::closeRequested, this, &MainWindow::hideRunResult);

    tabs_ = new QTabWidget(central);
    tabs_->setDocumentMode(true);
    settingsTabs_.clear();
    for (UiTab tab : kAllTabs) {
        // The binding keys of the controls tab have their own tables (ui/controls_panel.h).
        auto* page = new SettingsTab(
            tab, ctx_.model, editorContext_,
            [this](const KeyDef& key) { return keyVisible(key) && !IsBindingKey(key); }, tabs_);
        if (tab == UiTab::Controls) {
            buildControlsExtras(page);
        } else if (tab == UiTab::Game) {
            page->addTopWidget(buildGameExtras());
        } else if (tab == UiTab::Graphics) {
            page->addTopWidget(buildGraphicsExtras());
        } else if (tab == UiTab::Audio) {
            // M5: device line, warnings and the test tone below the schema's [audio] keys.
            audioPanel_ = new AudioPanel(ctx_, page);
            page->addBottomWidget(audioPanel_);
            page->setEmptyNoteVisible(false);
            connect(audioPanel_, &AudioPanel::refreshDevicesRequested, this, [this] {
                if (!ctx_.audio.simulated) {
                    ctx_.audio = EnumerateAudioDevices();
                }
                QTimer::singleShot(0, this, &MainWindow::rebuildUi);
            });
        } else if (tab == UiTab::Paths) {
            page->addBottomWidget(buildPathsExtras());
        } else if (tab == UiTab::Advanced) {
            page->addTopWidget(buildDeveloperToggle());
            page->addBottomWidget(buildAdvancedExtras());
        }
        connect(page, &SettingsTab::valueEdited, this, [this](const KeyDef*) { onValueEdited(); });
        connect(page, &SettingsTab::tabReset, this, [this] { onValueEdited(); });
        tabs_->addTab(page, TabTitle(tab));
        settingsTabs_.append(page);
    }
    layout->addWidget(tabs_, 1);

    auto* footer = new QHBoxLayout();
    footerStatus_ = new QLabel(central);
    footerStatus_->setObjectName(QStringLiteral("footerStatus"));
    footerStatus_->setWordWrap(true);
    saveButton_ = new QPushButton(T("main.save"), central);
    saveButton_->setObjectName(QStringLiteral("saveButton"));
    saveButton_->setToolTip(T("main.save.tip").arg(ctx_.paths.iniPath));
    playButton_ = new QPushButton(T("main.play"), central);
    playButton_->setObjectName(QStringLiteral("playButton"));
    playButton_->setDefault(true);
    ScaleFont(playButton_, 1.35, true);
    playButton_->setMinimumHeight(playButton_->fontMetrics().height() * 2);
    playButton_->setMinimumWidth(playButton_->fontMetrics().horizontalAdvance(T("main.play")) +
                                 fontMetrics().height() * 6);
    playButton_->setToolTip(T("main.play.tip"));
    footer->addWidget(footerStatus_, 1);
    footer->addWidget(saveButton_, 0, Qt::AlignVCenter);
    footer->addSpacing(gap);
    footer->addWidget(playButton_, 0, Qt::AlignVCenter);
    layout->addLayout(footer);
    connect(saveButton_, &QPushButton::clicked, this, [this] { saveSettings(true); });
    connect(playButton_, &QPushButton::clicked, this, &MainWindow::startGame);

    setCentralWidget(central);
    updatePresetCombo();
    updateFooter();
}

void MainWindow::rebuildUi() {
    rebuilding_ = true;
    const int index = tabs_ != nullptr ? tabs_->currentIndex() : 0;
    QWidget* old = takeCentralWidget();
    delete old;
    statusPanel_ = nullptr;
    tabs_ = nullptr;
    bindingsPanel_ = nullptr;
    padTest_ = nullptr;
    audioPanel_ = nullptr;
    presetCombo_ = nullptr;
    frameRateCombo_ = nullptr;
    isoBox_ = nullptr;
    editorContext_.schema = &ctx_.model.schema();
    buildUi();
    tabs_->setCurrentIndex(index);
    if (hasResult_) {
        showRunResult(lastResult_);
    }
    rebuilding_ = false;
}

QWidget* MainWindow::buildHeader() {
    auto* header = new QWidget(this);
    auto* outer = new QVBoxLayout(header);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* row = new QHBoxLayout();
    const int unit = fontMetrics().height();
    auto* icon = new QLabel(header);
    icon->setPixmap(AppIconPixmap(unit * 3, devicePixelRatioF()));
    row->addWidget(icon, 0, Qt::AlignVCenter);
    row->addSpacing(unit / 2);
    auto* titles = new QVBoxLayout();
    titles->setSpacing(0);
    auto* title = new QLabel(QStringLiteral("FuturamaPC"), header);
    ScaleFont(title, 1.9, true);
    auto* subtitle = MutedLabel(T("main.subtitle"), header);
    subtitle->setWordWrap(false);
    titles->addWidget(title);
    titles->addWidget(subtitle);
    row->addLayout(titles);
    row->addStretch(1);

    auto* right = new QGridLayout();
    auto* langLabel = new QLabel(T("main.ui_language"), header);
    languageCombo_ = new QComboBox(header);
    languageCombo_->setObjectName(QStringLiteral("languageCombo"));
    languageCombo_->addItem(QStringLiteral("Deutsch"), QStringLiteral("de"));
    languageCombo_->addItem(QStringLiteral("English"), QStringLiteral("en"));
    languageCombo_->setCurrentIndex(CurrentLanguage() == Language::German ? 0 : 1);
    langLabel->setBuddy(languageCombo_);
    auto* wizardButton = new QPushButton(T("main.wizard"), header);
    wizardButton->setToolTip(T("main.wizard.tip"));
    right->addWidget(langLabel, 0, 0, Qt::AlignRight);
    right->addWidget(languageCombo_, 0, 1);
    right->addWidget(wizardButton, 1, 0, 1, 2);
    row->addLayout(right);
    outer->addLayout(row);

    connect(languageCombo_, &QComboBox::activated, this, [this](int) {
        const Language lang = languageCombo_->currentData().toString() == QLatin1String("de")
                                  ? Language::German
                                  : Language::English;
        QTimer::singleShot(0, this, [this, lang] { setUiLanguage(lang); });
    });
    connect(wizardButton, &QPushButton::clicked, this,
            [this] { QTimer::singleShot(0, this, [this] { runWizard(); }); });

    // Where everything is and where the settings come from.
    QStringList info;
    info.append(
        T("main.info_game").arg(ctx_.paths.gameFound() ? ctx_.paths.gameExe : T("main.info_game_missing")));
    info.append(
        T("main.info_config").arg(ctx_.paths.iniPath) +
        (ctx_.paths.portable ? QStringLiteral(" (") + T("main.portable") + QLatin1Char(')') : QString()));
    auto* infoLabel = MutedLabel(info.join(QStringLiteral("\n")), header);
    infoLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    outer->addWidget(infoLabel);
    for (const IniDocument::Problem& p : ctx_.model.ini().problems()) {
        outer->addWidget(ColoredLabel(ProblemLine(p), Severity::Error, header));
    }
    return header;
}

bool MainWindow::keyVisible(const KeyDef& key) const {
    // Developer keys only on request - or when the INI sets them (so nothing is hidden that is in effect).
    return !key.developer || ctx_.setting(QStringLiteral("ui/developer")) == QLatin1String("1") ||
           ctx_.model.isExplicit(key);
}

QWidget* MainWindow::buildDeveloperToggle() {
    int hidden = 0;
    for (const KeyDef* key : ctx_.model.schema().allKeys()) {
        hidden += key->developer ? 1 : 0;
    }
    auto* box = new QCheckBox(T("adv.developer"), this);
    box->setChecked(ctx_.setting(QStringLiteral("ui/developer")) == QLatin1String("1"));
    box->setToolTip(T("adv.developer.tip").arg(hidden));
    box->setVisible(hidden > 0);
    connect(box, &QCheckBox::toggled, this, [this](bool on) {
        ctx_.setSetting(QStringLiteral("ui/developer"), on ? QStringLiteral("1") : QStringLiteral("0"));
        QTimer::singleShot(0, this, &MainWindow::rebuildUi);
    });
    return box;
}

QWidget* MainWindow::buildGameExtras() {
    isoBox_ = new IsoStatusBox(ctx_, this);
    connect(isoBox_, &IsoStatusBox::isoChosen, this, [this](const QString& path) {
        if (const KeyDef* iso = ctx_.model.schema().find(QStringLiteral("paths"), QStringLiteral("iso"))) {
            ctx_.model.setValue(*iso, path);
            onValueEdited();
        }
    });
    return isoBox_;
}

void MainWindow::prepareScreenshot() {
    if (isoBox_ != nullptr) {
        isoBox_->checkNow();
    }
}

QWidget* MainWindow::buildGraphicsExtras() {
    auto* box = new QGroupBox(T("graphics.quality"), this);
    auto* grid = new QGridLayout(box);
    grid->setColumnStretch(1, 1);
    grid->setHorizontalSpacing(fontMetrics().height());
    auto* presetLabel = new QLabel(T("graphics.preset"), box);
    presetLabel->setProperty(SettingsTab::kFormLabelProperty, true);
    presetCombo_ = new QComboBox(box);
    presetCombo_->setObjectName(QStringLiteral("presetCombo"));
    for (QualityPreset p :
         {QualityPreset::Classic, QualityPreset::Standard, QualityPreset::Maximum, QualityPreset::Custom}) {
        presetCombo_->addItem(PresetTitle(p), static_cast<int>(p));
    }
    presetLabel->setBuddy(presetCombo_);
    const bool presetsAvailable =
        !ComputePreset(QualityPreset::Standard, ctx_.model.schema(), ctx_.gpu).values.isEmpty();
    presetCombo_->setEnabled(presetsAvailable);
    QString presetTip;
    for (QualityPreset p : {QualityPreset::Classic, QualityPreset::Standard, QualityPreset::Maximum}) {
        presetTip += QStringLiteral("<b>") + PresetTitle(p).toHtmlEscaped() + QStringLiteral(":</b> ") +
                     PresetSummary(p, ctx_.model.schema(), ctx_.gpu).toHtmlEscaped() + QStringLiteral("<br>");
    }
    presetCombo_->setToolTip(QStringLiteral("<qt>") + presetTip + QStringLiteral("</qt>"));
    grid->addWidget(presetLabel, 0, 0);
    grid->addWidget(presetCombo_, 0, 1, Qt::AlignLeft);
    connect(presetCombo_, &QComboBox::activated, this, [this](int index) { applyPreset(index); });
    int r = 1;
    grid->addWidget(MutedLabel(GpuSummary(ctx_), box), r++, 0, 1, 2);
    if (!ctx_.gpu.vrrHintText.isEmpty()) {
        grid->addWidget(MutedLabel(T("graphics.vrr").arg(ctx_.gpu.vrrHintText.text()), box), r++, 0, 1, 2);
    }
    if (ctx_.gpuFromScreens && !ctx_.gpuNote.isEmpty()) {
        grid->addWidget(ColoredLabel(T("graphics.gpu_fallback").arg(ctx_.gpuNote), Severity::Warning, box),
                        r++, 0, 1, 2);
    }
    // Frame rate: game builds before M3d have no player key for it (a developer key at most) - show the logic rate
    // read-only. Since M3d the schema's [render] frame_rate (60 | 120 | display) is a regular graphics key.
    bool schemaHasFrameRate = false;
    for (const KeyDef* key : ctx_.model.schema().keysForTab(UiTab::Graphics)) {
        schemaHasFrameRate = schemaHasFrameRate || key->name == QLatin1String("frame_rate");
    }
    if (!schemaHasFrameRate) {
        auto* label = new QLabel(T("graphics.frame_rate"), box);
        label->setProperty(SettingsTab::kFormLabelProperty, true);
        auto* combo = new QComboBox(box);
        combo->addItem(T("graphics.frame_rate_value").arg(60));
        combo->setEnabled(false);
        frameRateCombo_ = combo;
        combo->setToolTip(T("graphics.frame_rate.tip"));
        auto* line = new QHBoxLayout();
        line->setSpacing(fontMetrics().height() / 2);
        line->addWidget(combo);
        line->addWidget(MutedLabel(T("graphics.frame_rate.note"), box), 1);
        grid->addWidget(label, r, 0);
        grid->addLayout(line, r++, 1);
    }
    return box;
}

QWidget* MainWindow::buildPathsExtras() {
    auto* box = new QGroupBox(T("paths.folders"), this);
    auto* grid = new QGridLayout(box);
    grid->setColumnStretch(1, 1);
    grid->setHorizontalSpacing(fontMetrics().height());
    struct Entry {
        const char* label;
        std::function<QString()> path;
        bool isFile;
    };
    // Review M3c finding 19: only the locations the tab cannot edit (user data and screenshots have their own rows).
    const QList<Entry> entries = {
        {"paths.config_file", [this] { return ctx_.paths.iniPath; }, true},
        {"paths.log_dir", [this] { return LogDir(ctx_.userdata()); }, false},
        {"paths.saves_dir",
         [this] {
             const QString u = ctx_.userdata();
             return u.isEmpty() ? QString()
                                : QDir::toNativeSeparators(
                                      QDir(QDir::fromNativeSeparators(u)).filePath(QStringLiteral("hdd")));
         },
         false},
        {"paths.game_dir", [this] { return ctx_.paths.gameDir; }, false},
    };
    int r = 0;
    for (const Entry& e : entries) {
        const QString path = e.path();
        const QString folder = e.isFile ? QFileInfo(path).absolutePath() : path;
        auto* name = new QLabel(T(e.label), box);
        name->setProperty(SettingsTab::kFormLabelProperty, true);
        auto* value = new QLabel(path.isEmpty() ? T("paths.unknown") : path, box);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setWordWrap(true);
        auto* open = new QToolButton(box);
        open->setText(T("editor.open_folder"));
        const bool exists = !folder.isEmpty() && QFileInfo(folder).isDir();
        open->setEnabled(exists);
        open->setToolTip(exists ? T("editor.open_folder.tip") : T("paths.not_created"));
        connect(open, &QToolButton::clicked, this,
                [folder] { QDesktopServices::openUrl(QUrl::fromLocalFile(folder)); });
        grid->addWidget(name, r, 0, Qt::AlignLeft | Qt::AlignVCenter);
        grid->addWidget(value, r, 1, Qt::AlignVCenter);
        grid->addWidget(open, r, 2, Qt::AlignVCenter);
        ++r;
    }
    grid->addWidget(MutedLabel(T("paths.folders.note"), box), r, 0, 1, 3);
    return box;
}

QWidget* MainWindow::buildAdvancedExtras() {
    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* iniBox = new QGroupBox(T("adv.ini_notes"), container);
    auto* iniLayout = new QVBoxLayout(iniBox);
    const QList<IniDocument::Problem>& problems = ctx_.model.ini().problems();
    const QList<UnknownEntry> unknown = ctx_.model.unknownEntries();
    for (const IniDocument::Problem& p : problems) {
        iniLayout->addWidget(ColoredLabel(ProblemLine(p), Severity::Error, iniBox));
    }
    for (const UnknownEntry& e : unknown) {
        auto* row = new QHBoxLayout();
        const QString where = e.section.isEmpty() ? e.key : QStringLiteral("[%1] %2").arg(e.section, e.key);
        const QString text = (e.unknownSection ? T("adv.unknown_section") : T("adv.unknown_key"))
                                 .arg(where, e.value)
                                 .arg(e.line);
        row->addWidget(ColoredLabel(text, e.unknownSection ? Severity::Warning : Severity::Error, iniBox), 1);
        auto* remove = new QPushButton(T("adv.remove"), iniBox);
        row->addWidget(remove, 0, Qt::AlignTop);
        iniLayout->addLayout(row);
        connect(remove, &QPushButton::clicked, this, [this, e, remove] {
            ctx_.model.removeUnknown(e);
            remove->setEnabled(false);
            remove->setText(T("adv.removed"));
            onValueEdited();
        });
    }
    if (problems.isEmpty() && unknown.isEmpty()) {
        iniLayout->addWidget(MutedLabel(T("adv.ini_ok"), iniBox));
    }
    layout->addWidget(iniBox);

    auto* programBox = new QGroupBox(T("adv.program"), container);
    auto* programLayout = new QVBoxLayout(programBox);
    QString source = ctx_.schemaSource == QLatin1String("cache")  ? T("adv.schema_cache")
                     : ctx_.schemaSource == QLatin1String("file") ? T("adv.schema_file")
                                                                  : T("adv.schema_exe");
    programLayout->addWidget(
        MutedLabel(T("adv.schema").arg(FormatBuild(ctx_.model.schema().build), source), programBox));
    programLayout->addWidget(MutedLabel(
        T("adv.launcher_version").arg(QStringLiteral(LAUNCHER_VERSION), QStringLiteral(QT_VERSION_STR)),
        programBox));
    for (const QString& w : ctx_.schemaWarnings) {
        programLayout->addWidget(ColoredLabel(T("adv.schema_warning").arg(w), Severity::Warning, programBox));
    }
    auto* buttons = new QHBoxLayout();
    auto* refresh = new QPushButton(T("adv.refresh"), programBox);
    refresh->setToolTip(T("adv.refresh.tip"));
    refresh->setEnabled(ctx_.paths.gameFound());
    buttons->addWidget(refresh);
    buttons->addStretch(1);
    programLayout->addLayout(buttons);
    connect(refresh, &QPushButton::clicked, this,
            [this] { QTimer::singleShot(0, this, &MainWindow::refreshCapabilities); });
    layout->addWidget(programBox);
    return container;
}

void MainWindow::buildControlsExtras(SettingsTab* page) {
    bindingsPanel_ = new BindingsPanel(ctx_.model, padPoller_, [this](const KeyDef& key) { return keyVisible(key); },
                                       page);
    if (bindingsPanel_->hasBindings()) {
        page->addTopWidget(bindingsPanel_);
        page->setEmptyNoteVisible(false);
        connect(bindingsPanel_, &BindingsPanel::edited, this, [this] { onValueEdited(); });
        // "Auf Standard zuruecksetzen" of the tab also resets the bindings (before the window refreshes).
        connect(page, &SettingsTab::tabReset, bindingsPanel_, &BindingsPanel::resetAll);
        page->setExtraErrors([this] { return bindingsPanel_ != nullptr ? bindingsPanel_->errorCount() : 0; },
                             [this] { return bindingsPanel_ != nullptr && bindingsPanel_->focusFirstError(); });
        page->setExtraResetCount([this] { return bindingsPanel_ != nullptr ? bindingsPanel_->changedRowCount() : 0; });
    } else {
        // Game builds before M4: no [input] keys yet - the controller test still works.
        auto* note = new QLabel(T("controls.no_bindings"), page);
        note->setWordWrap(true);
        page->addTopWidget(note);
        page->setEmptyNoteVisible(false);
    }
    padTest_ = new PadTestPanel(ctx_.model, padPoller_, page);
    page->addBottomWidget(padTest_);
}

void MainWindow::selectTab(UiTab tab) {
    for (qsizetype i = 0; i < settingsTabs_.size(); ++i) {
        if (settingsTabs_[i]->tab() == tab) {
            tabs_->setCurrentIndex(static_cast<int>(i));
        }
    }
}

void MainWindow::onValueEdited() {
    for (SettingsTab* t : settingsTabs_) {
        t->refreshFromModel();
    }
    if (bindingsPanel_ != nullptr) {
        bindingsPanel_->refreshFromModel();
    }
    if (padTest_ != nullptr) {
        padTest_->refreshFromModel();
    }
    if (isoBox_ != nullptr) {
        isoBox_->refresh();
    }
    if (audioPanel_ != nullptr) {
        audioPanel_->refresh();
    }
    updatePresetCombo();
    updateFooter();
}

void MainWindow::updatePresetCombo() {
    if (frameRateCombo_ != nullptr) {
        // Game builds before M3d: the displayed frame rate is the game logic rate ([render] frame_rate follows
        // [game] tick_rate unless it is set; the advanced tab can still set it for tests).
        const ConfigSchema& schema = ctx_.model.schema();
        const KeyDef* frameRate = schema.find(QStringLiteral("render"), QStringLiteral("frame_rate"));
        const KeyDef* tickRate = schema.find(QStringLiteral("game"), QStringLiteral("tick_rate"));
        std::optional<qint64> fps;
        if (frameRate != nullptr && ctx_.model.isExplicit(*frameRate)) {
            fps = ParseIntText(ctx_.model.value(*frameRate));
        } else if (tickRate != nullptr) {
            fps = ParseIntText(ctx_.model.value(*tickRate));
        }
        frameRateCombo_->setItemText(0, fps && *fps > 0 ? T("graphics.frame_rate_value").arg(*fps)
                                                         : T("graphics.frame_rate_free"));
    }
    if (presetCombo_ == nullptr) {
        return;
    }
    const QualityPreset current =
        DetectPreset(ctx_.model.schema(), ctx_.gpu, [this](const KeyDef& k) { return ctx_.model.value(k); });
    presetCombo_->setCurrentIndex(presetCombo_->findData(static_cast<int>(current)));
}

void MainWindow::applyPreset(int index) {
    const auto preset = static_cast<QualityPreset>(presetCombo_->itemData(index).toInt());
    if (preset == QualityPreset::Custom) {
        updatePresetCombo();
        return;
    }
    for (const auto& [name, value] : ComputePreset(preset, ctx_.model.schema(), ctx_.gpu).values) {
        ctx_.model.setValue(name, value);
    }
    onValueEdited();
}

void MainWindow::updateFooter() {
    int errors = 0;
    for (qsizetype i = 0; i < settingsTabs_.size(); ++i) {
        const int n = settingsTabs_[i]->errorCount();
        errors += n;
        const QString title = TabTitle(settingsTabs_[i]->tab());
        tabs_->setTabText(static_cast<int>(i), n > 0 ? title + QStringLiteral(" ⚠") : title);
    }
    QPalette p = footerStatus_->palette();
    if (errors > 0) {
        footerStatus_->setText(T("main.status.errors").arg(errors));
        p.setColor(QPalette::WindowText, SeverityColor(Severity::Error));
    } else if (ctx_.model.isDirty()) {
        footerStatus_->setText(T("main.status.unsaved").arg(ctx_.model.pendingCount()));
        p.setColor(QPalette::WindowText, SeverityColor(Severity::Warning));
    } else if (!ctx_.model.fileExists()) {
        footerStatus_->setText(T("main.status.new_file"));
        p.setColor(QPalette::WindowText, MutedTextColor(footerStatus_));
    } else {
        footerStatus_->setText(T("main.status.saved"));
        p.setColor(QPalette::WindowText, MutedTextColor(footerStatus_));
    }
    footerStatus_->setPalette(p);
    saveButton_->setEnabled(ctx_.model.isDirty() || !ctx_.model.fileExists());
    playButton_->setEnabled(ctx_.paths.gameFound() && !session_.isRunning());
    if (!ctx_.paths.gameFound()) {
        playButton_->setToolTip(T("main.no_game"));
    }
}

bool MainWindow::saveSettings(bool interactive) {
    const QList<const KeyDef*> errors = ctx_.model.keysWithErrors();
    if (!errors.isEmpty()) {
        if (interactive) {
            QStringList names;
            for (const KeyDef* k : errors) {
                names.append(k->displayLabel());
            }
            Inform(this, QMessageBox::Warning, T("main.save"),
                   T("main.save_blocked").arg(names.join(QStringLiteral(", "))));
        }
        return false;
    }
    QString error;
    if (!ctx_.model.save(&error, ctx_.newIniHeader())) {
        if (interactive) {
            Inform(this, QMessageBox::Critical, T("main.save"),
                   T("main.save_failed").arg(ctx_.paths.iniPath, error));
        }
        return false;
    }
    if (interactive && !ctx_.model.lastBackup().isEmpty()) {
        // Review M3c finding 20: the file had invalid UTF-8; the original bytes are in <ini>.bak.
        Inform(this, QMessageBox::Information, T("main.save"), T("main.backup_written").arg(ctx_.model.lastBackup()));
    }
    if (!rebuilding_ && tabs_ != nullptr) {
        onValueEdited();
    }
    return true;
}

bool MainWindow::checkBeforePlay() {
    if (!ctx_.paths.gameFound()) {
        Inform(this, QMessageBox::Critical, T("main.play"), T("main.no_game"));
        return false;
    }
    const QList<const KeyDef*> errors = ctx_.model.keysWithErrors();
    if (!errors.isEmpty()) {
        QStringList names;
        for (const KeyDef* k : errors) {
            names.append(k->displayLabel());
        }
        Inform(this, QMessageBox::Warning, T("main.play"),
               T("main.play_blocked").arg(names.join(QStringLiteral(", "))));
        selectTab(errors.first()->tab);
        for (SettingsTab* t : settingsTabs_) {
            if (t->tab() == errors.first()->tab) {
                t->focusFirstError();
            }
        }
        return false;
    }
    // Lines the host rejects (unknown keys in known sections, broken lines) end the start with exit code 1.
    QStringList rejected;
    for (const IniDocument::Problem& p : ctx_.model.ini().problems()) {
        if (p.line > 0) {
            rejected.append(T("main.reject_line").arg(p.line).arg(IniDocument::ProblemText(p)));
        }
    }
    for (const UnknownEntry& e : ctx_.model.unknownEntries()) {
        if (!e.unknownSection) {
            rejected.append(e.section.isEmpty() ? e.key : QStringLiteral("[%1] %2").arg(e.section, e.key));
        }
    }
    if (!rejected.isEmpty()) {
        const int choice = AskQuestion(this, QMessageBox::Warning, T("main.play"),
                                       T("main.ini_rejected").arg(rejected.join(QStringLiteral(", "))),
                                       {T("main.show_notes"), T("main.start_anyway"), T("common.cancel")}, 0);
        if (choice == 0) {
            selectTab(UiTab::Advanced);
        }
        if (choice != 1) {
            return false;
        }
    }
    if (const KeyDef* iso = ctx_.model.schema().find(QStringLiteral("paths"), QStringLiteral("iso"))) {
        const QString value = ctx_.model.value(*iso).trimmed();
        if (value.isEmpty()) {
            const int choice = AskQuestion(this, QMessageBox::Information, T("main.play"), T("main.no_iso"),
                                           {T("main.open_wizard"), T("common.cancel")}, 0);
            if (choice == 0) {
                QTimer::singleShot(0, this, [this] { runWizard(); });
            }
            return false;
        }
        if (!QFileInfo(ResolvePath(value, ctx_.model.iniDir())).isFile()) {
            const int choice =
                AskQuestion(this, QMessageBox::Warning, T("main.play"), T("main.iso_missing").arg(value),
                            {T("main.start_anyway"), T("common.cancel")}, 1);
            if (choice != 0) {
                return false;
            }
        } else if (isoBox_ != nullptr) {
            // Review M3c finding 9: a known unsupported version or a non-Xbox file ends the start with exit code 2.
            const std::optional<IsoCheckResult> check = isoBox_->currentResult();
            if (check && (check->verdict() == IsoVerdict::UnsupportedVersion ||
                          check->verdict() == IsoVerdict::NotXboxImage)) {
                const QString why = check->message.isEmpty()
                                        ? (check->verdict() == IsoVerdict::NotXboxImage ? T("wiz.iso.not_xbox")
                                                                                        : check->versionDisplayName())
                                        : check->message.text();
                const int choice = AskQuestion(
                    this, QMessageBox::Warning, T("main.play"),
                    T("main.iso_unsupported").arg(QDir::toNativeSeparators(ResolvePath(value, ctx_.model.iniDir())), why),
                    {T("main.choose_iso"), T("main.start_anyway"), T("common.cancel")}, 2);
                if (choice == 0) {
                    selectTab(UiTab::Game);
                    QTimer::singleShot(0, this, [this] {
                        if (isoBox_ != nullptr) {
                            isoBox_->browseForImage();
                        }
                    });
                }
                if (choice != 1) {
                    return false;
                }
            }
        }
    }
    return true;
}

void MainWindow::startGame() {
    if (session_.isRunning() || !checkBeforePlay()) {
        return;
    }
    if ((ctx_.model.isDirty() || !ctx_.model.fileExists()) && !saveSettings(true)) {
        return;
    }
    hideRunResult();
    playButton_->setEnabled(false);
    if (session_.start(ctx_.paths.gameExe, ctx_.paths.iniPath, ctx_.logFile())) {
        hide();
    }
}

void MainWindow::onGameFinished(const GameRunResult& result) {
    lastResult_ = result;
    hasResult_ = true;
    showRunResult(result);
    if (!isVisible()) {
        show();
    }
    raise();
    activateWindow();
    updateFooter();
    emit gameFinished(result);
}

void MainWindow::showRunResult(const GameRunResult& result) {
    lastResult_ = result;
    hasResult_ = true;
    statusPanel_->showResult(result);
    statusPanel_->setVisible(true);
}

void MainWindow::hideRunResult() {
    hasResult_ = false;
    if (statusPanel_ != nullptr) {
        statusPanel_->setVisible(false);
    }
}

bool MainWindow::runWizard() {
    FirstRunWizard wizard(ctx_, this);
    connect(&wizard, &FirstRunWizard::uiLanguageChanged, this, [this](Language) { rebuildUi(); });
    if (wizard.exec() != QDialog::Accepted) {
        return false;
    }
    wizard.applyToModel();
    const bool saved = saveSettings(true);
    rebuildUi();
    return saved;
}

void MainWindow::setUiLanguage(Language language) {
    SetLanguage(language);
    ctx_.setSetting(QStringLiteral("ui/language"), LanguageCode(language));
    rebuildUi();
}

void MainWindow::refreshCapabilities() {
    if (ctx_.model.isDirty()) {
        const int choice =
            AskQuestion(this, QMessageBox::Question, T("adv.refresh"), T("main.unsaved_question"),
                        {T("main.save"), T("main.discard"), T("common.cancel")}, 0);
        if (choice == 2 || choice < 0 || (choice == 0 && !saveSettings(true))) {
            return;
        }
    }
    StartupOptions refresh = options_;
    refresh.refresh = true;
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = LoadSchema(ctx_, refresh, &error);
    if (ok) {
        LoadGpu(ctx_, refresh);
        QString iniError;
        LoadIni(ctx_, &iniError);
    }
    QApplication::restoreOverrideCursor();
    if (!ok) {
        Inform(this, QMessageBox::Critical, T("adv.refresh"), error);
        return;
    }
    rebuildUi();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (ctx_.model.isDirty()) {
        const int choice =
            AskQuestion(this, QMessageBox::Question, T("main.window_title"), T("main.unsaved_question"),
                        {T("main.save"), T("main.discard"), T("common.cancel")}, 0);
        if (choice == 2 || choice < 0 || (choice == 0 && !saveSettings(true))) {
            event->ignore();
            return;
        }
    }
    ctx_.setSetting(QStringLiteral("window/geometry"), QString::fromLatin1(saveGeometry().toBase64()));
    event->accept();
}

void MainWindow::changeEvent(QEvent* event) {
    if (event->type() == QEvent::ThemeChange && !rebuilding_ && tabs_ != nullptr) {
        QTimer::singleShot(0, this, &MainWindow::rebuildUi);
    }
    QMainWindow::changeEvent(event);
}

}  // namespace fl::ui
