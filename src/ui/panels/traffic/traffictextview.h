#ifndef TRAFFICTEXTVIEW_H
#define TRAFFICTEXTVIEW_H

#include <QPlainTextEdit>

#include "domain/trafficmessage.h"
#include "ui/common/payloadcodec.h"

// 监视区的文本显示层：JCom 式"文本框刷新"——每条消息一行文本
// （行号、时间、方向、载荷），随到达持续追加，跟随模式下始终钉在底部。
//
// 选 QPlainTextEdit 而不是自绘列表：用户要的是"任意选中、随段复制"，
// 这是文本框的语义。QPlainTextEdit 按块（行）存储、只排版可见块，
// 配合三个约束就是高速日志台的标准做法：
//   1. setMaximumBlockCount——文档内存有硬上界，从头部整块淘汰；
//   2. setUndoRedoEnabled(false)——撤销栈会随几十万次追加无限增长，必须关；
//   3. 批量追加——一次游标推进把整批写完，追加成本与消息速率解耦
//      （本项目的攒批管线已在 TrafficPanel::flushPending 16ms 一拍）。
//
// 行内容按到达时的显示格式渲染后固化（控制台语义）：切换编码格式只影响新行。
class TrafficTextView : public QPlainTextEdit
{
    Q_OBJECT
public:
    explicit TrafficTextView(QWidget *parent = nullptr);

    // 追加一批消息。载荷按 format 渲染后固化为本行文本，此后不再重渲染。
    void appendMessages(const QList<Traffic::Message> &messages, PayloadCodec::Format format);
    void clearAll();
    int lineCount() const { return document()->blockCount(); }
    void scrollToBottom();

private:
    quint64 m_nextNumber = 1; // 下一条的绝对行号，头部淘汰不回退
    void enforceCharBudget();
};

#endif
