#ifndef PORTDRAFT_H
#define PORTDRAFT_H

#include "receiveframing.h"
#include <QString>
#include <QVariantMap>
#include <QUuid>

// 端点草稿：界面上一份可编辑的端口配置值对象。只有类型与参数，没有运行时状态——
// 真实传输对象与持久化配置是独立类型。领域类型独立成头，避免每个只想引用它的
// 单元被迫包含 UI 控件声明。
struct PortDraft
{
    // 端点类型，同时也是类型下拉框的索引；非限定枚举可隐式转 int。
    enum Kind
    {
        Serial = 0,
        TcpClient,
        TcpServer,
        Udp
    };

    QUuid id = QUuid::createUuid();
    int kind = Serial; // PortDraft::Kind
    QVariantMap values;
    // 异常断开后自动重新打开。用户主动停止会取消本次恢复，选项本身保留。
    // 串口只在同名设备重新出现时打开；TCP 客户端和 UDP 按 AutoRecoverRetryMs 重试。
    // TCP 服务器不使用：监听还在时客户端要自己再连，监听失败时再点一次即可。
    bool autoRecover = false;
    static constexpr int AutoRecoverRetryMs = 1000;
    // 接收组帧策略。贴在草稿上而不是运行时里：它随端点配置一起编辑、随 open 一起下发，
    // 也可在连接中热更新；串口、TCP 和 UDP 都按此用户规则组帧。
    ReceiveFraming framing;
};

// 跨线程命令（GUI → I/O 线程的 openEndpoint）按值携带草稿，需要元类型。
Q_DECLARE_METATYPE(PortDraft)

// 端点的**短标识**：把"具体是哪个口"压成一个人能念出来的名字。
//   串口 → `COM3`；TCP 客户端/服务器 → `127.0.0.1:9000`；UDP → `127.0.0.1:9001`
// 未配置（关键字段为空）时返回空串——宁可只说得出"是哪个端点"，也别拿默认值
// 冒充用户配的端口。
//
// 与 UI 的 portDraftSummary()（长摘要，含波特率/校验/流控）分工不同：那个用于参数
// 展示，这个只用于"标识是谁"，越短越好，且**必须能在 domain 层取到**——运行时装配
// 状态行正文时也要用（`src/app` 不能依赖 `src/ui`，见 services.pri）。
inline QString portShortLabel(const PortDraft &draft)
{
    const auto &v = draft.values;
    // 网络端点的三种取法只有键名不同，拼法一致：host:port。
    const auto joined = [&v](const char *hostKey, const char *portKey) {
        const QString host = v.value(QLatin1String(hostKey)).toString();
        const QString port = v.value(QLatin1String(portKey)).toString();
        return (host.isEmpty() || port.isEmpty()) ? QString()
                                                  : host + QLatin1Char(':') + port;
    };
    switch (draft.kind) {
    case PortDraft::Serial:
        return v.value(QStringLiteral("serial.port")).toString();
    case PortDraft::TcpClient:
        return joined("client.host", "client.port");
    case PortDraft::TcpServer:
        return joined("server.address", "server.port");
    case PortDraft::Udp:
        return joined("udp.address", "udp.port");
    }
    return {};
}

#endif
