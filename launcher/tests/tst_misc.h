#pragma once

#include <QObject>

class MiscTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void translationsComplete();
    void presets();
    void presetDetection();
    void settingsModelEdits();
    void settingsModelDynamicDefault();
    void settingsModelSaveMerges();
    void settingsModelUnknownEntries();
    void exitCodes();
    void failureCause();
    void presetMemory();
    void logTail();
    void cliCache();
    void gameSessionRun();
    void gameSessionConfigError();
    void frameRateEstimate();
    void audioTestTone();
    void audioDeviceList();
    void audioValueMapping();
};
