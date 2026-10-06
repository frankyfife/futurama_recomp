// First-run wizard (QWizard): UI language, ISO (checked with futurama.exe --check-iso), game language,
// display (monitor, window or borderless fullscreen), quality preset (Klassisch / Standard / Maximum,
// clamped to the GPU), summary. Pages whose keys the schema lacks are skipped. accept() leaves the
// choices as pending edits in the model; the caller saves.
#pragma once

#include "core/iso_check.h"
#include "core/l10n.h"
#include "core/presets.h"
#include "ui/app_context.h"

#include <QWizard>

namespace fl {
class IsoCheckJob;
}

namespace fl::ui {

struct WizardState {
    QString iso;
    QString checkedIso;  // the path `check` belongs to
    IsoCheckResult check;
    bool checking = false;
    QString gameLanguage;
    QString monitor;
    bool fullscreen = false;
    QString windowScale;
    QualityPreset preset = QualityPreset::Standard;

    bool isoAcceptable() const;  // checked, matches `iso`, and an Xbox image (or the check could not run)
};

class FirstRunWizard : public QWizard {
    Q_OBJECT
public:
    enum PageId { WelcomePage, IsoPage, LanguagePage, DisplayPage, QualityPage, FinishPage };

    explicit FirstRunWizard(AppContext& ctx, QWidget* parent = nullptr);
    ~FirstRunWizard() override;

    AppContext& context() { return ctx_; }
    WizardState& state() { return state_; }
    IsoCheckJob* isoJob() { return isoJob_; }

    // Writes the choices into the model as pending edits.
    void applyToModel();
    // Runs the ISO check synchronously (screenshots / automation).
    void checkIsoNow();
    // The next page after `page` that the schema supports.
    int nextPageAfter(int page) const;
    bool pageAvailable(int page) const;
    void retranslateAll();

signals:
    void uiLanguageChanged(fl::Language language);
    void isoStateChanged();

private:
    AppContext& ctx_;
    WizardState state_;
    IsoCheckJob* isoJob_ = nullptr;
};

// Short description of a preset's values ("Nativ (3840 × 2160) · 4× MSAA · Anisotrop 16×").
QString PresetSummary(QualityPreset preset, const ConfigSchema& schema, const GpuInfo& gpu);
QString PresetTitle(QualityPreset preset);
// One line about the graphics card ("NVIDIA GeForce RTX 5090 – MSAA bis 8×, anisotrop bis 16×").
QString GpuSummary(const AppContext& ctx);

}  // namespace fl::ui
