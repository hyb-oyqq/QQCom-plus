#include "commandformats.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStringDecoder>
#include <QStringList>

namespace {

// "5A A5 04" → 原始字节。容忍无分隔写法；非法字符直接跳过，不做严格校验——
// 外部文件里偶尔有手写脏数据，宁可少一个字节也不要整条指令导入失败。
QByteArray hexTextToBytes(const QString &text)
{
    QByteArray bytes;
    QString digits;
    digits.reserve(2);
    for (const QChar ch : text) {
        if (ch.isSpace()) continue;
        digits.append(ch);
        if (digits.size() < 2) continue;
        bool ok = false;
        const int value = digits.toInt(&ok, 16);
        if (ok) bytes.append(static_cast<char>(value));
        digits.clear();
    }
    return bytes;
}

// 原始字节 → "5A A5 04"（大写、空格分隔）。与表格里 content 的书写法保持一致，
// 这样导入进来的 HEX 指令用户能直接在发送框里看懂、改得动。
QString bytesToHexText(const QByteArray &bytes)
{
    return QString::fromLatin1(bytes.toHex(' ').toUpper());
}

QString isoUtcNow()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

// ── JCom ────────────────────────────────────────────────────────────────
// 顶层是数组，每个元素一个分组；只取 InitSendList（面板已无周期发送），
// CycleSendList / RecvHexList / RecvTxtList 在导入时忽略、导出时输出空数组，
// 保证 JCom 自己能原样打开。

QList<CommandSet> parseJCom(const QJsonArray &groups)
{
    QList<CommandSet> sets;
    for (const auto &groupValue : groups) {
        const QJsonObject group = groupValue.toObject();

        CommandSet set;
        set.name = group.value(QStringLiteral("GroupName")).toString();
        if (set.name.trimmed().isEmpty()) set.name = QStringLiteral("未命名分组");

        const QJsonArray items = group.value(QStringLiteral("InitSendList")).toArray();
        for (const auto &itemValue : items) {
            const QJsonObject item = itemValue.toObject();
            CommandItem cmd;
            cmd.description = item.value(QStringLiteral("PacketName")).toString();
            cmd.enabled = item.value(QStringLiteral("Checked")).toBool(true);
            cmd.delayMs = item.value(QStringLiteral("Delay")).toInt(50);
            cmd.isHex = item.value(QStringLiteral("HexMode")).toBool(false);
            if (cmd.isHex) {
                // JCom 的 HEX 内容是 base64 编码的原始字节（如 "WqUEACYgBAAcAAAAH18="）
                const QByteArray raw = QByteArray::fromBase64(
                    item.value(QStringLiteral("HexByteData")).toString().toUtf8());
                cmd.content = bytesToHexText(raw);
            } else {
                cmd.content = item.value(QStringLiteral("TxtData")).toString();
            }
            // TailType 与结尾符取值一一对应（0=无 1=CR+LF 2=CR 3=LF）
            // 实测样本分布 {0:82, 1:21, 2:1}，未覆盖 3，3=LF 由下拉顺序推断；未知值回退"无"
            int tailType = item.value(QStringLiteral("TailType")).toInt(0);
            switch (tailType) {
                case 1: cmd.tail = "CR+LF"; break;
                case 2: cmd.tail = "CR"; break;
                case 3: cmd.tail = "LF"; break;
                default: cmd.tail = "无"; break;
            }
            set.commands.append(cmd);
        }
        sets.append(set);
    }
    return sets;
}

QJsonArray toJCom(const QList<CommandSet> &sets)
{
    QJsonArray groups;
    for (const auto &set : sets) {
        QJsonArray initList;
        for (int i = 0; i < set.commands.size(); ++i) {
            const CommandItem &cmd = set.commands.at(i);
            QJsonObject item;
            item[QStringLiteral("hexFieldContent")] = QJsonArray();
            item[QStringLiteral("Number")] = i; // 行号冗余，JCom 自己会重排
            item[QStringLiteral("Checked")] = cmd.enabled;
            item[QStringLiteral("Delay")] = cmd.delayMs;
            item[QStringLiteral("PacketName")] = cmd.description;
            item[QStringLiteral("HexMode")] = cmd.isHex;
            item[QStringLiteral("HexByteData")] = cmd.isHex
                ? QString::fromUtf8(hexTextToBytes(cmd.content).toBase64())
                : QString();
            item[QStringLiteral("Enable")] = false;  // 触发/使能语义 JCom 独有，导出置 false
            item[QStringLiteral("Trigger")] = false;
            item[QStringLiteral("TxtData")] = cmd.isHex ? QString() : cmd.content;
            // 将字符串 tail 转换为 TailType 整数
            int tailType = 0;
            if (cmd.tail == "CR+LF") tailType = 1;
            else if (cmd.tail == "CR") tailType = 2;
            else if (cmd.tail == "LF") tailType = 3;
            item[QStringLiteral("TailType")] = tailType;
            initList.append(item);
        }
        QJsonObject group;
        group[QStringLiteral("GroupName")] = set.name;
        group[QStringLiteral("InitSendList")] = initList;
        group[QStringLiteral("CycleSendList")] = QJsonArray();
        group[QStringLiteral("RecvHexList")] = QJsonArray();
        group[QStringLiteral("RecvTxtList")] = QJsonArray();
        groups.append(group);
    }
    return groups;
}

// ── BLEDebugger ─────────────────────────────────────────────────────────
// 顶层是对象，groups[] 是分组名列表、items[] 靠 group 字段归属。
// 没有延时和勾选概念：导入一律 enabled=true、delayMs=50。

QString bleEndingToTail(const QString &ending)
{
    const QString key = ending.trimmed().toLower();
    // 实测样本出现 none / lf / crlf；"cr" 未出现，按与 JCom 一致的惯例映射 CR。
    if (key == QStringLiteral("crlf")) return "CR+LF";
    if (key == QStringLiteral("cr"))   return "CR";
    if (key == QStringLiteral("lf"))   return "LF";
    return "无"; // "none" 与未知值
}

QString tailToBleEnding(const QString &tail)
{
    if (tail == "CR+LF") return QStringLiteral("crlf");
    if (tail == "CR")    return QStringLiteral("cr");
    if (tail == "LF")    return QStringLiteral("lf");
    return QStringLiteral("none");
}

QList<CommandSet> parseBle(const QJsonObject &root)
{
    QList<CommandSet> sets;
    QHash<QString, int> indexByName;

    // groups[] 显式列出分组名，即使一条指令都没有也要建出来——用户看得见这个分组。
    for (const auto &value : root.value(QStringLiteral("groups")).toArray()) {
        const QString name = value.toString();
        if (name.isEmpty() || indexByName.contains(name)) continue;
        CommandSet set;
        set.name = name;
        indexByName.insert(name, sets.size());
        sets.append(set);
    }

    for (const auto &value : root.value(QStringLiteral("items")).toArray()) {
        const QJsonObject item = value.toObject();

        CommandItem cmd;
        cmd.description = item.value(QStringLiteral("name")).toString();
        cmd.content = item.value(QStringLiteral("data")).toString();
        // encoding: ASCII → 文本；HEX → data 直接就是空格分隔大写 HEX
        cmd.isHex = item.value(QStringLiteral("encoding")).toString().compare(
                        QStringLiteral("HEX"), Qt::CaseInsensitive) == 0;
        cmd.tail = bleEndingToTail(item.value(QStringLiteral("endingData")).toString());
        const QUuid id = QUuid::fromString(item.value(QStringLiteral("id")).toString());
        cmd.id = id.isNull() ? QUuid::createUuid() : id;
        cmd.enabled = true;  // blemsg 无勾选概念
        cmd.delayMs = 50;    // blemsg 无延时概念

        QString groupName = item.value(QStringLiteral("group")).toString();
        if (groupName.isEmpty()) groupName = QStringLiteral("默认指令集");
        auto it = indexByName.find(groupName);
        if (it == indexByName.end()) {
            // groups[] 里没列出、但 items 里引用了：补建一个分组，别把指令丢掉
            CommandSet set;
            set.name = groupName;
            indexByName.insert(groupName, sets.size());
            sets.append(set);
            it = indexByName.find(groupName);
        }
        sets[it.value()].commands.append(cmd);
    }
    return sets;
}

QJsonObject toBle(const QList<CommandSet> &sets)
{
    const QString now = isoUtcNow();
    QJsonArray groups;
    QJsonArray items;

    for (const auto &set : sets) {
        groups.append(set.name);
        for (const auto &cmd : set.commands) {
            QJsonObject item;
            item[QStringLiteral("id")] = cmd.id.toString(QUuid::WithoutBraces);
            // blemsg 的 name 就是指令名：没起名字的用内容兜底，避免导出一堆空名称
            item[QStringLiteral("name")] = cmd.description.isEmpty() ? cmd.content : cmd.description;
            item[QStringLiteral("group")] = set.name;
            item[QStringLiteral("data")] = cmd.content;
            item[QStringLiteral("encoding")] = cmd.isHex ? QStringLiteral("HEX")
                                                        : QStringLiteral("ASCII");
            item[QStringLiteral("endingData")] = tailToBleEnding(cmd.tail);
            item[QStringLiteral("createdAt")] = now;
            items.append(item);
        }
    }

    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("exportedAt")] = now;
    root[QStringLiteral("groups")] = groups;
    root[QStringLiteral("items")] = items;
    return root;
}

// ── QQCom 自有格式 ──────────────────────────────────────────────────

QList<CommandSet> parseQQCom(const QJsonObject &root)
{
    QList<CommandSet> sets;
    const QJsonArray array = root.value(QStringLiteral("commandSets")).toArray();
    for (const auto &value : array) {
        sets.append(CommandSet::fromJson(value.toObject()));
    }
    return sets;
}

QJsonObject toQQCom(const QList<CommandSet> &sets)
{
    QJsonArray array;
    for (const auto &set : sets) {
        array.append(set.toJson());
    }
    QJsonObject root;
    root[QStringLiteral("version")] = QStringLiteral("1.0");
    root[QStringLiteral("commandSets")] = array;
    return root;
}

// ── VOFA+ ───────────────────────────────────────────────────────────────
// 文件是一棵命令树：{"type":"cmds","ctx":{name,is_group,subCmds:[...]}}。
// ctx 本身只是外壳（样本里名字就叫 "group"），不建分组。
// 第一级 subCmds 里的分组 → 一个 CommandSet，名字用这一级的 name。
// 再往下的分组不再建分组：名字直接填进发送框，默认不勾选。勾上或点发送就按普通文本发出去。
// 叶子才是原来的指令。hex_on 为真时 cmd_hex 就是要发的字节；为假时 cmd_hex
// 仍是十六进制，但是文本的字节（如 "2B 43 52 ..." = "+CR,RESET;"）。
// loop_ms / loop_count 是 VOFA 自己的重复发送，不映射到延时。

bool vofaNodeIsGroup(const QJsonObject &node)
{
    if (node.value(QStringLiteral("is_group")).toBool(false)) return true;
    return !node.value(QStringLiteral("subCmds")).toArray().isEmpty();
}

QString vofaTextFromHex(const QString &hex)
{
    const QByteArray raw = hexTextToBytes(hex);
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString text = decoder.decode(raw);
    // 文本指令偶发不是 UTF-8。解码失败时按字节原样留下，避免整条丢成空。
    return decoder.hasError() ? QString::fromLatin1(raw) : text;
}

CommandItem vofaTitleItem(const QString &name)
{
    CommandItem cmd;
    const QString trimmed = name.trimmed().isEmpty()
        ? QStringLiteral("未命名") : name.trimmed();
    cmd.description = trimmed;
    cmd.content = trimmed; // 发送框里就是分组名，不另做“禁止发送”标记
    cmd.enabled = false;
    cmd.isHex = false;
    cmd.tail = QStringLiteral("无");
    return cmd;
}

CommandItem vofaLeafItem(const QJsonObject &node)
{
    CommandItem cmd;
    QString name = node.value(QStringLiteral("name")).toString().trimmed();
    if (name.isEmpty()) name = node.value(QStringLiteral("intro")).toString().trimmed();
    cmd.description = name;
    const bool hexOn = node.value(QStringLiteral("hex_on")).toBool(false);
    const QString hex = node.value(QStringLiteral("cmd_hex")).toString();
    cmd.isHex = hexOn;
    cmd.content = hexOn ? bytesToHexText(hexTextToBytes(hex)) : vofaTextFromHex(hex);
    cmd.tail = QStringLiteral("无");
    cmd.delayMs = 50;
    // 没有正文时，名字本身填进发送框，默认不勾选。有正文的叶子照常勾选。
    if (cmd.content.trimmed().isEmpty()) {
        if (!cmd.description.trimmed().isEmpty()) cmd.content = cmd.description;
        cmd.enabled = false;
    } else {
        cmd.enabled = true;
    }
    return cmd;
}

void appendVofaNodes(CommandSet &set, const QJsonArray &nodes)
{
    for (const auto &value : nodes) {
        const QJsonObject node = value.toObject();
        if (vofaNodeIsGroup(node)) {
            set.commands.append(vofaTitleItem(node.value(QStringLiteral("name")).toString()));
            appendVofaNodes(set, node.value(QStringLiteral("subCmds")).toArray());
            continue;
        }
        const CommandItem cmd = vofaLeafItem(node);
        if (cmd.description.trimmed().isEmpty() && cmd.content.trimmed().isEmpty()) continue;
        set.commands.append(cmd);
    }
}

QString vofaLooseSetName(const QJsonObject &ctx)
{
    // 外壳名字 "group" 不是用户起的分组名。第一级若直接是叶子，归到这个兜底分组。
    const QString name = ctx.value(QStringLiteral("name")).toString().trimmed();
    if (name.isEmpty() || name.compare(QLatin1String("group"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("未命名分组");
    return name;
}

QList<CommandSet> parseVofa(const QJsonObject &root)
{
    const QJsonObject ctx = root.value(QStringLiteral("ctx")).toObject();
    QList<CommandSet> sets;
    CommandSet loose;
    bool looseOpen = false;

    const auto flushLoose = [&]() {
        if (!looseOpen || loose.commands.isEmpty()) {
            loose = CommandSet();
            looseOpen = false;
            return;
        }
        sets.append(loose);
        loose = CommandSet();
        looseOpen = false;
    };

    for (const auto &value : ctx.value(QStringLiteral("subCmds")).toArray()) {
        const QJsonObject node = value.toObject();
        if (vofaNodeIsGroup(node)) {
            flushLoose();
            CommandSet set;
            const QString name = node.value(QStringLiteral("name")).toString().trimmed();
            set.name = name.isEmpty() ? QStringLiteral("未命名分组") : name;
            appendVofaNodes(set, node.value(QStringLiteral("subCmds")).toArray());
            sets.append(set);
            continue;
        }
        const CommandItem cmd = vofaLeafItem(node);
        if (cmd.description.trimmed().isEmpty() && cmd.content.trimmed().isEmpty()) continue;
        if (!looseOpen) {
            loose.name = vofaLooseSetName(ctx);
            looseOpen = true;
        }
        loose.commands.append(cmd);
    }
    flushLoose();
    return sets;
}

} // namespace

namespace CommandFormats {

QList<Format> allFormats()
{
    // VOFA+ 只进不导出：树被展平后写不回去，也不能让格式轮转测试去导出它。
    return {Format::QQCom, Format::JCom, Format::BleDebugger};
}

QString label(Format format)
{
    switch (format) {
    case Format::QQCom:   return QCoreApplication::translate("CommandFormats", "QQCom (*.json)");
    case Format::JCom:        return QCoreApplication::translate("CommandFormats", "JCom (*.port)");
    case Format::BleDebugger: return QCoreApplication::translate("CommandFormats", "BLE 调试器 (*.blemsg)");
    case Format::Vofa:        return QCoreApplication::translate("CommandFormats", "VOFA+ (*.json)");
    }
    return QString();
}

QString suffix(Format format)
{
    switch (format) {
    case Format::QQCom:   return QStringLiteral("json");
    case Format::JCom:        return QStringLiteral("port");
    case Format::BleDebugger: return QStringLiteral("blemsg");
    case Format::Vofa:        return QStringLiteral("json");
    }
    return QStringLiteral("json");
}

QString saveFilter(Format format)
{
    return label(format) + QStringLiteral(";;") +
           QCoreApplication::translate("CommandFormats", "所有文件 (*.*)");
}

bool parseFile(const QString &filePath, QList<CommandSet> &outSets,
               QString &errorMessage, Format *detected)
{
    errorMessage.clear();
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        errorMessage = QCoreApplication::translate("CommandFormats", "无法打开文件");
        return false;
    }
    const QByteArray data = file.readAll();
    file.close();

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (doc.isNull()) {
        errorMessage = QCoreApplication::translate("CommandFormats", "JSON 解析失败: %1")
                           .arg(parseError.errorString());
        return false;
    }

    // 只看结构不看扩展名：数组 → JCom；对象再按关键字段区分 blemsg 与自有格式
    if (doc.isArray()) {
        if (detected) *detected = Format::JCom;
        outSets = parseJCom(doc.array());
        return true;
    }

    const QJsonObject root = doc.object();
    if (root.contains(QStringLiteral("items")) && root.contains(QStringLiteral("groups"))) {
        if (detected) *detected = Format::BleDebugger;
        outSets = parseBle(root);
        return true;
    }
    if (root.contains(QStringLiteral("commandSets"))) {
        if (detected) *detected = Format::QQCom;
        outSets = parseQQCom(root);
        return true;
    }
    if (root.value(QStringLiteral("type")).toString() == QLatin1String("cmds")
        && root.value(QStringLiteral("ctx")).isObject()) {
        if (detected) *detected = Format::Vofa;
        outSets = parseVofa(root);
        if (outSets.isEmpty()) {
            errorMessage = QCoreApplication::translate(
                "CommandFormats", "VOFA+ 文件里没有可导入的指令");
            return false;
        }
        return true;
    }

    errorMessage = QCoreApplication::translate(
        "CommandFormats",
        "无法识别的指令集文件格式（支持 QQCom / JCom / BLE 调试器 / VOFA+）");
    return false;
}

bool writeFile(const QString &filePath, Format format,
               const QList<CommandSet> &sets, QString &errorMessage)
{
    errorMessage.clear();
    QJsonDocument doc;
    switch (format) {
    case Format::QQCom:   doc = QJsonDocument(toQQCom(sets)); break;
    case Format::JCom:        doc = QJsonDocument(toJCom(sets));      break;
    case Format::BleDebugger: doc = QJsonDocument(toBle(sets));       break;
    case Format::Vofa:
        errorMessage = QCoreApplication::translate(
            "CommandFormats", "VOFA+ 文件只能导入，不能按该格式导出");
        return false;
    }

    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        errorMessage = QCoreApplication::translate("CommandFormats", "无法写入文件：%1")
                           .arg(file.errorString());
        return false;
    }
    const auto data = doc.toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size() || !file.commit()) {
        errorMessage = QCoreApplication::translate("CommandFormats", "保存文件失败：%1")
                           .arg(file.errorString());
        return false;
    }
    return true;
}

} // namespace CommandFormats
