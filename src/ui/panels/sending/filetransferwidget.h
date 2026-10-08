#ifndef FILETRANSFERWIDGET_H
#define FILETRANSFERWIDGET_H

#include <QObject>

#include "domain/trafficmessage.h"

class EndpointRuntime;
class FileTransferService;
class PreferencesStore;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTimer;
class QWidget;

// 发送文件和接收文件各是一张独立页面。两页共用一次传输：进行中时另一页不能再开。
class FileTransferWidget final : public QObject
{
    Q_OBJECT

public:
    explicit FileTransferWidget(QWidget *host);

    QWidget *sendPage() const;
    QWidget *receivePage() const;

    void setRuntime(EndpointRuntime *runtime);
    void setPreferences(PreferencesStore *store);
    void setEndpointOpen(int index, bool open);

public slots:
    void rawDataReceived(int index, const Traffic::Source &source, const QByteArray &payload);

private:
    struct Page {
        bool sending = false;
        QString prefPrefix;
        QWidget *root = nullptr;
        QComboBox *mode = nullptr;
        QPushButton *endpointA = nullptr;
        QPushButton *endpointB = nullptr;
        QPushButton *block128 = nullptr;
        QPushButton *block1024 = nullptr;
        QSpinBox *chunk = nullptr;
        QLabel *chunkGapLabel = nullptr;
        QSpinBox *chunkGap = nullptr;
        QLineEdit *path = nullptr;
        QPushButton *browse = nullptr;
        QComboBox *beforeFormat = nullptr;
        QLineEdit *beforeText = nullptr;
        QComboBox *afterFormat = nullptr;
        QLineEdit *afterText = nullptr;
        QSpinBox *beforeDelay = nullptr;
        QSpinBox *afterDelay = nullptr;
        QPushButton *startButton = nullptr;
        QProgressBar *progress = nullptr;
        QLabel *stats = nullptr;
        QLabel *status = nullptr;
    };

    void buildPage(Page *page);
    void choosePath(Page *page);
    void startTransfer(Page *page);
    void updateControls();
    void updatePage(Page *page);
    void updateProgress(qint64 current, qint64 total);
    void updateStats(qint64 transferred, qint64 total, int retries, int crcErrors,
        int payloadCrc16, quint32 payloadCrc32, qint64 elapsedMs);
    void setStatus(const QString &text);
    void refreshFooter(Page *page);
    void scheduleFileDigest(Page *page);
    void startFileDigest(Page *page);
    QString fileStatusText(const Page *page) const;
    QString fileCardText(const Page *page) const;
    QString fileCardTip(const Page *page) const;
    void updateCommandPreview();
    void savePreferences();
    void loadPage(Page *page, bool useLegacy);
    void syncReceivePath(Page *page, bool modeSwitched);
    bool ymodemSelected(const Page *page) const;
    int endpoint(const Page *page) const;
    bool encodeField(const QLineEdit *edit, const QComboBox *format,
        QByteArray *bytes, QString *error) const;
    QString fieldPreview(const QLineEdit *edit, const QComboBox *format) const;
    QVariant pref(const QString &prefix, const QString &key, const QVariant &fallback, bool useLegacy) const;

    // 选中文件后先算整文件校验，底部显示文件身份。传输真正搬到字节后才换成速率行。
    struct DigestCache {
        quint64 generation = 0;
        bool hashing = false;
        bool again = false;
        bool pending = false;
        bool ready = false;
        bool failed = false;
        QString path;
        qint64 size = 0;
        qint64 modifiedMs = 0;
        quint16 crc16 = 0;
        quint32 crc32 = 0;
    };

    DigestCache *digestFor(Page *page);
    const DigestCache *digestFor(const Page *page) const;

    FileTransferService *m_service = nullptr;
    PreferencesStore *m_preferences = nullptr;
    bool m_loading = false;
    bool m_showingLive = false;
    bool m_statusFromService = false;
    QString m_serviceStatus;
    QString m_liveText;
    QString m_liveTip;
    Page *m_resultPage = nullptr;
    QTimer *m_sendDigestTimer = nullptr;
    QTimer *m_receiveDigestTimer = nullptr;
    DigestCache m_sendDigest;
    DigestCache m_receiveDigest;
    Page m_send;
    Page m_receive;
    bool m_openFlags[2] = {false, false};
};

#endif // FILETRANSFERWIDGET_H
