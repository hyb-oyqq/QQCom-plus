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

// 行号 7 位右对齐 + 空格 + 时间 12 字符 + 空格。超过 9,999,999 行后
// 行号会自然加宽，只影响那一行的列对齐——先到那个量级的是内存预算。
constexpr int NumberColumns = 7;

// 载荷里的控制字符会破坏"一条消息一行"的块结构（\r\n 是串口文本的常态），
// 转成可见的转义序列：显示成换行会让人误读行号，字节事实仍在原始记录里。
QString visibleText(QString text)
{
    text.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    text.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    return text;
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
    // 文本控制台不折行：折行会让"一条消息一行"的行号语义变糊，长报文水平滚动查看。
    setLineWrapMode(QPlainTextEdit::NoWrap);
    // 整行等宽，行号、时间、载荷才能上下对齐，看起来才是命令行而不是比例字体的表格。
    QFont console = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    console.setPointSize(10);
    setFont(console);
}

void TrafficTextView::appendMessages(const QList<Traffic::Message> &messages, PayloadCodec::Format format)
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
    bool separate = !document()->isEmpty();

    for (const auto &r : messages) {
        // 方向是主语义；非自动来源（手动/定时/转发/示例）以复合标记挂在方向上，
        // 与表格方向列同一套规则。发送状态属于过程信息，不进固化的控制台行。
        const auto name = Traffic::directionName(r.direction);
        const QString dir = r.isData() && r.origin != Traffic::Origin::Automatic
            ? QStringLiteral("%1·%2").arg(name, Traffic::originName(r.origin))
            : name;

        QString payload = visibleText(PayloadCodec::render(r.raw, format));
        if (r.truncated) payload += tr(" …");
        if (r.isFragment() && r.hasMoreFragments) payload += tr(" ⏎");
        if (payload.size() > MaxPayloadChars) {
            payload.truncate(MaxPayloadChars);
            payload += tr(" …");
        }

        if (separate)
            cursor.insertText(QString(QChar::ParagraphSeparator));
        cursor.setCharFormat(metaFormat);
        cursor.insertText(QStringLiteral("%1 %2 ")
                              .arg(QString::number(m_nextNumber).rightJustified(NumberColumns),
                                   r.timestamp.toString(QStringLiteral("HH:mm:ss.zzz"))));
        // 方向色随消息，与表格的 RX/TX 配色一致，扫描时一眼分出收发。
        QTextCharFormat dirFormat;
        dirFormat.setForeground(r.direction == Traffic::Direction::Transmit ? TxColor : RxColor);
        cursor.setCharFormat(dirFormat);
        cursor.insertText(dir + QLatin1Char(' '));
        cursor.setCharFormat(bodyFormat);
        cursor.insertText(payload);
        separate = true;
        ++m_nextNumber;
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
