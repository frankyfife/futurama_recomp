// The disc image (ISO) on the "Spiel" tab: verdict of `futurama.exe --check-iso` (shared contract item 2), run in
// the background whenever the configured image changes, and a button that picks another file. The wizard's ISO page
// uses the same texts (IsoVerdictHtml / IsoDetailLines).
#pragma once

#include "core/iso_check.h"
#include "ui/app_context.h"

#include <QGroupBox>
#include <QString>
#include <QStringList>

#include <optional>

class QLabel;
class QPushButton;

namespace fl {
class IsoCheckJob;
}

namespace fl::ui {

// "<b>✓ Supported version detected.</b>" in the colour of the verdict (rich text).
QString IsoVerdictHtml(const IsoCheckResult& result);
// The host's own explanation, "Futurama – European retail release, XISO, 1.75 GB", the SHA-1 and hints.
// `wizard`: the wizard's wording (with the SHA-1 and "you can continue and change it later").
QStringList IsoDetailLines(const IsoCheckResult& result, bool wizard);

class IsoStatusBox : public QGroupBox {
    Q_OBJECT
public:
    IsoStatusBox(AppContext& ctx, QWidget* parent = nullptr);
    ~IsoStatusBox() override;

    // Checks the configured image again if it changed (path, size or time stamp) since the last check.
    void refresh();
    // Screenshots and tests: runs a pending check synchronously.
    void checkNow();
    // Review M3c finding 9: the verdict for the configured image as it is now (checked synchronously if the
    // background check has not finished); nullopt without an image, without futurama.exe or if the file is missing.
    std::optional<IsoCheckResult> currentResult();

    // Opens the file dialog ("Ändern…").
    void browseForImage() { browse(); }

signals:
    // The user picked another image (native separators).
    void isoChosen(const QString& path);

private:
    QString configuredPath() const;  // resolved [paths] iso, empty if not set
    static QString FileKey(const QString& path);
    void showState();
    void browse();

    AppContext& ctx_;
    IsoCheckJob* job_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* details_ = nullptr;
    QPushButton* change_ = nullptr;
    QString checkedKey_;  // FileKey of the last finished check
    QString runningKey_;  // FileKey of the running check
    IsoCheckResult result_;
};

}  // namespace fl::ui
