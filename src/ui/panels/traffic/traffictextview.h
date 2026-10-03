#ifndef TRAFFICTEXTVIEW_H
#define TRAFFICTEXTVIEW_H

#include <QPlainTextEdit>

#include "domain/trafficmessage.h"
#include "ui/common/payloadcodec.h"

// 监视区的文本显示层：JCom 式"文本框刷新"——每条消息按行号起头，时间/方向/长度
// 按显示开关拼接，载荷按文本本来的样子显示：\r\n / \n / \r 还原成真实换行，
// 续行不带行号紧随其后，跟随模式下始终钉在底部。
//
// 选 QPlainTextEdit 而不是自绘列表：用户要的是"任意选中、随段复制"，
// 这是文本框的语义。QPlainTextEdit 按块（行）存储、只排版可见块，
// 配合三个约束就是高速日志台的标准做法：
//   1. setMaximumBlockCount——文档内存有硬上界，从头部整块淘汰；
//   2. setUndoRedoEnabled(false)——撤销栈会随几十万次追加无限增长，必须关；
//   3. 批量追加——一次游标推进把整批写完，追加成本与消息速率解耦
//      （本项目的攒批管线已在 TrafficPanel::flushPending 16ms 一拍）。
//
// 行内容按到达时的显示格式与开关组合渲染后固化（控制台语义）：
// 切换编码格式或显示开关只影响新行，已固化的文本不重排。
// 含 ANSI 颜色标识（SGR 序列）的载荷按前景色分片渲染，转义序列本身不进显示。
class TrafficTextView : public QPlainTextEdit
{
    Q_OBJECT
public:
    // 行前缀的开关组合。全部在到达时生效，不回溯已有行；
    // pure 为真时其余前缀选项被忽略——只显示数据本身。
    struct DisplayOptions
    {
        bool timestamp = true; // 行首 HH:mm:ss.zzz
        bool direction = true; // RX/TX 方向标记（含手动/定时等来源复合标记）
        bool length = false;   // [N B] 真实字节数（不受预览截断影响）
        bool hex = false;      // 行尾附 HEX 对照（HEX 格式下自然省略）
        bool pure = false;     // 纯数据视图：无行号、时间、方向等一切前缀
    };

    explicit TrafficTextView(QWidget *parent = nullptr);

    // 追加一批消息。载荷按 format 与 options 渲染后固化为本行文本，此后不再重渲染。
    void appendMessages(const QList<Traffic::Message> &messages, PayloadCodec::Format format,
        const DisplayOptions &options);
    void clearAll();
    int lineCount() const { return document()->blockCount(); }
    void scrollToBottom();
    // 自动换行开关：开启时按视口宽度折行（横向滚动条随之消失），关闭时长报文水平滚动。
    void setLineWrapping(bool enabled);

private:
    quint64 m_nextNumber = 1; // 下一条的绝对行号，头部淘汰不回退
    void enforceCharBudget();
};

#endif
