#ifndef COMMANDITEM_H
#define COMMANDITEM_H

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
    QString packMode = "无";     // 组包模式："无" / "组包"
    bool enabled = true;         // 是否启用（对应勾选框）

    CommandItem() : id(QUuid::createUuid()) {}

    // 序列化
    QJsonObject toJson() const;
    static CommandItem fromJson(const QJsonObject &json);

    // 验证
    bool isValid() const { return !content.trimmed().isEmpty(); }
};

#endif
