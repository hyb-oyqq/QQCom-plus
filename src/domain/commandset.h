#ifndef COMMANDSET_H
#define COMMANDSET_H

#include "commanditem.h"
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QUuid>

// 指令集：一组自定义指令的集合，对应一个设备或场景。
// 包含名称、备注和指令列表，可序列化为JSON。
struct CommandSet
{
    QUuid id;                       // 唯一标识
    QString name;                   // 分组名称（如"知舟-转台"）
    QString notes;                  // 备注
    QList<CommandItem> commands;    // 指令列表

    CommandSet() : id(QUuid::createUuid()) {}
    explicit CommandSet(const QString &name) : id(QUuid::createUuid()), name(name) {}

    // 序列化
    QJsonObject toJson() const;
    static CommandSet fromJson(const QJsonObject &json);

    // 验证
    bool isValid() const { return !name.trimmed().isEmpty(); }
};

#endif
