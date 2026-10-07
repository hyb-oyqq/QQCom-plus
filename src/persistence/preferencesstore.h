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
    // Copy once on a product rename; never overwrite a target or modify the source.
    static bool migrateLegacyFile(const QString &source, const QString &target, QString *error = nullptr);
    QStringList addressHistory(bool localAddress) const;
    void rememberAddress(const QString &address, bool localAddress);
    static constexpr int MaxAddressHistory = 20;

    // 显示偏好的通用键值通道（监视区开关、编码格式等）。地址历史有领域语义走上面的
    // 方法；这里只承载"记住用户上次的界面选择"这类无结构偏好的存取。
    QVariant value(const QString &key, const QVariant &fallback = {}) const;
    void setValue(const QString &key, const QVariant &value);
    // 删除一个键（清场用）。与 setValue(key, QVariant()) 不等价：后者留下 @Invalid()，
    // 读回来是"无效值"而不是"默认值"，toBool() 会得到 false。
    void remove(const QString &key);

signals:
    void addressHistoryChanged();
    void saveFailed(const QString &message);

private:
    QSettings *m_settings;
};

#endif
