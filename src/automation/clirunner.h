#ifndef CLIRUNNER_H
#define CLIRUNNER_H

#include <QJsonObject>
#include <QStringList>

namespace Cli {
struct Command {
    QString name;
    QString tool;
    QJsonObject arguments;
    QString instance;
    int timeoutMs = 5000;
    bool autoStart = true;
};
bool parse(const QStringList &arguments, Command &command, QString &error);
int run(const QStringList &arguments, const QString &version);
}

#endif
