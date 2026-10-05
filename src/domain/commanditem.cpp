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

QString CommandItem::contentWithTail() const
{
    QString result = content;
    if (tail == "CR") {
        result += "\r";
    } else if (tail == "LF") {
        result += "\n";
    } else if (tail == "CR+LF") {
        result += "\r\n";
    }
    return result;
}
