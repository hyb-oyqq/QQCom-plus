#ifndef TRAFFICTEXTVIEW_H
#define TRAFFICTEXTVIEW_H

#include <QPlainTextEdit>
#include <QVector>

#include "domain/trafficmessage.h"
#include "ui/common/payloadcodec.h"

// 监视区的文本显示层：JCom 式"文本框刷新"。
// 显示前缀打开时，每条消息另起一行，时间/方向/长度按开关拼接。
// 关掉前缀是纯数据：只画接收字节，新数据接在上一包后面，换行只来自载荷自己的
// \r\n / \n / \r；发送、状态、警告、错误和 HEX 对照都不出现。
// 行号画在左侧独立槽里，颜色跟端口：A 琥珀、B 青。正文前景留给数据自己的颜色标识，
// 复制正文不带行号。槽在纯数据和有前缀时都在，不跟「前缀行号」开关。
// 槽只重画可见行，滚动时跟视口一起挪，不在每次追加里重扫整份历史。
//
// 选 QPlainTextEdit 而不是自绘列表：用户要的是"任意选中、随段复制"，
// 这是文本框的语义。QPlainTextEdit 按块（行）存储、只排版可见块，
// 配合三个约束就是高速日志台的标准做法：
//   1. setMaximumBlockCount——文档内存有硬上界，从头部整块淘汰；
//   2. setUndoRedoEnabled(false)——撤销栈会随几十万次追加无限增长，必须关；
//   3. 批量追加——一次游标推进把整批写完，追加成本与消息速率解耦
//      （本项目的攒批管线已在 TrafficPanel::flushPending 16ms 一拍）。
//   3b. **整批包在 beginEditBlock/endEditBlock 里**。不在编辑块中的每一次
//      insertText 都会单独结算一整轮文档变更（排版标脏 + 尺寸信号 +
//      滚动条重算），实测固定开销约 100 µs/次，而插入本身只要 1~2 µs；
//      包进编辑块后 103 µs/次 → 1.7 µs/次（约 62 倍）。这一条是"把条件与
//      显示设置重投影到全部历史"能成立的前提，见 tests/perf/traffic_perf.cpp。
//
// 视图是**投影**：同一批记录可以按不同格式 + 显示开关反复重画。
// 显示设置/编码格式变化由面板触发一次重投影（clearAll + 重新 appendMessages），
// 所以"改开关只影响新行"这个旧约定已经不存在了。
// 含 ANSI 颜色标识（SGR 序列）的载荷按前景色分片渲染，转义序列本身不进显示。
// 22 只取消加粗，39 只取消前景色；整段复位才回到这一行的底色。端口色不进正文。
class TrafficTextView : public QPlainTextEdit
{
    Q_OBJECT
public:
    // 行前缀的开关组合。视图是**投影**：同一批记录可以按不同开关组合反复重画，
    // 所以这些开关不再"只在到达时生效"，而是每行长什么样。
    // prefix 为假时进入纯数据：时间、端口方向、长度、HEX 对照全部不画，
    // 并且只输出接收字节。行号仍在左侧槽。alwaysShowHex 在这时被忽略。
    struct DisplayOptions
    {
        bool prefix = true;            // 前缀总开关：端口、方向、箭头和对端随它一起出现
        bool prefixLineNumber = true;  // 保留字段。行号已改画在左侧槽，不再写入正文
        bool prefixTimestamp = true;   // 行首 HH:mm:ss.zzz
        bool prefixLength = true;      // 方向后面的 [NB]；端口方向关掉时改放在时间后面
        bool alwaysShowHex = false;    // 有前缀时，数据后另起一行显示 HEX 对照（HEX 格式下自动省略）

        // 面板用它判断"外观是否变了、要不要重投影"。写成显式比较而不是依赖
        // 默认的 operator==：C++17 下没有默认比较，且逐字段列出更不容易漏改。
        // 状态/警告/错误不在这里：它们决定哪些行进来，归 TrafficFilter::Options。
        bool operator==(const DisplayOptions &other) const
        {
            return prefix == other.prefix && prefixLineNumber == other.prefixLineNumber
                && prefixTimestamp == other.prefixTimestamp && prefixLength == other.prefixLength
                && alwaysShowHex == other.alwaysShowHex;
        }
        bool operator!=(const DisplayOptions &other) const { return !(*this == other); }
    };

    // 文本历史的**内存安全阀**，不是"能看多久"的策略（用户钦定：像 JCom 一样一直留着）。
    // 早先的 100 万块 / 2.56 亿字符看着很大，但真实纵深由"每条报文渲染出多少行"决定：
    // 一条 442 B、展开成 13 行的报文，100 万块只装得下 7 万多条，压测一小时就摸到顶，
    // 于是"拖到最前面"发现最早的数据没了；JCom 只有一条纯文本缓冲（1.53M 行原样留着），
    // 一对比就看出差别。现在按"几十小时压测也碰不到"来定，碰到就是这台机器内存快满了，
    // 届时必须播报（blocksReleased → 状态栏摘要）。
    static constexpr int kSafetyBlockBudget = 4000000;
    static constexpr qint64 kSafetyCharBudget = 512LL * 1024 * 1024;

    explicit TrafficTextView(QWidget *parent = nullptr);

    // 追加一批消息。载荷按 format 与 options 渲染后写进文档。
    // 这是**唯一**的渲染入口：增量到达与全量重投影走的是同一条路径，
    // 重投影 = clearAll() + 对新一批 appendMessages()，没有第二套渲染逻辑。
    void appendMessages(const QList<Traffic::Message> &messages, PayloadCodec::Format format,
        const DisplayOptions &options);
    void clearAll();
    int lineCount() const { return document()->blockCount(); }
    // 注入上限（默认即内存安全阀，见 .cpp 顶部）。测试用它把预算压到几十行来验证
    // "从头部释放 + 播报"；将来的"内存紧张时收紧"也从这里进。
    void setBudget(int blocks, qint64 chars);
    // 会话内被安全阀从头部释放掉的块（≈行）累计数；clearAll 归零。
    int releasedBlocks() const { return m_releasedBlocks; }
    void scrollToBottom();
    // 自动换行开关：开启时按视口宽度折行（横向滚动条随之消失），关闭时长报文水平滚动。
    void setLineWrapping(bool enabled);
    // 端口名变了就重画行号槽。名字只进悬停，不进正文。
    void setPortLabels(const QString &portA, const QString &portB);
    // 左侧行号槽。块号从 0 起，号是会话里还在的第一行往后数。没有记录时返回 0 / -1。
    quint64 lineNumberAt(int blockNumber) const;
    int lineEndpointAt(int blockNumber) const;
    bool lineMixedAt(int blockNumber) const;
    QString lineTipAt(int blockNumber) const;
    // 行号槽是独立控件，绘制走这里。只画这次脏区盖住的可见行。
    void paintGutter(QPaintEvent *event);

signals:
    // 安全阀触发、从头部整块释放历史时发出（面板据此在状态栏播报）。
    // 淘汰**不许无声**：屏幕上看不到的那几行，可能是用户唯一想回看的东西。
    void blocksReleased(int count);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    // 一条物理行的端口归属。-1 是桥接这类不属于某一端的行。
    // 同一行里混进了第二个端口时 mixed 为真，行号仍跟开头那个端口。
    struct LineMark
    {
        int endpoint = -1;
        bool mixed = false;
    };

    quint64 m_nextNumber = 1; // 下一条的绝对行号，头部淘汰不回退
    int m_releasedBlocks = 0; // 见 releasedBlocks()
    int m_blockBudget = kSafetyBlockBudget;
    qint64 m_charBudget = kSafetyCharBudget;
    QVector<LineMark> m_marks;
    QString m_portLabels[2];
    void trimHeadBlocks(int blocks); // 唯一的头部淘汰动作：行数预算与字符预算都走它
    void enforceCharBudget();
    void syncGutter();
    void placeGutter();
    void updateGutter(const QRect &rect, int dy);
    void dropMarks(int count);
    void paintLineNumber(QPainter *painter, const QRect &rect, const QTextBlock &block) const;
    QString lineTip(const LineMark &mark) const;
    QWidget *m_bar = nullptr;
};

#endif
