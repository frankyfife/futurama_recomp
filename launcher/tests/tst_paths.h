#pragma once

#include <QObject>

class PathsTest : public QObject {
    Q_OBJECT
private slots:
    void gamePathsDefault();
    void gamePathsPortable();
    void gamePathsExplicitConfig();
    void findGameDir();
    void userdataAndLog();
    void commandLineQuoting();
    void checkIsoThroughCli();
    void checkIsoJob();
    void printSchemaAndGpuThroughCli();
};
