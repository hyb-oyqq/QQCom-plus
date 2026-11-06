#ifndef COMMANDITEM_H
#define COMMANDITEM_H

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QUuid>

// 单条自定义指令：内容、描述、格式、延时、组包选项。
// 纯值对象，无界面依赖，可独立序列化/反序列化。
struct CommandItem
{
    QUuid id;                    // 唯一标识
    QString content;             // 指令内容
    QString description;         // 描述文本（按钮显示）
    bool isHex = false;          // 是否按HEX格式发送
    int delayMs = 50;            // 发送前延时（毫秒）
    QString tail = "无";         // 结尾符："无" / "CR" / "LF" / "CR+LF"
    QString packMode = "无";     // 组包模式："无" / "组包"
    bool enabled = true;         // 是否启用（对应勾选框）

    CommandItem() : id(QUuid::createUuid()) {}

    // 序列化
    QJsonObject toJson() const;
    static CommandItem fromJson(const QJsonObject &json);

    // 验证
    bool isValid() const { return !content.trimmed().isEmpty(); }

    // 结尾符的**原始字节**（\r / \n / \r\n；"无" 时为空）。
    // 它是发送侧唯一该用的形态：结尾符是"字节"，不是"文本"。
    // 早先只有 contentWithTail() 把结尾符当文本拼进内容里，HEX 指令就因此丢掉了
    // 结尾符——"AA 55" + "\r\n" 交给 fromHex 解析时，两个控制字符不是十六进制数字，
    // 会被静默跳过。正确顺序是"先解析内容、再追加结尾符字节"。
    QByteArray tailBytes() const;

    // 按文本形态追加结尾符。仅用于确实需要"一整段文本"的场景（如发送区预填）；
    // 发送路径请用 tailBytes()，否则 HEX 指令会丢结尾符。
    QString contentWithTail() const;

    bool operator==(const CommandItem &other) const
    {
        return id == other.id && content == other.content && description == other.description
            && isHex == other.isHex && delayMs == other.delayMs && tail == other.tail
            && packMode == other.packMode && enabled == other.enabled;
    }
    bool operator!=(const CommandItem &other) const { return !(*this == other); }
};

#endif
