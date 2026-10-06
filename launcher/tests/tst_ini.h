#pragma once

#include <QObject>

class IniTest : public QObject {
    Q_OBJECT
private slots:
    void hostRules();
    void roundTripUnchanged_data();
    void roundTripUnchanged();
    void setValueKeepsCommentColumn();
    void setValueAppendsAlignedKey();
    void setValueNewSection();
    void setValueKeyEqualsStyle();
    void removeKey();
    void encodeValue_data();
    void encodeValue();
    void specialPathRoundTrip();
    void problemsAndBom();
    void newKeyNotAfterBrokenLine();
    void saveAtomically();
};
