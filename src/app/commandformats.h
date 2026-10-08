#ifndef COMMANDFORMATS_H
#define COMMANDFORMATS_H

#include "domain/commandset.h"

#include <QList>
#include <QString>

// 指令集文件的外部格式适配层：QQCom 自有格式 / JCom(*.port) / BLE 调试器(*.blemsg)
// / VOFA+(*.cmds.json，只导入)。
//
// 导入侧按 JSON 顶层结构自动嗅探，不看扩展名——用户会随手改扩展名，而且同一个
// 工具导出的文件也可能被重命名。
//
// 导出侧必须让用户显式选格式：JCom 顶层是数组、blemsg 顶层是对象，两边的解析器
// 都按顶层类型硬判别，任何"混合体"都会被两边同时拒绝，所以"单一文件同时兼容两种
// 格式"在结构上不可行，只能导入全兼容 + 导出选格式。VOFA+ 不在导出列表里。
namespace CommandFormats
{
enum class Format
{
    QQCom,       // {"version":"1.0","commandSets":[...]}
    JCom,        // [ {"GroupName":..., "InitSendList":[...]} ]
    BleDebugger, // {"version":1,"groups":[...],"items":[...]}
    Vofa         // {"type":"cmds","ctx":{"subCmds":[...]}}，仅导入
};

// 对话框下拉与文件对话框的顺序来源
QList<Format> allFormats();
QString label(Format format);      // "QQCom (*.json)" 这类显示名
QString suffix(Format format);     // "json" / "port" / "blemsg"
QString saveFilter(Format format); // QFileDialog 用的过滤器

// 读文件并解析出指令集。失败时 errorMessage 说明支持哪些格式。
// detected 非空时回填嗅探结果，供界面提示"已识别为 XX 格式"。
bool parseFile(const QString &filePath, QList<CommandSet> &outSets,
               QString &errorMessage, Format *detected = nullptr);

// 按指定格式写文件
bool writeFile(const QString &filePath, Format format,
               const QList<CommandSet> &sets, QString &errorMessage);
}

#endif
