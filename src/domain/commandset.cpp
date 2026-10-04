#include "commandset.h"
#include <QJsonArray>

QJsonObject CommandSet::toJson() const
{
    QJsonObject json;
    json["id"] = id.toString();
    json["name"] = name;
    json["notes"] = notes;

    QJsonArray commandsArray;
    for (const auto &cmd : commands) {
        commandsArray.append(cmd.toJson());
    }
    json["commands"] = commandsArray;

    return json;
}

CommandSet CommandSet::fromJson(const QJsonObject &json)
{
    CommandSet set;
    set.id = QUuid::fromString(json["id"].toString());
    if (set.id.isNull()) set.id = QUuid::createUuid();
    set.name = json["name"].toString();
    set.notes = json["notes"].toString();

    QJsonArray commandsArray = json["commands"].toArray();
    for (const auto &cmdValue : commandsArray) {
        set.commands.append(CommandItem::fromJson(cmdValue.toObject()));
    }

    return set;
}
