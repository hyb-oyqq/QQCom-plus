#include "endpointformat.h"

#include <QObject>
#include <QMap>
#include <QStringList>

// Closed: the action to take. Opening: a client is still dialling. Open: the action that stops it.
// The button states itself, so the connecting wait lives on the same label.
QString portActionName(int kind, bool open, bool connecting)
{
    if (connecting && !open && kind == PortDraft::TcpClient)
        return QObject::tr("正在连接");
    switch (kind) {
    case PortDraft::TcpServer: return open ? QObject::tr("停止") : QObject::tr("监听");
    case PortDraft::Udp: return open ? QObject::tr("解绑") : QObject::tr("绑定");
    default: return open ? QObject::tr("断开") : QObject::tr("连接");
    }
}

// The glyph beside that text: a link for the actions that dial out, an access point for the
// ones that listen, and a broken link once the endpoint is open. Colour keeps living in the
// stylesheet's open property, so the icon only has to carry the shape.
QString portActionIconName(int kind, bool open)
{
    if (open) return QStringLiteral(":/icons/link-off.svg");
    return kind <= PortDraft::TcpClient ? QStringLiteral(":/icons/link-variant.svg")
                                        : QStringLiteral(":/icons/access-point.svg");
}

QString portKindName(int kind)
{
    return QStringList{QObject::tr("串口"), QObject::tr("TCP 客户端"),
                       QObject::tr("TCP 服务器"), QObject::tr("UDP")}.value(kind);
}

QString endpointLabel(int index, int kind)
{
    return QString(QChar::fromLatin1('A' + char(index))) + QStringLiteral(" · ") + portKindName(kind);
}

QString portDraftSummary(const PortDraft &draft)
{
    const auto &v = draft.values;
    switch (draft.kind) {
    case PortDraft::Serial: {
        const auto port = v.value("serial.port").toString();
        const auto parity = v.value("serial.parity", "None").toString();
        const QMap<QString, QString> parityCodes{{"None", "N"}, {"Even", "E"}, {"Odd", "O"},
                                                 {"Mark", "M"}, {"Space", "S"}};
        const auto flow = v.value("serial.flow", "None").toString();
        return QStringLiteral("%1 · %2 · %3%4%5 · %6")
            .arg(port.isEmpty() ? QObject::tr("未选择串口") : port,
                 v.value("serial.baud", "115200").toString(),
                 v.value("serial.bits", "8").toString(), parityCodes.value(parity, "?"),
                 v.value("serial.stop", "1").toString(), flow == "None" ? QObject::tr("无流控") : flow);
    }
    case PortDraft::TcpClient:
        return v.value("client.host", "127.0.0.1").toString()
            + ":" + v.value("client.port", 9000).toString();
    case PortDraft::TcpServer:
        return v.value("server.address", "0.0.0.0").toString()
            + ":" + v.value("server.port", 9000).toString();
    default:
        return v.value("udp.address", "0.0.0.0").toString()
            + ":" + v.value("udp.port", 0).toString();
    }
}

QString portDraftDetails(const PortDraft &draft)
{
    if (draft.kind != PortDraft::Serial)
        return QObject::tr("地址：%1").arg(portDraftSummary(draft));
    const auto &v = draft.values;
    const QMap<QString, QString> parityNames{{"None", QObject::tr("无校验")},
        {"Even", QObject::tr("偶校验")}, {"Odd", QObject::tr("奇校验")},
        {"Mark", QObject::tr("标记校验")}, {"Space", QObject::tr("空校验")}};
    const auto port = v.value("serial.port").toString();
    const auto parity = v.value("serial.parity", "None").toString();
    const auto flow = v.value("serial.flow", "None").toString();
    return QObject::tr("串口：%1\n波特率：%2 bps\n数据位：%3\n校验：%4\n停止位：%5\n流控：%6")
        .arg(port.isEmpty() ? QObject::tr("未选择串口") : port,
             v.value("serial.baud", "115200").toString(), v.value("serial.bits", "8").toString(),
             parityNames.value(parity, parity), v.value("serial.stop", "1").toString(),
             flow == "None" ? QObject::tr("无流控") : flow);
}
