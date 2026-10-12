#pragma once

#include <QObject>

// M4: binding strings of the [input] keys (core/bindings.h, INTERFACES.md 4.26), the controls part of the sample
// schema, validation and the INI round trip of bindings, key/mouse/pad capture mapping, the simulated pad reader.
class BindingsTest : public QObject {
    Q_OBJECT
private slots:
    void tokens();
    void bindingLists();
    void schemaKeys();
    void validationAndEquivalence();
    void iniRoundTrip();
    void captureMapping();
    void padStates();
    void conflicts();
    void displayTexts();
    void sameNamesAsHost();
    void gameControlsLikeHost();
};
