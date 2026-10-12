#pragma once

#include <QObject>

class SchemaTest : public QObject {
    Q_OBJECT
private slots:
    void sampleSchema();
    void defaultsAsIniText();
    void tabsAndOrder();
    void tolerantParsing();
    void rejectsBrokenDocuments();
    void gpuInfo();
    void isoResult();
    void audioTab();
};
