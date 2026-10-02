#ifndef PREFERENCESSTORE_H
#define PREFERENCESSTORE_H

#include <QObject>
#include <QStringList>
#include <QVariant>

class QSettings;

class PreferencesStore final : public QObject
{
    Q_OBJECT
public:
    explicit PreferencesStore(QObject *parent = nullptr, const QString &fileName = {});
    QStringList addressHistory(bool localAddress) const;
    void rememberAddress(const QString &address, bool localAddress);
    static constexpr int MaxAddressHistory = 20;

    // 显示偏好的通用键值通道（监视区开关、编码格式等）。地址历史有领域语义走上面的
    // 方法；这里只承载"记住用户上次的界面选择"这类无结构偏好的存取。
    QVariant value(const QString &key, const QVariant &fallback = {}) const;
    void setValue(const QString &key, const QVariant &value);

signals:
    void addressHistoryChanged();
    void saveFailed(const QString &message);

private:
    QSettings *m_settings;
};

#endif
