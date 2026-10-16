#include "traffictextview.h"
#include "ui/common/apptheme.h"

#include <QChar>
#include <QColor>
#include <QFontMetrics>
#include <QHelpEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QToolTip>

namespace {

// 行号槽是视口的兄弟，不是正文的一层。滚动时 Qt 只重画视口，
// 把号画进正文的 paintEvent 里，号会停在原地。槽自己接滚动和文档变化。
class LineNumberBar : public QWidget
{
public:
    explicit LineNumberBar(TrafficTextView *view) : QWidget(view), m_view(view) {}

protected:
    void paintEvent(QPaintEvent *event) override { m_view->paintGutter(event); }

    bool event(QEvent *event) override
    {
        if (event->type() != QEvent::ToolTip) return QWidget::event(event);
        auto *help = static_cast<QHelpEvent *>(event);
        const QTextCursor cursor = m_view->cursorForPosition(QPoint(0, help->pos().y()));
        const QString tip = m_view->lineTipAt(cursor.block().blockNumber());
        if (!tip.isEmpty()) {
            QToolTip::showText(help->globalPos(), tip, this);
            return true;
        }
        QToolTip::hideText();
        return true;
    }

private:
    TrafficTextView *m_view;
};

} // namespace

namespace {

// 内存安全阀的取值与理由见头文件（kSafetyBlockBudget / kSafetyCharBudget）。
// 不再设单行载荷字符上限：完整显示是显式需求（HEX 对照尤其需要看到全部字节），
// 内存由这两个总预算兜底——行数上限挡堆行，字符预算挡"行少但极长"的会话，
// 超限都从头部整块淘汰，总内存永远有界。
constexpr int CharTrimReservePercent = 10; // 一次淘汰到预算的 90%，避免逐批都做头部搬移

// 语义配色：与工作台主色（连接按钮 #1D4ED8）同一族的高饱和色，浅底上都够深、够清楚。
// 前缀元信息（行号/时间/长度）与正文同色（#1C2430）——用户钦定：前缀加粗后与正文
// 同色，靠字重区分，不再靠颜色深浅分主次；对比度即正文对比度，比原先的灰前缀更高。
// HEX 对照行保持浅色不动：它是数据行的附注，不是前缀的一部分。
//
// 这些颜色画进 QTextCharFormat，样式表改不了它们。浅色值是界面测试钉死的，不能漂。
// 深色表面（监视区底 #1e2433）上同一套深字会看不见，所以深色主题另取一档亮色。
// 端口身份色不跟着变：A 琥珀、B 青与徽章同一套，深色表也这么定。
QColor MetaColor()
{
    return AppTheme::isDark() ? QColor(0xE8, 0xEC, 0xF1) : QColor(0x1C, 0x24, 0x30);
}
QColor BodyColor() { return MetaColor(); }
QColor HexCompanionColor()
{
    return AppTheme::isDark() ? QColor(0x8B, 0x95, 0xA8) : QColor(0x6B, 0x76, 0x86);
}
QColor TxColor()
{
    return AppTheme::isDark() ? QColor(0x8E, 0xB6, 0xFF) : QColor(0x1D, 0x4E, 0xD8);
}
QColor RxColor()
{
    return AppTheme::isDark() ? QColor(0x34, 0xD3, 0x99) : QColor(0x04, 0x6C, 0x4E);
}
QColor PortAColor() { return QColor(0xB4, 0x53, 0x09); }
QColor PortBColor() { return QColor(0x0E, 0x74, 0x90); }
QColor StateColor()
{
    return AppTheme::isDark() ? QColor(0xC4, 0xB5, 0xFD) : QColor(0x6D, 0x28, 0xD9);
}
QColor WarningColor()
{
    return AppTheme::isDark() ? QColor(0xF6, 0xD7, 0xA8) : QColor(0x92, 0x40, 0x0E);
}
QColor ErrorColor()
{
    return AppTheme::isDark() ? QColor(0xF0, 0xA8, 0xA8) : QColor(0xC6, 0x28, 0x28);
}

QColor portColor(int endpointIndex)
{
    if (endpointIndex == 0) return PortAColor();
    if (endpointIndex == 1) return PortBColor();
    return MetaColor();
}

QString portLetter(int endpointIndex)
{
    if (endpointIndex < 0 || endpointIndex > 25) return {};
    return QString(QChar::fromLatin1('A' + char(endpointIndex)));
}

// 收发箭头只画在数据行上，用 ASCII，13px 下比单字符箭头清楚。
// 发送 -->>，接收 <<--。状态、警告、错误不画箭头，也不补空格。
QString directionArrow(Traffic::Direction direction)
{
    return direction == Traffic::Direction::Transmit
        ? QStringLiteral("-->>")
        : QStringLiteral("<<--");
}

QColor directionColor(Traffic::Direction direction)
{
    switch (direction) {
    case Traffic::Direction::Transmit: return TxColor();
    case Traffic::Direction::Receive: return RxColor();
    case Traffic::Direction::State: return StateColor();
    case Traffic::Direction::Warning: return WarningColor();
    case Traffic::Direction::Error: return ErrorColor();
    }
    return MetaColor();
}

// 正文底色：数据行保持近黑（数据是主体），错误/警告行整行着色——
// 这两类是排障时的第一目标，颜色比文字更快被眼睛抓住。
QColor bodyColorFor(Traffic::Direction direction)
{
    if (direction == Traffic::Direction::Error) return ErrorColor();
    if (direction == Traffic::Direction::Warning) return WarningColor();
    return BodyColor();
}

// 行号顶格左对齐：右对齐补的空格在自动换行下看起来像首行缩进，
// 与不带前缀的续行/HEX 对照行错位。数字位数不同带来的列微差
// 远不如"每一行都从同一列开始"重要。

// ANSI SGR 前景色调色板：浅色背景下可直接阅读的标准 8 色 + 亮色 8 色。
// "白/亮白"映射为浅灰——纯白在浅底上不可见，控制台惯例按可读性处理。
// 深色底上反过来：黑和深灰看不见，白才是正文色。
QColor ansiColor(int index, bool bright)
{
    static const QColor light[8] = {
        QColor(0x3F, 0x4A, 0x5A), QColor(0xC6, 0x28, 0x28), QColor(0x1A, 0x7F, 0x37),
        QColor(0x9A, 0x67, 0x00), QColor(0x1D, 0x4E, 0xD8), QColor(0xA0, 0x20, 0x9E),
        QColor(0x00, 0x85, 0x77), QColor(0x5C, 0x6B, 0x7A)};
    static const QColor lightBright[8] = {
        QColor(0x6B, 0x76, 0x86), QColor(0xE0, 0x52, 0x52), QColor(0x2D, 0xA4, 0x4E),
        QColor(0xB0, 0x88, 0x00), QColor(0x4D, 0x7F, 0xE3), QColor(0xC0, 0x4A, 0xD4),
        QColor(0x00, 0xA0, 0xBB), QColor(0x8A, 0x94, 0xA6)};
    static const QColor dark[8] = {
        QColor(0xD5, 0xDB, 0xE3), QColor(0xF0, 0xA8, 0xA8), QColor(0x6E, 0xE7, 0xB7),
        QColor(0xFB, 0xBF, 0x24), QColor(0x93, 0xC5, 0xFD), QColor(0xE8, 0x79, 0xF9),
        QColor(0x67, 0xE8, 0xF9), QColor(0xE8, 0xEC, 0xF1)};
    static const QColor darkBright[8] = {
        QColor(0x9A, 0xA3, 0xB2), QColor(0xFC, 0xA5, 0xA5), QColor(0x86, 0xEF, 0xAC),
        QColor(0xFD, 0xE6, 0x8A), QColor(0xBF, 0xDB, 0xFE), QColor(0xF0, 0xAB, 0xFC),
        QColor(0xA5, 0xF3, 0xFC), QColor(0xF8, 0xFA, 0xFC)};
    const QColor *table = AppTheme::isDark() ? (bright ? darkBright : dark)
                                             : (bright ? lightBright : light);
    return table[index];
}

// 文本分片：一段文本 + 它的字符格式。ANSI 解析按颜色边界切分载荷，
// 同格式连续文本合并成一片；无 ANSI 的载荷整体是一片（快速路径）。
struct TextRun
{
    QString text;
    QTextCharFormat fmt;
};

// 把载荷切成带格式的分片。支持 SGR 前景色（30-37 / 90-97）、256 色里的标准色、
// 加粗（1）与复位。22 只取消加粗，39 只取消前景色；整段复位才是 0。
// 设备日志常把级别色和普通字重写在同一段里（ESC[36;22m），把 22 当成整段复位
// 会把刚设上的颜色清回底色，整行看起来就像没有颜色。
// 其余序列（背景色、光标控制等）识别到即剥离、不渲染。
// 参数段超过 32 字符或没有终结的 'm'，按普通文本原样保留——被截断的转义序列
// 不该吞掉真实数据。整段没有 ESC 时直接走单分片快速路径，不付解析成本。
QList<TextRun> toRuns(const QString &payload, const QTextCharFormat &base)
{
    if (!payload.contains(QChar(0x1B))) return {{payload, base}};
    QList<TextRun> runs;
    QTextCharFormat fmt = base;
    QString plain;
    plain.reserve(payload.size());
    const auto flush = [&] {
        if (!plain.isEmpty()) {
            runs.append({plain, fmt});
            plain.clear();
        }
    };
    for (int i = 0; i < payload.size();) {
        const bool esc = payload.at(i) == QChar(0x1B) && i + 1 < payload.size()
            && payload.at(i + 1) == QLatin1Char('[');
        int end = -1;
        if (esc) {
            int j = i + 2;
            while (j < payload.size() && j - i < 32
                   && (payload.at(j).isDigit() || payload.at(j) == QLatin1Char(';')))
                ++j;
            if (j < payload.size() && payload.at(j) == QLatin1Char('m')) end = j;
        }
        if (!esc) {
            plain += payload.at(i);
            ++i;
            continue;
        }
        if (end < 0) { // 残缺序列：按字面显示，不吞字节
            plain += payload.at(i);
            ++i;
            continue;
        }
        flush();
        const QString params = payload.mid(i + 2, end - (i + 2));
        const auto codes = params.split(QLatin1Char(';'), Qt::KeepEmptyParts);
        const QStringList applied = codes.isEmpty() ? QStringList{QStringLiteral("0")} : codes;
        for (int c = 0; c < applied.size(); ++c) {
            bool ok = false;
            const int value = applied.at(c).toInt(&ok);
            if (!ok) {
                if (applied.at(c).isEmpty() && applied.size() == 1) fmt = base;
                continue;
            }
            if (value == 0) fmt = base;
            else if (value == 1) fmt.setFontWeight(QFont::Bold);
            else if (value == 22) fmt.setFontWeight(base.fontWeight());
            else if (value == 39) fmt.setForeground(base.foreground());
            else if (value >= 30 && value <= 37) fmt.setForeground(ansiColor(value - 30, false));
            else if (value >= 90 && value <= 97) fmt.setForeground(ansiColor(value - 90, true));
            else if (value == 38 && c + 2 < applied.size()) {
                // 256 色：38;5;n。0–15 落回上面那张 16 色表，其余保持底色。
                bool modeOk = false;
                bool indexOk = false;
                const int mode = applied.at(c + 1).toInt(&modeOk);
                const int index = applied.at(c + 2).toInt(&indexOk);
                if (modeOk && indexOk && mode == 5 && index >= 0 && index <= 15)
                    fmt.setForeground(ansiColor(index % 8, index >= 8));
                c += 2;
            }
        }
        i = end + 1;
    }
    flush();
    return runs;
}

} // namespace

TrafficTextView::TrafficTextView(QWidget *parent) : QPlainTextEdit(parent)
{
    setObjectName(QStringLiteral("trafficText"));
    setFrameShape(QFrame::NoFrame);
    setReadOnly(true);
    setUndoRedoEnabled(false);          // 见头文件注释：撤销栈必须关
    setMaximumBlockCount(m_blockBudget); // 从头部整块淘汰，回滚第 1 条在预算内永远可行
    setTextInteractionFlags(Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
    setCenterOnScroll(false);
    // 初次启动按窗口宽度折行。「设置」里关掉「自动换行」后改为水平滚动。
    setLineWrapMode(QPlainTextEdit::WidgetWidth);
    // 整行等宽，行号、时间、载荷才能上下对齐，看起来才是命令行而不是比例字体的表格。
    // 字体**不在代码里设**：全局 QSS 的 font-family 会覆盖 setFont()（实测），
    // 统一由 QSS 保证——与发送区正文/预览同一套，见用例 hexDisplayFontIsUniform。
    // 行号槽按当前字体的位数留白。空文档也留一列，第一行到达时不必再挤正文。
    m_bar = new LineNumberBar(this);
    connect(this, &QPlainTextEdit::updateRequest, this, &TrafficTextView::updateGutter);
    connect(document(), &QTextDocument::contentsChanged, m_bar, [this] { if (m_bar) m_bar->update(); });
    syncGutter();
}

quint64 TrafficTextView::lineNumberAt(int blockNumber) const
{
    if (blockNumber < 0 || blockNumber >= m_marks.size()) return 0;
    return m_nextNumber + quint64(blockNumber);
}

int TrafficTextView::lineEndpointAt(int blockNumber) const
{
    if (blockNumber < 0 || blockNumber >= m_marks.size()) return -1;
    return m_marks.at(blockNumber).endpoint;
}

bool TrafficTextView::lineMixedAt(int blockNumber) const
{
    if (blockNumber < 0 || blockNumber >= m_marks.size()) return false;
    return m_marks.at(blockNumber).mixed;
}

QString TrafficTextView::lineTipAt(int blockNumber) const
{
    if (blockNumber < 0 || blockNumber >= m_marks.size()) return {};
    return lineTip(m_marks.at(blockNumber));
}

void TrafficTextView::setPortLabels(const QString &portA, const QString &portB)
{
    if (m_portLabels[0] == portA && m_portLabels[1] == portB) return;
    m_portLabels[0] = portA;
    m_portLabels[1] = portB;
    if (m_bar) m_bar->update();
}

void TrafficTextView::setLineWrapping(bool enabled)
{
    // 折行模式下横向滚动条自动消失；切换是一次性重排，只在用户操作时发生。
    setLineWrapMode(enabled ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
}

void TrafficTextView::appendMessages(
    const QList<Traffic::Message> &messages, PayloadCodec::Format format, const DisplayOptions &options)
{
    if (messages.isEmpty()) return;

    // 一次渲染 = 一个编辑块。**这是整个追加路径最重要的一条**：
    // 不在编辑块里的每一次 insertText 都会单独走一整轮"文档变更 → 排版标脏 →
    // 滚动条重算 → 尺寸信号"，实测固定开销约 100 µs/次，而插入本身只要 1~2 µs。
    // 一条消息要做 4~7 次插入，于是 5~7 × 100 µs 全花在这笔固定开销上。
    // 包进编辑块后，整批只在 endEditBlock 时结算一次：实测 103 µs/次 → 1.7 µs/次，
    // 约 62 倍。这条对"重投影全部历史"是决定性的——没有它，重投影只能覆盖几千行。
    QTextCursor cursor(document());
    cursor.movePosition(QTextCursor::End);
    cursor.beginEditBlock();

    QTextCharFormat metaFormat;
    metaFormat.setForeground(MetaColor());
    // 前缀整段 DemiBold（用户钦定）：行号/时间/长度是扫读锚点，加粗后与正文的
    // 分界靠字重而不是颜色深浅（颜色已与正文同色），浅色主题下也一眼可辨。
    metaFormat.setFontWeight(QFont::DemiBold);
    // HEX 对照行：更浅的次要色，与数据行分层。
    QTextCharFormat hexFormat;
    hexFormat.setForeground(HexCompanionColor());

    // 有前缀时，空文档的首行直接写进已有空块，之后每条消息先补一个块分隔符。
    // 纯数据不在消息边界换行：文档里已有内容时，新字节接着上一包写。
    // QChar::ParagraphSeparator 是 QTextDocument 的块分隔符（appendPlainText 内部同款），
    // 用 '\n' 也成块但会在文本里留下歧义字符，显式用分隔符更干净。
    bool needSeparator = options.prefix && !document()->isEmpty();
    // 释放记账的两个起点：写入前的块数，以及本次插入的块分隔符个数。
    const int blocksBefore = document()->blockCount();
    int separators = 0;

    for (const auto &r : messages) {
        // 纯数据只留接收字节。发送、状态、警告、错误仍在模型里，打开前缀后重投影能看见。
        if (!options.prefix && r.direction != Traffic::Direction::Receive)
            continue;
        // 载荷里的换行还原成真实换行：文本框就该按文本本来的样子显示。
        // renderMultiline 让 UTF-8/GBK/ASCII 解码里的 \r \n 保持真实字符，
        // 这里统一成 \n；行号与时间只标首行，续行紧跟其后，
        // 多行报文在控制台里展开成多行物理文本，与 JCom 一致。
        //
        // 状态/警告/错误行的正文是 statusText（传输层给的"打开串口 COM1 失败：拒绝访问"
        // "设备错误：…"这类原因），它们的 raw 本来就是空的。以前这里只渲染 raw，
        // 于是这些行在屏幕上只剩一个前缀、冒号后面什么都没有——端口异常因此
        // "有行没内容"，看起来就像没有任何提示。**只有 raw 为空时**才拿 statusText 顶上：
        // 两类信息都在时（例如测试/转发构造的消息）载荷本身才是用户要看的东西，
        // 不能因为带了原因就把内容吞掉。
        QString payload = (!r.isData() && r.raw.isEmpty() && !r.statusText.isEmpty())
            ? r.statusText
            : PayloadCodec::renderMultiline(r.raw, format);
        if (r.truncated) payload += tr(" …");
        if (r.isFragment() && r.hasMoreFragments) payload += tr(" ⏎");
        payload.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        payload.replace(QLatin1Char('\r'), QLatin1Char('\n'));

        // 正文只用数据自己的颜色。端口色画在左侧行号槽，不再铺到这一行的字上。
        // 错误和警告仍整行着色：那是我们的事件，不是设备日志。
        QTextCharFormat bodyFormat;
        bodyFormat.setForeground(bodyColorFor(r.direction));
        const auto runs = toRuns(payload, bodyFormat);
        const int endpoint = r.source.endpointIndex;
        // 一条物理行记一次端口。新建的行追加，接在上一行后面的纯数据只更新混端口标记。
        bool lineMarked = false;
        const auto noteLine = [&](bool created) {
            if (lineMarked) return;
            lineMarked = true;
            if (created || m_marks.isEmpty()) {
                LineMark mark;
                mark.endpoint = endpoint;
                m_marks.append(mark);
                return;
            }
            LineMark &mark = m_marks.last();
            // 上一包以换行收尾时，新字节落在一个还没字的行上。这一行归新端口，不算混行。
            const bool blank = cursor.block().text().isEmpty();
            if (mark.endpoint < 0 || (blank && !mark.mixed)) mark.endpoint = endpoint;
            else if (endpoint >= 0 && endpoint != mark.endpoint) mark.mixed = true;
        };

        bool atMessageStart = true;
        for (const auto &run : runs) {
            int start = 0;
            for (;;) {
                const int nl = run.text.indexOf(QLatin1Char('\n'), start);
                const QString piece = nl < 0 ? run.text.mid(start)
                                             : run.text.mid(start, nl - start);
                if (needSeparator) {
                    cursor.insertText(QString(QChar::ParagraphSeparator));
                    ++separators;
                    needSeparator = false;
                    lineMarked = false;
                    noteLine(true);
                }
                if (!lineMarked) noteLine(false);
                if (atMessageStart) {
                    atMessageStart = false;
                    // 关掉前缀后只留数据。时间、长度同为元色，拼成一段一次插入：
                    // 插入次数决定追加成本，每条消息多一次文档插入都会直接吃掉刷新预算。
                    // 行号不进正文，画在左侧槽里。端口和方向跟着前缀总开关。
                    // 前缀与数据以冒号收尾分界（冒号随方向色）：
                    // "17:27:40.249 [B] TX+FWD [133B] -->> 127.0.0.1:53327: data"。
                    // 顺序：时间、端口、方向、字节数、箭头、对端、冒号、载荷。
                    // 各段按内容拼接，不补空格对齐。
                    // 方向和来源只用 ASCII，不写中文。
                    // 端口用方括号包住，和方向分色。没有端口的行不留空位。
                    if (options.prefix) {
                        QString metaPrefix;
                        if (options.prefixTimestamp)
                            metaPrefix += r.timestamp.toString(QStringLiteral("HH:mm:ss.zzz "));
                        if (!metaPrefix.isEmpty()) {
                            cursor.setCharFormat(metaFormat);
                            cursor.insertText(metaPrefix);
                        }
                        // 方向是主语义。非自动来源用加号挂在后面：TX+FWD、TX+MAN、TX+TIMER。
                        // 标记与文件日志同一套 ASCII，前缀里不出现中文。
                        const auto name = Traffic::directionName(r.direction);
                        const QString dir = r.isData() && r.origin != Traffic::Origin::Automatic
                            ? QStringLiteral("%1+%2").arg(name, Traffic::originName(r.origin))
                            : name;
                        const QString letter = portLetter(r.source.endpointIndex);
                        if (!letter.isEmpty()) {
                            QTextCharFormat portFormat;
                            portFormat.setForeground(portColor(r.source.endpointIndex));
                            portFormat.setFontWeight(QFont::DemiBold);
                            cursor.setCharFormat(portFormat);
                            // 整段 [A]/[B] 一次插入，颜色断言按片段精确匹配。
                            cursor.insertText(QStringLiteral("[%1] ").arg(letter));
                        }
                        QTextCharFormat dirFormat;
                        dirFormat.setForeground(directionColor(r.direction));
                        // 方向令牌维持现状：分色 + DemiBold——与元信息同级字重，
                        // 前缀是一个整体，字重不齐会显得像两种信息。
                        dirFormat.setFontWeight(QFont::DemiBold);
                        cursor.setCharFormat(dirFormat);
                        // 令牌单独插入：颜色测试按 "RX" 这类子串命中这一段。
                        cursor.insertText(dir);
                        // 字节数紧跟方向令牌，用元信息色，不染成方向色。
                        // STATE/WARN/ERR 没有载荷，不标长度。
                        if (options.prefixLength && r.isData()) {
                            cursor.setCharFormat(metaFormat);
                            cursor.insertText(QStringLiteral(" [%1B]").arg(r.displayBytes()));
                            cursor.setCharFormat(dirFormat);
                        }
                        // 收发都画箭头。有对端时地址跟在箭头后：收是谁发来的，发是写给谁的。
                        // 串口没有对端，箭头仍然留着。状态行不画箭头，也不留空位。
                        if (r.isData()) {
                            if (r.source.hasPeer()) {
                                cursor.insertText(QStringLiteral(" %1 %2")
                                    .arg(directionArrow(r.direction), r.source.peerAddress));
                            } else {
                                cursor.insertText(QStringLiteral(" %1").arg(directionArrow(r.direction)));
                            }
                        }
                        cursor.setCharFormat(dirFormat);
                        cursor.insertText(QStringLiteral(": "));
                    }
                }
                // 每个分片都显式带格式：ANSI 分色片段的边界由这里落进文档。
                cursor.setCharFormat(run.fmt);
                if (!piece.isEmpty()) cursor.insertText(piece);
                if (nl < 0) break;
                needSeparator = true; // 下一物理行另记一个行号
                lineMarked = false;
                start = nl + 1;
            }
        }
        // HEX 对照另起一行：与数据行头尾相接会把两条信息挤在一行，
        // 长报文下根本看不到对照；独占一行才便于上下比对字节。
        // 完整展开全部字节——对照的意义就在于看全，内存由文档总预算兜底。
        // 不受前缀开关控制：关掉行号/时间/方向后，对照行仍然单独跟在数据后面。
        // 纯数据不允许 HEX 对照：对照行自己就要占一行，和“接着输出”冲突。
        if (options.prefix && options.alwaysShowHex && format != PayloadCodec::Format::Hex
            && !r.raw.isEmpty()) {
            const auto hex = QString::fromLatin1(r.raw.toHex(' ')).toUpper();
            if (needSeparator) needSeparator = false; // 载荷以换行收尾：续块此时是空的，行号已经记过
            else {
                cursor.insertText(QString(QChar::ParagraphSeparator));
                ++separators;
                LineMark mark;
                mark.endpoint = endpoint;
                m_marks.append(mark);
            }
            cursor.setCharFormat(hexFormat);
            cursor.insertText(hex);
        }
        // 有前缀时每条另起一行。纯数据时新字节接在上一包后面，
        // 换行只来自载荷里的 0A / 0D 0A。
        needSeparator = options.prefix;
    }
    // 结算这一批的全部文档变更（也是上面那笔固定开销唯一被付掉的地方）。
    cursor.endEditBlock();
    // 字符预算会在 trimHeadBlocks 里先丢掉对应行号。行数预算是 Qt 在编辑块结束时
    // 自己删块，行号还留着。两笔不能各 drop 一次，否则行号会跳、槽和正文对不齐。
    const int marksAfterWrite = m_marks.size();
    enforceCharBudget();
    const int droppedByCharBudget = marksAfterWrite - m_marks.size();
    // 释放记账：本次写入的块数 = 插入的块分隔符数（每个分隔符起一个新块；空文档时
    // 首块复用那个已有的空块，所以这里不加 1）。写入后的块数少于"原有的 + 写入的"，
    // 差额就是从头部被释放掉的行——Qt 的 maximumBlockCount 与上面的字符预算两条
    // 路径都算在内。**必须播报**：这是用户唯一能知道"拖到最前面为什么不全"的出口。
    const int released = blocksBefore + separators - document()->blockCount();
    const int stillHeld = released - droppedByCharBudget;
    if (stillHeld > 0) dropMarks(stillHeld);
    if (released > 0) {
        m_releasedBlocks += released;
        emit blocksReleased(released);
    }
    syncGutter();
    // 不在这里滚动：跟随与否是面板的状态，由面板在追加后统一处理滚动条。
}

void TrafficTextView::trimHeadBlocks(int blocks)
{
    if (blocks <= 0) return;
    QTextCursor trimmer(document());
    trimmer.movePosition(QTextCursor::Start);
    trimmer.movePosition(QTextCursor::NextBlock, QTextCursor::KeepAnchor, blocks);
    trimmer.removeSelectedText();
    dropMarks(blocks);
    // 行号是绝对计数，不因头部淘汰回退。槽上的号跟着最早还在的那一行走。
}

void TrafficTextView::dropMarks(int count)
{
    if (count <= 0) return;
    if (count >= m_marks.size()) m_marks.clear();
    else m_marks.remove(0, count);
    m_nextNumber += quint64(count);
}

void TrafficTextView::setBudget(int blocks, qint64 chars)
{
    m_blockBudget = qMax(1, blocks);
    m_charBudget = qMax<qint64>(1, chars);
    setMaximumBlockCount(m_blockBudget);
    // 收紧到当前文档之下要立刻生效（maximumBlockCount 只保证此后不再超）：
    // "改了上限却没变化"这种静默失效，比上限本身更容易让人误判。
    if (document()->blockCount() > m_blockBudget)
        trimHeadBlocks(document()->blockCount() - m_blockBudget);
    // setMaximumBlockCount 只保证此后不再超。上面那一行负责立刻收紧，
    // 字符预算在 enforceCharBudget 里走同一条淘汰。
    enforceCharBudget();
    syncGutter();
}

void TrafficTextView::enforceCharBudget()
{
    // 行数预算由 maximumBlockCount 兜底；字符预算在这里手动执行。
    // 只有超限时才进这段：预算内的会话一个块都不动。
    if (document()->characterCount() <= m_charBudget) return;
    const qint64 target = m_charBudget * (100 - CharTrimReservePercent) / 100;
    qint64 counted = 0;
    int blocks = 0;
    for (QTextBlock block = document()->firstBlock(); block.isValid()
         && document()->characterCount() - counted > target; block = block.next()) {
        counted += block.length(); // block.length 含块分隔符
        ++blocks;
    }
    trimHeadBlocks(blocks);
    syncGutter();
}

void TrafficTextView::clearAll()
{
    document()->clear(); // maximumBlockCount 是文档属性，clear() 不重置它
    m_marks.clear();
    m_nextNumber = 1;
    m_releasedBlocks = 0; // 清空是新会话：释放计数跟着归零
    syncGutter();
}

void TrafficTextView::scrollToBottom()
{
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}

void TrafficTextView::syncGutter()
{
    // 宽度只看还在屏幕上的行号位数，不看已经淘汰掉的累计。
    // 4 位起跳，避免 999 进到 1000 时正文被挤一下。
    const quint64 last = m_nextNumber + quint64(qMax(0, m_marks.size() - 1));
    int digits = 4;
    for (quint64 probe = 9999; last > probe && digits < 12; probe = probe * 10 + 9)
        ++digits;
    const int width = fontMetrics().horizontalAdvance(QLatin1Char('0')) * digits
        + fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 2 + 8;
    if (width != viewportMargins().left())
        setViewportMargins(width, 0, 0, 0);
    placeGutter();
    if (m_bar) m_bar->update();
}

void TrafficTextView::placeGutter()
{
    if (!m_bar) return;
    const QRect frame = viewport()->geometry();
    m_bar->setGeometry(QRect(frame.left() - viewportMargins().left(), frame.top(),
                             viewportMargins().left(), frame.height()));
}

void TrafficTextView::updateGutter(const QRect &rect, int dy)
{
    Q_UNUSED(rect);
    if (!m_bar) return;
    if (dy != 0) m_bar->scroll(0, dy);
    else m_bar->update();
}

QString TrafficTextView::lineTip(const LineMark &mark) const
{
    QString letter = portLetter(mark.endpoint);
    if (letter.isEmpty()) return {};
    QString tip = letter;
    if (mark.endpoint >= 0 && mark.endpoint < 2 && !m_portLabels[mark.endpoint].isEmpty())
        tip += QStringLiteral(" · ") + m_portLabels[mark.endpoint];
    if (mark.mixed) tip += tr(" · 这一行里还有另一个端口");
    return tip;
}

void TrafficTextView::paintLineNumber(QPainter *painter, const QRect &rect,
    const QTextBlock &block) const
{
    const int index = block.blockNumber();
    if (index < 0 || index >= m_marks.size()) return;
    const LineMark &mark = m_marks.at(index);
    painter->setPen(portColor(mark.endpoint));
    const QString number = QString::number(m_nextNumber + quint64(index));
    const QRect textRect = rect.adjusted(0, 0, -6, 0);
    painter->drawText(textRect, Qt::AlignRight | Qt::AlignVCenter, number);
    if (mark.mixed) {
        // 这一行开头是一个端口，中间又接上了另一个。点画在号的右边，颜色跟后到的那个。
        const QColor other = mark.endpoint == 0 ? PortBColor() : PortAColor();
        painter->setPen(Qt::NoPen);
        painter->setBrush(other);
        const int radius = 3;
        painter->drawEllipse(QPoint(rect.right() - radius, rect.center().y()), radius, radius);
    }
}

void TrafficTextView::paintGutter(QPaintEvent *event)
{
    if (!m_bar) return;
    QPainter painter(m_bar);
    painter.setFont(font());
    painter.fillRect(event->rect(), viewport()->palette().color(QPalette::Base));
    // 只画这次脏区盖住的可见块。滚动由槽自己 scroll，不在这里重扫历史。
    QTextBlock block = firstVisibleBlock();
    int y = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    const int limit = event->rect().bottom();
    while (block.isValid() && y <= limit) {
        const int height = qRound(blockBoundingRect(block).height());
        if (block.isVisible() && y + height >= event->rect().top())
            paintLineNumber(&painter, QRect(0, y, m_bar->width(), height), block);
        y += height;
        block = block.next();
    }
}

void TrafficTextView::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    placeGutter();
}
