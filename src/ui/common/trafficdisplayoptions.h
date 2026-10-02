#ifndef TRAFFICDISPLAYOPTIONS_H
#define TRAFFICDISPLAYOPTIONS_H

// 监视面板"怎么显示"的开关聚合。过滤条件（TrafficFilter::Options）决定"哪些行
// 出现"，本结构决定"出现的行长什么样"——两者都是纯值类型，任何开关都不改动
// Traffic::Message 本身：原始字节是唯一事实，派生文本一律现算。
//
// 开关清单对齐参考实现（BLE 调试工具"日志设置"页）：显示数据长度、显示来源
// 标识（BLE 场景是属性 UUID，这里对应端口/对端列）、同时显示 HEX、精简日志、
// 时间戳开关。方向与触发方式列是本工具特有维度，一并提供开关。
//
// 只存意图，不存解释：列号 → 可见性的翻译由面板完成（TrafficModel::Column
// 属于 panels 层，这里不反向依赖），将来导出、快照渲染可复用同一份开关。
struct TrafficDisplayOptions
{
    // 精简模式：收发记录仅保留数据（隐藏端口/对端/方向/来源/字节列，数据列占满），
    // 状态与错误记录仍显示说明文本；时间戳跟随 showTimestamp，不受精简影响。
    bool compact = false;

    bool showTimestamp = true; // 时间列（墙钟，毫秒精度）
    bool showPort = true;      // 端点标识列（"A · 串口"）
    bool showPeer = true;      // 对端地址列（TCP/UDP 对端、UDP 来源地址）
    bool showDirection = true; // RX/TX/状态/警告/错误列
    bool showLength = true;    // 字节数列（真实长度，不是预览截断后长度）
    bool showOrigin = true;    // 触发方式列（手动/定时/转发/示例）
    // 数据列在解码文本之外同时渲染一行 HEX。解码文本是有损视图（替换符、\xNN
    // 转义），HEX 是唯一无损视图，两者并存便于对照；默认关闭避免每行过高。
    bool showHexAlongside = false;
};

#endif
