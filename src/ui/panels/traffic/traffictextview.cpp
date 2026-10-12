#include "traffictextview.h"

#include <QChar>
#include <QColor>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>

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
const QColor MetaColor(0x1C, 0x24, 0x30);
const QColor BodyColor(0x1C, 0x24, 0x30);
// HEX 对照行的颜色：它是数据行的**附注**而不是数据本身。用比正文和前缀都浅的
// 次要中性色，上下两行一眼就能分出"哪行是主体、哪行是对照"，不必读内容。
const QColor HexCompanionColor(0x6B, 0x76, 0x86);
// 方向令牌：前缀里的语义色，同样按"高对比"选——这几个值在浅底上的对比度都 ≥ 6:1
// （RX 与警告比初版各深了一档，初版在加粗后仍显轻）。改色先看这张表。
const QColor TxColor(0x1D, 0x4E, 0xD8);     // 发送：工作台主蓝
const QColor RxColor(0x04, 0x6C, 0x4E);     // 接收：饱和绿（深一档，加粗后更实）
// 端口字母用独立色，不跟方向色走。A 琥珀、B 青，与端口配置行前的徽章同色；
// 和顶栏过滤按钮的使能黄、RX 绿、TX 蓝错开。桥接等不属于某一端的行不画字母。
const QColor PortAColor(0xB4, 0x53, 0x09);
const QColor PortBColor(0x0E, 0x74, 0x90);
const QColor StateColor(0x6D, 0x28, 0xD9);  // 状态迁移：紫
const QColor WarningColor(0x92, 0x40, 0x0E); // 警告：深琥珀（同上，深一档）
const QColor ErrorColor(0xC6, 0x28, 0x28);  // 错误：饱和红

QColor portColor(int endpointIndex)
{
    if (endpointIndex == 0) return PortAColor;
    if (endpointIndex == 1) return PortBColor;
    return MetaColor;
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
    case Traffic::Direction::Transmit: return TxColor;
    case Traffic::Direction::Receive: return RxColor;
    case Traffic::Direction::State: return StateColor;
    case Traffic::Direction::Warning: return WarningColor;
    case Traffic::Direction::Error: return ErrorColor;
    }
    return MetaColor;
}

// 正文底色：数据行保持近黑（数据是主体），错误/警告行整行着色——
// 这两类是排障时的第一目标，颜色比文字更快被眼睛抓住。
QColor bodyColorFor(Traffic::Direction direction)
{
    if (direction == Traffic::Direction::Error) return ErrorColor;
    if (direction == Traffic::Direction::Warning) return WarningColor;
    return BodyColor;
}

// 行号顶格左对齐：右对齐补的空格在自动换行下看起来像首行缩进，
// 与不带前缀的续行/HEX 对照行错位。数字位数不同带来的列微差
// 远不如"每一行都从同一列开始"重要。

// ANSI SGR 前景色调色板：浅色背景下可直接阅读的标准 8 色 + 亮色 8 色。
// "白/亮白"映射为浅灰——纯白在浅底上不可见，控制台惯例按可读性处理。
const QColor AnsiPalette[8] = {
    QColor(0x3F, 0x4A, 0x5A), QColor(0xC6, 0x28, 0x28), QColor(0x1A, 0x7F, 0x37),
    QColor(0x9A, 0x67, 0x00), QColor(0x1D, 0x4E, 0xD8), QColor(0xA0, 0x20, 0x9E),
    QColor(0x00, 0x85, 0x77), QColor(0x5C, 0x6B, 0x7A)};
const QColor AnsiBrightPalette[8] = {
    QColor(0x6B, 0x76, 0x86), QColor(0xE0, 0x52, 0x52), QColor(0x2D, 0xA4, 0x4E),
    QColor(0xB0, 0x88, 0x00), QColor(0x4D, 0x7F, 0xE3), QColor(0xC0, 0x4A, 0xD4),
    QColor(0x00, 0xA0, 0xBB), QColor(0x8A, 0x94, 0xA6)};

// 文本分片：一段文本 + 它的字符格式。ANSI 解析按颜色边界切分载荷，
// 同格式连续文本合并成一片；无 ANSI 的载荷整体是一片（快速路径）。
struct TextRun
{
    QString text;
    QTextCharFormat fmt;
};

// 把载荷切成带格式的分片。支持 SGR 前景色（30-37 / 90-97）、加粗（1）与
// 复位（0 / 22 / 39）；其余序列（背景色、光标控制等）识别到即剥离、不渲染。
// 参数段超过 24 字符或没有终结的 'm'，按普通文本原样保留——被截断的转义序列
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
            while (j < payload.size() && j - i < 24
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
        auto codes = params.split(QLatin1Char(';'), Qt::SkipEmptyParts);
        if (params.isEmpty()) codes.append(QStringLiteral("0"));
        for (const auto &code : codes) {
            bool ok = false;
            const int value = code.toInt(&ok);
            if (!ok) continue;
            if (value == 0) fmt = base;
            else if (value == 1) fmt.setFontWeight(QFont::Bold);
            else if (value == 22 || value == 39) {
                fmt = base;
            } else if (value >= 30 && value <= 37) fmt.setForeground(AnsiPalette[value - 30]);
            else if (value >= 90 && value <= 97) fmt.setForeground(AnsiBrightPalette[value - 90]);
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
    metaFormat.setForeground(MetaColor);
    // 前缀整段 DemiBold（用户钦定）：行号/时间/长度是扫读锚点，加粗后与正文的
    // 分界靠字重而不是颜色深浅（颜色已与正文同色），浅色主题下也一眼可辨。
    metaFormat.setFontWeight(QFont::DemiBold);
    // HEX 对照行：更浅的次要色，与数据行分层。
    QTextCharFormat hexFormat;
    hexFormat.setForeground(HexCompanionColor);

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

        // 有前缀时正文近黑（错误/警告整行着色），端口色只画在 [A]/[B] 上。
        // 纯数据没有这些标记，整段接收字节用端口色，A、B 混在一段里也能分开。
        QTextCharFormat bodyFormat;
        bodyFormat.setForeground(options.prefix ? bodyColorFor(r.direction)
                                                : portColor(r.source.endpointIndex));
        const auto runs = toRuns(payload, bodyFormat);

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
                }
                if (atMessageStart) {
                    atMessageStart = false;
                    // 关掉前缀后只留数据。行号、时间、长度各自独立，同为元色，拼成一段一次插入：
                    // 插入次数决定追加成本，每条消息多一次文档插入都会直接吃掉刷新预算。
                    // 端口和方向跟着前缀总开关，不再单独成项。
                    // 前缀与数据以冒号收尾分界（冒号随方向色）：
                    // "1328 17:27:40.249 [B] TX+FWD [133B] -->> 127.0.0.1:53327: data"。
                    // 顺序：行号、时间、端口、方向、字节数、箭头、对端、冒号、载荷。
                    // 各段按内容拼接，不补空格对齐。
                    // 方向和来源只用 ASCII，不写中文。
                    // 端口用方括号包住，和方向分色。没有端口的行不留空位。
                    if (options.prefix) {
                        QString metaPrefix;
                        if (options.prefixLineNumber)
                            metaPrefix += QStringLiteral("%1 ").arg(m_nextNumber);
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
                needSeparator = true; // 续行不带行号
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
            if (needSeparator) needSeparator = false; // 载荷以换行收尾：续块此时是空的
            else {
                cursor.insertText(QString(QChar::ParagraphSeparator));
                ++separators;
            }
            cursor.setCharFormat(hexFormat);
            cursor.insertText(hex);
        }
        // 行号按消息计，一条多行报文共用一个行号；空载荷也保留前缀行。
        ++m_nextNumber;
        // 有前缀时每条另起一行。纯数据时新字节接在上一包后面，
        // 换行只来自载荷里的 0A / 0D 0A。
        needSeparator = options.prefix;
    }
    // 结算这一批的全部文档变更（也是上面那笔固定开销唯一被付掉的地方）。
    cursor.endEditBlock();
    enforceCharBudget();
    // 释放记账：本次写入的块数 = 插入的块分隔符数（每个分隔符起一个新块；空文档时
    // 首块复用那个已有的空块，所以这里不加 1）。写入后的块数少于"原有的 + 写入的"，
    // 差额就是从头部被释放掉的行——Qt 的 maximumBlockCount 与上面的字符预算两条
    // 路径都算在内。**必须播报**：这是用户唯一能知道"拖到最前面为什么不全"的出口。
    const int released = blocksBefore + separators - document()->blockCount();
    if (released > 0) {
        m_releasedBlocks += released;
        emit blocksReleased(released);
    }
    // 不在这里滚动：跟随与否是面板的状态，由面板在追加后统一处理滚动条。
}

void TrafficTextView::trimHeadBlocks(int blocks)
{
    if (blocks <= 0) return;
    QTextCursor trimmer(document());
    trimmer.movePosition(QTextCursor::Start);
    trimmer.movePosition(QTextCursor::NextBlock, QTextCursor::KeepAnchor, blocks);
    trimmer.removeSelectedText();
    // 行号是绝对计数，不因头部淘汰回退；用户若正回滚在头部被淘汰的区域，
    // 视图内容整体上移——这是控制台语义的可接受代价，跟随模式下无感知。
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
    enforceCharBudget();
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
}

void TrafficTextView::clearAll()
{
    document()->clear(); // maximumBlockCount 是文档属性，clear() 不重置它
    m_nextNumber = 1;
    m_releasedBlocks = 0; // 清空是新会话：释放计数跟着归零
}

void TrafficTextView::scrollToBottom()
{
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}
