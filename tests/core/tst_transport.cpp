#include <QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include "app/endpointruntime.h"
#include "app/receiveframer.h"
#include "app/commandmanager.h"
#include "persistence/preferencesstore.h"
#include "transport/serialtransport.h"
#include "transport/tcpclienttransport.h"
#include "transport/tcpservertransport.h"
#include "transport/transportfactory.h"
#include "transport/udptransport.h"

using namespace Qt::StringLiterals;

namespace {

// 传输层无 UI 核心测试：全部走本机回环，不需要真实串口硬件。
// 覆盖四种端点的打开/关闭/收发语义，以及 EndpointRuntime 的消息装配。

constexpr int PortScanBegin = 45450;
constexpr int PortScanEnd = 45500;

PortDraft makeDraft(int kind, const QVariantMap &values)
{
    PortDraft draft;
    draft.kind = kind;
    draft.values = values;
    return draft;
}

int lastState(const QSignalSpy &spy)
{
    return spy.isEmpty() ? -1 : spy.last().at(1).toInt();
}

int peerCount(const QSignalSpy &peerSpy)
{
    return peerSpy.isEmpty() ? 0
        : peerSpy.last().at(1).value<QList<Transport::PeerInfo>>().size();
}

QUuid firstPeerId(const QSignalSpy &peerSpy)
{
    if (peerSpy.isEmpty()) return {};
    const auto peers = peerSpy.last().at(1).value<QList<Transport::PeerInfo>>();
    return peers.isEmpty() ? QUuid() : peers.constFirst().id;
}

// 回环端口存在被其他进程占用的可能：从候选区间里挑一个 listen/bind
// 成功的。start() 的结果是同步发布的，逐个试到成功即可。
template <typename StartFn>
int findFreePort(StartFn &&start)
{
    for (int candidate = PortScanBegin; candidate <= PortScanEnd; ++candidate)
        if (start(candidate)) return candidate;
    return 0;
}

} // namespace

class TransportTest : public QObject
{
    Q_OBJECT

private slots:
    void preferencesMigrationPreservesBothFiles()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto source = dir.filePath("old/preferences.ini");
        const auto target = dir.filePath("new/preferences.ini");
        QString error;
        QVERIFY(PreferencesStore::migrateLegacyFile(source, target, &error));
        QVERIFY(!QFileInfo::exists(target));
        {
            PreferencesStore oldStore(nullptr, source);
            oldStore.setValue("commands/sets", QByteArray("legacy-command-data"));
            oldStore.setValue("serial/baud", 2000000);
        }
        QFile original(source);
        QVERIFY(original.open(QIODevice::ReadOnly));
        const auto originalBytes = original.readAll();
        original.close();
        QVERIFY2(PreferencesStore::migrateLegacyFile(source, target, &error), qPrintable(error));
        {
            PreferencesStore newStore(nullptr, target);
            QCOMPARE(newStore.value("commands/sets").toByteArray(), QByteArray("legacy-command-data"));
            QCOMPARE(newStore.value("serial/baud").toInt(), 2000000);
            newStore.setValue("serial/baud", 115200);
        }
        QVERIFY(PreferencesStore::migrateLegacyFile(source, target, &error));
        PreferencesStore newStore(nullptr, target);
        QCOMPARE(newStore.value("serial/baud").toInt(), 115200);
        QVERIFY(!PreferencesStore::migrateLegacyFile(source, source + "/blocked.ini", &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), originalBytes);
    }

    void framerHandlesFragmentedBoundariesAndTinyThreshold()
    {
        ReceiveFraming config;
        config.mode = ReceiveFraming::Delimiter;
        config.delimiter = "aba";
        config.bufferThresholdBytes = 100000;
        ReceiveFramer framer(config);
        QVERIFY(framer.feed("xxab").isEmpty());
        const auto frames = framer.feed("aabaYYab");
        QCOMPARE(frames.size(), 2);
        QCOMPARE(frames[0].bytes, QByteArray("xx"));
        QCOMPARE(frames[1].bytes, QByteArray());
        QCOMPARE(framer.feed("a").first().bytes, QByteArray("YY"));
        QVERIFY(framer.flush(ReceiveFramer::Closed).isEmpty());

        config.mode = ReceiveFraming::LengthPrefixU16LE;
        config.bufferThresholdBytes = 1;
        framer.setConfig(config);
        const auto partial = framer.feed(QByteArray(1, '\x02'));
        QCOMPARE(partial.size(), 1);
        QCOMPARE(partial.first().trigger, ReceiveFramer::Threshold);
        QCOMPARE(framer.pendingBytes(), 0);
    }

    void runtimeSeparatesTcpPeerFramesAndFlushesDisconnect()
    {
        QList<Traffic::Message> received;
        EndpointRuntime runtime;
        auto draft = makeDraft(PortDraft::TcpServer,
            {{"server.address", "127.0.0.1"}, {"server.port", 0}});
        draft.framing.mode = ReceiveFraming::Delimiter;
        draft.framing.delimiter = "\n";
        draft.framing.idleTimeoutMs = 5000;
        connect(&runtime, &EndpointRuntime::messageReceived, this,
            [&received](const Traffic::Message &message) {
                if (message.direction == Traffic::Direction::Receive) received.append(message);
            });
        runtime.openEndpoint(0, draft);
        auto *listener = runtime.findChild<QTcpServer *>();
        QVERIFY(listener && listener->isListening());
        auto *transport = runtime.findChild<AbstractTransport *>();
        QSignalSpy chunks(transport, &AbstractTransport::dataReceived);
        QTcpSocket first, second;
        first.connectToHost(QHostAddress::LocalHost, listener->serverPort());
        second.connectToHost(QHostAddress::LocalHost, listener->serverPort());
        QTRY_COMPARE(first.state(), QAbstractSocket::ConnectedState);
        QTRY_COMPARE(second.state(), QAbstractSocket::ConnectedState);
        first.write("first-tail");
        QTRY_COMPARE(chunks.size(), 1);
        second.write("second\n");
        QTRY_COMPARE(received.size(), 1);
        QCOMPARE(received[0].raw, QByteArray("second"));
        QVERIFY(received[0].source.peerAddress.endsWith(QString(":%1").arg(second.localPort())));
        const auto firstSource = chunks.first()[1].value<Traffic::Source>();
        first.disconnectFromHost();
        QTRY_COMPARE_WITH_TIMEOUT(received.size(), 2, 1000);
        QCOMPARE(received[1].raw, QByteArray("first-tail"));
        QCOMPARE(received[1].source.peerId, firstSource.peerId);
        QCOMPARE(received[1].source.peerAddress, firstSource.peerAddress);
        second.write("shutdown-tail");
        QTRY_COMPARE(chunks.size(), 3);
        runtime.closeEndpoint(0);
        QCOMPARE(received.size(), 3);
        QCOMPARE(received.last().raw, QByteArray("shutdown-tail"));
        QCOMPARE(received.last().source.peerId, received.first().source.peerId);
    }

    void runtimeIdleRestartsAfterEveryArrival()
    {
        QList<Traffic::Message> received;
        EndpointRuntime runtime;
        auto draft = makeDraft(PortDraft::TcpServer,
            {{"server.address", "127.0.0.1"}, {"server.port", 0}});
        draft.framing.mode = ReceiveFraming::IdleTimeout;
        draft.framing.idleTimeoutMs = 240;
        connect(&runtime, &EndpointRuntime::messageReceived, this,
            [&received](const Traffic::Message &message) {
                if (message.direction == Traffic::Direction::Receive) received.append(message);
            });
        runtime.openEndpoint(0, draft);
        auto *listener = runtime.findChild<QTcpServer *>();
        QVERIFY(listener && listener->isListening());
        QSignalSpy chunks(runtime.findChild<AbstractTransport *>(), &AbstractTransport::dataReceived);
        QTcpSocket client;
        client.connectToHost(QHostAddress::LocalHost, listener->serverPort());
        QTRY_COMPARE(client.state(), QAbstractSocket::ConnectedState);
        client.write("one");
        QTRY_COMPARE(chunks.size(), 1);
        QTest::qWait(140);
        client.write("two");
        QTRY_COMPARE(chunks.size(), 2);
        QTest::qWait(120); // 已过首次输入的期限，但距最后输入尚不足 240 ms。
        QVERIFY(received.isEmpty());
        QTRY_COMPARE(received.size(), 1);
        QCOMPARE(received.first().raw, QByteArray("onetwo"));
        QVERIFY(!received.first().source.peerId.isNull());
        runtime.closeEndpoint(0);
    }

    void tcpServerStopsMultipleActivePeers()
    {
        TcpServerTransport server(0);
        server.start(makeDraft(PortDraft::TcpServer,
            {{"server.address", "127.0.0.1"}, {"server.port", 0}}), 1);
        auto *listener = server.findChild<QTcpServer *>();
        QVERIFY(listener && listener->isListening());
        QSignalSpy peers(&server, &AbstractTransport::peerListChanged);
        QTcpSocket first, second;
        first.connectToHost(QHostAddress::LocalHost, listener->serverPort());
        second.connectToHost(QHostAddress::LocalHost, listener->serverPort());
        QTRY_COMPARE(peerCount(peers), 2);
        server.stop();
        server.stop(); // 幂等关闭。
        QTRY_COMPARE(first.state(), QAbstractSocket::UnconnectedState);
        QTRY_COMPARE(second.state(), QAbstractSocket::UnconnectedState);
        QCOMPARE(server.write("closed", {}), qint64(-1));
    }

    void commandManagerUsesSelectedFileFormat()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        PreferencesStore store(nullptr, directory.filePath("preferences.ini"));
        auto *manager = CommandManager::instance();
        manager->load(&store);
        CommandSet group(QStringLiteral("设备指令"));
        CommandItem command;
        command.content = "01 AB FF";
        command.isHex = true;
        command.description = "probe";
        command.tail = "CR+LF";
        group.commands.append(command);
        for (const auto format : CommandFormats::allFormats()) {
            const auto path = directory.filePath("commands." + CommandFormats::suffix(format));
            QString error, detectedLabel;
            QVERIFY2(manager->exportToFile(path, format, {group}, error), qPrintable(error));
            QList<CommandSet> parsed;
            CommandFormats::Format detected;
            QVERIFY(CommandFormats::parseFile(path, parsed, error, &detected));
            QCOMPARE(detected, format);
            QCOMPARE(parsed.size(), 1);
            QCOMPARE(parsed.first().commands.first().content, command.content);
            const auto before = manager->setCount();
            QVERIFY2(manager->importFromFile(path, error, &detectedLabel), qPrintable(error));
            QCOMPARE(manager->setCount(), before + 1);
            QCOMPARE(detectedLabel, CommandFormats::label(format));
            QVERIFY(manager->allSets().last().id != group.id);
            manager->undo();
            QCOMPARE(manager->setCount(), before);
        }
        QString error;
        QVERIFY(!manager->exportToFile(directory.filePath("missing/commands.json"),
            CommandFormats::Format::QQCom, {group}, error));
        QVERIFY(!error.isEmpty());
    }

    // 接收组帧器：纯逻辑，不需要端口与事件循环——把字节流片段喂进去，
    // 看它按定界/长度/兜底三种规则吐出什么。
    void framerSplitsOnDelimiter()
    {
        ReceiveFraming config;
        config.mode = ReceiveFraming::Delimiter;
        config.delimiter = QByteArray::fromHex("0D0A");
        config.bufferThresholdBytes = 1000;
        ReceiveFramer framer(config);

        // 半帧先到：缓冲里没有结束符，一条都不该输出。
        QVERIFY(framer.feed(QByteArray("abc")).isEmpty());
        QCOMPARE(framer.pendingBytes(), 3);
        // 结束符补齐：出一帧，默认剥离结束符。
        auto frames = framer.feed(QByteArray("\r\n"));
        QCOMPARE(frames.size(), 1);
        QCOMPARE(frames.at(0).bytes, QByteArray("abc"));
        QVERIFY(frames.at(0).complete);
        QCOMPARE(framer.pendingBytes(), 0);

        // 一次收到两帧（粘包）+ 下一帧的前半段：两帧即时输出，半个留下。
        frames = framer.feed(QByteArray("one\r\ntwo\r\nthr"));
        QCOMPARE(frames.size(), 2);
        QCOMPARE(frames.at(0).bytes, QByteArray("one"));
        QCOMPARE(frames.at(1).bytes, QByteArray("two"));
        QCOMPARE(framer.pendingBytes(), 3);
    }

    void framerFollowsLengthPrefix()
    {
        ReceiveFraming config;
        config.mode = ReceiveFraming::LengthPrefixU16LE;
        config.includeLengthPrefix = true; // 默认保留帧头
        config.bufferThresholdBytes = 1000;
        ReceiveFramer framer(config);

        // 帧头说"后面还有 2 字节"，但一个也没到：继续等。
        QVERIFY(framer.feed(QByteArray::fromHex("0200")).isEmpty());
        QVERIFY(framer.feed(QByteArray("xy")).size() == 1);
        // 半帧：帧头说 3 字节，只到了 1 字节 → 不出帧。
        QVERIFY(framer.feed(QByteArray::fromHex("0300")).isEmpty());
        QVERIFY(framer.feed(QByteArray("a")).isEmpty());
        auto frames = framer.feed(QByteArray("bc"));
        QCOMPARE(frames.size(), 1);
        QCOMPARE(frames.at(0).bytes, QByteArray::fromHex("0300616263")); // 含帧头
        QVERIFY(frames.at(0).complete);

        // 跨包拆分：帧头与 payload 分两次到，收齐才出帧。
        frames = framer.feed(QByteArray::fromHex("0100")); // payload 1 字节
        QVERIFY(frames.isEmpty());
        frames = framer.feed(QByteArray("Z"));
        QCOMPARE(frames.size(), 1);
        QCOMPARE(frames.at(0).bytes, QByteArray::fromHex("01005A"));

        // 长度 0 的空帧也要收：不收就会永远等第 3 字节。
        // 花括号构造：写成 ReceiveFramer x(ReceiveFraming()) 会被解析成函数声明。
        ReceiveFramer stripped{ReceiveFraming()};
        ReceiveFraming noPrefix = config;
        noPrefix.includeLengthPrefix = false;
        stripped.setConfig(noPrefix);
        frames = stripped.feed(QByteArray::fromHex("0000"));
        QCOMPARE(frames.size(), 1);
        QCOMPARE(frames.at(0).bytes, QByteArray()); // 剥离帧头后为空帧
        QCOMPARE(stripped.pendingBytes(), 0);
    }

    void framerSplitsOnIdleTimeout()
    {
        ReceiveFraming config;
        config.mode = ReceiveFraming::IdleTimeout;
        config.idleTimeoutMs = 10;
        config.bufferThresholdBytes = 8;
        ReceiveFramer framer(config);

        QVERIFY(framer.feed(QByteArray("abc")).isEmpty());
        QCOMPARE(framer.pendingBytes(), 3);
        auto frames = framer.flush(ReceiveFramer::Idle);
        QCOMPARE(frames.size(), 1);
        QCOMPARE(frames.at(0).bytes, QByteArray("abc"));
        QVERIFY(!frames.at(0).complete);
        QCOMPARE(frames.at(0).trigger, ReceiveFramer::Idle);
        QCOMPARE(framer.pendingBytes(), 0);

        // 空闲模式仍受长度兜底限制，单次输入达到阈值立即输出。
        frames = framer.feed(QByteArray(8, 'x'));
        QCOMPARE(frames.size(), 1);
        QCOMPARE(frames.at(0).trigger, ReceiveFramer::Threshold);
        QCOMPARE(frames.at(0).bytes, QByteArray(8, 'x'));
    }

    void framerFallbacksBoundBuffer()
    {
        // 缓冲阈值：慢速/无定界协议不能无限滞留，也不能无界增长。
        ReceiveFraming config;
        config.mode = ReceiveFraming::Delimiter;
        config.delimiter = QByteArray::fromHex("0D0A");
        config.bufferThresholdBytes = 8;
        config.logHints = true;
        ReceiveFramer framer(config);

        auto frames = framer.feed(QByteArray(8, 'x'));
        QCOMPARE(frames.size(), 1);
        QCOMPARE(frames.at(0).bytes, QByteArray(8, 'x'));
        QVERIFY(!frames.at(0).complete); // 兜底切出来的仍是流片段
        QCOMPARE(frames.at(0).trigger, ReceiveFramer::Threshold);
        QVERIFY(!frames.at(0).hint.isEmpty());
        QCOMPARE(framer.pendingBytes(), 0);

        // 空闲超时：flush 把残留输出，空缓冲则什么也不发。
        framer.feed(QByteArray("tail"));
        QCOMPARE(framer.pendingBytes(), 4);
        auto idle = framer.flush(ReceiveFramer::Idle);
        QCOMPARE(idle.size(), 1);
        QCOMPARE(idle.at(0).bytes, QByteArray("tail"));
        QCOMPARE(idle.at(0).trigger, ReceiveFramer::Idle);
        QVERIFY(framer.flush(ReceiveFramer::Idle).isEmpty());

        // 低于阈值的残留才会留在缓冲区，关闭时应由 Closed 收尾输出。
        framer.feed(QByteArray("end"));
        auto closed = framer.flush(ReceiveFramer::Closed);
        QCOMPARE(closed.size(), 1);
        QCOMPARE(closed.at(0).bytes, QByteArray("end"));
        QCOMPARE(closed.at(0).trigger, ReceiveFramer::Closed);

        // 直通模式：收到什么出什么，不做任何拆分。
        ReceiveFramer passthrough{ReceiveFraming()};
        auto out = passthrough.feed(QByteArray("a\r\nb"));
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.at(0).bytes, QByteArray("a\r\nb"));
        QVERIFY(out.at(0).complete);

        // 配置不合法（结束符模式却没有结束符）时退回直通，绝不把数据吞在缓冲里。
        ReceiveFraming broken;
        broken.mode = ReceiveFraming::Delimiter;
        ReceiveFramer fallback(broken);
        out = fallback.feed(QByteArray("kept"));
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.at(0).bytes, QByteArray("kept"));
        QCOMPARE(fallback.pendingBytes(), 0);
    }

    // 组帧端到端：字节流经 EndpointRuntime 的组帧器后，界面看到的是帧而不是片段。
    // 引擎的单测只覆盖 feed/flush，这里覆盖"传输事件 → 组帧 → 消息装配"这条链路。
    void runtimeFramesByteStreamByDelimiter()
    {
        EndpointRuntime runtime;
        QSignalSpy states(&runtime, &EndpointRuntime::endpointStateChanged);
        QSignalSpy messages(&runtime, &EndpointRuntime::messageReceived);

        // 先借一个空闲端口：运行时反复开同一端点（扫描候选端口）会与传输对象的
        // deleteLater 收尾打架，这里只开一次。
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        const quint16 port = probe.serverPort();
        probe.close();

        PortDraft draft = makeDraft(PortDraft::TcpServer,
            {{"server.address", "127.0.0.1"}, {"server.port", int(port)}, {"server.clients", 4}});
        ReceiveFraming framing;
        framing.mode = ReceiveFraming::Delimiter;
        framing.delimiter = QByteArray::fromHex("0D0A");
        framing.idleTimeoutMs = 120;
        framing.bufferThresholdBytes = 300;
        framing.logHints = true; // 兜底输出要发警告事件
        draft.framing = framing;

        runtime.openEndpoint(0, draft);
        QTRY_COMPARE(lastState(states), int(Transport::State::Open));

        // 只看接收消息：状态/错误事件混在同一个信号里，断言前先按方向过滤。
        const auto received = [&messages] {
            QList<Traffic::Message> rx;
            for (const auto &params : messages) {
                const auto message = params.at(0).value<Traffic::Message>();
                if (message.direction == Traffic::Direction::Receive) rx.append(message);
            }
            return rx;
        };

        // 空闲超时从“收到第一段数据”开始计算；端口刚打开时没有数据，
        // 等待一个超时时间也不应产生空的接收消息。
        QTest::qWait(160);
        QCOMPARE(received().size(), 0);

        QTcpSocket client;
        client.connectToHost(QStringLiteral("127.0.0.1"), port);
        QVERIFY(client.waitForConnected(3000));

        const auto waitReceived = [&](int count) { QTRY_VERIFY(received().size() >= count); };

        // 半帧先到：定界未到，只应出"one"这一帧，"t"留在缓冲里。
        client.write(QByteArray("one\r\nt"));
        QVERIFY(client.waitForBytesWritten(1000));
        waitReceived(1);
        // 半帧留在缓冲里：此刻只有"one"一条（再等下去空闲超时会兜底把"t"也输出）。
        QCOMPARE(received().size(), 1);
        QCOMPARE(received().at(0).raw, QByteArray("one"));
        // 按定界收齐的是完整帧。
        QCOMPARE(received().at(0).framing, Traffic::Framing::Message);

        // 补齐第二帧：跨包到达的半帧在这里收齐。
        client.write(QByteArray("wo\r\n"));
        QVERIFY(client.waitForBytesWritten(1000));
        waitReceived(2);
        QCOMPARE(received().size(), 2);
        QCOMPARE(received().at(1).raw, QByteArray("two"));

        // 不带结束符的残留：空闲超时兜底输出，仍是流片段，并给出提示日志。
        client.write(QByteArray("tail"));
        QVERIFY(client.waitForBytesWritten(1000));
        waitReceived(3);
        const auto tail = received().at(2);
        QCOMPARE(tail.raw, QByteArray("tail"));
        QCOMPARE(tail.framing, Traffic::Framing::Stream);
        QVERIFY(tail.hasMoreFragments);
        QTRY_VERIFY([&messages] {
            for (const auto &params : messages) {
                const auto message = params.at(0).value<Traffic::Message>();
                if (message.direction == Traffic::Direction::Warning) return true;
            }
            return false;
        }());

        client.disconnectFromHost();
        QTest::qWait(60);
        runtime.closeEndpoint(0);
    }

    void factoryCreatesEveryKind()
    {
        for (int kind = PortDraft::Serial; kind <= PortDraft::Udp; ++kind) {
            auto *transport = TransportFactory::create(kind, 0, this);
            QVERIFY2(transport, qPrintable(QString::number(kind)));
            delete transport;
        }
    }

    void tcpClientAndServerExchange()
    {
        TcpServerTransport server(0);
        QSignalSpy serverState(&server, &AbstractTransport::stateChanged);
        QSignalSpy serverPeers(&server, &AbstractTransport::peerListChanged);
        QSignalSpy serverData(&server, &AbstractTransport::dataReceived);
        QSignalSpy serverErrors(&server, &AbstractTransport::errorOccurred);

        const int port = findFreePort([&server, &serverState](int candidate) {
            server.start(makeDraft(PortDraft::TcpServer,
                {{"server.address", "127.0.0.1"}, {"server.port", candidate}, {"server.clients", 8}}), 1);
            const bool open = lastState(serverState) == int(Transport::State::Open);
            if (!open) serverState.clear(); // 成功的那次记录保留，供后续断言使用
            return open;
        });
        QVERIFY2(port != 0, "没有可用的回环监听端口");

        TcpClientTransport client(1);
        QSignalSpy clientState(&client, &AbstractTransport::stateChanged);
        QSignalSpy clientData(&client, &AbstractTransport::dataReceived);
        client.start(makeDraft(PortDraft::TcpClient,
            {{"client.host", "127.0.0.1"}, {"client.port", port}, {"client.timeout", 5000}}), 1);
        QTRY_COMPARE(lastState(clientState), int(Transport::State::Open));
        QTRY_COMPARE(peerCount(serverPeers), 1);
        const auto peerId = firstPeerId(serverPeers);
        QVERIFY(!peerId.isNull());

        // 客户端 → 服务器：来源携带 peer 身份与地址
        QCOMPARE(client.write("ping", QUuid()), qint64(4));
        QTRY_VERIFY(!serverData.isEmpty());
        QCOMPARE(serverData.last().at(2).toByteArray(), QByteArray("ping"));
        const auto rxSource = serverData.last().at(1).value<Traffic::Source>();
        QCOMPARE(rxSource.peerId, peerId);
        QVERIFY(rxSource.peerAddress.startsWith(u"127.0.0.1:"_s));

        // 服务器 → 指定 peer
        QCOMPARE(server.write("pong", peerId), qint64(4));
        QTRY_VERIFY(!clientData.isEmpty());
        QCOMPARE(clientData.last().at(2).toByteArray(), QByteArray("pong"));

        // 服务器 → 全部客户端（peerId 为空的广播语义）
        QCOMPARE(server.write("all", QUuid()), qint64(3));
        QTRY_VERIFY(clientData.count() >= 2);
        bool broadcastSeen = false;
        for (int i = 0; i < clientData.count(); ++i)
            broadcastSeen |= clientData.at(i).at(2).toByteArray() == QByteArray("all");
        QVERIFY(broadcastSeen);

        // 未知 peer 的定向写是失败而不是静默广播
        QCOMPARE(server.write("ghost", QUuid::createUuid()), qint64(-1));

        client.stop();
        QTRY_COMPARE(lastState(clientState), int(Transport::State::Closed));
        QTRY_COMPARE(peerCount(serverPeers), 0);
        server.stop();
        QCOMPARE(lastState(serverState), int(Transport::State::Open)); // stop 不发状态事件，代次由运行时管理
    }

    void tcpClientReportsConnectionRefused()
    {
        TcpClientTransport client(0);
        QSignalSpy state(&client, &AbstractTransport::stateChanged);
        QSignalSpy errors(&client, &AbstractTransport::errorOccurred);
        // 127.0.0.1:1 几乎必然没有监听；拒绝连接要变成显式错误而不是永远 Opening。
        client.start(makeDraft(PortDraft::TcpClient,
            {{"client.host", "127.0.0.1"}, {"client.port", 1}, {"client.timeout", 5000}}), 1);
        QTRY_COMPARE(lastState(state), int(Transport::State::Closed));
        QVERIFY(errors.count() >= 1);
        QCOMPARE(client.write("x", QUuid()), qint64(-1));
    }

    void tcpServerEnforcesClientLimit()
    {
        TcpServerTransport server(0);
        QSignalSpy serverState(&server, &AbstractTransport::stateChanged);
        QSignalSpy serverPeers(&server, &AbstractTransport::peerListChanged);
        QSignalSpy serverErrors(&server, &AbstractTransport::errorOccurred);
        const int port = findFreePort([&server, &serverState](int candidate) {
            server.start(makeDraft(PortDraft::TcpServer,
                {{"server.address", "127.0.0.1"}, {"server.port", candidate}, {"server.clients", 1}}), 1);
            const bool open = lastState(serverState) == int(Transport::State::Open);
            if (!open) serverState.clear();
            return open;
        });
        QVERIFY(port != 0);

        QTcpSocket first;
        first.connectToHost(QHostAddress::LocalHost, quint16(port));
        QTRY_COMPARE(peerCount(serverPeers), 1);

        QTcpSocket second;
        second.connectToHost(QHostAddress::LocalHost, quint16(port));
        // 超限连接被立即拒绝：服务器仍只有 1 个 peer，第二个客户端被断开。
        QTRY_VERIFY(serverErrors.count() >= 1);
        QTRY_COMPARE(peerCount(serverPeers), 1);
        QTRY_COMPARE(second.state(), QAbstractSocket::UnconnectedState);
        first.disconnectFromHost();
        QTRY_COMPARE(peerCount(serverPeers), 0);
        server.stop();
    }

    void udpPreservesDatagramBoundaryAndSource()
    {
        UdpTransport receiver(0);
        QSignalSpy receiverState(&receiver, &AbstractTransport::stateChanged);
        const int port = findFreePort([&receiver, &receiverState](int candidate) {
            receiver.start(makeDraft(PortDraft::Udp,
                {{"udp.address", "127.0.0.1"}, {"udp.port", candidate},
                 {"udp.target", ""}, {"udp.targetPort", 0}}), 1);
            const bool open = lastState(receiverState) == int(Transport::State::Open);
            if (!open) receiverState.clear();
            return open;
        });
        QVERIFY2(port != 0, "没有可用的回环绑定端口");

        UdpTransport sender(1);
        QSignalSpy senderState(&sender, &AbstractTransport::stateChanged);
        sender.start(makeDraft(PortDraft::Udp,
            {{"udp.address", "127.0.0.1"}, {"udp.port", 0},
             {"udp.target", "127.0.0.1"}, {"udp.targetPort", port}}), 1);
        QCOMPARE(lastState(senderState), int(Transport::State::Open));

        QSignalSpy received(&receiver, &AbstractTransport::dataReceived);
        // 三个数据报（含一个零长）：边界逐一保留，不合并、不拆分。
        QCOMPARE(sender.write(QByteArray("ab"), QUuid()), qint64(2));
        QCOMPARE(sender.write(QByteArray(), QUuid()), qint64(0));
        QCOMPARE(sender.write(QByteArray("cde"), QUuid()), qint64(3));
        QTRY_COMPARE(received.count() >= 3, true);
        QCOMPARE(received.count(), 3);
        const QList<QByteArray> expected = {QByteArray("ab"), QByteArray(), QByteArray("cde")};
        for (int i = 0; i < 3; ++i) {
            QCOMPARE(received.at(i).at(2).toByteArray(), expected.at(i));
            QCOMPARE(received.at(i).at(3).toInt(), int(Traffic::Framing::Datagram));
            const auto source = received.at(i).at(1).value<Traffic::Source>();
            QVERIFY(source.peerAddress.startsWith(u"127.0.0.1:"_s));
        }
        sender.stop();
        receiver.stop();
    }

    void udpWithoutTargetRejectsSend()
    {
        UdpTransport receiver(0);
        QSignalSpy state(&receiver, &AbstractTransport::stateChanged);
        receiver.start(makeDraft(PortDraft::Udp,
            {{"udp.address", "127.0.0.1"}, {"udp.port", 0},
             {"udp.target", ""}, {"udp.targetPort", 0}}), 1);
        QCOMPARE(lastState(state), int(Transport::State::Open));
        const auto detail = state.last().at(2).toString();
        QVERIFY(detail.contains(QStringLiteral("仅接收")));
        QVERIFY(detail.contains(QStringLiteral("不会建立连接或自动回发")));
        // 仅接收的端点没有发送目标：写操作显式失败，不悄悄挑选目的地。
        QCOMPARE(receiver.write("x", QUuid()), qint64(-1));
        receiver.stop();
    }

    void serialRejectsInvalidPortName()
    {
        SerialTransport transport(0);
        QSignalSpy state(&transport, &AbstractTransport::stateChanged);
        QSignalSpy errors(&transport, &AbstractTransport::errorOccurred);
        transport.start(makeDraft(PortDraft::Serial,
            {{"serial.port", "__not_a_serial_port__"}, {"serial.baud", "115200"},
             {"serial.bits", "8"}, {"serial.parity", "None"}, {"serial.stop", "1"},
             {"serial.flow", "None"}}), 1);
        QTRY_COMPARE(lastState(state), int(Transport::State::Closed));
        QVERIFY(errors.count() >= 1);
        QCOMPARE(transport.write("x", QUuid()), qint64(-1));
    }

    void endpointRuntimeAssemblesAndRoutesMessages()
    {
        EndpointRuntime runtime;
        QSignalSpy states(&runtime, &EndpointRuntime::endpointStateChanged);
        QSignalSpy messages(&runtime, &EndpointRuntime::messageReceived);
        QSignalSpy peers(&runtime, &EndpointRuntime::endpointPeersChanged);

        // A 端：UDP 仅接收；逐个候选端口直到打开成功。
        int port = 0;
        for (int candidate = PortScanBegin; candidate <= PortScanEnd && port == 0; ++candidate) {
            runtime.openEndpoint(0, makeDraft(PortDraft::Udp,
                {{"udp.address", "127.0.0.1"}, {"udp.port", candidate},
                 {"udp.target", ""}, {"udp.targetPort", 0}}));
            if (lastState(states) == int(Transport::State::Open)) {
                port = candidate;
            } else {
                runtime.closeEndpoint(0);
                states.clear();
            }
        }
        QVERIFY(port != 0);

        // B 端：UDP 发往 A。
        runtime.openEndpoint(1, makeDraft(PortDraft::Udp,
            {{"udp.address", "127.0.0.1"}, {"udp.port", 0},
             {"udp.target", "127.0.0.1"}, {"udp.targetPort", port}}));
        QCOMPARE(lastState(states), int(Transport::State::Open));

        // 手动发送：B 出现 TX LocalCompleted 消息，A 收到 RX 消息。
        runtime.sendPayload(1, "ping", int(Traffic::Origin::Manual));
        const auto findMessage = [&messages](Traffic::Direction dir, int idx, const QByteArray &raw) {
            for (const auto &params : messages) {
                const auto m = params.at(0).value<Traffic::Message>();
                if (m.direction == dir && m.source.endpointIndex == idx && m.raw == raw)
                    return m;
            }
            return Traffic::Message{};
        };
        // TX：已打开且目标是明确地址 → 本地写出完成；身份与单调时钟已打戳。
        const auto tx = findMessage(Traffic::Direction::Transmit, 1, "ping");
        QCOMPARE(tx.state, Traffic::State::LocalCompleted);
        QVERIFY(tx.id != 0 && tx.monotonicNs > 0);
        // RX：轮询重查 spy（QTRY 的表达式每次迭代都重新执行 findMessage）。
        QTRY_VERIFY(!findMessage(Traffic::Direction::Receive, 0, "ping").frameId.isNull());
        const auto rx = findMessage(Traffic::Direction::Receive, 0, "ping");
        QCOMPARE(rx.framing, Traffic::Framing::Datagram);
        QVERIFY(!rx.source.peerAddress.isEmpty());
        QVERIFY(!rx.frameId.isNull());

        // UDP 没有连接事件：首次收到每个来源时，运行时要把来源传给界面，
        // 同时留下不会自动修改发送目标的明确提示。
        const auto observedPeers = [&peers] {
            QStringList result;
            for (const auto &params : peers)
                result.append(params.at(1).toStringList());
            return result;
        };
        QTRY_VERIFY(observedPeers().contains(rx.source.peerAddress));
        const auto peerNotices = [&messages] {
            int count = 0;
            for (const auto &params : messages) {
                const auto message = params.at(0).value<Traffic::Message>();
                if (message.direction == Traffic::Direction::State && message.source.endpointIndex == 0
                    && message.statusText.contains(QStringLiteral("UDP 无连接")))
                    ++count;
            }
            return count;
        };
        QCOMPARE(peerNotices(), 1);
        runtime.sendPayload(1, "ping-again", int(Traffic::Origin::Manual));
        QTRY_VERIFY(!findMessage(Traffic::Direction::Receive, 0, "ping-again").frameId.isNull());
        QCOMPARE(peerNotices(), 1); // 同一来源不重复刷状态日志

        // 仅接收端点（已打开但没有发送目标）发送：显式 Failed，不静默丢弃。
        const int baseline = messages.count();
        runtime.sendPayload(0, "nope", int(Traffic::Origin::Manual));
        QCOMPARE(messages.count(), baseline + 1);
        const auto failed = messages.last().at(0).value<Traffic::Message>();
        QCOMPARE(failed.state, Traffic::State::Failed);

        runtime.closeEndpoint(1);
        runtime.closeEndpoint(0);
        QTRY_COMPARE(lastState(states), int(Transport::State::Closed));
        // 关闭时的空列表广播用于复位界面显示。
        QVERIFY(!peers.isEmpty());
        QVERIFY(peers.last().at(1).toStringList().isEmpty());
    }
};

QTEST_MAIN(TransportTest)
#include "tst_transport.moc"
