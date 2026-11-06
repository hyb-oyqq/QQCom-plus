#ifndef COMMANDAUTOMATION_H
#define COMMANDAUTOMATION_H

#include <QJsonObject>

class CommandManager;

namespace CommandAutomation {
bool handles(const QString &tool);
bool isMutation(const QString &tool);
// Validates the entire patch before changing the manager. Uses the same
// snapshots and notifications as GUI edits; never sends a command.
QJsonObject execute(CommandManager *manager, const QString &tool, const QJsonObject &arguments);
}

#endif
