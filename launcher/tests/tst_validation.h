#pragma once

#include <QObject>

class ValidationTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void choices();
    void booleans();
    void integers();
    void floats();
    void resolutions();
    void paths();
    void equivalence();
    void boolStyle();
};
