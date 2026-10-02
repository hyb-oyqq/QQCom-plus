#ifndef PREFERENCESSTORE_H
#define PREFERENCESSTORE_H

#include <QObject>
#include <QStringList>

class QSettings;

class PreferencesStore final : public QObject
{
    Q_OBJECT
public:
    explicit PreferencesStore(QObject *parent = nullptr, const QString &fileName = {});
    QStringList addressHistory(bool localAddress) const;
    void rememberAddress(const QString &address, bool localAddress);
    static constexpr int MaxAddressHistory = 20;

signals:
    void addressHistoryChanged();
    void saveFailed(const QString &message);

private:
    QSettings *m_settings;
};

#endif
