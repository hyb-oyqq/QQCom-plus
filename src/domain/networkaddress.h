#ifndef NETWORKADDRESS_H
#define NETWORKADDRESS_H

#include <QRegularExpression>

QRegularExpression ipv4AddressPattern();
QRegularExpression hostNamePattern();
bool isValidNetworkAddress(const QString &address, bool allowHostName = false);

#endif
