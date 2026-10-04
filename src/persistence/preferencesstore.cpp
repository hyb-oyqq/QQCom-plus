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

QVariant PreferencesStore::value(const QString &key, const QVariant &fallback) const
{
    return m_settings->value(key, fallback);
}

void PreferencesStore::setValue(const QString &key, const QVariant &value)
{
    // 显示偏好不带 sync：QSettings 在自身析构（即应用退出）时统一落盘，
    // 高频开关切换若每次强制刷盘，Windows 上会有可感知的停顿。
    // 地址历史走 rememberAddress，那才是需要即时持久化的关键数据。
    m_settings->setValue(key, value);
}

void PreferencesStore::remove(const QString &key)
{
    // 真正的删键。写 QVariant() 只会留下 "@Invalid()"，读回时拿到的不是"键不存在"
    // 而是"值无效"——调用方再 toBool() 就得到 false，等于把默认开启的开关悄悄关掉。
    m_settings->remove(key);
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
