#include "trafficpanel.h"
#include "traffictextview.h"
#include "ui/common/combometrics.h"
#include "ui/common/endpointformat.h"
#include "ui/common/tintedicon.h"
#include "persistence/preferencesstore.h"

#include <QComboBox>
#include <QFileDialog>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <utility>

namespace {

// 顶栏按钮与菜单条目的字形尺寸，与动作按钮一致。
constexpr int BarIconSize = 16;

// 单条消息最多保留的预览字节数。原始长度另存于 rawLength，显示的是"本条实际有多大"，
// 而不是"我们留下了多少"——两者不等即说明发生了截断。
constexpr int PreviewLimit = 4096;

// 模型的历史预算：4 MiB 或 20000 条，先到者为准。
constexpr qint64 HistoryByteBudget = 4 * 1024 * 1024;

// 只限字节是不够的：串口遥测常见几十字节的小帧，光靠 4 MiB 预算能堆到六万条以上，
// 而每条 Message（时间、来源、三个 QUuid、两个 QString）的元数据开销远大于它那
// 64 字节的载荷——"限了字节"不等于"限了内存"。
constexpr int HistoryRowBudget = 20000;

// 真实消息到达的攒批窗口：一帧左右的时间足够把同一次 readyRead 的多条消息
// 合成一个插入通知；再长会让人感觉记录"迟一拍"。
constexpr int FlushIntervalMs = 16;

// 丢弃最旧记录直到回到预算内。逐条移除而不是整体截断，保留最新的连续上下文。
// 字节与条数两个上限先到者为准；返回丢弃的条数，由调用方决定用哪种模型通知。
int trimToBudget(QList<Traffic::Message> &records, qint64 &bytes)
{
    int dropped = 0;
    while (!records.isEmpty()
           && (bytes > HistoryByteBudget || records.size() > HistoryRowBudget)) {
        bytes -= records.first().raw.size();
        records.removeFirst();
        ++dropped;
    }
    return dropped;
}

void enforceBudget(QList<Traffic::Message> &records)
{
    qint64 bytes = 0;
    for (const auto &record : records)
        bytes += record.raw.size();
    trimToBudget(records, bytes);
}

// 保留预览长度，并把 rawLength 修正为真实长度。
void applyPreviewLimit(Traffic::Message &record)
{
    record.rawLength = record.rawLength > 0 ? record.rawLength : record.raw.size();
    if (record.raw.size() > PreviewLimit) {
        record.raw.truncate(PreviewLimit);
        record.truncated = true;
    } else if (record.rawLength > record.raw.size()) {
        record.truncated = true;
    }
}

} // namespace

TrafficModel::TrafficModel(QObject *parent) : QObject(parent) {}
int TrafficModel::rowCount() const { return m_records.size(); }
const Traffic::Message &TrafficModel::record(int row) const { return m_records.at(row); }

void TrafficModel::replaceRecords(QList<Traffic::Message> records)
{
    // This preview model has a hard cap; live batching belongs to the I/O milestone.
    if (records.size() > 2000) records = records.last(2000);
    for (auto &r : records)
        applyPreviewLimit(r);
    enforceBudget(records);
    m_records = std::move(records);
    m_rawBytes = 0;
    for (const auto &r : m_records)
        m_rawBytes += r.raw.size();
}

void TrafficModel::appendRecords(QList<Traffic::Message> records)
{
    if (records.isEmpty()) return;
    for (auto &r : records)
        applyPreviewLimit(r);
    const int added = records.size();

    qint64 incomingBytes = 0;
    for (const auto &r : records)
        incomingBytes += r.raw.size();

    // 先把本批计入总量，再算出"包含本批在内"要淘汰多少条。反过来（先裁后插）
    // 会让上限超出整整一批，把硬约束变成软约束。
    int drop = 0;
    qint64 keptBytes = m_rawBytes;
    while (drop < m_records.size()
           && (keptBytes + incomingBytes > HistoryByteBudget
               || (m_records.size() - drop) + added > HistoryRowBudget)) {
        keptBytes -= m_records.at(drop).raw.size();
        ++drop;
    }

    if (drop > 0) m_records.remove(0, drop);
    m_rawBytes = keptBytes + incomingBytes;
    m_records += records;
}

bool TrafficFilter::accepts(const Traffic::Message &r, const Options &options, PayloadCodec::Format format)
{
    // 端点与方向是消息自带的事实字段，直接按结构判等，不解析显示文本。
    if (options.endpoints.isEmpty() || !options.endpoints.contains(r.source.endpointId)) return false;
    switch (r.direction) {
    case Traffic::Direction::Receive:
        if (!options.rx) return false;
        break;
    case Traffic::Direction::Transmit:
        if (!options.tx) return false;
        break;
    case Traffic::Direction::State:
        // 状态迁移不属于任何一个方向开关，跟随 RX 一起显示。
        if (!options.rx && !options.tx) return false;
        break;
    default:
        // 错误/警告日志有独立开关：排障时才打开，平时不让异常事件刷屏。
        if (!options.events) return false;
        break;
    }
    if (options.search.isEmpty()) return true;

    // 搜索匹配当前显示格式的渲染文本、UTF-8 文本与两种 HEX 写法，所见即可搜；
    // 也匹配来源的对端地址和自由说明文本。逐项短路，命中即返回，
    // 避免对每行做无谓的渲染与 HEX 展开。
    const auto needle = options.search;
    if (r.source.peerAddress.contains(needle, Qt::CaseInsensitive)) return true;
    if (r.source.endpointName.contains(needle, Qt::CaseInsensitive)) return true;
    if (r.statusText.contains(needle, Qt::CaseInsensitive)) return true;
    if (r.detail.contains(needle, Qt::CaseInsensitive)) return true;
    if (PayloadCodec::render(r.raw, format).contains(needle, Qt::CaseInsensitive)) return true;
    if (PayloadCodec::render(r.raw, PayloadCodec::Format::Utf8).contains(needle, Qt::CaseInsensitive)) return true;
    if (QString(r.raw.toHex()).contains(needle, Qt::CaseInsensitive)) return true;
    return QString::fromLatin1(r.raw.toHex(' ')).contains(needle, Qt::CaseInsensitive);
}

TrafficPanel::TrafficPanel(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setObjectName("Surface");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 12);
    layout->setSpacing(12);

    // 单行顶栏：A/B 端点开关、编码切换、过滤关键词、显示设置、导出、
    // 滚动到底、清空。
    auto *top = new QHBoxLayout;
    top->setSpacing(6);

    // A/B 端点开关直接放在选项栏里：点哪个看哪个，都点看全部，无需进菜单。
    const auto portToggle = [this](const QString &letter, const QString &name) {
        auto *button = new QPushButton(letter, this);
        button->setObjectName(name);
        button->setCheckable(true);
        button->setChecked(true);
        button->setToolTip(tr("显示端点 %1 的记录").arg(letter));
        connect(button, &QPushButton::toggled, this, &TrafficPanel::updatePortVisibility);
        return button;
    };
    m_portAButton = portToggle(QStringLiteral("A"), QStringLiteral("showPortA"));
    top->addWidget(m_portAButton);
    m_portBButton = portToggle(QStringLiteral("B"), QStringLiteral("showPortB"));
    top->addWidget(m_portBButton);

    m_format = new QComboBox;
    m_format->setObjectName("trafficFormat");
    // 显示格式与发送区共用 PayloadCodec 的同一套解析/渲染规则。
    for (const auto format : PayloadCodec::formats())
        m_format->addItem(PayloadCodec::formatName(format));
    fitComboToItems(m_format);
    top->addWidget(m_format);

    m_search = new QLineEdit;
    m_search->setObjectName("trafficSearch");
    m_search->setPlaceholderText(tr("搜索文本 / HEX / 对端地址"));
    m_search->setClearButtonEnabled(true);
    top->addWidget(m_search, 1);

    // 日志显示设置：方向开关与行内容开关收进菜单，顶栏只留一个入口。
    auto *settings = new QPushButton(tr("显示设置"));
    settings->setObjectName("displaySettings");
    settings->setIcon(mutedIcon(QStringLiteral(":/icons/cog.svg"), BarIconSize));
    settings->setToolTip(tr("日志显示选项：RX/TX、行前缀、自动换行"));
    auto *displayMenu = new QMenu(settings);
    m_rxAction = displayMenu->addAction(tr("显示 RX"));
    m_rxAction->setObjectName("showRx");
    m_rxAction->setCheckable(true);
    m_rxAction->setChecked(true);
    connect(m_rxAction, &QAction::toggled, this, &TrafficPanel::updateFilter);
    m_txAction = displayMenu->addAction(tr("显示 TX"));
    m_txAction->setObjectName("showTx");
    m_txAction->setCheckable(true);
    m_txAction->setChecked(true);
    connect(m_txAction, &QAction::toggled, this, &TrafficPanel::updateFilter);
    m_errorAction = displayMenu->addAction(tr("显示错误日志"));
    m_errorAction->setObjectName("showErrorLog");
    m_errorAction->setCheckable(true);
    m_errorAction->setChecked(true);
    m_errorAction->setToolTip(tr("错误与警告事件（端口错误、写入失败等）的显示开关"));
    connect(m_errorAction, &QAction::toggled, this, &TrafficPanel::updateFilter);
    displayMenu->addSeparator();
    // 行内容开关：控制台语义，全部在消息到达时生效，已固化的行不重排。
    const auto rowToggle = [this, &displayMenu](const char *name, const QString &text, bool checked) {
        auto *action = displayMenu->addAction(text);
        action->setObjectName(QString::fromLatin1(name));
        action->setCheckable(true);
        action->setChecked(checked);
        connect(action, &QAction::toggled, this, &TrafficPanel::updateFilter);
        return action;
    };
    m_numberAction = rowToggle("showLineNumber", tr("显示行号"), true);
    m_timestampAction = rowToggle("showTimestamp", tr("显示时间戳"), true);
    m_directionAction = rowToggle("showDirection", tr("显示数据方向"), true);
    m_lengthAction = rowToggle("showLength", tr("显示数据长度"), false);
    m_hexAction = rowToggle("showHex", tr("同时显示 HEX"), false);
    m_hexAction->setToolTip(tr("数据行之后另起一行显示 HEX 对照（HEX 显示格式下自动省略）"));
    m_pureAction = rowToggle("pureData", tr("纯数据模式"), false);
    m_pureAction->setToolTip(tr("移除行号、时间、方向等一切前缀，只显示数据本身"));
    // 纯数据开启时，行前缀开关全部失效置灰，状态一目了然。
    connect(m_pureAction, &QAction::toggled, this, [this](bool pure) {
        for (auto *action : {m_numberAction, m_timestampAction, m_directionAction, m_lengthAction,
                             m_hexAction})
            action->setEnabled(!pure);
    });
    m_wrapAction = rowToggle("wrapTraffic", tr("自动换行"), false);
    m_wrapAction->setToolTip(tr("按窗口宽度折行显示；关闭时长报文水平滚动"));
    connect(m_wrapAction, &QAction::toggled, this, [this](bool wrap) {
        m_text->setLineWrapping(wrap);
        savePreferences();
    });
    displayMenu->addSeparator();
    // 记录到文件：与"导出"互补。导出是"把当前所见存一份"，记录是"边看边记"——
    // 记录行跟随当前显示格式（HEX 模式记 HEX、文本模式记文本），所见即所记。
    m_recordAction = displayMenu->addAction(tr("记录到文件"));
    m_recordAction->setObjectName("recordTraffic");
    m_recordAction->setCheckable(true);
    m_recordAction->setToolTip(tr("把新到达的消息按当前显示格式持续写入日志文件"));
    connect(m_recordAction, &QAction::toggled, this, [this](bool enabled) {
        toggleLogRecording(enabled);
        savePreferences();
    });
    settings->setMenu(displayMenu);
    top->addWidget(settings);

    auto *exportButton = new QPushButton(tr("导出"));
    top->addWidget(exportButton);

    m_follow = new QPushButton(tr("滚动到底"));
    m_follow->setObjectName("followTraffic");
    m_follow->setCheckable(true);
    m_follow->setChecked(true);
    m_follow->setIcon(mutedIcon(QStringLiteral(":/icons/arrow-down.svg"), BarIconSize));
    m_follow->setToolTip(tr("始终滚动到最新记录；向上翻阅时自动取消"));
    top->addWidget(m_follow);

    auto *clear = new QPushButton(tr("清空"));
    clear->setObjectName("clearTraffic");
    top->addWidget(clear);
    layout->addLayout(top);

    m_model = new TrafficModel(this);
    // 攒批入模：16ms 一拍把同批消息合成一次插入通知，事件开销与消息速率解耦。
    m_flushTimer = new QTimer(this);
    m_flushTimer->setSingleShot(true);
    m_flushTimer->setInterval(FlushIntervalMs);
    connect(m_flushTimer, &QTimer::timeout, this, &TrafficPanel::flushPending);
    // 文本视图是唯一显示视图：每条消息按行号、时间、方向起头，载荷按文本本来的
    // 样子展开。模型只做结构化存储（导出/记录/搜索），不再驱动任何逐帧绘制。
    m_text = new TrafficTextView;
    auto *empty = new QLabel(tr("等待接收数据\n\n在上方配置 A、B 端点，收发记录将在这里显示。"));
    empty->setAlignment(Qt::AlignCenter);
    empty->setObjectName("EmptyState");
    empty->setWordWrap(true);
    m_empty = empty;
    m_stack = new QStackedWidget;
    m_stack->addWidget(m_text);
    m_stack->addWidget(empty);
    layout->addWidget(m_stack, 1);

    // RX/TX 的过滤连接在创建菜单动作时已完成，这里不再重复 connect。
    connect(m_search, &QLineEdit::textChanged, this, &TrafficPanel::updateFilter);
    connect(m_format, &QComboBox::currentIndexChanged, this, [this](int) {
        // 控制台语义：格式切换只影响新到达的行，已固化的文本不重渲染。
        savePreferences();
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        m_pending.clear();
        m_model->replaceRecords({});
        m_text->clearAll();
        m_totalAccepted = 0;
        updateSummary();
    });
    connect(exportButton, &QPushButton::clicked, this, &TrafficPanel::exportVisible);
    connect(m_follow, &QPushButton::toggled, this, [this](bool follow) {
        if (follow) m_text->scrollToBottom();
        savePreferences();
    });
    // 任何形式的向上翻阅（拖动、滚轮、翻页）都解除“滚动到底”。
    // 视图自己改滚动范围时不算翻阅，否则每次追加都会把跟随关掉；
    // 追加后立刻钉回底部，不会误触发。
    connect(m_text->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int position) {
        if (position < m_text->verticalScrollBar()->maximum()) m_follow->setChecked(false);
    });
    updateSummary();
    // 文本视图按到达时的过滤条件收流，条件在这里做第一次汇聚；
    // m_preferences 此时尚未注入，updateFilter 里的 savePreferences 是空操作。
    updateFilter();
}

void TrafficPanel::setPorts(const QList<PortDraft> &ports)
{
    // 端点身份在会话内稳定；这里只刷新两个开关携带的身份并重算可见集合。
    m_portAButton->setProperty("endpoint", ports.value(0).id);
    m_portBButton->setProperty("endpoint", ports.value(1).id);
    updatePortVisibility();
}

void TrafficPanel::loadExample(const QList<PortDraft> &ports)
{
    if (ports.isEmpty()) return;
    // 构造真实形状的消息，而不是"看起来像"的占位数据：示例走的是与真实数据
    // 完全相同的筛选、格式化与导出路径，将来换了字段不会只有示例数据对不上。
    const auto start = QDateTime::currentDateTime().addSecs(-2);
    const QList<QByteArray> data{QByteArray::fromHex("010300000002c40b"), QByteArray::fromHex("010304001900286bf6"),
        QByteArray("device.ready\r\n"), QByteArray("status: OK\r\n"),
        QByteArray::fromHex("aa550601100020003f0d0a"), QByteArray("ping\r\n"), QByteArray("pong\r\n"),
        QByteArray::fromHex("01060001000119ca"), QByteArray::fromHex("01060001000119ca"),
        QStringLiteral("温度: 25.0 C\r\n").toUtf8(), QByteArray("heartbeat: 128\r\n"), QByteArray("OK\r\n")};
    QList<Traffic::Message> records;
    records.reserve(data.size());
    quint64 id = 1;
    for (int i = 0; i < data.size(); ++i) {
        const int index = (i / 3) % ports.size();
        const auto &port = ports.at(index);
        // 交替演示三种边界语义：流片段、数据报、完整消息，悬停即可看出区别。
        const auto framing = i % 3 == 0 ? Traffic::Framing::Stream
                                         : (i % 3 == 1 ? Traffic::Framing::Datagram : Traffic::Framing::Message);
        auto message = Traffic::makeSampleMessage(id++, start.addMSecs(i * 137),
            Traffic::makeSource(port, index), i % 3 == 0 ? Traffic::Direction::Transmit : Traffic::Direction::Receive,
            data.at(i), framing);
        // 串口与 TCP Client 是字节流，一条记录只是一段，标注它还有后续片段。
        if (framing == Traffic::Framing::Stream && i + 1 < data.size()) {
            message.fragmentIndex = 0;
            message.hasMoreFragments = true;
        }
        if (port.kind == PortDraft::TcpServer || port.kind == PortDraft::Udp)
            message.source.peerAddress = QStringLiteral("127.0.0.1:%1").arg(52000 + index);
        records.append(message);
    }
    m_model->replaceRecords(records);
    m_text->clearAll();
    // 示例数据与 replaceRecords 同语义：整批直达模型，不经显示过滤。
    // 文本行按当前显示格式固化。
    m_totalAccepted = records.size();
    m_text->appendMessages(records, currentDisplayFormat(), m_displayOptions);
    updateSummary();
    if (m_follow->isChecked()) m_text->scrollToBottom();
}

void TrafficPanel::appendMessages(const QList<Traffic::Message> &messages)
{
    if (messages.isEmpty()) return;
    // 逐条入模 + 滚动的事件开销与消息速率成正比；
    // 先攒进缓冲，16ms 一拍整批入模，插入通知次数与消息速率解耦。
    m_pending += messages;
    if (!m_flushTimer->isActive()) m_flushTimer->start();
}

void TrafficPanel::appendMessage(const Traffic::Message &message)
{
    m_pending.append(message);
    if (!m_flushTimer->isActive()) m_flushTimer->start();
}

void TrafficPanel::flushPending()
{
    if (m_pending.isEmpty()) return;
    auto batch = std::move(m_pending);
    m_pending.clear();
    m_model->appendRecords(batch);
    writeLog(batch); // 记录与显示同流：进模型的批次才落日志
    // 文本视图按到达时的过滤条件逐条判定（TrafficFilter::accepts 的唯一显示出口），
    // 行按当前显示格式与开关组合固化后不再重渲染——控制台语义。
    const auto format = currentDisplayFormat();
    QList<Traffic::Message> accepted;
    accepted.reserve(batch.size());
    for (const auto &message : batch)
        if (TrafficFilter::accepts(message, m_textOptions, format)) accepted.append(message);
    m_totalAccepted += accepted.size();
    m_text->appendMessages(accepted, format, m_displayOptions);
    if (m_follow->isChecked()) m_text->scrollToBottom();
    updateSummary();
}

// m_format 下标 → 格式枚举的唯一换算点：格式切换的过滤联动与日志落盘都从这里取，
// 换算规则（越界退回 HEX）只维护这一份。
PayloadCodec::Format TrafficPanel::currentDisplayFormat() const
{
    return PayloadCodec::formats().value(m_format->currentIndex(), PayloadCodec::Format::Hex);
}

void TrafficPanel::toggleLogRecording(bool enabled)
{
    if (!enabled) {
        closeLogFile();
        return;
    }
    if (m_logPath.isEmpty()) {
        // 首次开启才选择文件；之后重新勾选沿用上次路径，继续追加而不是另起文件。
        m_logPath = QFileDialog::getSaveFileName(this, tr("选择日志文件"),
            QStringLiteral("easyDebug-log.txt"), tr("文本文件 (*.txt *.log *.tsv)"));
        if (m_logPath.isEmpty()) {
            // 用户取消 = 不开启；阻断信号复位勾选，避免显示与实际状态不一致。
            const QSignalBlocker blocker(m_recordAction);
            m_recordAction->setChecked(false);
            return;
        }
    }
    m_logFile = new QFile(m_logPath, this);
    if (!m_logFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        // 错误提示经摘要信号顶替状态栏文本；界面状态没有其他变化。
        reportSummary(tr("日志文件无法打开：%1").arg(m_logFile->errorString()));
        delete m_logFile;
        m_logFile = nullptr;
        const QSignalBlocker blocker(m_recordAction);
        m_recordAction->setChecked(false);
        return;
    }
}

void TrafficPanel::writeLog(const QList<Traffic::Message> &messages)
{
    if (!m_logFile || messages.isEmpty()) return;
    // 一条一行，与监视区同源渲染：方向是事实字段，内容由当前显示格式决定。
    const auto format = currentDisplayFormat();
    QTextStream out(m_logFile);
    for (const auto &r : messages) {
        out << r.timestamp.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
            << '\t' << endpointLabel(r.source.endpointIndex, r.source.endpointKind)
            << '\t' << (r.source.hasPeer() ? r.source.peerAddress : QStringLiteral("-"))
            << '\t' << Traffic::directionName(r.direction)
            << '\t' << PayloadCodec::render(r.raw, format)
            << '\n';
    }
    out.flush();
    if (out.status() != QTextStream::Ok) {
        reportSummary(tr("日志写入失败，记录已停止：%1").arg(m_logFile->errorString()));
        m_logFile->close();
        delete m_logFile;
        m_logFile = nullptr;
        const QSignalBlocker blocker(m_recordAction);
        m_recordAction->setChecked(false);
    }
}

void TrafficPanel::closeLogFile()
{
    if (m_logFile) {
        m_logFile->close();
        delete m_logFile;
        m_logFile = nullptr;
    }
}

void TrafficPanel::setPreferences(PreferencesStore *store)
{
    m_preferences = store;
    loadPreferences();
}

void TrafficPanel::loadPreferences()
{
    if (!m_preferences) return;
    // 恢复过程中每个 setChecked 都会联动 updateFilter→savePreferences；
    // 若不抑制，"恢复了 rx 才轮到 whole"的中间状态会把已存的值覆盖掉。
    m_loading = true;
    // 只在值能对上现有格式时恢复，配置里残留的无效文本不动当前选择。
    const auto format = m_preferences->value("traffic/format").toString();
    if (m_format->findText(format) >= 0)
        m_format->setCurrentText(format);
    m_rxAction->setChecked(m_preferences->value("traffic/rx", true).toBool());
    m_txAction->setChecked(m_preferences->value("traffic/tx", true).toBool());
    m_errorAction->setChecked(m_preferences->value("traffic/errors", true).toBool());
    m_numberAction->setChecked(m_preferences->value("traffic/number", true).toBool());
    m_timestampAction->setChecked(m_preferences->value("traffic/timestamp", true).toBool());
    m_directionAction->setChecked(m_preferences->value("traffic/direction", true).toBool());
    m_lengthAction->setChecked(m_preferences->value("traffic/length", false).toBool());
    m_hexAction->setChecked(m_preferences->value("traffic/hex", false).toBool());
    m_pureAction->setChecked(m_preferences->value("traffic/pure", false).toBool());
    m_wrapAction->setChecked(m_preferences->value("traffic/wrap", false).toBool());
    m_follow->setChecked(m_preferences->value("traffic/follow", true).toBool());
    m_portAButton->setChecked(m_preferences->value("traffic/portA", true).toBool());
    m_portBButton->setChecked(m_preferences->value("traffic/portB", true).toBool());
    m_logPath = m_preferences->value("traffic/recordPath").toString();
    const bool recordOn = m_preferences->value("traffic/record", false).toBool();
    if (recordOn) {
        // 上次开着记录：沿用原文件继续追加；文件不可用则复位勾选（含提示）。
        m_recordAction->setChecked(true);
    }
    m_loading = false;
    updatePortVisibility();
    updateFilter(); // 恢复完成后统一刷新一次过滤
}

void TrafficPanel::savePreferences()
{
    if (!m_preferences || m_loading) return;
    m_preferences->setValue("traffic/format", m_format->currentText());
    m_preferences->setValue("traffic/rx", m_rxAction->isChecked());
    m_preferences->setValue("traffic/tx", m_txAction->isChecked());
    m_preferences->setValue("traffic/errors", m_errorAction->isChecked());
    m_preferences->setValue("traffic/number", m_numberAction->isChecked());
    m_preferences->setValue("traffic/timestamp", m_timestampAction->isChecked());
    m_preferences->setValue("traffic/direction", m_directionAction->isChecked());
    m_preferences->setValue("traffic/length", m_lengthAction->isChecked());
    m_preferences->setValue("traffic/hex", m_hexAction->isChecked());
    m_preferences->setValue("traffic/pure", m_pureAction->isChecked());
    m_preferences->setValue("traffic/wrap", m_wrapAction->isChecked());
    m_preferences->setValue("traffic/follow", m_follow->isChecked());
    m_preferences->setValue("traffic/portA", m_portAButton->isChecked());
    m_preferences->setValue("traffic/portB", m_portBButton->isChecked());
    m_preferences->setValue("traffic/record", m_recordAction->isChecked());
    if (!m_logPath.isEmpty()) m_preferences->setValue("traffic/recordPath", m_logPath);
}

TrafficPanel::~TrafficPanel()
{
    savePreferences();
    closeLogFile();
}

void TrafficPanel::updatePortVisibility()
{
    // A/B 为独立开关：勾选集合即当前显示范围，两个都勾相当于看全部。
    m_visibleEndpoints.clear();
    for (auto *button : {m_portAButton, m_portBButton})
        if (button->isChecked()) m_visibleEndpoints.insert(button->property("endpoint").toUuid());
    updateFilter();
}

void TrafficPanel::updateFilter()
{
    TrafficFilter::Options options;
    options.endpoints = m_visibleEndpoints;
    options.rx = m_rxAction->isChecked();
    options.tx = m_txAction->isChecked();
    options.events = m_errorAction->isChecked();
    options.search = m_search->text();
    // 文本视图的到达时过滤用这一份条件的快照；写入点只有这里。
    m_textOptions = options;
    // 行内容的开关组合同样在到达时生效：这里重建快照，flushPending 直读。
    m_displayOptions.number = m_numberAction->isChecked();
    m_displayOptions.timestamp = m_timestampAction->isChecked();
    m_displayOptions.direction = m_directionAction->isChecked();
    m_displayOptions.length = m_lengthAction->isChecked();
    m_displayOptions.hex = m_hexAction->isChecked();
    m_displayOptions.pure = m_pureAction->isChecked();
    updateSummary();
    savePreferences(); // 过滤开关的汇聚点：任一开关变化即落盘
}

void TrafficPanel::updateSummary()
{
    // 模型是导出用的结构化存储（有上限），屏幕上的历史在文本视图里（上限高得多），
    // 摘要按"会话累计接收条数"播报，不随任何一方的淘汰缩水。
    m_stack->setCurrentWidget(m_totalAccepted == 0 ? static_cast<QWidget *>(m_empty)
                                                   : static_cast<QWidget *>(m_text));
    reportSummary(tr("共 %1 条").arg(m_totalAccepted));
}

void TrafficPanel::reportSummary(const QString &text)
{
    if (text == m_lastSummary) return;
    m_lastSummary = text;
    emit summaryChanged(text);
}

void TrafficPanel::exportVisible()
{
    if (!m_model->rowCount()) { reportSummary(tr("当前没有可导出的记录")); return; }
    const auto path = QFileDialog::getSaveFileName(this, tr("导出记录"), "easyDebug-preview.tsv", tr("制表符文件 (*.tsv)"));
    if (path.isEmpty()) return;
    // 导出列与显示列解耦：导出的是完整的结构化事实。
    // 时间用 ISO 格式带毫秒，方向/边界/来源用稳定名称，HEX 用大写无分隔。
    QByteArray output("# easyDebug UI preview - sample data only\n"
                      "Time\tPort\tPeer\tDirection\tOrigin\tFraming\tState\tBytes\tTruncated\tHEX\n");
    for (int i = 0; i < m_model->rowCount(); ++i) {
        const auto &r = m_model->record(i);
        auto clean = [](QString text) {
            text.replace(QLatin1Char('\t'), QLatin1Char(' '));
            text.replace(QLatin1Char('\n'), QLatin1Char(' '));
            text.replace(QLatin1Char('\r'), QLatin1Char(' '));
            return text;
        };
        output += QString("%1\t%2\t%3\t%4\t%5\t%6\t%7\t%8\t%9\t%10\n")
                      .arg(r.timestamp.toString(Qt::ISODateWithMs),
                           clean(endpointLabel(r.source.endpointIndex, r.source.endpointKind)),
                           clean(r.source.peerAddress), Traffic::directionName(r.direction),
                           Traffic::originName(r.origin), Traffic::framingName(r.framing),
                           Traffic::stateName(r.state), QString::number(r.displayBytes()),
                           r.truncated ? QStringLiteral("yes") : QStringLiteral("no"),
                           clean(PayloadCodec::render(r.raw, PayloadCodec::Format::Hex)))
                      .toUtf8();
    }
    QSaveFile file(path);
    const bool ok = file.open(QIODevice::WriteOnly) && file.write(output) == output.size() && file.commit();
    reportSummary(ok ? tr("记录已导出") : tr("导出失败：%1").arg(file.errorString()));
}
