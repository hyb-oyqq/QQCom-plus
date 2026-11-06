#include "filetransferwidget.h"

#include "app/commandmanager.h"
#include "app/endpointruntime.h"
#include "app/filetransferservice.h"
#include "domain/commanditem.h"
#include "persistence/preferencesstore.h"
#include "ui/common/bytesformat.h"
#include "ui/common/payloadcodec.h"
#include "ui/common/tintedicon.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace {

bool lookupCommand(const QString &id, CommandItem *command)
{
    if (!command || id.isEmpty()) return false;
    const QUuid uuid(id);
    const auto sets = CommandManager::instance()->allSets();
    for (const auto &set : sets) {
        for (const auto &item : set.commands) {
            if (item.id == uuid) {
                *command = item;
                return true;
            }
        }
    }
    return false;
}

QString scopedName(const QString &prefix, const QString &suffix)
{
    return prefix + suffix;
}

constexpr int GlyphSize = 14;

QString tailKeyFromCommand(const QString &tail)
{
    if (tail == QLatin1String("CR")) return QStringLiteral("cr");
    if (tail == QLatin1String("LF")) return QStringLiteral("lf");
    if (tail == QLatin1String("CR+LF")) return QStringLiteral("crlf");
    return QStringLiteral("none");
}

// 旧配置把结尾符单独存着。文本模式并成转义，HEX 模式并成字节，避免去掉下拉后丢掉已选结尾。
QString appendStoredTail(const QString &text, const QString &key, bool hex)
{
    QString suffix;
    if (key == QLatin1String("cr"))
        suffix = hex ? QStringLiteral("0D") : QStringLiteral("\\r");
    else if (key == QLatin1String("lf"))
        suffix = hex ? QStringLiteral("0A") : QStringLiteral("\\n");
    else if (key == QLatin1String("crlf"))
        suffix = hex ? QStringLiteral("0D 0A") : QStringLiteral("\\r\\n");
    if (suffix.isEmpty()) return text;
    if (text.trimmed().isEmpty()) return suffix;
    return hex ? text + QLatin1Char(' ') + suffix : text + suffix;
}

bool pathIsDirectory(const QString &path)
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) return false;
    const QFileInfo info(trimmed);
    if (info.isDir()) return true;
    if (info.exists()) return false;
    // 还不存在、也没有扩展名的路径按目录记，避免把用户选的新目录当成 RAW 文件。
    return info.suffix().isEmpty();
}

QString defaultReceiveDirectory()
{
    const QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString folder = QDir(docs.isEmpty() ? QDir::homePath() : docs).filePath(QStringLiteral("QQCom"));
    QDir().mkpath(folder);
    return QDir::cleanPath(folder);
}

QString defaultRawReceivePath(const QString &current)
{
    QString folder = defaultReceiveDirectory();
    const QString trimmed = current.trimmed();
    const QFileInfo info(trimmed);
    if (!trimmed.isEmpty() && info.isDir())
        folder = QDir::cleanPath(info.absoluteFilePath());
    QDir().mkpath(folder);
    const QString name = QStringLiteral("raw_%1.bin")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    return QDir::cleanPath(QDir(folder).filePath(name));
}

void pinFixed(QWidget *widget)
{
    if (widget) widget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

QString crc16Text(quint16 value)
{
    return QStringLiteral("%1").arg(value, 4, 16, QChar('0')).toUpper();
}

QString crc32Text(quint32 value)
{
    return QStringLiteral("%1").arg(value, 8, 16, QChar('0')).toUpper();
}

struct FileDigest
{
    QString path;
    qint64 size = 0;
    bool ok = false;
    quint16 crc16 = 0;
    quint32 crc32 = 0;
};

FileDigest hashFile(const QString &path)
{
    FileDigest out;
    out.path = path;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return out;
    PayloadChecksum sum;
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(64 * 1024);
        if (chunk.isEmpty()) break;
        sum.feed(chunk.constData(), chunk.size());
        out.size += chunk.size();
    }
    out.ok = file.error() == QFile::NoError;
    out.crc16 = sum.crc16;
    out.crc32 = sum.crc32();
    return out;
}

} // namespace

FileTransferWidget::FileTransferWidget(QWidget *host) : QObject(host)
{
    m_send.sending = true;
    m_send.prefPrefix = QStringLiteral("file/send");
    m_receive.sending = false;
    m_receive.prefPrefix = QStringLiteral("file/receive");
    buildPage(&m_send);
    buildPage(&m_receive);

    m_service = new FileTransferService(this);
    connect(m_service, &FileTransferService::statusChanged, this, &FileTransferWidget::setStatus);
    connect(m_service, &FileTransferService::progressChanged, this, &FileTransferWidget::updateProgress);
    connect(m_service, &FileTransferService::statsChanged, this, &FileTransferWidget::updateStats);
    connect(m_service, &FileTransferService::transferFinished, this, &FileTransferWidget::setStatus);
    connect(m_service, &FileTransferService::transferFailed, this,
        [this](const QString &text) { setStatus(tr("失败：%1").arg(text)); });
    connect(m_service, &FileTransferService::busyChanged, this, [this](bool) { updateControls(); });

    const auto armDigestTimer = [this](QTimer **slot, Page *page) {
        auto *timer = new QTimer(this);
        timer->setSingleShot(true);
        timer->setInterval(150);
        *slot = timer;
        connect(timer, &QTimer::timeout, this, [this, page] { startFileDigest(page); });
    };
    armDigestTimer(&m_sendDigestTimer, &m_send);
    armDigestTimer(&m_receiveDigestTimer, &m_receive);

    updateControls();
}

QWidget *FileTransferWidget::sendPage() const
{
    return m_send.root;
}

QWidget *FileTransferWidget::receivePage() const
{
    return m_receive.root;
}

void FileTransferWidget::buildPage(Page *page)
{
    const QString prefix = page->sending ? QStringLiteral("fileTransfer") : QStringLiteral("fileReceive");
    page->root = new QWidget(qobject_cast<QWidget *>(parent()));
    page->root->setObjectName(page->sending ? QStringLiteral("fileSendPage") : QStringLiteral("fileReceivePage"));

    auto *root = new QVBoxLayout(page->root);
    // 上边距把页签和第一行控件拉开。页签底边原来几乎贴着端点行。
    root->setContentsMargins(0, 4, 0, 0);
    root->setSpacing(2);

    page->endpointA = new QPushButton(QStringLiteral("A"));
    page->endpointA->setObjectName(scopedName(prefix, QStringLiteral("EndpointA")));
    page->endpointA->setCheckable(true);
    page->endpointA->setChecked(true);
    page->endpointA->setToolTip(tr("使用端点 A"));
    pinFixed(page->endpointA);
    page->endpointB = new QPushButton(QStringLiteral("B"));
    page->endpointB->setObjectName(scopedName(prefix, QStringLiteral("EndpointB")));
    page->endpointB->setCheckable(true);
    page->endpointB->setToolTip(tr("使用端点 B"));
    pinFixed(page->endpointB);
    auto *endpointGroup = new QButtonGroup(this);
    endpointGroup->setExclusive(true);
    endpointGroup->addButton(page->endpointA, 0);
    endpointGroup->addButton(page->endpointB, 1);

    page->mode = new QComboBox;
    page->mode->setObjectName(scopedName(prefix, QStringLiteral("Mode")));
    page->mode->addItem(QStringLiteral("RAW"), QStringLiteral("raw"));
    page->mode->addItem(QStringLiteral("XMODEM"), QStringLiteral("xmodem"));
    page->mode->addItem(QStringLiteral("YMODEM"), QStringLiteral("ymodem"));
    pinFixed(page->mode);

    auto *topRow = new QHBoxLayout;
    topRow->setSpacing(4);
    topRow->addWidget(new QLabel(tr("端点")));
    topRow->addWidget(page->endpointA);
    topRow->addWidget(page->endpointB);
    topRow->addWidget(new QLabel(tr("方式")));
    topRow->addWidget(page->mode);
    if (page->sending) {
        page->block128 = new QPushButton(QStringLiteral("128"));
        page->block128->setObjectName(QStringLiteral("fileTransferBlock128"));
        page->block128->setCheckable(true);
        page->block128->setToolTip(tr("XMODEM 固定 128 字节。YMODEM 可选 128 或 1024"));
        pinFixed(page->block128);
        page->block1024 = new QPushButton(QStringLiteral("1024"));
        page->block1024->setObjectName(QStringLiteral("fileTransferBlock1024"));
        page->block1024->setCheckable(true);
        page->block1024->setChecked(true);
        page->block1024->setToolTip(tr("YMODEM 数据块 1024 字节"));
        pinFixed(page->block1024);
        auto *blockGroup = new QButtonGroup(this);
        blockGroup->setExclusive(true);
        blockGroup->addButton(page->block128, 128);
        blockGroup->addButton(page->block1024, 1024);
        page->chunk = new QSpinBox;
        page->chunk->setObjectName(QStringLiteral("fileTransferChunk"));
        page->chunk->setRange(1, 64 * 1024);
        page->chunk->setSingleStep(128);
        page->chunk->setValue(16 * 1024);
        page->chunk->setSuffix(QStringLiteral(" B"));
        page->chunk->setButtonSymbols(QAbstractSpinBox::NoButtons);
        page->chunk->setToolTip(tr("RAW 每次连续写出的字节数。链路 MTU 较小时改小"));
        pinFixed(page->chunk);
        page->chunkGapLabel = new QLabel(tr("间隔"));
        page->chunkGapLabel->setToolTip(tr("发完一包后，再隔这么久发下一包。0 表示马上发下一包"));
        page->chunkGap = new QSpinBox;
        page->chunkGap->setObjectName(QStringLiteral("fileTransferChunkGap"));
        page->chunkGap->setRange(0, 600000);
        page->chunkGap->setSingleStep(10);
        page->chunkGap->setValue(0);
        page->chunkGap->setSuffix(QStringLiteral(" ms"));
        page->chunkGap->setButtonSymbols(QAbstractSpinBox::NoButtons);
        page->chunkGap->setToolTip(tr("发完一包后，再隔这么久发下一包。0 表示马上发下一包"));
        pinFixed(page->chunkGap);
        page->blockLabel = new QLabel(tr("分包"));
        page->blockLabel->setObjectName(QStringLiteral("fileTransferBlockLabel"));
        topRow->addWidget(page->blockLabel);
        topRow->addWidget(page->block128);
        topRow->addWidget(page->block1024);
        topRow->addWidget(page->chunk);
        topRow->addWidget(page->chunkGapLabel);
        topRow->addWidget(page->chunkGap);
    }
    page->path = new QLineEdit;
    page->path->setObjectName(scopedName(prefix, QStringLiteral("Path")));
    page->browse = new QPushButton;
    page->browse->setObjectName(scopedName(prefix, QStringLiteral("Browse")));
    setMutedIcon(page->browse, QStringLiteral(":/icons/folder.svg"), GlyphSize);
    page->browse->setIconSize(QSize(GlyphSize, GlyphSize));
    pinFixed(page->browse);
    topRow->addWidget(page->path, 1);
    topRow->addWidget(page->browse);
    root->addLayout(topRow);

    auto *commandRow = new QHBoxLayout;
    commandRow->setSpacing(4);
    const auto addCommand = [&](const QString &title, QComboBox **format, QLineEdit **text,
                                 QSpinBox **delay, const QString &suffix,
                                 const QString &delaySuffix, const QString &delayLabel) {
        auto *titleLabel = new QLabel(title);
        titleLabel->setToolTip(title == tr("前置")
            ? tr("文件开始前发出。留空则跳过")
            : tr("文件结束后发出。留空则跳过"));
        commandRow->addWidget(titleLabel);
        *format = new QComboBox;
        (*format)->setObjectName(scopedName(prefix, suffix + QStringLiteral("Format")));
        (*format)->addItem(tr("文本"), QStringLiteral("text"));
        (*format)->addItem(QStringLiteral("HEX"), QStringLiteral("hex"));
        (*format)->setToolTip(tr("文本支持转义 \\r \\n \\t \\0 \\\\ \\xNN；HEX 直接写字节，回车是 0D 0A"));
        pinFixed(*format);
        commandRow->addWidget(*format);
        *text = new QLineEdit;
        (*text)->setObjectName(scopedName(prefix, suffix + QStringLiteral("Text")));
        (*text)->setPlaceholderText(QStringLiteral("AT+START\\r\\n"));
        (*text)->setClearButtonEnabled(true);
        (*text)->setMinimumWidth(48);
        commandRow->addWidget(*text, 1);
        auto *waitLabel = new QLabel(delayLabel);
        waitLabel->setToolTip(tr("这段等待结束后才进入下一步。等待期间收到的字节不写入文件"));
        commandRow->addWidget(waitLabel);
        *delay = new QSpinBox;
        (*delay)->setObjectName(scopedName(prefix, delaySuffix));
        (*delay)->setRange(0, 600000);
        (*delay)->setSingleStep(100);
        (*delay)->setSuffix(QStringLiteral(" ms"));
        (*delay)->setButtonSymbols(QAbstractSpinBox::NoButtons);
        (*delay)->setToolTip(tr("这段等待结束后才进入下一步。等待期间收到的字节不写入文件"));
        pinFixed(*delay);
        commandRow->addWidget(*delay);
    };
    addCommand(tr("前置"), &page->beforeFormat, &page->beforeText,
        &page->beforeDelay, QStringLiteral("Before"), QStringLiteral("BeforeDelay"), tr("发出后"));
    commandRow->addSpacing(8);
    addCommand(tr("后置"), &page->afterFormat, &page->afterText,
        &page->afterDelay, QStringLiteral("After"), QStringLiteral("AfterDelay"), tr("传完后"));
    root->addLayout(commandRow);

    page->startButton = new QPushButton(page->sending ? tr("发送文件") : tr("接收文件"));
    page->startButton->setObjectName(scopedName(prefix, QStringLiteral("Start")));
    page->startButton->setIcon(actionStateIcon(
        page->sending ? QStringLiteral(":/icons/file-upload.svg")
                      : QStringLiteral(":/icons/file-download.svg"),
        GlyphSize));
    page->startButton->setIconSize(QSize(GlyphSize, GlyphSize));
    page->startButton->setToolTip(page->sending
            ? tr("先发前置指令，再把文件发到所选端点，最后发后置指令")
            : tr("先发前置指令，再把端点上的数据保存到本机，最后发后置指令"));
    pinFixed(page->startButton);
    page->progress = new QProgressBar;
    page->progress->setObjectName(scopedName(prefix, QStringLiteral("Progress")));
    page->progress->setRange(0, 100);
    page->progress->setValue(0);
    page->progress->setTextVisible(true);
    page->progress->setAlignment(Qt::AlignCenter);
    const int actionHeight = page->startButton->sizeHint().height();
    page->startButton->setFixedHeight(actionHeight);
    page->progress->setFixedHeight(actionHeight);
    auto *actionRow = new QHBoxLayout;
    actionRow->setSpacing(4);
    actionRow->addWidget(page->progress, 1);
    actionRow->addWidget(page->startButton);
    root->addLayout(actionRow);

    page->stats = new QLabel;
    page->stats->setObjectName(scopedName(prefix, QStringLiteral("Stats")));
    page->stats->setTextInteractionFlags(Qt::TextSelectableByMouse);
    page->status = new QLabel(tr("未选择"));
    page->status->setObjectName(scopedName(prefix, QStringLiteral("Status")));
    auto *footRow = new QHBoxLayout;
    footRow->setSpacing(8);
    footRow->addWidget(page->status);
    footRow->addWidget(page->stats, 1);
    root->addLayout(footRow);

    const auto refresh = [this, page] {
        if (!m_loading) syncReceivePath(page, true);
        if ((!m_service || !m_service->isBusy()) && page == m_resultPage) {
            m_showingLive = false;
            m_statusFromService = false;
            m_resultPage = nullptr;
        }
        updateControls();
        savePreferences();
        scheduleFileDigest(page);
    };
    connect(page->mode, &QComboBox::currentIndexChanged, this, [refresh](int) { refresh(); });
    connect(page->endpointA, &QPushButton::toggled, this, [this](bool) {
        updateControls();
        savePreferences();
    });
    connect(page->endpointB, &QPushButton::toggled, this, [this](bool) {
        updateControls();
        savePreferences();
    });
    if (page->block128) {
        connect(page->block128, &QPushButton::toggled, this, [this](bool) {
            updateControls();
            savePreferences();
        });
        connect(page->block1024, &QPushButton::toggled, this, [this](bool) {
            updateControls();
            savePreferences();
        });
    }
    if (page->chunk) {
        connect(page->chunk, &QSpinBox::valueChanged, this, [this](int) { savePreferences(); });
    }
    if (page->chunkGap) {
        connect(page->chunkGap, &QSpinBox::valueChanged, this, [this](int) { savePreferences(); });
    }
    connect(page->path, &QLineEdit::textChanged, this, [this, page](const QString &) {
        if ((!m_service || !m_service->isBusy()) && page == m_resultPage) {
            m_showingLive = false;
            m_statusFromService = false;
            m_resultPage = nullptr;
        }
        updateControls();
        savePreferences();
        scheduleFileDigest(page);
    });
    const auto bindSide = [this, page](bool before) {
        QLineEdit *text = before ? page->beforeText : page->afterText;
        QComboBox *format = before ? page->beforeFormat : page->afterFormat;
        connect(text, &QLineEdit::textChanged, this, [this](const QString &) {
            updateCommandPreview();
            savePreferences();
        });
        connect(format, &QComboBox::currentIndexChanged, this, [this](int) {
            updateCommandPreview();
            savePreferences();
        });
    };
    bindSide(true);
    bindSide(false);
    connect(page->beforeDelay, &QSpinBox::valueChanged, this, [this](int) { savePreferences(); });
    connect(page->afterDelay, &QSpinBox::valueChanged, this, [this](int) { savePreferences(); });
    connect(page->browse, &QPushButton::clicked, this, [this, page] { choosePath(page); });
    connect(page->startButton, &QPushButton::clicked, this, [this, page] {
        if (m_service && m_service->isBusy())
            m_service->stop();
        else
            startTransfer(page);
    });
}

bool FileTransferWidget::isBusy() const
{
    return m_service && m_service->isBusy();
}

void FileTransferWidget::setRuntime(EndpointRuntime *runtime)
{
    m_service->setRuntime(runtime);
}

void FileTransferWidget::setPreferences(PreferencesStore *store)
{
    m_preferences = store;
    if (!m_preferences) return;
    m_loading = true;
    const QString legacyDirection = m_preferences->value(QStringLiteral("file/direction"),
                                              QStringLiteral("send")).toString();
    const bool legacyIsReceive = legacyDirection == QLatin1String("receive");
    loadPage(&m_send, !legacyIsReceive);
    loadPage(&m_receive, legacyIsReceive);
    m_loading = false;
    updateCommandPreview();
    updateControls();
}

void FileTransferWidget::loadPage(Page *page, bool useLegacy)
{
    const QString mode = pref(page->prefPrefix, QStringLiteral("mode"), QStringLiteral("raw"), useLegacy).toString();
    const int modeIndex = page->mode->findData(mode);
    if (modeIndex >= 0) page->mode->setCurrentIndex(modeIndex);
    if (pref(page->prefPrefix, QStringLiteral("endpoint"), 0, useLegacy).toInt() == 1)
        page->endpointB->setChecked(true);
    else
        page->endpointA->setChecked(true);
    if (page->block128) {
        if (pref(page->prefPrefix, QStringLiteral("block"), 1024, useLegacy).toInt() == 128)
            page->block128->setChecked(true);
        else
            page->block1024->setChecked(true);
    }
    if (page->chunk)
        page->chunk->setValue(pref(page->prefPrefix, QStringLiteral("rawChunk"), 16 * 1024, useLegacy).toInt());
    if (page->chunkGap)
        page->chunkGap->setValue(pref(page->prefPrefix, QStringLiteral("rawGap"), 0, useLegacy).toInt());
    if (!page->sending) {
        const bool ymodem = mode == QLatin1String("ymodem");
        QString shown = pref(page->prefPrefix,
            ymodem ? QStringLiteral("dir") : QStringLiteral("rawPath"), QString(), useLegacy).toString();
        if (shown.isEmpty())
            shown = pref(page->prefPrefix, QStringLiteral("path"), QString(), useLegacy).toString();
        page->path->setText(shown);
    } else {
        page->path->setText(pref(page->prefPrefix, QStringLiteral("path"), QString(), useLegacy).toString());
    }
    const auto loadSide = [&](bool before) {
        const QString stem = before ? QStringLiteral("before") : QStringLiteral("after");
        QComboBox *format = before ? page->beforeFormat : page->afterFormat;
        QLineEdit *text = before ? page->beforeText : page->afterText;
        const QString textKey = page->prefPrefix + QLatin1Char('/') + stem + QStringLiteral("Text");
        const QString tailKeyName = stem + QStringLiteral("Tail");
        const auto dropTail = [&] {
            m_preferences->remove(page->prefPrefix + QLatin1Char('/') + tailKeyName);
            if (useLegacy) m_preferences->remove(QStringLiteral("file/") + tailKeyName);
        };
        if (m_preferences->value(textKey).isValid()) {
            const bool hex = pref(page->prefPrefix, stem + QStringLiteral("Hex"), 0, useLegacy).toInt() == 1;
            format->setCurrentIndex(hex ? 1 : 0);
            const QString merged = appendStoredTail(m_preferences->value(textKey).toString(),
                pref(page->prefPrefix, tailKeyName, QStringLiteral("none"), useLegacy).toString(), hex);
            text->setText(merged);
            if (merged != m_preferences->value(textKey).toString())
                m_preferences->setValue(textKey, merged);
            dropTail();
        } else {
            // 旧配置只存了快捷指令 id。内容和结尾符合成一段填进输入框。
            CommandItem command;
            const QString id = pref(page->prefPrefix, stem + QStringLiteral("Id"), QString(), useLegacy).toString();
            if (!lookupCommand(id, &command)) {
                dropTail();
                return;
            }
            format->setCurrentIndex(command.isHex ? 1 : 0);
            text->setText(appendStoredTail(command.content, tailKeyFromCommand(command.tail), command.isHex));
            dropTail();
        }
    };
    loadSide(true);
    loadSide(false);
    page->beforeDelay->setValue(pref(page->prefPrefix, QStringLiteral("beforeDelay"), 0, useLegacy).toInt());
    page->afterDelay->setValue(pref(page->prefPrefix, QStringLiteral("afterDelay"), 0, useLegacy).toInt());
    syncReceivePath(page, false);
}

QVariant FileTransferWidget::pref(const QString &prefix, const QString &key, const QVariant &fallback,
    bool useLegacy) const
{
    const QVariant scoped = m_preferences->value(prefix + QLatin1Char('/') + key);
    if (scoped.isValid()) return scoped;
    if (useLegacy) {
        const QVariant old = m_preferences->value(QStringLiteral("file/") + key);
        if (old.isValid()) return old;
    }
    return fallback;
}

void FileTransferWidget::setEndpointOpen(int index, bool open)
{
    if (index < 0 || index >= 2) return;
    m_openFlags[index] = open;
    updateControls();
}

void FileTransferWidget::rawDataReceived(int index, const Traffic::Source &source,
    const QByteArray &payload)
{
    m_service->rawDataReceived(index, source, payload);
}

bool FileTransferWidget::ymodemSelected(const Page *page) const
{
    return page->mode->currentData().toString() == QLatin1String("ymodem");
}

bool FileTransferWidget::xmodemSelected(const Page *page) const
{
    return page->mode->currentData().toString() == QLatin1String("xmodem");
}

int FileTransferWidget::endpoint(const Page *page) const
{
    return page->endpointB->isChecked() ? 1 : 0;
}

void FileTransferWidget::setStatus(const QString &text)
{
    m_statusFromService = true;
    m_serviceStatus = text;
    updateControls();
}

void FileTransferWidget::syncReceivePath(Page *page, bool modeSwitched)
{
    if (!page || page->sending) return;
    const QString current = page->path->text().trimmed();
    const QString prefix = page->prefPrefix + QLatin1Char('/');
    const auto remembered = [this, &prefix](const QString &key) {
        if (!m_preferences) return QString();
        return m_preferences->value(prefix + key).toString().trimmed();
    };
    const auto remember = [this, &prefix](const QString &key, const QString &value) {
        if (!m_preferences || m_loading || value.isEmpty()) return;
        m_preferences->setValue(prefix + key, QDir::cleanPath(value));
    };
    if (ymodemSelected(page)) {
        // 从 RAW 切过来时，框里还是文件路径，先记下再换成目录。
        if (modeSwitched && !current.isEmpty() && !pathIsDirectory(current))
            remember(QStringLiteral("rawPath"), current);
        QString dir = remembered(QStringLiteral("dir"));
        if (dir.isEmpty())
            dir = pathIsDirectory(current) ? current : defaultReceiveDirectory();
        dir = QDir::cleanPath(dir);
        if (QDir::cleanPath(current) != dir)
            page->path->setText(dir);
        return;
    }
    if (modeSwitched && pathIsDirectory(current))
        remember(QStringLiteral("dir"), current);
    if (!current.isEmpty() && !pathIsDirectory(current))
        return;
    QString raw = remembered(QStringLiteral("rawPath"));
    if (raw.isEmpty() || pathIsDirectory(raw))
        raw = defaultRawReceivePath(current);
    raw = QDir::cleanPath(raw);
    if (QDir::cleanPath(current) != raw)
        page->path->setText(raw);
}

void FileTransferWidget::updateCommandPreview()
{
    const auto apply = [this](Page *page) {
        const auto show = [this](QLineEdit *edit, const QComboBox *format) {
            if (!edit || !format) return;
            const bool hex = format->currentIndex() == 1;
            edit->setPlaceholderText(hex
                ? tr("0D 0A，留空不发")
                : QStringLiteral("AT+START\\r\\n"));
        };
        show(page->beforeText, page->beforeFormat);
        show(page->afterText, page->afterFormat);
        if (page->beforeText)
            page->beforeText->setToolTip(fieldPreview(page->beforeText, page->beforeFormat));
        if (page->afterText)
            page->afterText->setToolTip(fieldPreview(page->afterText, page->afterFormat));
    };
    apply(&m_send);
    apply(&m_receive);
}

QString FileTransferWidget::fieldPreview(const QLineEdit *edit, const QComboBox *format) const
{
    const bool hex = format && format->currentIndex() == 1;
    if (!edit || edit->text().trimmed().isEmpty()) {
        return hex
            ? tr("留空不发送。HEX 直接写字节，回车写成 0D 0A")
            : tr("留空不发送。字符串支持转义 \\r \\n \\t \\0 \\\\ \\xNN");
    }
    const auto parsed = PayloadCodec::parse(edit->text(),
        hex ? PayloadCodec::Format::Hex : PayloadCodec::Format::Escape);
    if (!parsed.valid()) return parsed.error;
    const QString hexText = PayloadCodec::render(parsed.bytes.left(48), PayloadCodec::Format::Hex);
    return tr("%1 字节 · %2%3")
        .arg(parsed.bytes.size())
        .arg(hexText, parsed.bytes.size() > 48 ? QStringLiteral(" …") : QString());
}

void FileTransferWidget::savePreferences()
{
    if (!m_preferences || m_loading) return;
    const auto savePage = [this](const Page &page) {
        const QString prefix = page.prefPrefix + QLatin1Char('/');
        m_preferences->setValue(prefix + QStringLiteral("mode"), page.mode->currentData().toString());
        m_preferences->setValue(prefix + QStringLiteral("endpoint"), endpoint(&page));
        m_preferences->setValue(prefix + QStringLiteral("path"), page.path->text());
        if (!page.sending) {
            const QString shown = page.path->text().trimmed();
            if (!shown.isEmpty()) {
                if (ymodemSelected(&page))
                    m_preferences->setValue(prefix + QStringLiteral("dir"), QDir::cleanPath(shown));
                else if (!pathIsDirectory(shown))
                    m_preferences->setValue(prefix + QStringLiteral("rawPath"), QDir::cleanPath(shown));
            }
        }
        if (page.block128)
            m_preferences->setValue(prefix + QStringLiteral("block"), page.block128->isChecked() ? 128 : 1024);
        if (page.chunk)
            m_preferences->setValue(prefix + QStringLiteral("rawChunk"), page.chunk->value());
        if (page.chunkGap)
            m_preferences->setValue(prefix + QStringLiteral("rawGap"), page.chunkGap->value());
        const auto saveSide = [&](bool before) {
            const QString stem = before ? QStringLiteral("before") : QStringLiteral("after");
            const QComboBox *format = before ? page.beforeFormat : page.afterFormat;
            const QLineEdit *text = before ? page.beforeText : page.afterText;
            m_preferences->setValue(prefix + stem + QStringLiteral("Hex"), format->currentIndex() == 1 ? 1 : 0);
            m_preferences->setValue(prefix + stem + QStringLiteral("Text"), text->text());
        };
        saveSide(true);
        saveSide(false);
        m_preferences->setValue(prefix + QStringLiteral("beforeDelay"), page.beforeDelay->value());
        m_preferences->setValue(prefix + QStringLiteral("afterDelay"), page.afterDelay->value());
    };
    savePage(m_send);
    savePage(m_receive);
}

bool FileTransferWidget::encodeField(const QLineEdit *edit, const QComboBox *format,
    QByteArray *bytes, QString *error) const
{
    bytes->clear();
    if (!edit || edit->text().trimmed().isEmpty()) return true;
    const bool hex = format && format->currentIndex() == 1;
    const auto parsed = PayloadCodec::parse(edit->text(),
        hex ? PayloadCodec::Format::Hex : PayloadCodec::Format::Escape);
    if (!parsed.valid()) {
        *error = parsed.error;
        return false;
    }
    if (parsed.bytes.isEmpty()) {
        *error = tr("指令没有可发送的字节");
        return false;
    }
    *bytes = parsed.bytes;
    return true;
}

void FileTransferWidget::choosePath(Page *page)
{
    QString path;
    if (page->sending) {
        path = QFileDialog::getOpenFileName(page->root, tr("选择发送文件"), page->path->text());
    } else if (ymodemSelected(page)) {
        const QString current = page->path->text().trimmed();
        const QString start = (!current.isEmpty() && QFileInfo(current).isDir())
            ? current
            : defaultReceiveDirectory();
        path = QFileDialog::getExistingDirectory(page->root, tr("选择接收目录"), start);
    } else {
        path = QFileDialog::getSaveFileName(page->root, tr("选择接收文件"), page->path->text());
    }
    if (!path.isEmpty()) page->path->setText(path);
}

void FileTransferWidget::startTransfer(Page *page)
{
    const int target = endpoint(page);
    if (!m_openFlags[target] || page->path->text().trimmed().isEmpty()) return;
    m_resultPage = page;
    QString error;
    FileTransferSides sides;
    if (!encodeField(page->beforeText, page->beforeFormat, &sides.before, &error)
        || !encodeField(page->afterText, page->afterFormat, &sides.after, &error)) {
        setStatus(error);
        return;
    }
    sides.beforeDelayMs = page->beforeDelay->value();
    sides.afterDelayMs = page->afterDelay->value();
    const QString path = page->path->text().trimmed();
    if (page->sending) {
        if (xmodemSelected(page))
            m_service->startXModemSend(target, path, sides);
        else if (ymodemSelected(page))
            m_service->startYModemSend(target, path, page->block128->isChecked() ? 128 : 1024, sides);
        else
            m_service->startRawSend(target, path, sides,
                page->chunk ? page->chunk->value() : 16 * 1024,
                page->chunkGap ? page->chunkGap->value() : 0);
    } else if (xmodemSelected(page)) {
        m_service->startXModemReceive(target, path, sides);
    } else if (ymodemSelected(page)) {
        m_service->startYModemReceive(target, path, sides);
    } else {
        m_service->startRawReceive(target, path, sides);
    }
}

void FileTransferWidget::updateControls()
{
    updatePage(&m_send);
    updatePage(&m_receive);
    refreshFooter(&m_send);
    refreshFooter(&m_receive);
}

void FileTransferWidget::updatePage(Page *page)
{
    if (!page->root) return;
    const bool busy = m_service && m_service->isBusy();
    const bool ready = m_openFlags[endpoint(page)];
    const bool hasPath = !page->path->text().trimmed().isEmpty();
    const bool ymodem = ymodemSelected(page);
    const bool xmodem = xmodemSelected(page);
    const bool framed = xmodem || ymodem;
    page->mode->setEnabled(!busy);
    page->endpointA->setEnabled(!busy);
    page->endpointB->setEnabled(!busy);
    if (page->block128) {
        // XMODEM 固定 128，标签和按钮一起藏，避免只剩「分包」几个字。
        if (page->blockLabel)
            page->blockLabel->setVisible(ymodem);
        page->block128->setVisible(ymodem);
        page->block1024->setVisible(ymodem);
        page->block128->setEnabled(!busy && ymodem);
        page->block1024->setEnabled(!busy && ymodem);
    }
    if (page->chunk) {
        page->chunk->setVisible(!framed);
        page->chunk->setEnabled(!busy && !framed);
    }
    if (page->chunkGap) {
        page->chunkGap->setVisible(!framed);
        page->chunkGap->setEnabled(!busy && !framed);
    }
    if (page->chunkGapLabel)
        page->chunkGapLabel->setVisible(!framed);
    page->path->setEnabled(!busy);
    page->browse->setEnabled(!busy);
    page->beforeFormat->setEnabled(!busy);
    page->afterFormat->setEnabled(!busy);
    page->beforeText->setEnabled(!busy);
    page->afterText->setEnabled(!busy);
    page->beforeDelay->setEnabled(!busy);
    page->afterDelay->setEnabled(!busy);
    const QString idleText = page->sending ? tr("发送文件") : tr("接收文件");
    page->startButton->setText(busy ? tr("停止") : idleText);
    page->startButton->setIcon(actionStateIcon(
        busy ? QStringLiteral(":/icons/stop.svg")
             : (page->sending ? QStringLiteral(":/icons/file-upload.svg")
                              : QStringLiteral(":/icons/file-download.svg")),
        GlyphSize));
    page->startButton->setEnabled(busy || (ready && hasPath));
    if (page->startButton->property("busy").toBool() != busy) {
        page->startButton->setProperty("busy", busy);
        page->startButton->style()->unpolish(page->startButton);
        page->startButton->style()->polish(page->startButton);
    }
    if (page->sending) {
        page->path->setPlaceholderText(tr("选择要发送的文件"));
        page->browse->setText(tr("选择文件"));
    } else if (ymodem) {
        page->path->setPlaceholderText(tr("选择 YMODEM 保存目录"));
        page->browse->setText(tr("选择目录"));
    } else if (xmodem) {
        page->path->setPlaceholderText(tr("选择 XMODEM 保存文件"));
        page->browse->setText(tr("保存为"));
    } else {
        page->path->setPlaceholderText(tr("选择 RAW 保存文件"));
        page->browse->setText(tr("保存为"));
    }
    if (busy)
        page->startButton->setToolTip(tr("停止当前这一次文件传输"));
    else if (!ready)
        page->startButton->setToolTip(tr("先连接端点 %1").arg(endpoint(page) == 0 ? QStringLiteral("A") : QStringLiteral("B")));
    else if (!hasPath)
        page->startButton->setToolTip(page->sending ? tr("先选择要发送的文件") : tr("先选择保存位置"));
    else
        page->startButton->setToolTip(page->sending
                ? tr("先发前置指令，再把文件发到所选端点，最后发后置指令")
                : tr("先发前置指令，再把端点上的数据保存到本机，最后发后置指令"));
    if (xmodem) {
        page->mode->setToolTip(page->sending
                ? tr("XMODEM：128 字节块，单次 EOT，不传文件名。对端发 C 用 CRC16（133 字节），发 NAK 用 1 字节校验和（132 字节）")
                : tr("XMODEM 写入所选文件。先发 C 要 CRC，超时再发 NAK。对端直接发来 132 字节帧时按校验和接收。单次 EOT 结束。长度按 128 字节对齐，末尾可能含 0x1A"));
    } else if (!ymodem) {
        page->mode->setToolTip(page->sending
                ? tr("RAW 按原始字节发送。分包是每次连续写出的字节数，间隔是发完一包后再发下一包的等待")
                : tr("RAW 把收到的字节写入文件。点停止后才保存；前后指令等待期间的字节不写入"));
    } else if (!page->sending) {
        page->mode->setToolTip(tr("YMODEM 保存到所选目录，文件名和长度从协议头读取。分包大小由发送端决定"));
    } else {
        const int block = page->block128 && page->block128->isChecked() ? 128 : 1024;
        page->mode->setToolTip(tr("YMODEM 数据块 %1 字节（数据帧 %2 字节，含 CRC16）；文件头固定 133 字节")
                                   .arg(block)
                                   .arg(block + 5));
    }
}

void FileTransferWidget::updateProgress(qint64 current, qint64 total)
{
    const auto apply = [&](QProgressBar *bar) {
        if (!bar) return;
        if (total <= 0) {
            bar->setRange(0, 0);
            bar->setFormat(tr("%1").arg(formatBytes(current)));
            return;
        }
        bar->setRange(0, 1000);
        const int value = int(qBound<qint64>(qint64(0), current * 1000 / total, qint64(1000)));
        bar->setValue(value);
        bar->setFormat(QStringLiteral("%1%").arg(value / 10.0, 0, 'f', 1));
    };
    apply(m_send.progress);
    apply(m_receive.progress);
}

FileTransferWidget::DigestCache *FileTransferWidget::digestFor(Page *page)
{
    return page && page->sending ? &m_sendDigest : &m_receiveDigest;
}

const FileTransferWidget::DigestCache *FileTransferWidget::digestFor(const Page *page) const
{
    return page && page->sending ? &m_sendDigest : &m_receiveDigest;
}

QString FileTransferWidget::fileStatusText(const Page *page) const
{
    const QString path = page->path->text().trimmed();
    if (path.isEmpty()) return tr("未选择");
    const QFileInfo info(path);
    if (!page->sending && ymodemSelected(page))
        return info.exists() && info.isDir() ? tr("待接收") : tr("需要目录");
    if (info.exists() && info.isFile())
        return page->sending ? tr("待发送") : tr("待接收");
    if (!info.exists() && !page->sending) return tr("待接收");
    return info.exists() ? tr("不是文件") : tr("找不到");
}

QString FileTransferWidget::fileCardText(const Page *page) const
{
    const QString path = page->path->text().trimmed();
    if (path.isEmpty()) return tr("选择文件后显示大小和 CRC");
    const QFileInfo info(path);
    const QString name = info.fileName().isEmpty() ? path : info.fileName();
    if (!page->sending && ymodemSelected(page)) {
        if (!info.exists() || !info.isDir()) return tr("请改为保存目录");
        return tr("%1 · 文件名和大小由对端给出").arg(name);
    }
    if (!info.exists()) {
        return page->sending ? tr("%1 · 找不到文件").arg(name) : tr("%1 · 新文件").arg(name);
    }
    if (!info.isFile()) return tr("%1 · 不是文件").arg(name);

    const QString head = page->sending
        ? tr("%1 · %2").arg(name, formatBytes(info.size()))
        : tr("%1 · 将覆盖 %2").arg(name, formatBytes(info.size()));
    const DigestCache *cache = digestFor(page);
    const QString absolute = QDir::cleanPath(info.absoluteFilePath());
    const bool sameFile = cache && cache->path == absolute && cache->size == info.size()
        && cache->modifiedMs == info.lastModified().toMSecsSinceEpoch();
    if (cache && sameFile && cache->ready) {
        return tr("%1 · CRC16 %2 · CRC32 %3")
            .arg(head, crc16Text(cache->crc16), crc32Text(cache->crc32));
    }
    if (cache && sameFile && cache->failed) return tr("%1 · 无法计算 CRC").arg(head);
    return tr("%1 · CRC 计算中").arg(head);
}

QString FileTransferWidget::fileCardTip(const Page *page) const
{
    const QString path = page->path->text().trimmed();
    if (path.isEmpty()) return tr("选择文件后，这里显示大小、CRC16 和 CRC32");
    if (!page->sending && ymodemSelected(page))
        return tr("YMODEM 的文件名、长度和校验要等对端文件头。这里只确认保存目录");
    const QFileInfo info(path);
    if (!info.exists()) {
        return page->sending
            ? tr("这个路径上没有文件")
            : tr("路径还不存在。接收完成后才会有大小和 CRC");
    }
    if (!page->sending && info.isFile()) {
        return tr("CRC16 为 XMODEM（初值 0），CRC32 为 IEEE，按磁盘上的现有文件计算。"
                  "开始接收后这份文件会被覆盖，底部再改成这次写入的速率和校验");
    }
    return tr("CRC16 为 XMODEM（初值 0），CRC32 为 IEEE，按整个文件计算。"
              "开始传输后，底部改成速率、进度和同一算法的载荷校验");
}

void FileTransferWidget::refreshFooter(Page *page)
{
    if (!page || !page->stats || !page->status) return;
    const bool busy = m_service && m_service->isBusy();
    // 进行中两页都看同一组计数。结束后只留在发起的那一页，另一页回到自己的文件信息。
    const bool showLive = m_showingLive && (busy || page == m_resultPage);
    if (showLive) {
        page->stats->setText(m_liveText);
        page->stats->setToolTip(m_liveTip);
        if (m_statusFromService) page->status->setText(m_serviceStatus);
        return;
    }
    page->stats->setText(fileCardText(page));
    page->stats->setToolTip(fileCardTip(page));
    if (m_statusFromService && (busy || page == m_resultPage))
        page->status->setText(m_serviceStatus);
    else
        page->status->setText(fileStatusText(page));
}

void FileTransferWidget::scheduleFileDigest(Page *page)
{
    if (!page) return;
    DigestCache *cache = digestFor(page);
    QTimer *timer = page->sending ? m_sendDigestTimer : m_receiveDigestTimer;
    const QString path = page->path->text().trimmed();
    const bool directoryOnly = !page->sending && ymodemSelected(page);
    const QFileInfo info(path);
    const bool hashable = !directoryOnly && !path.isEmpty() && info.exists() && info.isFile();
    if (!hashable) {
        ++cache->generation;
        cache->pending = false;
        cache->ready = false;
        cache->failed = false;
        cache->path.clear();
        cache->size = 0;
        cache->modifiedMs = 0;
        if (timer) timer->stop();
        refreshFooter(page);
        return;
    }
    const QString absolute = QDir::cleanPath(info.absoluteFilePath());
    const qint64 modifiedMs = info.lastModified().toMSecsSinceEpoch();
    if (cache->ready && !cache->failed && cache->path == absolute && cache->size == info.size()
        && cache->modifiedMs == modifiedMs) {
        cache->pending = false;
        if (timer) timer->stop();
        refreshFooter(page);
        return;
    }
    cache->pending = true;
    cache->ready = false;
    cache->failed = false;
    cache->path = absolute;
    cache->size = info.size();
    cache->modifiedMs = modifiedMs;
    ++cache->generation;
    refreshFooter(page);
    if (timer) timer->start();
}

void FileTransferWidget::startFileDigest(Page *page)
{
    DigestCache *cache = digestFor(page);
    if (!cache || cache->path.isEmpty()) return;
    if (cache->hashing) {
        cache->again = true;
        return;
    }
    cache->hashing = true;
    const quint64 generation = cache->generation;
    const QString path = cache->path;
    auto *watcher = new QFutureWatcher<FileDigest>(this);
    connect(watcher, &QFutureWatcher<FileDigest>::finished, this, [this, page, watcher, generation] {
        const FileDigest digest = watcher->result();
        watcher->deleteLater();
        DigestCache *cache = digestFor(page);
        cache->hashing = false;
        if (generation == cache->generation && digest.path == cache->path) {
            cache->pending = false;
            cache->ready = digest.ok;
            cache->failed = !digest.ok;
            if (digest.ok) {
                cache->crc16 = digest.crc16;
                cache->crc32 = digest.crc32;
            }
            refreshFooter(page);
        }
        if (cache->again) {
            cache->again = false;
            startFileDigest(page);
        }
    });
    watcher->setFuture(QtConcurrent::run([path] { return hashFile(path); }));
}

void FileTransferWidget::updateStats(qint64 transferred, qint64 total, int retries, int crcErrors,
    int payloadCrc16, quint32 payloadCrc32, qint64 elapsedMs)
{
    const bool stillIdle = transferred == 0 && elapsedMs == 0 && retries == 0 && crcErrors == 0;
    if (stillIdle && !m_showingLive) return;

    const qint64 perSecond = elapsedMs > 0 ? transferred * 1000 / elapsedMs : 0;
    const QString amount = total > 0
        ? tr("%1 / %2").arg(formatBytes(transferred), formatBytes(total))
        : formatBytes(transferred);
    QStringList parts;
    parts << tr("%1/s").arg(formatBytes(perSecond)) << amount
          << tr("%1 s").arg(elapsedMs / 1000.0, 0, 'f', 1);
    if (retries > 0) parts << tr("重试 %1").arg(retries);
    if (crcErrors > 0) parts << tr("CRC错误 %1").arg(crcErrors);
    parts << tr("CRC16 %1").arg(crc16Text(quint16(payloadCrc16)))
          << tr("CRC32 %1").arg(crc32Text(payloadCrc32));
    m_showingLive = true;
    m_liveText = parts.join(QStringLiteral(" · "));
    m_liveTip = tr("速率和用时从文件内容第一次发出或写入开始算，不含前置等待。"
                   "CRC16（XMODEM）和 CRC32（IEEE）只累计文件载荷，不含协议帧头和前后指令。"
                   "重试与 CRC 错误只在出现时显示");
    refreshFooter(&m_send);
    refreshFooter(&m_receive);
}
