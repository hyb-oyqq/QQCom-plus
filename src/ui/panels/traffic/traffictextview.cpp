#include "traffictextview.h"

#include <QChar>
#include <QColor>
#include <QFontDatabase>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>

namespace {

// 文本历史的行数上限。JCom 实测 34 万行仍流畅；QPlainTextEdit 只排版可见块，
// 100 万行的追加与绘制成本同样与总行数无关，剩下的只是文本内存。
// 淘汰由文档自己完成：从头部整块删除，"停止跟随回滚第 1 条"始终成立。
constexpr int BlockBudget = 1000000;
// 单行载荷的字符上限。4KB 报文 HEX 渲染 8192 字符，不设限的话
// 1M 行 × 8KB 字符能把内存推到不可接受。控制台只保留可见的那一份，
// 超出的尾部以省略号示意；字节事实仍在原始记录里，导出与悬停不受影响。
constexpr int MaxPayloadChars = 4096;
// 全文档的总字符预算（UTF-16 下约 512MB 的文本量）。行数上限挡不住
// "行数不多但每行极长"的会话，两个预算先到者为准；超限从头部整块淘汰，
// 与 maximumBlockCount 的淘汰方向一致，行号因此始终连续。
constexpr qint64 CharBudget = 256LL * 1024 * 1024;
constexpr int CharTrimReservePercent = 10; // 一次淘汰到预算的 90%，避免逐批都做头部搬移

const QColor MetaColor(0x5C, 0x6B, 0x7A);
const QColor BodyColor(0x1C, 0x24, 0x30);
const QColor TxColor(0x35, 0x65, 0xce);
const QColor RxColor(0x00, 0x85, 0x77);

// 行号 7 位右对齐 + 空格。超过 9,999,999 行后行号会自然加宽，
// 只影响那一行的列对齐——先到那个量级的是内存预算。
constexpr int NumberColumns = 7;

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

// 行尾 HEX 附注最多展开的字节数：无界展开会让一条 4KB 报文拖出 12K 字符，
// 对照用途看前几十字节足够，更多字节仍以省略号示意。
constexpr int HexTailBytes = 64;

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
    setMaximumBlockCount(BlockBudget);  // 从头部整块淘汰，回滚第 1 条在预算内永远可行
    setTextInteractionFlags(Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
    setCenterOnScroll(false);
    // 文本控制台默认不折行：折行会让"一条消息一行"的行号语义变糊，
    // 长报文水平滚动查看；自动换行是显示设置里的显式开关。
    setLineWrapMode(QPlainTextEdit::NoWrap);
    // 整行等宽，行号、时间、载荷才能上下对齐，看起来才是命令行而不是比例字体的表格。
    QFont console = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    console.setPointSize(10);
    setFont(console);
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

    // 一次游标推进写完整批：movePosition(End) 只做一次，之后逐行顺序插入，
    // 中间不回读文档、不碰视口，插入成本与批内行数线性、与文档总行数无关。
    QTextCursor cursor(document());
    cursor.movePosition(QTextCursor::End);

    QTextCharFormat metaFormat;
    metaFormat.setForeground(MetaColor);
    QTextCharFormat bodyFormat;
    bodyFormat.setForeground(BodyColor);

    // 空文档只有一个空块：首行直接写进它；之后每行先补一个块分隔符。
    // QChar::ParagraphSeparator 是 QTextDocument 的块分隔符（appendPlainText 内部同款），
    // 用 '\n' 也成块但会在文本里留下歧义字符，显式用分隔符更干净。
    bool needSeparator = !document()->isEmpty();

    for (const auto &r : messages) {
        // 载荷里的换行还原成真实换行：文本框就该按文本本来的样子显示。
        // renderMultiline 让 UTF-8/GBK/ASCII 解码里的 \r \n 保持真实字符，
        // 这里统一成 \n；行号与时间只标首行，续行紧跟其后，
        // 多行报文在控制台里展开成多行物理文本，与 JCom 一致。
        QString payload = PayloadCodec::renderMultiline(r.raw, format);
        if (r.truncated) payload += tr(" …");
        if (r.isFragment() && r.hasMoreFragments) payload += tr(" ⏎");
        if (payload.size() > MaxPayloadChars) {
            payload.truncate(MaxPayloadChars);
            payload += tr(" …");
        }
        payload.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        payload.replace(QLatin1Char('\r'), QLatin1Char('\n'));

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
                    needSeparator = false;
                }
                if (atMessageStart) {
                    atMessageStart = false;
                    // 纯数据模式省略一切前缀；其余模式行号恒在，时间/长度按开关拼接。
                    // 行号/时间/长度同为元色，拼成一段一次插入：插入次数决定追加成本，
                    // 每条消息多一次文档插入都会直接吃掉刷新预算。
                    if (!options.pure) {
                        QString metaPrefix =
                            QStringLiteral("%1 ")
                                .arg(QString::number(m_nextNumber).rightJustified(NumberColumns));
                        if (options.timestamp)
                            metaPrefix += r.timestamp.toString(QStringLiteral("HH:mm:ss.zzz "));
                        if (options.length)
                            metaPrefix += QStringLiteral("[%1 B] ").arg(r.displayBytes());
                        if (metaPrefix.size() > NumberColumns + 1) {
                            cursor.setCharFormat(metaFormat);
                            cursor.insertText(metaPrefix);
                        }
                        if (options.direction) {
                            // 方向是主语义；非自动来源（手动/定时/转发/示例）以复合标记
                            // 挂在方向上，与导出的方向列同一套规则。
                            const auto name = Traffic::directionName(r.direction);
                            const QString dir = r.isData() && r.origin != Traffic::Origin::Automatic
                                ? QStringLiteral("%1·%2").arg(name, Traffic::originName(r.origin))
                                : name;
                            QTextCharFormat dirFormat;
                            dirFormat.setForeground(
                                r.direction == Traffic::Direction::Transmit ? TxColor : RxColor);
                            cursor.setCharFormat(dirFormat);
                            cursor.insertText(dir + QLatin1Char(' '));
                        }
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
        // HEX 对照附注：非 HEX 显示格式且开关开启时，行尾以元色补前 64 字节的 HEX。
        if (!options.pure && options.hex && format != PayloadCodec::Format::Hex && !r.raw.isEmpty()) {
            auto hex = QString::fromLatin1(r.raw.left(HexTailBytes).toHex(' ')).toUpper();
            if (r.raw.size() > HexTailBytes) hex += tr(" …");
            cursor.setCharFormat(metaFormat);
            cursor.insertText(QStringLiteral("  HEX ") + hex);
        }
        // 行号按消息计，一条多行报文共用一个行号；空载荷也保留前缀行。
        ++m_nextNumber;
        // 消息之间必然换行：下一条消息从新块开始（旧行为同款）。
        needSeparator = true;
    }
    enforceCharBudget();
    // 不在这里滚动：跟随与否是面板的状态，由面板在追加后统一处理滚动条。
}

void TrafficTextView::enforceCharBudget()
{
    // 行数预算由 maximumBlockCount 兜底；字符预算在这里手动执行。
    // 只有超限时才进这段：预算内的会话一个块都不动。
    if (document()->characterCount() <= CharBudget) return;
    const qint64 target = CharBudget * (100 - CharTrimReservePercent) / 100;
    qint64 counted = 0;
    int blocks = 0;
    for (QTextBlock block = document()->firstBlock(); block.isValid()
         && document()->characterCount() - counted > target; block = block.next()) {
        counted += block.length(); // block.length 含块分隔符
        ++blocks;
    }
    if (blocks <= 0) return;
    QTextCursor trimmer(document());
    trimmer.movePosition(QTextCursor::Start);
    trimmer.movePosition(QTextCursor::NextBlock, QTextCursor::KeepAnchor, blocks);
    trimmer.removeSelectedText();
    // 行号是绝对计数，不因头部淘汰回退；用户若正回滚在头部被淘汰的区域，
    // 视图内容整体上移——这是控制台语义的可接受代价，跟随模式下无感知。
}

void TrafficTextView::clearAll()
{
    document()->clear(); // maximumBlockCount 是文档属性，clear() 不重置它
    m_nextNumber = 1;
}

void TrafficTextView::scrollToBottom()
{
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}
