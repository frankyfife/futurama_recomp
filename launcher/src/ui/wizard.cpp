#include "ui/wizard.h"

#include "core/game_cli.h"
#include "core/validation.h"
#include "ui/app_style.h"
#include "ui/iso_status.h"
#include "ui/key_editors.h"
#include "ui/screen_info.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QRadioButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWizardPage>

namespace fl::ui {
namespace {

const QString kIso = QStringLiteral("paths.iso");
const QString kLanguage = QStringLiteral("settings.language");
const QString kFullscreen = QStringLiteral("render.fullscreen");
const QString kWindowScale = QStringLiteral("render.window_scale");

QLabel* WrapLabel(QWidget* parent) {
    auto* l = new QLabel(parent);
    l->setWordWrap(true);
    return l;
}

EditorContext MakeEditorContext(AppContext& ctx) {
    EditorContext ec;
    ec.gpu = &ctx.gpu;
    ec.schema = &ctx.model.schema();
    ec.iniDir = [&ctx] { return ctx.model.iniDir(); };
    ec.currentValue = [&ctx](const KeyDef& key) { return ctx.model.value(key); };
    return ec;
}

class WizardPageBase : public QWizardPage {
public:
    WizardPageBase(FirstRunWizard* wizard, int pageId) : wizard_(wizard), pageId_(pageId) {}
    virtual void retranslate() = 0;
    int nextId() const override { return wizard_->nextPageAfter(pageId_); }

protected:
    FirstRunWizard* wizard_;
    int pageId_;
};

// --- 1. welcome + UI language ---------------------------------------------------------------------
class WelcomePageImpl final : public WizardPageBase {
public:
    explicit WelcomePageImpl(FirstRunWizard* wizard) : WizardPageBase(wizard, FirstRunWizard::WelcomePage) {
        auto* layout = new QVBoxLayout(this);
        text_ = WrapLabel(this);
        ownCopy_ = WrapLabel(this);
        auto* row = new QHBoxLayout();
        languageLabel_ = new QLabel(this);
        language_ = new QComboBox(this);
        language_->addItem(QStringLiteral("Deutsch"), QStringLiteral("de"));
        language_->addItem(QStringLiteral("English"), QStringLiteral("en"));
        language_->setCurrentIndex(CurrentLanguage() == Language::German ? 0 : 1);
        languageLabel_->setBuddy(language_);
        row->addWidget(languageLabel_);
        row->addWidget(language_);
        row->addStretch(1);
        configAt_ = WrapLabel(this);
        configAt_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(text_);
        layout->addSpacing(fontMetrics().height() / 2);
        layout->addWidget(ownCopy_);
        layout->addSpacing(fontMetrics().height());
        layout->addLayout(row);
        layout->addStretch(1);
        layout->addWidget(configAt_);
        connect(language_, &QComboBox::activated, this, [this](int) {
            const Language lang = language_->currentData().toString() == QLatin1String("de")
                                      ? Language::German
                                      : Language::English;
            SetLanguage(lang);
            wizard_->context().setSetting(QStringLiteral("ui/language"), LanguageCode(lang));
            wizard_->retranslateAll();
            emit wizard_->uiLanguageChanged(lang);
        });
    }
    void retranslate() override {
        setTitle(T("wiz.welcome.title"));
        setSubTitle(T("wiz.welcome.subtitle"));
        text_->setText(T("wiz.welcome.text"));
        ownCopy_->setText(T("wiz.welcome.own_copy"));
        languageLabel_->setText(T("wiz.welcome.ui_language"));
        configAt_->setText(T("wiz.welcome.config_at").arg(wizard_->context().paths.iniPath));
        language_->setCurrentIndex(CurrentLanguage() == Language::German ? 0 : 1);
    }

private:
    QLabel* text_;
    QLabel* ownCopy_;
    QLabel* languageLabel_;
    QComboBox* language_;
    QLabel* configAt_;
};

// --- 2. ISO ---------------------------------------------------------------------------------------
class IsoPageImpl final : public WizardPageBase {
public:
    explicit IsoPageImpl(FirstRunWizard* wizard) : WizardPageBase(wizard, FirstRunWizard::IsoPage) {
        auto* layout = new QVBoxLayout(this);
        text_ = WrapLabel(this);
        auto* row = new QHBoxLayout();
        edit_ = new QLineEdit(this);
        edit_->setObjectName(QStringLiteral("isoEdit"));
        edit_->setClearButtonEnabled(true);
        browse_ = new QPushButton(this);
        row->addWidget(edit_, 1);
        row->addWidget(browse_);
        result_ = WrapLabel(this);
        result_->setTextFormat(Qt::RichText);
        result_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        details_ = WrapLabel(this);
        details_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(text_);
        layout->addSpacing(fontMetrics().height() / 2);
        layout->addLayout(row);
        layout->addSpacing(fontMetrics().height() / 2);
        layout->addWidget(result_);
        layout->addWidget(details_);
        layout->addStretch(1);
        debounce_ = new QTimer(this);
        debounce_->setSingleShot(true);
        debounce_->setInterval(400);
        connect(debounce_, &QTimer::timeout, this, [this] { startCheck(); });
        connect(edit_, &QLineEdit::textEdited, this, [this](const QString& text) {
            // Review M3c finding 10: a path pasted in quotes (Explorer's "Als Pfad kopieren") without them.
            wizard_->state().iso = QDir::toNativeSeparators(CleanPathInput(text));
            showResult();
            emit completeChanged();
            debounce_->start();
        });
        connect(browse_, &QPushButton::clicked, this, [this] { browse(); });
        connect(wizard_, &FirstRunWizard::isoStateChanged, this, [this] {
            showResult();
            emit completeChanged();
        });
    }
    void initializePage() override {
        edit_->setText(wizard_->state().iso);
        if (wizard_->state().checkedIso != wizard_->state().iso && !wizard_->state().checking) {
            startCheck();
        }
        showResult();
    }
    bool isComplete() const override { return wizard_->state().isoAcceptable(); }
    void retranslate() override {
        setTitle(T("wiz.iso.title"));
        setSubTitle(T("wiz.iso.subtitle"));
        text_->setText(T("wiz.iso.text"));
        browse_->setText(T("wiz.iso.browse"));
        edit_->setPlaceholderText(T("wiz.iso.placeholder"));
        showResult();
    }

private:
    void browse() {
        QString start = QFileInfo(wizard_->state().iso).absolutePath();
        if (wizard_->state().iso.isEmpty() || !QFileInfo::exists(start)) {
            start = QDir::homePath();
        }
        const QString file =
            QFileDialog::getOpenFileName(this, T("wiz.iso.dialog"), start, T("editor.iso_filter"));
        if (!file.isEmpty()) {
            wizard_->state().iso = QDir::toNativeSeparators(file);
            edit_->setText(wizard_->state().iso);
            startCheck();
        }
    }

    void startCheck() {
        WizardState& s = wizard_->state();
        if (s.iso.isEmpty() || !QFileInfo(s.iso).isFile()) {
            s.checking = false;
            showResult();
            emit completeChanged();
            return;
        }
        s.checking = true;
        wizard_->isoJob()->start(s.iso);
        showResult();
        emit completeChanged();
    }

    void showResult() {
        const WizardState& s = wizard_->state();
        auto colored = [](Severity sev, const QString& html) {
            return QStringLiteral("<span style=\"color:%1\">%2</span>")
                .arg(ColorCss(SeverityColor(sev)), html);
        };
        details_->clear();
        if (s.iso.isEmpty()) {
            result_->setText(T("wiz.iso.none"));
            return;
        }
        if (s.checking) {
            result_->setText(T("wiz.iso.checking"));
            return;
        }
        if (!QFileInfo(s.iso).isFile()) {
            result_->setText(colored(Severity::Error, T("wiz.iso.not_found")));
            return;
        }
        if (s.checkedIso != s.iso) {
            result_->setText(T("wiz.iso.checking"));
            return;
        }
        const IsoCheckResult& r = s.check;
        result_->setText(IsoVerdictHtml(r));
        const QStringList lines = IsoDetailLines(r, true);
        details_->setText(lines.join(QLatin1Char('\n')));
    }

    QLabel* text_;
    QLineEdit* edit_;
    QPushButton* browse_;
    QLabel* result_;
    QLabel* details_;
    QTimer* debounce_;
};

// --- 3. game language -----------------------------------------------------------------------------
class LanguagePageImpl final : public WizardPageBase {
public:
    explicit LanguagePageImpl(FirstRunWizard* wizard) : WizardPageBase(wizard, FirstRunWizard::LanguagePage) {
        auto* layout = new QVBoxLayout(this);
        text_ = WrapLabel(this);
        auto* row = new QHBoxLayout();
        label_ = new QLabel(this);
        combo_ = new QComboBox(this);
        label_->setBuddy(combo_);
        row->addWidget(label_);
        row->addWidget(combo_);
        row->addStretch(1);
        note_ = WrapLabel(this);
        layout->addWidget(text_);
        layout->addSpacing(fontMetrics().height() / 2);
        layout->addLayout(row);
        layout->addWidget(note_);
        layout->addStretch(1);
        connect(combo_, &QComboBox::activated, this,
                [this](int) { wizard_->state().gameLanguage = combo_->currentData().toString(); });
    }
    void initializePage() override { fill(); }
    void retranslate() override {
        setTitle(T("wiz.lang.title"));
        setSubTitle(T("wiz.lang.subtitle"));
        text_->setText(T("wiz.lang.text"));
        if (const KeyDef* key = wizard_->context().model.schema().find(kLanguage)) {
            label_->setText(key->displayLabel() + QLatin1Char(':'));
            note_->setText(key->help.text());
        }
        fill();
    }

private:
    void fill() {
        const KeyDef* key = wizard_->context().model.schema().find(kLanguage);
        combo_->clear();
        if (key == nullptr) {
            return;
        }
        for (const ChoiceDef& c : key->choices) {
            if (!c.isRange()) {
                combo_->addItem(c.label.text(), c.value);
            }
        }
        const int index = combo_->findData(wizard_->state().gameLanguage, Qt::UserRole, Qt::MatchFixedString);
        combo_->setCurrentIndex(index >= 0 ? index : 0);
        wizard_->state().gameLanguage = combo_->currentData().toString();
    }

    QLabel* text_;
    QLabel* label_;
    QComboBox* combo_;
    QLabel* note_;
};

// --- 4. display -----------------------------------------------------------------------------------
class DisplayPageImpl final : public WizardPageBase {
public:
    explicit DisplayPageImpl(FirstRunWizard* wizard) : WizardPageBase(wizard, FirstRunWizard::DisplayPage) {
        AppContext& ctx = wizard_->context();
        const ConfigSchema& schema = ctx.model.schema();
        auto* layout = new QVBoxLayout(this);
        text_ = WrapLabel(this);
        layout->addWidget(text_);
        layout->addSpacing(fontMetrics().height() / 2);
        auto* form = new QFormLayout();
        form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
        if (const KeyDef* monitor = schema.findByWidget(UiWidget::Monitor)) {
            monitorLabel_ = new QLabel(this);
            monitor_ = CreateKeyEditor(*monitor, MakeEditorContext(ctx), this);
            form->addRow(monitorLabel_, monitor_);
            connect(monitor_, &KeyEditor::edited, this,
                    [this](const QString& v) { wizard_->state().monitor = v; });
        } else {
            // No monitor key in this build: show where the game will appear.
            monitorInfo_ = WrapLabel(this);
            form->addRow(monitorInfo_);
        }
        if (schema.find(kFullscreen) != nullptr) {
            modeLabel_ = new QLabel(this);
            auto* modes = new QVBoxLayout();
            window_ = new QRadioButton(this);
            fullscreen_ = new QRadioButton(this);
            modes->addWidget(window_);
            modes->addWidget(fullscreen_);
            form->addRow(modeLabel_, modes);
            auto* group = new QButtonGroup(this);
            group->addButton(window_);
            group->addButton(fullscreen_);
            connect(fullscreen_, &QRadioButton::toggled, this, [this](bool on) {
                wizard_->state().fullscreen = on;
                if (scale_ != nullptr) {
                    scale_->setEnabled(!on);
                }
            });
        }
        if (const KeyDef* scale = schema.find(kWindowScale)) {
            scaleLabel_ = new QLabel(this);
            scale_ = CreateKeyEditor(*scale, MakeEditorContext(ctx), this);
            form->addRow(scaleLabel_, scale_);
            connect(scale_, &KeyEditor::edited, this,
                    [this](const QString& v) { wizard_->state().windowScale = v; });
        }
        layout->addLayout(form);
        layout->addSpacing(fontMetrics().height() / 2);
        note_ = WrapLabel(this);
        layout->addWidget(note_);
        layout->addStretch(1);
    }
    void initializePage() override {
        const WizardState& s = wizard_->state();
        if (monitor_ != nullptr) {
            monitor_->setValue(s.monitor);
        }
        if (fullscreen_ != nullptr) {
            (s.fullscreen ? fullscreen_ : window_)->setChecked(true);
        }
        if (scale_ != nullptr) {
            scale_->setValue(s.windowScale);
            scale_->setEnabled(!s.fullscreen);
        }
    }
    void retranslate() override {
        const ConfigSchema& schema = wizard_->context().model.schema();
        setTitle(T("wiz.display.title"));
        setSubTitle(T("wiz.display.subtitle"));
        text_->setText(T("wiz.display.text"));
        if (monitorLabel_ != nullptr) {
            monitorLabel_->setText(schema.findByWidget(UiWidget::Monitor)->displayLabel() + QLatin1Char(':'));
        }
        if (monitorInfo_ != nullptr) {
            const GpuOutput* primary = wizard_->context().gpu.primaryOutput();
            monitorInfo_->setText(primary != nullptr
                                      ? T("wiz.display.monitor_info").arg(OutputDisplayText(*primary, false))
                                      : QString());
        }
        if (modeLabel_ != nullptr) {
            modeLabel_->setText(T("wiz.display.mode"));
            window_->setText(T("wiz.display.window"));
            fullscreen_->setText(T("wiz.display.fullscreen"));
        }
        if (scaleLabel_ != nullptr) {
            scaleLabel_->setText(schema.find(kWindowScale)->displayLabel() + QLatin1Char(':'));
        }
        QString note = T("wiz.display.note");
        if (wizard_->context().gpuFromScreens) {
            note += QStringLiteral("\n") + T("wiz.display.from_screens");
        }
        note_->setText(note);
    }

private:
    QLabel* text_ = nullptr;
    QLabel* monitorLabel_ = nullptr;
    KeyEditor* monitor_ = nullptr;
    QLabel* monitorInfo_ = nullptr;
    QLabel* modeLabel_ = nullptr;
    QRadioButton* window_ = nullptr;
    QRadioButton* fullscreen_ = nullptr;
    QLabel* scaleLabel_ = nullptr;
    KeyEditor* scale_ = nullptr;
    QLabel* note_ = nullptr;
};

// --- 5. quality preset ----------------------------------------------------------------------------
class QualityPageImpl final : public WizardPageBase {
public:
    explicit QualityPageImpl(FirstRunWizard* wizard) : WizardPageBase(wizard, FirstRunWizard::QualityPage) {
        auto* layout = new QVBoxLayout(this);
        text_ = WrapLabel(this);
        layout->addWidget(text_);
        layout->addSpacing(fontMetrics().height() / 2);
        auto* group = new QButtonGroup(this);
        const QList<QualityPreset> presets = {QualityPreset::Classic, QualityPreset::Standard,
                                              QualityPreset::Maximum, QualityPreset::Custom};
        for (QualityPreset p : presets) {
            Option o;
            o.preset = p;
            o.radio = new QRadioButton(this);
            ScaleFont(o.radio, 1.0, true);
            o.description = WrapLabel(this);
            o.description->setIndent(fontMetrics().height() + fontMetrics().height() / 2);
            group->addButton(o.radio);
            layout->addWidget(o.radio);
            layout->addWidget(o.description);
            layout->addSpacing(fontMetrics().height() / 3);
            connect(o.radio, &QRadioButton::toggled, this, [this, p](bool on) {
                if (on) {
                    wizard_->state().preset = p;
                }
            });
            options_.append(o);
        }
        layout->addSpacing(fontMetrics().height() / 2);
        gpu_ = WrapLabel(this);
        layout->addWidget(gpu_);
        layout->addStretch(1);
    }
    void initializePage() override {
        for (Option& o : options_) {
            o.radio->setChecked(o.preset == wizard_->state().preset);
        }
    }
    void retranslate() override {
        AppContext& ctx = wizard_->context();
        setTitle(T("wiz.quality.title"));
        setSubTitle(T("wiz.quality.subtitle"));
        text_->setText(T("wiz.quality.text"));
        for (Option& o : options_) {
            o.radio->setText(o.preset == QualityPreset::Custom ? T("wiz.quality.keep")
                                                               : PresetTitle(o.preset));
            o.description->setText(o.preset == QualityPreset::Custom
                                       ? T("wiz.quality.keep.detail")
                                       : PresetSummary(o.preset, ctx.model.schema(), ctx.gpu));
            const bool visible = o.preset != QualityPreset::Custom || ctx.model.fileExists();
            o.radio->setVisible(visible);
            o.description->setVisible(visible);
        }
        gpu_->setText(GpuSummary(ctx));
    }

private:
    struct Option {
        QualityPreset preset = QualityPreset::Standard;
        QRadioButton* radio = nullptr;
        QLabel* description = nullptr;
    };
    QLabel* text_;
    QList<Option> options_;
    QLabel* gpu_;
};

// --- 6. summary -----------------------------------------------------------------------------------
class FinishPageImpl final : public WizardPageBase {
public:
    explicit FinishPageImpl(FirstRunWizard* wizard) : WizardPageBase(wizard, FirstRunWizard::FinishPage) {
        auto* layout = new QVBoxLayout(this);
        summary_ = WrapLabel(this);
        summary_->setTextFormat(Qt::RichText);
        summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        note_ = WrapLabel(this);
        layout->addWidget(summary_);
        layout->addSpacing(fontMetrics().height());
        layout->addWidget(note_);
        layout->addStretch(1);
    }
    void initializePage() override { retranslate(); }
    void retranslate() override {
        AppContext& ctx = wizard_->context();
        const WizardState& s = wizard_->state();
        const ConfigSchema& schema = ctx.model.schema();
        setTitle(T("wiz.finish.title"));
        setSubTitle(T("wiz.finish.subtitle"));
        auto row = [](QString name, QString value) {
            if (name.endsWith(QLatin1Char(':'))) {  // form labels ("Darstellung:") without the colon here
                name.chop(1);
            }
            if (!value.isEmpty()) {
                value[0] = value[0].toUpper();
            }
            return QStringLiteral("<tr><td style=\"padding-right:12px\"><b>%1</b></td><td>%2</td></tr>")
                .arg(name.toHtmlEscaped(), value.toHtmlEscaped());
        };
        QString html = QStringLiteral("<table>");
        html += row(T("wiz.finish.iso"), s.iso.isEmpty() ? T("wiz.finish.none") : s.iso);
        if (s.checkedIso == s.iso && !s.iso.isEmpty() && s.check.runError.isEmpty()) {
            html += row(T("wiz.finish.version"), s.check.versionDisplayName());
        }
        if (const KeyDef* key = schema.find(kLanguage)) {
            html += row(key->displayLabel(), ChoiceText(*key, s.gameLanguage));
        }
        if (const KeyDef* monitor = schema.findByWidget(UiWidget::Monitor)) {
            const GpuOutput* o = OutputForMonitorValue(*monitor, s.monitor, ctx.gpu);
            const GpuOutput* primary = ctx.gpu.primaryOutput();
            html += row(monitor->displayLabel(), o != nullptr         ? OutputDisplayText(*o)
                                                 : primary != nullptr ? T("monitor.auto_named").arg(primary->name)
                                                                      : T("monitor.auto"));
        }
        if (schema.find(kFullscreen) != nullptr) {
            html += row(T("wiz.display.mode"),
                        s.fullscreen ? T("wiz.display.fullscreen") : T("wiz.display.window"));
        }
        html += row(T("wiz.finish.quality"),
                    s.preset == QualityPreset::Custom ? T("wiz.quality.keep") : PresetTitle(s.preset));
        html += QStringLiteral("</table>");
        summary_->setText(html);
        note_->setText(T("wiz.finish.note").arg(ctx.paths.iniPath));
    }

private:
    QLabel* summary_;
    QLabel* note_;
};

}  // namespace

bool WizardState::isoAcceptable() const {
    if (iso.isEmpty() || checking || checkedIso != iso) {
        return false;
    }
    const IsoVerdict v = check.verdict();
    return v != IsoVerdict::NotXboxImage;
}

QString PresetTitle(QualityPreset preset) {
    switch (preset) {
    case QualityPreset::Classic:
        return T("preset.classic");
    case QualityPreset::Standard:
        return T("preset.standard");
    case QualityPreset::Maximum:
        return T("preset.maximum");
    case QualityPreset::Custom:
        return T("preset.custom");
    }
    return QString();
}

QString PresetSummary(QualityPreset preset, const ConfigSchema& schema, const GpuInfo& gpu) {
    const PresetValues values = ComputePreset(preset, schema, gpu);
    if (values.values.isEmpty()) {
        return T("preset.unavailable");
    }
    QStringList parts;
    const QString res = values.value(QLatin1String(kKeyResolution));
    if (!res.isEmpty()) {
        if (IsAutoResolution(res)) {
            const GpuOutput* o = gpu.primaryOutput();
            // resolution auto: the 4:3 area of the window, at most the full height of the monitor.
            parts.append(o != nullptr && o->nativeSize().isValid()
                             ? T("preset.native_size").arg(SizeText(FitAspect43(o->nativeSize())))
                             : T("preset.native"));
        } else {
            const auto size = ParseResolutionText(res);
            parts.append(size && size->isValid() ? SizeText(*size) : res);
        }
    }
    const QString scale = values.value(QLatin1String(kKeyRenderScale));
    if (const auto s = ParseFloatText(scale); s && *s > 1.0) {
        parts.append(T("preset.supersampling").arg(QLocale().toString(*s, 'g', 3)));
    }
    if (schema.find(QLatin1String(kKeyMsaa)) != nullptr) {
        const QString m = values.value(QLatin1String(kKeyMsaa));
        parts.append(ParseIntText(m).value_or(0) > 1 ? T("preset.msaa").arg(m) : T("preset.no_aa"));
    }
    if (schema.find(QLatin1String(kKeyAnisotropy)) != nullptr) {
        const QString a = values.value(QLatin1String(kKeyAnisotropy));
        parts.append(ParseIntText(a).value_or(0) > 1 ? T("preset.aniso").arg(a) : T("preset.no_aniso"));
    }
    if (ParseBoolText(values.value(QLatin1String(kKeyLinearAa))).value_or(false)) {
        parts.append(T("preset.linear"));
    }
    // Review M3c finding 21: what the back buffers need, and whether the video memory limited the preset.
    if (preset != QualityPreset::Classic && values.videoMemoryMb > 0) {
        const QString size = values.videoMemoryMb >= 1024
                                 ? QLocale().toString(static_cast<double>(values.videoMemoryMb) / 1024.0, 'f', 1) +
                                       QStringLiteral(" GB")
                                 : QLocale().toString(values.videoMemoryMb) + QStringLiteral(" MB");
        parts.append(T("preset.memory").arg(size));
    }
    if (values.limitedByMemory) {
        parts.append(T("preset.memory_limited"));
    }
    QString text = parts.join(QStringLiteral(" · "));
    if (preset == QualityPreset::Classic) {
        text += QStringLiteral(" – ") + T("preset.classic.note");
    }
    return text;
}

QString GpuSummary(const AppContext& ctx) {
    if (ctx.gpuFromScreens || ctx.gpu.adapter() == nullptr) {
        return T("gpu.unknown");
    }
    QString text = ctx.gpu.adapter()->name;
    QStringList caps;
    if (ctx.gpu.maxMsaa() > 1) {
        caps.append(T("gpu.msaa_max").arg(ctx.gpu.maxMsaa()));
    }
    if (ctx.gpu.maxAnisotropy > 1) {
        caps.append(T("gpu.aniso_max").arg(ctx.gpu.maxAnisotropy));
    }
    if (!caps.isEmpty()) {
        text += QStringLiteral(" – ") + caps.join(QStringLiteral(", "));
    }
    return T("gpu.line").arg(text);
}

FirstRunWizard::FirstRunWizard(AppContext& ctx, QWidget* parent) : QWizard(parent), ctx_(ctx) {
    setWizardStyle(QWizard::ModernStyle);
    setOption(QWizard::NoBackButtonOnStartPage, true);
    setOption(QWizard::HaveHelpButton, false);
    setWindowIcon(AppIcon());
    setPixmap(QWizard::LogoPixmap, AppIconPixmap(48, devicePixelRatioF()));
    isoJob_ = new IsoCheckJob(ctx.paths.gameExe, ctx.paths.cacheDir(), this);
    connect(isoJob_, &IsoCheckJob::finished, this, [this](const IsoCheckResult& result, const QString& iso) {
        if (iso == state_.iso) {
            state_.check = result;
            state_.checkedIso = iso;
            state_.checking = false;
            emit isoStateChanged();
        }
    });

    // Initial state from the INI (a re-run of the wizard shows the current settings).
    const ConfigSchema& schema = ctx.model.schema();
    if (const KeyDef* iso = schema.find(kIso)) {
        state_.iso = QDir::toNativeSeparators(ctx.model.value(*iso));
    }
    if (const KeyDef* lang = schema.find(kLanguage)) {
        const bool explicitValue = ctx.model.iniValue(*lang).has_value();
        state_.gameLanguage = explicitValue
                                  ? ctx.model.value(*lang)
                                  : (CurrentLanguage() == Language::German ? QStringLiteral("german")
                                                                           : QStringLiteral("english"));
    }
    if (const KeyDef* monitor = schema.findByWidget(UiWidget::Monitor)) {
        state_.monitor = ctx.model.value(*monitor);
    }
    if (const KeyDef* fs = schema.find(kFullscreen)) {
        state_.fullscreen = ParseBoolText(ctx.model.value(*fs)).value_or(false);
    }
    if (const KeyDef* scale = schema.find(kWindowScale)) {
        state_.windowScale = ctx.model.value(*scale);
    }
    if (ctx.model.fileExists()) {
        state_.preset = DetectPreset(schema, ctx.gpu, [&ctx](const KeyDef& k) { return ctx.model.value(k); });
    }

    setPage(WelcomePage, new WelcomePageImpl(this));
    setPage(IsoPage, new IsoPageImpl(this));
    setPage(LanguagePage, new LanguagePageImpl(this));
    setPage(DisplayPage, new DisplayPageImpl(this));
    setPage(QualityPage, new QualityPageImpl(this));
    setPage(FinishPage, new FinishPageImpl(this));
    setStartId(WelcomePage);
    retranslateAll();
    const int unit = fontMetrics().height();
    resize(unit * 48, unit * 36);
}

FirstRunWizard::~FirstRunWizard() = default;

bool FirstRunWizard::pageAvailable(int page) const {
    const ConfigSchema& schema = ctx_.model.schema();
    switch (page) {
    case LanguagePage:
        return schema.find(kLanguage) != nullptr;
    case DisplayPage:
        return schema.findByWidget(UiWidget::Monitor) != nullptr || schema.find(kFullscreen) != nullptr ||
               schema.find(kWindowScale) != nullptr;
    case QualityPage:
        return !ComputePreset(QualityPreset::Standard, schema, ctx_.gpu).values.isEmpty();
    default:
        return true;
    }
}

int FirstRunWizard::nextPageAfter(int page) const {
    for (int next = page + 1; next <= FinishPage; ++next) {
        if (pageAvailable(next)) {
            return next;
        }
    }
    return -1;
}

void FirstRunWizard::retranslateAll() {
    setWindowTitle(T("wiz.window_title"));
    setButtonText(QWizard::BackButton, T("wiz.back"));
    setButtonText(QWizard::NextButton, T("wiz.next"));
    setButtonText(QWizard::CommitButton, T("wiz.next"));
    setButtonText(QWizard::FinishButton, T("wiz.finish"));
    setButtonText(QWizard::CancelButton, T("wiz.cancel"));
    // Room around the longer German texts ("Fertigstellen") - the default width fits the text only.
    for (QWizard::WizardButton which : {QWizard::BackButton, QWizard::NextButton, QWizard::CommitButton,
                                        QWizard::FinishButton, QWizard::CancelButton}) {
        if (QAbstractButton* b = button(which)) {
            b->setMinimumWidth(b->fontMetrics().horizontalAdvance(b->text()) + b->fontMetrics().height() * 2);
        }
    }
    for (int id : pageIds()) {
        static_cast<WizardPageBase*>(page(id))->retranslate();
    }
}

void FirstRunWizard::checkIsoNow() {
    if (state_.iso.isEmpty()) {
        return;
    }
    const GameCli cli(ctx_.paths.gameExe, ctx_.paths.cacheDir());
    state_.check = GameCli::ToIsoResult(cli.checkIso(state_.iso));
    state_.checkedIso = state_.iso;
    state_.checking = false;
    emit isoStateChanged();
}

void FirstRunWizard::applyToModel() {
    SettingsModel& model = ctx_.model;
    const ConfigSchema& schema = model.schema();
    if (const KeyDef* iso = schema.find(kIso); iso != nullptr && !state_.iso.isEmpty()) {
        model.setValue(*iso, QDir::toNativeSeparators(state_.iso));
    }
    if (const KeyDef* lang = schema.find(kLanguage); lang != nullptr && !state_.gameLanguage.isEmpty()) {
        model.setValue(*lang, state_.gameLanguage);
    }
    if (const KeyDef* monitor = schema.findByWidget(UiWidget::Monitor)) {
        model.setValue(*monitor, state_.monitor);
    }
    if (const KeyDef* fs = schema.find(kFullscreen)) {
        model.setValue(*fs, fs->type == KeyType::Bool
                                ? BoolText(state_.fullscreen, model.value(*fs))
                                : (state_.fullscreen ? QStringLiteral("1") : QStringLiteral("0")));
    }
    if (const KeyDef* scale = schema.find(kWindowScale); scale != nullptr && !state_.windowScale.isEmpty()) {
        model.setValue(*scale, state_.windowScale);
    }
    if (state_.preset != QualityPreset::Custom) {
        for (const auto& [name, value] : ComputePreset(state_.preset, schema, ctx_.gpu).values) {
            model.setValue(name, value);
        }
    }
}

}  // namespace fl::ui
