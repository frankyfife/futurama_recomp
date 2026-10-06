#include "tst_ini.h"

#include "core/ini_document.h"
#include "core/l10n.h"
#include "test_support.h"

#include <QFileInfo>
#include <QTest>

using fl::IniDocument;
using fl::Language;
using fl::T;

namespace {

IniDocument Parse(const char* text) {
    IniDocument doc;
    doc.parse(QByteArray(text));
    return doc;
}

}  // namespace

void IniTest::hostRules() {
    const IniDocument doc = Parse("; comment\n"
                                  "# another comment\n"
                                  "top = level\n"
                                  "[Paths]\n"
                                  "  ISO   =   C:\\Games\\a b.iso   ; inline comment\n"
                                  "[render]\n"
                                  "msaa=4#not a comment\n"
                                  "msaa = 8 # comment wins, last occurrence wins\n"
                                  "quoted = \"  spaced ; value  \"\n"
                                  "hash = a;b\n"
                                  "tab\t=\tx\t;c\n"
                                  "[render]\n"
                                  "scaling = point\n");
    QCOMPARE(doc.value(QStringLiteral("paths"), QStringLiteral("iso")).value_or(QString()),
             QStringLiteral("C:\\Games\\a b.iso"));
    QCOMPARE(doc.value(QStringLiteral("PATHS"), QStringLiteral("Iso")).value_or(QString()),
             QStringLiteral("C:\\Games\\a b.iso"));
    QCOMPARE(doc.value(QStringLiteral("render"), QStringLiteral("msaa")).value_or(QString()),
             QStringLiteral("8"));
    QCOMPARE(doc.value(QStringLiteral("render"), QStringLiteral("quoted")).value_or(QString()),
             QStringLiteral("  spaced ; value  "));
    QCOMPARE(doc.value(QStringLiteral("render"), QStringLiteral("hash")).value_or(QString()),
             QStringLiteral("a;b"));
    QCOMPARE(doc.value(QStringLiteral("render"), QStringLiteral("tab")).value_or(QString()),
             QStringLiteral("x"));
    QCOMPARE(doc.value(QStringLiteral("render"), QStringLiteral("scaling")).value_or(QString()),
             QStringLiteral("point"));
    QVERIFY(!doc.value(QStringLiteral("render"), QStringLiteral("top")).has_value());
    QCOMPARE(doc.effectiveValue(QStringLiteral("render"), QStringLiteral("top")).value_or(QString()),
             QStringLiteral("level"));
    QCOMPARE(doc.sectionNames(), (QStringList{QStringLiteral("paths"), QStringLiteral("render")}));
    QVERIFY(doc.problems().isEmpty());
    // "msaa=4#not a comment": '#' without a blank before it belongs to the value (host rule).
    const IniDocument hashDoc = Parse("[r]\nk=4#x\n");
    QCOMPARE(hashDoc.value(QStringLiteral("r"), QStringLiteral("k")).value_or(QString()),
             QStringLiteral("4#x"));
}

void IniTest::roundTripUnchanged_data() {
    QTest::addColumn<QByteArray>("input");
    QTest::newRow("crlf") << QByteArray(
        "; head\r\n[paths]\r\niso = C:\\x y\\(1,2).iso ; c\r\n\r\n[debug]\r\nmsgbox=0\r\n");
    QTest::newRow("lf no trailing newline") << QByteArray("[a]\nk = v\n  ; indented comment\nx=1");
    QTest::newRow("bom + odd spacing") << QByteArray("\xEF\xBB\xBF[ Render ]\r\n\tMSAA\t=\t4\t\r\n");
    QTest::newRow("invalid lines kept") << QByteArray("[a\nno equals sign\n[b]\nk = \"quoted\"\n");
    QTest::newRow("empty") << QByteArray();
    QTest::newRow("unknown keys + sections")
        << QByteArray("[mods]\nfoo = bar\n[render]\nfuture_key = 1 ; later build\n");
}

void IniTest::roundTripUnchanged() {
    QFETCH(QByteArray, input);
    IniDocument doc;
    doc.parse(input);
    QCOMPARE(doc.serialize(), input);
}

void IniTest::setValueKeepsCommentColumn() {
    IniDocument doc = Parse("[log]\r\nlevel    = info   ; how much\r\nconsole = 1\r\n");
    QVERIFY(doc.setValue(QStringLiteral("log"), QStringLiteral("level"), QStringLiteral("debug")));
    QCOMPARE(doc.serialize(), QByteArray("[log]\r\nlevel    = debug  ; how much\r\nconsole = 1\r\n"));
    QVERIFY(
        doc.setValue(QStringLiteral("LOG"), QStringLiteral("Level"), QStringLiteral("a-much-longer-value")));
    QCOMPARE(doc.serialize(),
             QByteArray("[log]\r\nlevel    = a-much-longer-value ; how much\r\nconsole = 1\r\n"));
    // An empty value gets a blank after "= ".
    IniDocument empty = Parse("[a]\nk =\n");
    QVERIFY(empty.setValue(QStringLiteral("a"), QStringLiteral("k"), QStringLiteral("v")));
    QCOMPARE(empty.serialize(), QByteArray("[a]\nk = v\n"));
}

void IniTest::setValueAppendsAlignedKey() {
    IniDocument doc = Parse("[paths]\n"
                            "iso      = a.iso\n"
                            "userdata = d\n"
                            "\n"
                            "; comment before the next section\n"
                            "[debug]\n"
                            "msgbox = 0\n");
    QVERIFY(doc.setValue(QStringLiteral("paths"), QStringLiteral("xbe"), QStringLiteral("b.xbe")));
    QCOMPARE(doc.serialize(), QByteArray("[paths]\n"
                                         "iso      = a.iso\n"
                                         "userdata = d\n"
                                         "xbe      = b.xbe\n"
                                         "\n"
                                         "; comment before the next section\n"
                                         "[debug]\n"
                                         "msgbox = 0\n"));
    QVERIFY(doc.setValue(QStringLiteral("paths"), QStringLiteral("very_long_key_name"), QStringLiteral("1")));
    QVERIFY(doc.serialize().contains("xbe      = b.xbe\nvery_long_key_name = 1\n\n"));
    // The next key aligns with the block's column, not with the long outlier.
    QVERIFY(doc.setValue(QStringLiteral("paths"), QStringLiteral("disc_dir"), QStringLiteral("x")));
    QVERIFY(doc.serialize().contains("very_long_key_name = 1\ndisc_dir = x\n"));
    // A block written as "key = value" (the launcher's own sections) is not padded.
    IniDocument plain = Parse("[render]\nfullscreen = 1\n");
    QVERIFY(plain.setValue(QStringLiteral("render"), QStringLiteral("render_scale"), QStringLiteral("1.5")));
    QVERIFY(plain.setValue(QStringLiteral("render"), QStringLiteral("msaa"), QStringLiteral("8")));
    QCOMPARE(plain.serialize(), QByteArray("[render]\nfullscreen = 1\nrender_scale = 1.5\nmsaa = 8\n"));
}

void IniTest::setValueNewSection() {
    IniDocument doc = Parse("[paths]\r\niso = a\r\n");
    QVERIFY(doc.setValue(QStringLiteral("render"), QStringLiteral("msaa"), QStringLiteral("4")));
    QCOMPARE(doc.serialize(), QByteArray("[paths]\r\niso = a\r\n\r\n[render]\r\nmsaa = 4\r\n"));
    IniDocument fresh;
    fresh.parse(QByteArray());
    QVERIFY(fresh.setValue(QStringLiteral("paths"), QStringLiteral("iso"), QStringLiteral("x")));
    fresh.prependComments({QStringLiteral("header")});
    QCOMPARE(fresh.serialize(), QByteArray("; header\r\n\r\n[paths]\r\niso = x\r\n"));
}

void IniTest::setValueKeyEqualsStyle() {
    IniDocument doc = Parse("[a]\nfirst=1\n");
    QVERIFY(doc.setValue(QStringLiteral("a"), QStringLiteral("second"), QStringLiteral("2")));
    QCOMPARE(doc.serialize(), QByteArray("[a]\nfirst=1\nsecond=2\n"));
}

void IniTest::removeKey() {
    IniDocument doc = Parse("[a]\nk = 1\n; keep me\nk = 2\nother = 3\n[b]\nk = 4\n");
    QVERIFY(doc.remove(QStringLiteral("A"), QStringLiteral("K")));
    QCOMPARE(doc.serialize(), QByteArray("[a]\n; keep me\nother = 3\n[b]\nk = 4\n"));
    QVERIFY(!doc.remove(QStringLiteral("a"), QStringLiteral("k")));
}

void IniTest::encodeValue_data() {
    QTest::addColumn<QString>("value");
    QTest::addColumn<QString>("encoded");  // empty = not representable
    QTest::newRow("plain") << QStringLiteral("abc") << QStringLiteral("abc");
    QTest::newRow("empty") << QString() << QString();
    QTest::newRow("leading blank") << QStringLiteral(" x") << QStringLiteral("\" x\"");
    QTest::newRow("inline comment") << QStringLiteral("a ;b") << QStringLiteral("\"a ;b\"");
    QTest::newRow("hash comment") << QStringLiteral("a #b") << QStringLiteral("\"a #b\"");
    QTest::newRow("leading semicolon") << QStringLiteral(";x") << QStringLiteral("\";x\"");
    QTest::newRow("semicolon inside") << QStringLiteral("a;b") << QStringLiteral("a;b");
    QTest::newRow("outer quotes") << QStringLiteral("\"q\"") << QStringLiteral("\"\"q\"\"");
    QTest::newRow("newline") << QStringLiteral("a\nb") << QStringLiteral("<none>");
    QTest::newRow("quote inside") << QStringLiteral("a\" ;b") << QStringLiteral("a\" ;b");
    QTest::newRow("blank + quote + comment") << QStringLiteral(" a\" ;b") << QStringLiteral("<none>");
    QTest::newRow("iso path") << QStringLiteral("Y:\\xbox games\\Futurama (Jun 17, 2003 prototype)\\") +
                                     fl::test::SpecialIsoName()
                              << QStringLiteral("Y:\\xbox games\\Futurama (Jun 17, 2003 prototype)\\") +
                                     fl::test::SpecialIsoName();
}

void IniTest::encodeValue() {
    QFETCH(QString, value);
    QFETCH(QString, encoded);
    const auto result = IniDocument::EncodeValue(value);
    if (encoded == QLatin1String("<none>")) {
        QVERIFY(!result.has_value());
        return;
    }
    QVERIFY(result.has_value());
    QCOMPARE(*result, encoded);
    // The host reads it back unchanged.
    IniDocument doc;
    doc.parse(QByteArray("[s]\nk = ") + result->toUtf8() + QByteArray("\n"));
    QCOMPARE(doc.value(QStringLiteral("s"), QStringLiteral("k")).value_or(QStringLiteral("<missing>")),
             value);
}

void IniTest::specialPathRoundTrip() {
    const QString iso =
        QStringLiteral("Y:\\xbox games\\Futurama (Jun 17, 2003 prototype)\\") + fl::test::SpecialIsoName();
    IniDocument doc;
    doc.parse(QByteArray());
    QVERIFY(doc.setValue(QStringLiteral("paths"), QStringLiteral("iso"), iso));
    IniDocument reread;
    reread.parse(doc.serialize());
    QCOMPARE(reread.value(QStringLiteral("paths"), QStringLiteral("iso")).value_or(QString()), iso);
    QVERIFY(reread.serialize().contains("(En,Fr,De,Es,It).iso"));
}

void IniTest::problemsAndBom() {
    IniDocument doc = Parse("\xEF\xBB\xBF[a\nnovalue\n= x\n[b]\nk = 1\n");
    QVERIFY(doc.hasBom());
    QCOMPARE(doc.problems().size(), 3);
    QCOMPARE(doc.problems()[0].line, 1);
    QCOMPARE(doc.problems()[0].message, QStringLiteral("bad section header"));
    QCOMPARE(doc.problems()[1].message, QStringLiteral("expected key = value"));
    QCOMPARE(doc.problems()[2].message, QStringLiteral("empty key"));
    // Review M3c finding 13: codes for the UI's own texts.
    QCOMPARE(doc.problems()[0].code, IniDocument::ProblemCode::BadSectionHeader);
    QCOMPARE(doc.problems()[1].code, IniDocument::ProblemCode::ExpectedKeyValue);
    QCOMPARE(doc.problems()[2].code, IniDocument::ProblemCode::EmptyKey);
    QCOMPARE(IniDocument::ProblemText(doc.problems()[1]), T("ini.problem.expected_key_value"));
    QCOMPARE(T("ini.problem.expected_key_value", Language::German), QStringLiteral("erwartet: name = wert"));
    QVERIFY(doc.setValue(QStringLiteral("b"), QStringLiteral("k"), QStringLiteral("2")));
    QVERIFY(doc.serialize().startsWith("\xEF\xBB\xBF[a\n"));
    IniDocument latin1;
    latin1.parse(QByteArray("[a]\nk = M\xFC"
                            "nchen\n"));
    QCOMPARE(latin1.problems().size(), 1);
    QCOMPARE(latin1.problems()[0].line, 0);
    QCOMPARE(latin1.problems()[0].code, IniDocument::ProblemCode::InvalidUtf8);
    QVERIFY(latin1.hasInvalidUtf8());
    // The note stays after an edit (review M3c finding 20: before, it vanished with the first setValue).
    QVERIFY(latin1.setValue(QStringLiteral("a"), QStringLiteral("x"), QStringLiteral("1")));
    QCOMPARE(latin1.problems().size(), 1);
    QVERIFY(latin1.hasInvalidUtf8());
}

void IniTest::newKeyNotAfterBrokenLine() {
    // Review M3c finding 20: "[render" is a broken line (the host stops at it); a new key goes after the last real
    // key of the section, not behind the broken line.
    IniDocument doc = Parse("[render]\nmsaa = 4\n[render\n");
    QVERIFY(doc.setValue(QStringLiteral("render"), QStringLiteral("anisotropy"), QStringLiteral("8")));
    QCOMPARE(doc.serialize(), QByteArray("[render]\nmsaa = 4\nanisotropy = 8\n[render\n"));
    IniDocument onlyBroken = Parse("[render]\nbroken line\n");
    QVERIFY(onlyBroken.setValue(QStringLiteral("render"), QStringLiteral("msaa"), QStringLiteral("2")));
    QCOMPARE(onlyBroken.serialize(), QByteArray("[render]\nmsaa = 2\nbroken line\n"));
}

void IniTest::saveAtomically() {
    const QString dir = fl::test::MakeTestDir(QStringLiteral("ini save (a,b)"));
    const QString path = QDir(dir).filePath(QStringLiteral("sub dir/futurama.ini"));
    IniDocument doc;
    QString error;
    QVERIFY(doc.load(path, &error));
    QVERIFY(!doc.existed());
    QVERIFY(doc.setValue(QStringLiteral("paths"), QStringLiteral("iso"), QStringLiteral("C:\\a (1), b.iso")));
    QVERIFY2(doc.save(path, &error), qPrintable(error));
    IniDocument reread;
    QVERIFY(reread.load(path, &error));
    QVERIFY(reread.existed());
    QCOMPARE(reread.value(QStringLiteral("paths"), QStringLiteral("iso")).value_or(QString()),
             QStringLiteral("C:\\a (1), b.iso"));
    // No temporary files are left behind.
    QCOMPARE(QDir(QFileInfo(path).absolutePath()).entryList(QDir::Files),
             QStringList{QStringLiteral("futurama.ini")});
}
