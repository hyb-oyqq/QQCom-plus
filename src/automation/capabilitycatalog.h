#ifndef CAPABILITYCATALOG_H
#define CAPABILITYCATALOG_H

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include "domain/portdraft.h"

namespace CapabilityCatalog {
QJsonArray tools();
QJsonObject capabilities();
QJsonObject endpoint(const PortDraft &draft);
QJsonObject failure(const QString &code, const QString &message);
QString validateArguments(const QString &tool, const QJsonObject &arguments);
bool configure(const QJsonObject &arguments, PortDraft &draft, QString &error);
}

#endif
