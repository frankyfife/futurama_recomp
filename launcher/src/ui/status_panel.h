// Panel shown after the game exits: meaning of the exit code, the cause for start errors, the last
// 30 lines of futurama.log (or the game's console output when no log was written) and the buttons
// "Log oeffnen", "Ordner oeffnen", "Erneut versuchen".
#pragma once

#include "core/game_session.h"

#include <QFrame>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QToolButton;

namespace fl::ui {

class StatusPanel : public QFrame {
    Q_OBJECT
public:
    explicit StatusPanel(QWidget* parent = nullptr);
    void showResult(const GameRunResult& result);
    const GameRunResult& result() const { return result_; }

signals:
    void retryRequested();
    void closeRequested();

private:
    void openLog();
    void openFolder();

    GameRunResult result_;
    QLabel* title_ = nullptr;
    QLabel* detail_ = nullptr;
    QLabel* cause_ = nullptr;
    QLabel* linesCaption_ = nullptr;
    QPlainTextEdit* lines_ = nullptr;
    QPushButton* openLog_ = nullptr;
    QPushButton* openFolder_ = nullptr;
    QPushButton* retry_ = nullptr;
    QToolButton* close_ = nullptr;
};

}  // namespace fl::ui
