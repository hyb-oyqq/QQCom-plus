#include "transport/abstracttransport.h"

Traffic::Source AbstractTransport::makeSource(quint64 generation, const QUuid &peerId,
    const QString &peerAddress) const
{
    auto source = Traffic::makeSource(m_draft, m_endpointIndex);
    source.generation = generation;
    source.peerId = peerId;
    source.peerAddress = peerAddress;
    return source;
}
