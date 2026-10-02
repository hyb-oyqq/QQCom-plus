#include "preferencesstore.h"
#include "../domain/networkaddress.h"

#include <QSettings>
#include <QStandardPaths>

namespace {
QString historyKey(bool localAddress)
{
    return localAddress ? QStringLiteral("addressHistory/local") : QStringLiteral("addressHistory/remote");
}
}

PreferencesStore::PreferencesStore(QObject *parent, const QString &fileName) : QObject(parent)
{
    const auto path = fileName.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/preferences.ini"
        : fileName;
    m_settings = new QSettings(path, QSettings::IniFormat, this);
}

QStringList PreferencesStore::addressHistory(bool localAddress) const
{
    QStringList result;
    for (const auto &entry : m_settings->value(historyKey(localAddress)).toStringList()) {
        const auto address = entry.trimmed().toLower();
        if (isValidNetworkAddress(address, !localAddress) && !result.contains(address))
            result.append(address);
        if (result.size() == MaxAddressHistory) break;
    }
    return result;
}

void PreferencesStore::rememberAddress(const QString &input, bool localAddress)
{
    const auto address = input.trimmed().toLower();
    if (!isValidNetworkAddress(address, !localAddress)) return;
    auto history = addressHistory(localAddress);
    if (!history.isEmpty() && history.first() == address) return;
    history.removeAll(address);
    history.prepend(address);
    while (history.size() > MaxAddressHistory) history.removeLast();
    m_settings->setValue(historyKey(localAddress), history);
    m_settings->sync();
    if (m_settings->status() != QSettings::NoError)
        emit saveFailed(tr("地址历史保存失败，请检查配置目录的写入权限。"));
    emit addressHistoryChanged();
}
