#include "commandformats.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
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

} // namespace

namespace CommandFormats {

QList<Format> allFormats()
{
    return {Format::QQCom, Format::JCom, Format::BleDebugger};
}

QString label(Format format)
{
    switch (format) {
    case Format::QQCom:   return QCoreApplication::translate("CommandFormats", "QQCom (*.json)");
    case Format::JCom:        return QCoreApplication::translate("CommandFormats", "JCom (*.port)");
    case Format::BleDebugger: return QCoreApplication::translate("CommandFormats", "BLE 调试器 (*.blemsg)");
    }
    return QString();
}

QString suffix(Format format)
{
    switch (format) {
    case Format::QQCom:   return QStringLiteral("json");
    case Format::JCom:        return QStringLiteral("port");
    case Format::BleDebugger: return QStringLiteral("blemsg");
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

    errorMessage = QCoreApplication::translate(
        "CommandFormats",
        "无法识别的指令集文件格式（支持 QQCom / JCom / BLE 调试器 三种）");
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
