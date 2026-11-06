#include "commandautomation.h"
#include "capabilitycatalog.h"
#include "app/commandmanager.h"

#include <QJsonArray>
#include <QJsonDocument>

namespace {
QString id(const QUuid &value) { return value.toString(QUuid::WithoutBraces); }

QJsonObject commandJson(const CommandItem &command, int position)
{
    auto result = command.toJson();
    result.insert("id", id(command.id));
    result.insert("position", position);
    result.insert("format", command.isHex ? "hex" : "text");
    result.insert("lineEnding", command.tail == "CR" ? "cr" : command.tail == "LF" ? "lf"
        : command.tail == "CR+LF" ? "crlf" : "none");
    result.insert("packed", command.packMode == QStringLiteral("组包"));
    return result;
}

void patch(CommandItem &command, const QJsonObject &arguments)
{
    if (arguments.contains("content")) command.content = arguments.value("content").toString();
    if (arguments.contains("description")) command.description = arguments.value("description").toString();
    if (arguments.contains("format")) command.isHex = arguments.value("format") == "hex";
    if (arguments.contains("delayMs")) command.delayMs = arguments.value("delayMs").toInt();
    if (arguments.contains("enabled")) command.enabled = arguments.value("enabled").toBool();
    if (arguments.contains("packed")) command.packMode = arguments.value("packed").toBool() ? QStringLiteral("组包") : QStringLiteral("无");
    if (arguments.contains("lineEnding")) {
        const auto ending = arguments.value("lineEnding").toString();
        command.tail = ending == "cr" ? QStringLiteral("CR") : ending == "lf" ? QStringLiteral("LF")
            : ending == "crlf" ? QStringLiteral("CR+LF") : QStringLiteral("无");
    }
}
}

namespace CommandAutomation {
bool isMutation(const QString &tool)
{
    return QStringList{"qqcom_create_command_group", "qqcom_update_command_group", "qqcom_delete_command_group",
        "qqcom_add_command", "qqcom_update_command", "qqcom_delete_command"}.contains(tool);
}

bool handles(const QString &tool) { return tool == "qqcom_get_command_group" || isMutation(tool); }

QJsonObject execute(CommandManager *manager, const QString &tool, const QJsonObject &arguments)
{
    const auto validation = CapabilityCatalog::validateArguments(tool, arguments);
    if (!validation.isEmpty()) return CapabilityCatalog::failure("invalid_arguments", validation);
    if (!handles(tool)) return CapabilityCatalog::failure("unknown_tool", tool);
    const auto sets = manager->allSets();
    if (tool == "qqcom_create_command_group") {
        const auto name = arguments.value("name").toString().trimmed();
        if (name.isEmpty()) return CapabilityCatalog::failure("invalid_arguments", "Group name must not be blank");
        CommandSet group(name);
        group.notes = arguments.value("notes").toString();
        manager->addSet(group);
        return {{"created", true}, {"groupId", id(group.id)}, {"name", group.name}, {"notes", group.notes}};
    }

    const QUuid groupId(arguments.value("groupId").toString());
    if (groupId.isNull()) return CapabilityCatalog::failure("invalid_arguments", "groupId must be a UUID from commands");
    int groupIndex = -1;
    for (int i = 0; i < sets.size(); ++i) {
        if (sets.at(i).id != groupId) continue;
        if (groupIndex >= 0) return CapabilityCatalog::failure("ambiguous_id", "Group UUID is duplicated in stored data");
        groupIndex = i;
    }
    if (groupIndex < 0) return CapabilityCatalog::failure("group_not_found", "No group with this UUID");
    auto group = sets.at(groupIndex);
    if (tool == "qqcom_get_command_group") {
        const int offset = arguments.value("offset").toInt();
        const int limit = arguments.value("limit").toInt(50);
        if (offset > group.commands.size()) return CapabilityCatalog::failure("invalid_arguments", "offset exceeds command count");
        QJsonObject result{{"groupId", id(group.id)}, {"name", group.name}, {"notes", group.notes},
            {"totalCommands", group.commands.size()}, {"offset", offset}};
        // Bound full-content reads, including imported data; never silently trim
        // a payload that the agent may subsequently edit.
        constexpr qsizetype Budget = 2 * 1024 * 1024;
        auto bytes = QJsonDocument(result).toJson(QJsonDocument::Compact).size();
        QJsonArray commands;
        int next = offset;
        while (next < group.commands.size() && commands.size() < limit) {
            const auto row = commandJson(group.commands.at(next), next);
            const auto size = QJsonDocument(row).toJson(QJsonDocument::Compact).size() + 1;
            if (bytes + size > Budget) {
                if (commands.isEmpty()) return CapabilityCatalog::failure("response_too_large", "A stored command or group metadata exceeds the full-read budget");
                break;
            }
            commands.append(row);
            bytes += size;
            ++next;
        }
        if (bytes > Budget) return CapabilityCatalog::failure("response_too_large", "Group metadata exceeds the full-read budget");
        result.insert("commands", commands);
        result.insert("nextOffset", next);
        result.insert("hasMore", next < group.commands.size());
        return result;
    }
    if (tool == "qqcom_delete_command_group") {
        if (sets.size() <= 1) return CapabilityCatalog::failure("last_group", "At least one command group must remain");
        manager->removeSet(groupId);
        return {{"deleted", true}, {"groupId", id(groupId)}};
    }
    if (tool == "qqcom_update_command_group") {
        if (!arguments.contains("name") && !arguments.contains("notes"))
            return CapabilityCatalog::failure("invalid_arguments", "Supply name or notes to update");
        if (arguments.contains("name")) {
            group.name = arguments.value("name").toString().trimmed();
            if (group.name.isEmpty()) return CapabilityCatalog::failure("invalid_arguments", "Group name must not be blank");
        }
        if (arguments.contains("notes")) group.notes = arguments.value("notes").toString();
        manager->updateSet(groupId, group, true);
        return {{"updated", true}, {"groupId", id(groupId)}};
    }

    const bool add = tool == "qqcom_add_command";
    int row = -1;
    if (!add) {
        const QUuid commandId(arguments.value("commandId").toString());
        if (commandId.isNull()) return CapabilityCatalog::failure("invalid_arguments", "commandId must be a UUID from commands");
        for (int i = 0; i < group.commands.size(); ++i) {
            if (group.commands.at(i).id != commandId) continue;
            if (row >= 0) return CapabilityCatalog::failure("ambiguous_id", "Command UUID is duplicated in this group");
            row = i;
        }
        if (row < 0) return CapabilityCatalog::failure("command_not_found", "No command with this UUID in the selected group");
    }
    if (tool == "qqcom_delete_command") {
        const auto commandId = group.commands.at(row).id;
        group.commands.removeAt(row);
        manager->updateSet(groupId, group, true);
        return {{"deleted", true}, {"groupId", id(groupId)}, {"commandId", id(commandId)}};
    }
    if (!add && arguments.size() == 2)
        return CapabilityCatalog::failure("invalid_arguments", "Supply at least one command field or position to update");
    // Position is the zero-based index in the resulting list. Adding at size
    // appends; an update may move only within the existing list.
    const int position = arguments.value("position").toInt(add ? int(group.commands.size()) : row);
    const int maximum = int(group.commands.size()) - (add ? 0 : 1);
    if (position > maximum) return CapabilityCatalog::failure("invalid_arguments", "position is outside the command list");
    CommandItem command = add ? CommandItem() : group.commands.at(row);
    patch(command, arguments);
    if (add) group.commands.insert(position, command);
    else {
        group.commands[row] = command;
        group.commands.move(row, position);
    }
    manager->updateSet(groupId, group, true);
    return {{add ? "created" : "updated", true}, {"groupId", id(groupId)},
        {"commandId", id(command.id)}, {"position", position}};
}
}
