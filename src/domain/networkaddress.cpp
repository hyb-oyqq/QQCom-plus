#include "networkaddress.h"

QRegularExpression ipv4AddressPattern()
{
    const QString octet = QStringLiteral("(?:25[0-5]|2[0-4][0-9]|1[0-9]{2}|[1-9]?[0-9])");
    return QRegularExpression(QStringLiteral("%1(?:\\.%1){3}").arg(octet));
}

QRegularExpression hostNamePattern()
{
    const QString label = QStringLiteral("[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?");
    return QRegularExpression(QStringLiteral("%1(?:\\.%1)*\\.?").arg(label));
}

bool isValidNetworkAddress(const QString &address, bool allowHostName)
{
    if (address.isEmpty() || address.size() > 253) return false;
    static const QRegularExpression ipv4(QRegularExpression::anchoredPattern(ipv4AddressPattern().pattern()));
    if (ipv4.match(address).hasMatch()) return true;
    static const QRegularExpression numeric(QStringLiteral("^[0-9.]+$"));
    if (!allowHostName || numeric.match(address).hasMatch()) return false;
    static const QRegularExpression host(QRegularExpression::anchoredPattern(hostNamePattern().pattern()));
    return host.match(address).hasMatch();
}
