// Runs all launcher test classes (Qt Test). Extra arguments are passed to every class (e.g. a test
// function name or -v2). Qt Test's own stdout logger stays silent in some Windows console setups
// (observed with Qt 6.8.3 on the development machine, also for a minimal QTEST_GUILESS_MAIN probe), so
// unless "-o" is given each class logs into <exe dir>\test_results\<Class>.txt and the file is echoed.
#include "tst_ini.h"
#include "tst_misc.h"
#include "tst_paths.h"
#include "tst_schema.h"
#include "tst_validation.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTest>

#include <cstdio>
#include <memory>
#include <vector>

namespace {

int RunClass(QObject* test, const QStringList& baseArgs, bool explicitOutput) {
    QStringList args = baseArgs;
    QString file;
    if (!explicitOutput) {
        const QString dir =
            QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("test_results"));
        QDir().mkpath(dir);
        file =
            QDir(dir).filePath(QString::fromLatin1(test->metaObject()->className()) + QStringLiteral(".txt"));
        args << QStringLiteral("-o") << file + QStringLiteral(",txt");
    }
    const int failed = QTest::qExec(test, args);
    if (!file.isEmpty()) {
        QFile f(file);
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray text = f.readAll();
            std::fwrite(text.constData(), 1, static_cast<size_t>(text.size()), stdout);
            std::fflush(stdout);
        }
    }
    return failed;
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    const QStringList args = QCoreApplication::arguments();
    const bool explicitOutput = args.contains(QStringLiteral("-o"));
    std::vector<std::unique_ptr<QObject>> tests;
    tests.push_back(std::make_unique<IniTest>());
    tests.push_back(std::make_unique<SchemaTest>());
    tests.push_back(std::make_unique<ValidationTest>());
    tests.push_back(std::make_unique<PathsTest>());
    tests.push_back(std::make_unique<MiscTest>());
    int failed = 0;
    for (const auto& t : tests) {
        failed += RunClass(t.get(), args, explicitOutput);
    }
    std::printf("launcher_tests: %s (%d failed test function(s))\n", failed == 0 ? "PASS" : "FAIL", failed);
    return failed == 0 ? 0 : 1;
}
