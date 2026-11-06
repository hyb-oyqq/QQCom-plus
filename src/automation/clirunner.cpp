#include "clirunner.h"
#include "localcontrol.h"
#include "capabilitycatalog.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QMap>
#include <QProcess>
#include <QSet>
#include <QTextStream>
#include <QThread>

namespace {
const QMap<QString, QString> Tools{
    {"state", "qqcom_get_state"}, {"ports", "qqcom_list_ports"},
    {"configure", "qqcom_configure_endpoint"}, {"open", "qqcom_open_endpoint"},
    {"close", "qqcom_close_endpoint"}, {"send", "qqcom_send"},
    {"traffic", "qqcom_query_traffic"}, {"ui-text", "qqcom_read_ui_text"},
    {"commands", "qqcom_list_commands"}, {"clear", "qqcom_clear_traffic"},
    {"commands get", "qqcom_get_command_group"},
    {"commands create-group", "qqcom_create_command_group"},
    {"commands update-group", "qqcom_update_command_group"},
    {"commands delete-group", "qqcom_delete_command_group"},
    {"commands add", "qqcom_add_command"}, {"commands update", "qqcom_update_command"},
    {"commands delete", "qqcom_delete_command"}};

const char *Usage =
    "QQCom native JSON CLI\n"
    "  qqcom-cli.cmd describe                      List capabilities and schemas\n"
    "  qqcom-cli.cmd instances                     List live QQCom windows\n"
    "  qqcom-cli.cmd state                         Read the current window\n"
    "  qqcom-cli.cmd ports --refresh               Refresh/list serial ports\n"
    "  qqcom-cli.cmd configure A --type serial --param serial.port=COM3 --param serial.baud=115200\n"
    "  qqcom-cli.cmd open A                        Submit endpoint open\n"
    "  qqcom-cli.cmd send A --format hex --data \"AA 55\"\n"
    "  qqcom-cli.cmd traffic --endpoint A --limit 50\n"
    "  qqcom-cli.cmd ui-text --max-chars 8000\n"
    "  qqcom-cli.cmd commands | close A | clear\n"
    "  qqcom-cli.cmd commands get <group-uuid> [--offset 0 --limit 50]\n"
    "  qqcom-cli.cmd commands create-group --name <name> [--notes <notes>]\n"
    "  qqcom-cli.cmd commands update-group <group-uuid> [--name <name> --notes <notes>]\n"
    "  qqcom-cli.cmd commands delete-group <group-uuid>\n"
    "  qqcom-cli.cmd commands add <group-uuid> --content <payload> [command options]\n"
    "  qqcom-cli.cmd commands update <group-uuid> <command-uuid> [command options]\n"
    "  qqcom-cli.cmd commands delete <group-uuid> <command-uuid>\n"
    "Command options: --description, --format text|hex, --line-ending none|cr|lf|crlf,\n"
    "  --delay <0..10000 ms>, --enabled true|false, --packed true|false, --position <0-based>\n"
    "Updates patch only specified fields; edits never send. At least one group must remain.\n"
    "  qqcom-cli.cmd call <tool-name> --input-file <arguments.json or ->\n"
    "Options: --instance <UUID or PID>, --no-start, --timeout <100..30000 ms>\n"
    "An absent GUI is started automatically. Multiple windows require --instance.\n"
    "Open/close/send report submission, not device acknowledgement.\n";

int output(const QJsonObject &value, int code)
{
    QTextStream stream(stdout);
    stream.setEncoding(QStringConverter::Utf8);
    stream << QJsonDocument(value).toJson(QJsonDocument::Compact) << '\n';
    stream.flush();
    return code;
}
}

namespace Cli {
bool parse(const QStringList &arguments, Command &command, QString &error)
{
    QCommandLineParser parser;
    const QList<QCommandLineOption> options{
        {{"h", "help"}, "Show help"}, {{"v", "version"}, "Show version"},
        {"instance", "Target instance UUID or PID", "id"}, {"no-start", "Do not start a GUI"},
        {"timeout", "Control request timeout in milliseconds", "ms", "5000"},
        {"json", "JSON argument object for call", "json"}, {"input-file", "Read call arguments from JSON file or stdin (-)", "path"},
        {"type", "Port type for configure", "type"}, {"param", "Port parameter key=value; repeatable", "key=value"},
        {"auto-recover", "Automatic recovery true or false", "bool"},
        {"format", "Payload encoding", "format"}, {"data", "Payload", "data"},
        {"line-ending", "none, cr, lf or crlf", "suffix"}, {"refresh", "Request serial scan"},
        {"endpoint", "Query endpoint A or B", "endpoint"}, {"cursor", "History continuation cursor", "cursor"},
        {"limit", "Maximum returned records", "count"}, {"contains", "Literal text/HEX query", "text"},
        {"direction", "RX, TX, I, W, E or S", "direction"}, {"max-chars", "Maximum displayed characters", "count"},
        {"name", "Command group name", "name"}, {"notes", "Command group notes", "notes"},
        {"content", "Stored command payload", "content"}, {"description", "Command button label", "description"},
        {"delay", "Command delay in milliseconds (0..10000)", "ms"},
        {"enabled", "Include command in checked items (true or false)", "bool"},
        {"packed", "Command pack mode (true or false)", "bool"},
        {"position", "Zero-based position in resulting command list", "index"},
        {"offset", "Zero-based full-read offset", "index"}};
    parser.addOptions(options);
    if (!parser.parse(QStringList{"qqcom-cli"} + arguments)) { error = parser.errorText(); return false; }
    auto positional = parser.positionalArguments();
    command = {};
    command.name = parser.isSet("help") || positional.isEmpty() ? "help"
        : parser.isSet("version") ? "version" : positional.first();
    if (parser.isSet("version")) command.name = "version";
    if (command.name == "commands" && positional.size() > 1) {
        if (positional.at(1) != "list") command.name += ' ' + positional.at(1);
        positional.removeAt(1);
    }
    const QSet<QString> special{"help", "version", "describe", "instances", "call"};
    if (!special.contains(command.name) && !Tools.contains(command.name)) {
        error = "Unknown command: " + command.name; return false;
    }
    command.instance = parser.value("instance");
    command.autoStart = !parser.isSet("no-start");
    bool timeoutValid = false;
    command.timeoutMs = parser.value("timeout").toInt(&timeoutValid);
    if (!timeoutValid || command.timeoutMs < 100 || command.timeoutMs > 30000) {
        error = "--timeout must be an integer from 100 to 30000"; return false;
    }
    QSet<QString> allowed{"h", "help", "v", "version", "instance", "no-start", "timeout"};
    if (command.name == "help" || command.name == "version") return true;
    const bool endpointCommand = QSet<QString>{"configure", "open", "close", "send"}.contains(command.name);
    const bool commandOperation = command.name.startsWith("commands ");
    const bool commandPair = command.name == "commands update" || command.name == "commands delete";
    const int expected = commandPair ? 3 : endpointCommand || command.name == "call"
        || (commandOperation && command.name != "commands create-group") ? 2 : 1;
    if (positional.size() != expected) { error = "Unexpected or missing positional arguments; run --help"; return false; }
    if (endpointCommand) command.arguments.insert("endpoint", positional.at(1));
    if (commandOperation) {
        if (positional.size() >= 2) command.arguments.insert("groupId", positional.at(1));
        if (commandPair) command.arguments.insert("commandId", positional.at(2));
        if (command.name == "commands create-group" || command.name == "commands update-group") {
            allowed.unite({"name", "notes"});
            for (const auto &key : {"name", "notes"})
                if (parser.isSet(key)) command.arguments.insert(key, parser.value(key));
        } else if (command.name == "commands add" || command.name == "commands update") {
            allowed.unite({"content", "description", "format", "line-ending", "delay", "enabled", "packed", "position"});
            for (const auto &key : {"content", "description", "format"})
                if (parser.isSet(key)) command.arguments.insert(key, parser.value(key));
            if (parser.isSet("line-ending")) command.arguments.insert("lineEnding", parser.value("line-ending"));
        } else if (command.name == "commands get") allowed.unite({"offset", "limit"});
        for (const auto &key : {"enabled", "packed"}) {
            if (!allowed.contains(key) || !parser.isSet(key)) continue;
            const auto value = parser.value(key);
            if (value != "true" && value != "false") { error = QString("--") + key + " expects true or false"; return false; }
            command.arguments.insert(key, value == "true");
        }
        for (const auto &key : {"delay", "position", "offset", "limit"}) {
            if (!allowed.contains(key) || !parser.isSet(key)) continue;
            bool ok = false;
            const auto value = parser.value(key).toInt(&ok);
            if (!ok) { error = QString("--") + key + " expects an integer"; return false; }
            command.arguments.insert(QString(key) == "delay" ? "delayMs" : key, value);
        }
    } else if (command.name == "configure") {
        allowed.unite({"type", "param", "auto-recover"});
        if (parser.isSet("type")) command.arguments.insert("type", parser.value("type"));
        QJsonObject parameters;
        for (const auto &assignment : parser.values("param")) {
            const auto separator = assignment.indexOf('=');
            if (separator <= 0) { error = "--param expects key=value"; return false; }
            const auto key = assignment.left(separator);
            const auto text = assignment.mid(separator + 1);
            QJsonObject schema;
            for (const auto &type : CapabilityCatalog::capabilities().value("portTypes").toArray()) {
                const auto properties = type.toObject().value("parameters").toObject().value("properties").toObject();
                if (properties.contains(key)) schema = properties.value(key).toObject();
            }
            if (schema.isEmpty()) { error = "Unknown port parameter: " + key; return false; }
            if (schema.value("type") == "integer") {
                bool ok = false;
                const auto value = text.toLongLong(&ok);
                if (!ok) { error = key + " requires an integer"; return false; }
                parameters.insert(key, value);
            } else parameters.insert(key, text);
        }
        if (!parameters.isEmpty()) command.arguments.insert("parameters", parameters);
        if (parser.isSet("auto-recover")) {
            const auto value = parser.value("auto-recover");
            if (value != "true" && value != "false") { error = "--auto-recover expects true or false"; return false; }
            command.arguments.insert("autoRecover", value == "true");
        }
    } else if (command.name == "send") {
        allowed.unite({"format", "data", "line-ending"});
        for (const auto &key : {"format", "data"})
            if (parser.isSet(key)) command.arguments.insert(key, parser.value(key));
        if (parser.isSet("line-ending")) command.arguments.insert("lineEnding", parser.value("line-ending"));
    } else if (command.name == "traffic") {
        allowed.unite({"endpoint", "cursor", "limit", "contains", "direction"});
        for (const auto &key : {"endpoint", "cursor", "contains", "direction"})
            if (parser.isSet(key)) command.arguments.insert(key, parser.value(key));
        if (parser.isSet("limit")) {
            bool ok = false;
            const auto value = parser.value("limit").toInt(&ok);
            if (!ok) { error = "--limit expects an integer"; return false; }
            command.arguments.insert("limit", value);
        }
    } else if (command.name == "ui-text") {
        allowed.insert("max-chars");
        if (parser.isSet("max-chars")) {
            bool ok = false;
            const auto value = parser.value("max-chars").toInt(&ok);
            if (!ok) { error = "--max-chars expects an integer"; return false; }
            command.arguments.insert("maxChars", value);
        }
    } else if (command.name == "ports") {
        allowed.insert("refresh");
        if (parser.isSet("refresh")) command.arguments.insert("refresh", true);
    } else if (command.name == "call") {
        allowed.unite({"json", "input-file"});
        command.tool = positional.at(1);
        if (parser.isSet("json") && parser.isSet("input-file")) { error = "Choose --json or --input-file"; return false; }
        QByteArray data = parser.value("json").toUtf8();
        if (parser.isSet("input-file")) {
            const auto path = parser.value("input-file");
            QFile file;
            if (path == "-") {
                if (!file.open(stdin, QIODevice::ReadOnly)) { error = "Cannot read stdin"; return false; }
            } else {
                file.setFileName(path);
                if (!file.open(QIODevice::ReadOnly)) { error = file.errorString(); return false; }
            }
            data = file.read(1024 * 1024 + 1);
        }
        if (data.size() > 1024 * 1024) { error = "JSON arguments exceed 1 MiB"; return false; }
        if (!data.isEmpty()) {
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(data, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                error = "Expected a JSON argument object"; return false;
            }
            command.arguments = document.object();
        }
    }
    for (const auto &option : parser.optionNames())
        if (!allowed.contains(option)) { error = "Option --" + option + " is not valid for " + command.name; return false; }
    if (command.tool.isEmpty()) command.tool = Tools.value(command.name);
    if (!command.tool.isEmpty()) {
        error = CapabilityCatalog::validateArguments(command.tool, command.arguments);
        if (!error.isEmpty()) return false;
    }
    return true;
}

int run(const QStringList &arguments, const QString &version)
{
    Command command;
    QString error;
    if (!parse(arguments, command, error)) return output(CapabilityCatalog::failure("usage_error", error), 2);
    if (command.name == "help") { QTextStream(stdout) << Usage; return 0; }
    if (command.name == "version") return output({{"application", "QQCom"}, {"version", version}}, 0);
    if (command.name == "describe") {
        auto description = CapabilityCatalog::capabilities();
        QJsonObject commands;
        for (auto it = Tools.begin(); it != Tools.end(); ++it) commands.insert(it.key(), it.value());
        description.insert("version", version);
        description.insert("cli", QJsonObject{{"commands", commands}, {"discoveryDirectory", LocalControl::directory()},
            {"transport", "local-pipe"}, {"autoStart", true}, {"clientConfigurationRequired", false},
            {"executable", QCoreApplication::applicationFilePath()}, {"argumentPrefix", QJsonArray{"--cli"}},
            {"advancedCall", "call <tool> --input-file <arguments.json or ->"}});
        return output(description, 0);
    }
    auto instances = LocalControl::instances({}, command.instance);
    if (command.name == "instances") return output({{"instances", instances}}, 0);
    if (instances.isEmpty() && command.autoStart && command.instance.isEmpty()) {
        QProcess launcher;
        launcher.setProgram(QCoreApplication::applicationFilePath());
        launcher.setStandardInputFile(QProcess::nullDevice());
        launcher.setStandardOutputFile(QProcess::nullDevice());
        launcher.setStandardErrorFile(QProcess::nullDevice());
        if (!launcher.startDetached())
            return output(CapabilityCatalog::failure("start_failed", "Cannot start the QQCom window"), 3);
        QElapsedTimer clock;
        clock.start();
        while (instances.isEmpty() && clock.elapsed() < 10000) {
            QThread::msleep(100);
            instances = LocalControl::instances();
        }
    }
    if (instances.isEmpty()) return output(CapabilityCatalog::failure("no_instance", "No live QQCom window found"), 3);
    if (instances.size() != 1) {
        auto result = CapabilityCatalog::failure("multiple_instances", "Choose a window using --instance <UUID or PID>");
        result.insert("instances", instances);
        return output(result, 4);
    }
    auto result = LocalControl::call(instances.first().toObject(), command.tool, command.arguments, command.timeoutMs);
    const bool failed = result.contains("error");
    result.insert("instanceId", instances.first().toObject().value("instanceId"));
    result.insert("pid", instances.first().toObject().value("pid"));
    return output(result, failed ? 5 : 0);
}
}
