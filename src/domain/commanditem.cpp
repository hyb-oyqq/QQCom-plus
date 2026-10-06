#include "commanditem.h"

QJsonObject CommandItem::toJson() const
{
    QJsonObject json;
    json["id"] = id.toString();
    json["content"] = content;
    json["description"] = description;
    json["isHex"] = isHex;
    json["delayMs"] = delayMs;
    json["tail"] = tail;
    json["packMode"] = packMode;
    json["enabled"] = enabled;
    return json;
}

CommandItem CommandItem::fromJson(const QJsonObject &json)
{
    CommandItem item;
    item.id = QUuid::fromString(json["id"].toString());
    if (item.id.isNull()) item.id = QUuid::createUuid();
    item.content = json["content"].toString();
    item.description = json["description"].toString();
    item.isHex = json["isHex"].toBool();
    item.delayMs = json["delayMs"].toInt(50);
    item.tail = json["tail"].toString("无");
    item.packMode = json["packMode"].toString("无");
    item.enabled = json["enabled"].toBool(true);
    return item;
}

QByteArray CommandItem::tailBytes() const
{
    // 结尾符就是这三个字节序列之一。tail 字段的取值只有这四种，非法值按"无"处理
    // ——发送路径上多一个字节和少一个字节都是协议错误，宁可什么都不加。
    if (tail == QLatin1String("CR")) return QByteArray("\r", 1);
    if (tail == QLatin1String("LF")) return QByteArray("\n", 1);
    if (tail == QLatin1String("CR+LF")) return QByteArray("\r\n", 2);
    return {};
}

QString CommandItem::contentWithTail() const
{
    return content + QString::fromLatin1(tailBytes());
}
