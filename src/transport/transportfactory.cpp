#include "transport/transportfactory.h"
#include "transport/serialtransport.h"
#include "transport/tcpclienttransport.h"
#include "transport/tcpservertransport.h"
#include "transport/udptransport.h"

AbstractTransport *TransportFactory::create(int kind, int endpointIndex, QObject *parent)
{
    switch (kind) {
    case PortDraft::Serial: return new SerialTransport(endpointIndex, parent);
    case PortDraft::TcpClient: return new TcpClientTransport(endpointIndex, parent);
    case PortDraft::TcpServer: return new TcpServerTransport(endpointIndex, parent);
    case PortDraft::Udp: return new UdpTransport(endpointIndex, parent);
    }
    return nullptr;
}
