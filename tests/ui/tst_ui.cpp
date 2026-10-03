#include "mainwindow.h"
#include "domain/receiveframing.h"
#include "ui/common/endpointformat.h"
#include "ui/common/payloadcodec.h"
#include "ui/panels/sending/sendpanel.h"
#include "ui/panels/traffic/trafficpanel.h"
#include "ui/panels/traffic/trafficlogview.h"
#include "ui/panels/traffic/traffictextview.h"
#include "discovery/portdiscoveryservice.h"
#include "persistence/preferencesstore.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QInputMethodEvent>
#include <QLineEdit>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QStyleOptionSpinBox>
#include <QTabWidget>
#include <QTextBlock>
#include <QToolButton>
#include <QTemporaryDir>
#include <QtTest>

namespace {

// Every control in the port settings area must be able to paint its own content. The advanced
// combos once shipped with the value clipped away, because their width had been computed by
// hand instead of taken from the widget. This walks the area instead of naming fields, and
// checks the rect the value is painted into rather than the widget's outer box.
QStringList controlsThatClipContent(QWidget *area)
{
    QStringList problems;
    for (auto *combo : area->findChildren<QComboBox *>()) {
        // Editable fields scroll their text, and the serial port row deliberately elides long
        // device names: the popup and the tooltip carry the full label.
        if (!combo->isVisible() || combo->isEditable()
            || combo->objectName() == QLatin1String("serial.port"))
            continue;
        QStyleOptionComboBox option;
        option.initFrom(combo);
        option.currentText = combo->currentText();
        const QRect field = combo->style()->subControlRect(
            QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, combo);
        int widest = 0;
        QString widestItem;
        for (int i = 0; i < combo->count(); ++i) {
            const int width = combo->fontMetrics().horizontalAdvance(combo->itemText(i));
            if (width > widest) { widest = width; widestItem = combo->itemText(i); }
        }
        if (field.width() < widest)
            problems << QString("%1: field %2px cannot show \"%3\" (%4px)")
                            .arg(combo->objectName()).arg(field.width()).arg(widestItem).arg(widest);
    }
    for (auto *spin : area->findChildren<QAbstractSpinBox *>()) {
        if (!spin->isVisible()) continue;
        QStyleOptionSpinBox option;
        option.initFrom(spin);
        const QRect field = spin->style()->subControlRect(
            QStyle::CC_SpinBox, &option, QStyle::SC_SpinBoxEditField, spin);
        // Every value the range allows has to be readable, not just the one showing right now.
        auto *number = qobject_cast<QSpinBox *>(spin);
        QStringList samples;
        if (number) {
            for (const int value : {number->minimum(), number->maximum()})
                samples << number->prefix() + QString::number(value) + number->suffix();
        } else {
            samples << spin->specialValueText();
        }
        for (const auto &sample : samples) {
            const int needed = spin->fontMetrics().horizontalAdvance(sample);
            if (field.width() < needed)
                problems << QString("%1: field %2px cannot show \"%3\" (%4px)")
                                .arg(spin->objectName()).arg(field.width()).arg(sample).arg(needed);
        }
    }
    return problems;
}

} // namespace

class UiSmokeTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QApplication::setStyle("Fusion");
        // 显示选项现在会经 PreferencesStore 落盘（测试模式重定向到 qttest 目录），
        // 但该文件在两次运行之间残留：上一次的格式切换会改变这一次的默认值。
        // 每次从干净配置开始，用例的"默认状态"假设才稳定成立。
        QFile::remove(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
                      + QStringLiteral("/preferences.ini"));
        QFile style(":/styles/workbench.qss");
        QVERIFY(style.open(QIODevice::ReadOnly));
        qApp->setStyleSheet(QString::fromUtf8(style.readAll()));
    }

    void payload_data()
    {
        QTest::addColumn<int>("format");
        QTest::addColumn<QString>("input");
        QTest::addColumn<QByteArray>("lineEnding");
        QTest::addColumn<QByteArray>("expected");
        using F = PayloadCodec::Format;
        QTest::newRow("hex") << int(F::Hex) << "00 7F 80 FF" << QByteArray() << QByteArray::fromHex("007f80ff");
        QTest::newRow("compact") << int(F::Hex) << "007F80FF" << QByteArray() << QByteArray::fromHex("007f80ff");
        QTest::newRow("prefix") << int(F::Hex) << "0x01 0xA0" << QByteArray() << QByteArray::fromHex("01a0");
        QTest::newRow("escape") << int(F::Escape) << "A\\r\\n\\0\\xFF" << QByteArray() << QByteArray::fromHex("410d0a00ff");
        QTest::newRow("utf8") << int(F::Utf8) << QStringLiteral("中") << QByteArray() << QByteArray::fromHex("e4b8ad");
        QTest::newRow("gbk") << int(F::Gbk) << QStringLiteral("你好") << QByteArray() << QByteArray::fromHex("c4e3bac3");
        QTest::newRow("dec") << int(F::Dec) << "1 10 255" << QByteArray() << QByteArray::fromHex("010aff");
        QTest::newRow("dec-comma") << int(F::Dec) << "1, 10;255" << QByteArray() << QByteArray::fromHex("010aff");
        QTest::newRow("base64") << int(F::Base64) << "AUE=" << QByteArray() << QByteArray::fromHex("0141");
        QTest::newRow("base64-ws") << int(F::Base64) << "AU E=\r\n" << QByteArray() << QByteArray::fromHex("0141");
        QTest::newRow("base64-nopad") << int(F::Base64) << "AUE" << QByteArray() << QByteArray::fromHex("0141");
        QTest::newRow("suffix") << int(F::Utf8) << "AT" << QByteArray("\r\n") << QByteArray("AT\r\n");
        QTest::newRow("backslash") << int(F::Escape) << "a\\\\b" << QByteArray() << QByteArray("a\\b");
    }

    void payload()
    {
        QFETCH(int, format);
        QFETCH(QString, input);
        QFETCH(QByteArray, lineEnding);
        QFETCH(QByteArray, expected);
        const auto result = PayloadCodec::parse(input, PayloadCodec::Format(format), lineEnding);
        QVERIFY2(result.valid(), qPrintable(result.error));
        QCOMPARE(result.bytes, expected);
    }

    void invalidPayload_data()
    {
        QTest::addColumn<int>("format");
        QTest::addColumn<QString>("input");
        using F = PayloadCodec::Format;
        QTest::newRow("odd") << int(F::Hex) << "A";
        QTest::newRow("illegal") << int(F::Hex) << "01 GG";
        QTest::newRow("prefix-short") << int(F::Hex) << "0x1";
        QTest::newRow("comma") << int(F::Hex) << "01,02";
        QTest::newRow("dec-range") << int(F::Dec) << "256";
        QTest::newRow("dec-float") << int(F::Dec) << "1.5";
        QTest::newRow("dec-negative") << int(F::Dec) << "-1";
        QTest::newRow("base64-short") << int(F::Base64) << "A";
        QTest::newRow("base64-char") << int(F::Base64) << "AUE*";
        QTest::newRow("base64-extra-pad") << int(F::Base64) << "AUE==";
        QTest::newRow("short-escape") << int(F::Escape) << "\\xF";
        QTest::newRow("unknown-escape") << int(F::Escape) << "\\q";
        QTest::newRow("trailing-escape") << int(F::Escape) << "a\\";
        QTest::newRow("unrepresentable") << int(F::Ascii) << QStringLiteral("中");
    }

    void invalidPayload()
    {
        QFETCH(int, format);
        QFETCH(QString, input);
        const auto result = PayloadCodec::parse(input, PayloadCodec::Format(format));
        QVERIFY(!result.valid());
        QVERIFY(result.bytes.isEmpty());
    }

    void payloadSizeIncludesSuffix()
    {
        QVERIFY(PayloadCodec::parse(QString(65536, 'a'), PayloadCodec::Format::Utf8).valid());
        QVERIFY(!PayloadCodec::parse(QString(65536, 'a'), PayloadCodec::Format::Utf8, "\r\n").valid());
    }

    void renderFormats()
    {
        using F = PayloadCodec::Format;
        // "AB\r\n\t" + UTF-8 的“中”（E4 B8 AD） + 独立的 0xFF：覆盖可打印、控制、多字节与非法序列。
        const auto payload = QByteArray::fromHex("41420d0a09e4b8adff");
        QCOMPARE(PayloadCodec::render(payload, F::Hex), QStringLiteral("41 42 0D 0A 09 E4 B8 AD FF"));
        QCOMPARE(PayloadCodec::render(payload, F::Dec), QStringLiteral("65 66 13 10 9 228 184 173 255"));
        QCOMPARE(PayloadCodec::render(QByteArray("AE"), F::Base64), QStringLiteral("QUU="));
        QCOMPARE(PayloadCodec::render(payload, F::Utf8),
                 QStringLiteral("AB\\r\\n\\t中") + QString(QChar(0xFFFD)));
        QCOMPARE(PayloadCodec::render(QByteArray::fromHex("c4e3bac3"), F::Gbk), QStringLiteral("你好"));
        QCOMPARE(PayloadCodec::render(payload, F::Ascii),
                 QStringLiteral("AB\\r\\n\\t\\xE4\\xB8\\xAD\\xFF"));
        QCOMPARE(PayloadCodec::render(payload, F::Escape),
                 QStringLiteral("AB\\r\\n\\t\\xE4\\xB8\\xAD\\xFF"));
        // \0 与 \\ 的渲染分支：任何字节序列经转义字节渲染后必须能无损还原。
        const auto nulBackslash = QByteArray::fromHex("41005c4200");
        QCOMPARE(PayloadCodec::render(nulBackslash, F::Escape), QStringLiteral("A\\0\\\\B\\0"));
        QCOMPARE(PayloadCodec::parse(PayloadCodec::render(nulBackslash, F::Escape), F::Escape).bytes,
                 nulBackslash);
        // 渲染与解析互为逆变换：字节序列经 HEX/DEC/BASE64/转义字节往返保持不变。
        for (const auto format : {F::Hex, F::Dec, F::Base64, F::Escape}) {
            const auto round = PayloadCodec::parse(PayloadCodec::render(payload, format), format);
            QVERIFY2(round.valid(), qPrintable(round.error));
            QCOMPARE(round.bytes, payload);
        }
        // ASCII 是纯文本编码：可打印字符往返不变；控制字符交给“转义字节”格式表达。
        const QByteArray asciiBytes("AB");
        QCOMPARE(PayloadCodec::parse(PayloadCodec::render(asciiBytes, F::Ascii), F::Ascii).bytes, asciiBytes);
        QCOMPARE(PayloadCodec::parse(QStringLiteral("你好"), F::Gbk).bytes, QByteArray::fromHex("c4e3bac3"));
        // GBK 渲染是容错的：非法序列不报错、不影响后续字节（替换符呈现）。
        const QString gbkMixed = PayloadCodec::render(QByteArray::fromHex("c4e3ff41"), F::Gbk);
        QVERIFY(gbkMixed.startsWith(QStringLiteral("你")));
        QVERIFY(gbkMixed.endsWith(QStringLiteral("A")));
        // 空输入 + 行尾：字节数恰为行尾本身。
        QCOMPARE(PayloadCodec::parse(QString(), F::Utf8, "\r\n").bytes, QByteArray("\r\n"));
        // 输入侧上限（256 KiB 字符）先于输出侧上限（64 KiB 字节）生效。
        QVERIFY(PayloadCodec::parse(QString(256 * 1024 + 1, 'a'), F::Utf8)
                    .error.contains(QStringLiteral("输入过长")));
        // 组合框顺序与名称来自同一份 formats()，两个面板不会各写一份。
        const auto formats = PayloadCodec::formats();
        QCOMPARE(formats.size(), 7);
        QCOMPARE(PayloadCodec::formatName(formats.first()), QStringLiteral("HEX"));
        QCOMPARE(PayloadCodec::formatName(formats.at(3)), QStringLiteral("UTF-8"));
        QCOMPARE(PayloadCodec::formatName(formats.last()), QStringLiteral("转义字节"));
    }

    void filtersPreserveRawData()
    {
        TrafficModel model;
        TrafficFilter filter;
        const auto a = QUuid::createUuid(), b = QUuid::createUuid();
        PortDraft portA, portB;
        portA.id = a;
        portA.kind = PortDraft::Serial;
        portB.id = b;
        portB.kind = PortDraft::TcpServer;

        const auto sample = [&](quint64 id, const PortDraft &port, int index, Traffic::Direction direction,
                                const QByteArray &raw, Traffic::Framing framing = Traffic::Framing::Message) {
            return Traffic::makeSampleMessage(id, QDateTime(), Traffic::makeSource(port, index), direction, raw, framing);
        };

        model.replaceRecords({sample(1, portA, 0, Traffic::Direction::Receive, QByteArray::fromHex("007fff")),
            sample(2, portB, 1, Traffic::Direction::Transmit, "hello")});
        filter.setSourceModel(&model);
        // 端点为独立开关：勾选集合内的才显示，一个都不勾则不显示。
        TrafficFilter::Options options;
        options.endpoints = {a, b};
        options.search = QStringLiteral("7F FF");
        filter.setOptions(options);
        QCOMPARE(filter.rowCount(), 1);
        // 显示格式只影响渲染，模型里的原始字节始终不变。
        model.setFormat(PayloadCodec::Format::Utf8);
        QCOMPARE(model.record(0).raw, QByteArray::fromHex("007fff"));
        options.search.clear();
        options.endpoints = {a};
        options.tx = false;
        filter.setOptions(options);
        QCOMPARE(filter.rowCount(), 1);
        options.endpoints = {b};
        options.rx = false;
        options.tx = true;
        options.search = QStringLiteral("HELLO");
        filter.setOptions(options);
        QCOMPARE(filter.rowCount(), 1);
        options.search.clear();
        options.tx = false;
        filter.setOptions(options);
        QCOMPARE(filter.rowCount(), 0);
        options.endpoints.clear();
        filter.setOptions(options);
        QCOMPARE(filter.rowCount(), 0);
    }

    void messagesCarryStructuredFacts()
    {
        // 消息是值类型：原始字节、时间、来源、长度都是数据，显示设置改不了它们。
        PortDraft port;
        port.kind = PortDraft::TcpServer;
        const auto source = Traffic::makeSource(port, 1);
        QCOMPARE(source.endpointIndex, 1);
        QCOMPARE(source.endpointKind, int(PortDraft::TcpServer));
        QVERIFY(!source.hasPeer()); // 未连接时没有对端，字段留空而不是塞占位串

        Traffic::Message message;
        message.id = 7;
        message.timestamp = QDateTime::fromString(QStringLiteral("2026-10-02T12:00:00.123"), Qt::ISODateWithMs);
        message.direction = Traffic::Direction::Transmit;
        message.origin = Traffic::Origin::Forward;
        message.framing = Traffic::Framing::Stream;
        message.hasMoreFragments = true;
        message.source = source;
        message.raw = QByteArray::fromHex("0102");
        message.rawLength = 4096; // 真实长度远大于保留的预览
        message.truncated = true;
        message.state = Traffic::State::Partial;
        message.requestId = QUuid::createUuid();
        message.routeId = QUuid::createUuid();
        message.frameId = QUuid::createUuid();
        message.durationUs = 1500;

        // 语义查询：方向、来源、发送状态互不干扰。
        QVERIFY(message.isData());
        QVERIFY(message.isFragment());
        QCOMPARE(message.displayBytes(), Q_INT64_C(4096)); // 显示真实长度，不是预览长度
        QVERIFY(message.isValid());
        QCOMPARE(Traffic::directionName(message.direction), QStringLiteral("TX"));
        QCOMPARE(Traffic::originName(message.origin), QStringLiteral("转发"));
        QCOMPARE(Traffic::stateName(message.state), QStringLiteral("部分写出"));
        QCOMPARE(Traffic::framingName(message.framing), QStringLiteral("流片段"));
        // 非数据类消息不参与收发字节统计，isData() 是唯一的判定入口。
        Traffic::Message state;
        state.direction = Traffic::Direction::State;
        QVERIFY(!state.isData());
        QVERIFY(!state.isValid()); // 空消息无任何事实，不该被当成有效记录
    }

    void structuredFiltersUseFactsNotLabels()
    {
        TrafficModel model;
        TrafficFilter filter;
        filter.setSourceModel(&model);

        PortDraft portA, portB;
        portA.kind = PortDraft::Serial;
        portB.kind = PortDraft::TcpServer;
        const auto a = Traffic::makeSource(portA, 0);
        auto b = Traffic::makeSource(portB, 1);
        b.peerAddress = QStringLiteral("127.0.0.1:52344");

        const auto stream = Traffic::makeSampleMessage(
            1, QDateTime(), a, Traffic::Direction::Receive, QByteArray::fromHex("aa"), Traffic::Framing::Stream);
        const auto datagram = Traffic::makeSampleMessage(
            2, QDateTime(), b, Traffic::Direction::Receive, QByteArray::fromHex("bb"), Traffic::Framing::Datagram);
        const auto message = Traffic::makeSampleMessage(
            3, QDateTime(), b, Traffic::Direction::Receive, QByteArray(64, 'z'), Traffic::Framing::Message);
        model.replaceRecords({stream, datagram, message});

        TrafficFilter::Options options;
        options.endpoints = {a.endpointId, b.endpointId};

        // 只看完整消息：按 framing 字段判定，而不是靠"数据内容里有没有换行"这类猜测。
        options.textOnly = true;
        filter.setOptions(options);
        QCOMPARE(filter.rowCount(), 2);
        options.textOnly = false;
        filter.setOptions(options);
        QCOMPARE(filter.rowCount(), 3);

        // 最小长度：按真实字节数，不受预览截断影响。
        options.minBytes = 8;
        filter.setOptions(options);
        QCOMPARE(filter.rowCount(), 1);
        QCOMPARE(model.record(model.rowCount() - 1).raw.size(), 64);
        options.minBytes = 0;

        // 对端地址是来源的结构化字段，能被关键词直接命中，不必先渲染成显示文本。
        options.search = QStringLiteral("52344");
        filter.setOptions(options);
        QCOMPARE(filter.rowCount(), 2);

        // 关掉示例数据：真实的 RX/TX 记录不受影响，示例全部隐藏。
        options.search.clear();
        options.sample = false;
        filter.setOptions(options);
        QCOMPARE(filter.rowCount(), 0);
    }

    void portDraftsRemainIndependent()
    {
        MainWindow window;
        auto *a = window.findChild<PortSelectorWidget *>("endpointA");
        auto *b = window.findChild<PortSelectorWidget *>("endpointB");
        QVERIFY(a && b);
        // Both endpoints are configurable at once; no row has to be selected first.
        QVERIFY(a->isVisibleTo(&window) && b->isVisibleTo(&window));
        auto *serial = a->findChild<QComboBox *>("serial.port");
        auto *bType = b->findChild<QComboBox *>("portType");
        a->setSerialPorts({{"COM17", "USB Serial", {}, {}}});
        serial->setCurrentIndex(serial->findData("COM17"));
        bType->setCurrentIndex(3);
        auto *udpPort = b->findChild<QSpinBox *>("udp.port");
        udpPort->setValue(23456);
        QCOMPARE(a->draft().values.value("serial.port").toString(), QString("COM17"));
        QCOMPARE(a->draft().kind, 0);
        QCOMPARE(b->draft().kind, 3);
        QCOMPARE(b->draft().values.value("udp.port").toInt(), 23456);
        QVERIFY(!b->draft().values.contains("serial.portUnset"));
        QCOMPARE(a->findChild<QLabel *>("EndpointBadge")->text(), QString("A"));
        QCOMPARE(b->findChild<QLabel *>("EndpointBadge")->text(), QString("B"));
    }

    void portInputValidation()
    {
        PortSelectorWidget selector;
        PortDraft port;
        port.values.insert("serial.port", "COM3");
        selector.setDraft(port);
        auto *baud = selector.findChild<QComboBox *>("serial.baud");
        auto *edit = baud->lineEdit();
        QVERIFY(selector.hasValidConfiguration());
        edit->selectAll();
        QTest::keyClicks(edit, "250000xyz");
        QCOMPARE(edit->text(), QString("250000"));
        QInputMethodEvent ime;
        ime.setCommitString(QStringLiteral("我"));
        QApplication::sendEvent(edit, &ime);
        QCOMPARE(edit->text(), QString("250000"));
        QApplication::clipboard()->setText("115200qw");
        edit->selectAll();
        edit->paste();
        QCOMPARE(edit->text(), QString("250000"));
        QVERIFY(selector.hasValidConfiguration());
        edit->clear();
        QVERIFY(!selector.hasValidConfiguration());
        QVERIFY(selector.validationError().contains(QStringLiteral("波特率")));
        for (const auto &invalid : {"0", "-1", "1.5", "2147483648", "115200qw"}) {
            port.values.insert("serial.baud", invalid);
            selector.setDraft(port); // Imported/programmatic drafts must also be checked.
            QVERIFY2(!selector.hasValidConfiguration(), invalid);
        }
        baud->setEditText("2147483647");
        QVERIFY(selector.hasValidConfiguration());
        port = selector.draft();
        port.kind = 2;
        port.values.insert("server.address", "999.1.1.1");
        selector.setDraft(port);
        QVERIFY(!selector.hasValidConfiguration());
        auto *address = selector.findChild<QComboBox *>("server.address");
        address->setEditText("192.168.1.20");
        QVERIFY(selector.hasValidConfiguration());
        address->lineEdit()->selectAll();
        QApplication::clipboard()->setText("192.168.foo.1");
        address->lineEdit()->paste();
        QCOMPARE(address->currentText(), QString("192.168.1.20"));
        address->setEditText("192.168.");
        QVERIFY(!selector.hasValidConfiguration());
        port.kind = 1;
        port.values.insert("client.host", "device-1.example.com");
        selector.setDraft(port);
        QVERIFY(selector.hasValidConfiguration());
        selector.findChild<QComboBox *>("client.host")->setEditText("300.1.2.3");
        QVERIFY(!selector.hasValidConfiguration());
        selector.findChild<QComboBox *>("client.host")->setEditText("localhost");
        auto *remotePort = selector.findChild<QSpinBox *>("client.port");
        auto *portEdit = remotePort->findChild<QLineEdit *>();
        portEdit->selectAll();
        QTest::keyClicks(portEdit, "12345xyz");
        QCOMPARE(remotePort->value(), 12345);
        portEdit->clear();
        QVERIFY(!selector.hasValidConfiguration());
        remotePort->setValue(9000);
        QVERIFY(selector.hasValidConfiguration());
        for (const auto &invalid : {"0", "65536", "abc", "1.5"}) {
            port.values.insert("client.port", invalid);
            selector.setDraft(port);
            QVERIFY2(!selector.hasValidConfiguration(), invalid);
            QCOMPARE(selector.draft().values.value("client.port").toString(), QString(invalid));
        }
        port.values.insert("client.port", 65535);
        selector.setDraft(port);
        QVERIFY(selector.hasValidConfiguration());
        port.kind = 3;
        port.values.insert("udp.target", ""); // Receive-only UDP remains valid.
        selector.setDraft(port);
        QVERIFY(selector.hasValidConfiguration());
    }

    void endpointSummaryTracksConfigurationAndType()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.show();
        auto *selector = window.findChild<PortSelectorWidget *>("endpointA");
        QVERIFY(selector);
        auto *type = selector->findChild<QComboBox *>("portType");
        auto *badge = selector->findChild<QLabel *>("EndpointBadge");
        QVERIFY(type && badge);
        const auto summary = [selector] { return portDraftSummary(selector->draft()); };
        QVERIFY(summary().contains("115200"));
        QVERIFY(summary().contains("8N1"));
        QVERIFY(!summary().contains("127.0.0.1"));
        selector->setSerialPorts({{"COM3", "USB-SERIAL CH340", {}, "COM3"}});
        auto *serial = selector->findChild<QComboBox *>("serial.port");
        serial->setCurrentIndex(serial->findData("COM3"));
        QCOMPARE(summary(), QStringLiteral("COM3 · 115200 · 8N1 · 无流控"));
        selector->findChild<QComboBox *>("serial.baud")->setEditText("921600");
        selector->findChild<QComboBox *>("serial.bits")->setCurrentText("7");
        selector->findChild<QComboBox *>("serial.parity")->setCurrentText("Even");
        selector->findChild<QComboBox *>("serial.stop")->setCurrentText("2");
        selector->findChild<QComboBox *>("serial.flow")->setCurrentText("RTS/CTS");
        QCOMPARE(summary(), QString("COM3 · 921600 · 7E2 · RTS/CTS"));
        QVERIFY(badge->toolTip().contains(QStringLiteral("偶校验")));
        QVERIFY(!badge->toolTip().contains(QStringLiteral("状态"))); // State lives on the button.
        type->setCurrentIndex(1);
        // Identity is derived from the row and the live type; it is never user-edited.
        QCOMPARE(endpointLabel(0, type->currentIndex()), QStringLiteral("A · TCP 客户端"));
        QCOMPARE(summary(), QString("127.0.0.1:9000"));
        // No status label: the action button carries the state for every type.
        QVERIFY(!selector->findChild<QLabel *>("EndpointStatus"));
        auto *action = selector->findChild<QPushButton *>("endpointAction");
        QCOMPARE(action->text(), QStringLiteral("连接"));
        // The glyph follows the action: a link to dial out, an access point for the ones that
        // listen, a broken link once open. Compared as pixels because QIcon lost its
        // comparison operators in Qt 6.
        QVERIFY(!action->icon().isNull());
        const QSize glyphSize(16, 16);
        const QImage connectGlyph = action->icon().pixmap(glyphSize).toImage();
        type->setCurrentIndex(2);
        QVERIFY(action->icon().pixmap(glyphSize).toImage() != connectGlyph);
        type->setCurrentIndex(1);
        QCOMPARE(action->icon().pixmap(glyphSize).toImage(), connectGlyph);
        selector->setConnectionOpen(true);
        QVERIFY(action->icon().pixmap(glyphSize).toImage() != connectGlyph);
        selector->setConnectionOpen(false);
        QCOMPARE(action->icon().pixmap(glyphSize).toImage(), connectGlyph);
        type->setCurrentIndex(2);
        QCOMPARE(action->text(), QStringLiteral("监听"));
        type->setCurrentIndex(3);
        QCOMPARE(action->text(), QStringLiteral("绑定"));
        // Open state flips the verb and the colour flag, without a second indicator.
        selector->setConnectionOpen(true);
        QCOMPARE(action->text(), QStringLiteral("解绑"));
        QCOMPARE(action->property("open").toBool(), true);
        type->setCurrentIndex(2);
        QCOMPARE(action->text(), QStringLiteral("停止"));
        type->setCurrentIndex(1);
        QCOMPARE(action->text(), QStringLiteral("断开"));
        selector->setConnectionOpen(false);
        QCOMPARE(action->text(), QStringLiteral("连接"));
        QCOMPARE(action->property("open").toBool(), false);
        type->setCurrentIndex(0);
        QCOMPARE(endpointLabel(0, type->currentIndex()), QStringLiteral("A · 串口"));
        QCOMPARE(summary(), QString("COM3 · 921600 · 7E2 · RTS/CTS"));
        // The editable name field is gone; there is no "portName" widget any more.
        QVERIFY(!selector->findChild<QLineEdit *>("portName"));
        type->setCurrentIndex(3);
        QCOMPARE(endpointLabel(0, type->currentIndex()), QStringLiteral("A · UDP"));
        type->setCurrentIndex(0);
        for (const auto &setting : QMap<QString, QString>{{"None", "N"}, {"Odd", "O"}, {"Even", "E"},
                                                          {"Mark", "M"}, {"Space", "S"}}.asKeyValueRange()) {
            auto draft = selector->draft();
            draft.values.insert("serial.parity", setting.first);
            QVERIFY(portDraftSummary(draft).contains("7" + setting.second + "2"));
        }
        const auto artifactDir = qEnvironmentVariable("EASYDEBUG_UI_ARTIFACT_DIR");
        if (!artifactDir.isEmpty()) {
            QTest::qWait(30);
            QDir().mkpath(artifactDir);
            QVERIFY(window.grab().save(artifactDir + "/endpoint-summary.png"));
        }
    }

    void serialSelectionUsesPortIdentity()
    {
        PortSelectorWidget selector;
        PortDraft port;
        selector.setDraft(port);
        selector.setSerialPorts({{"COM3", "USB-SERIAL CH340", {}, "COM3"},
                                 {"COM17", "USB Serial Port", {}, "COM17"}});
        auto *combo = selector.findChild<QComboBox *>("serial.port");
        QVERIFY(combo && !combo->isEditable());
        QCOMPARE(combo->count(), 2);
        QCOMPARE(combo->currentIndex(), -1); // Scanning must not silently choose a device.
        combo->setCurrentIndex(combo->findData("COM17"));
        QVERIFY(combo->currentText().contains("USB Serial Port"));
        QCOMPARE(selector.draft().values.value("serial.port").toString(), QString("COM17"));
        QVERIFY(selector.hasValidConfiguration());
        selector.setSerialPorts({{"COM17", "Updated friendly name", {}, "COM17"},
                                 {"COM3", "USB-SERIAL CH340", {}, "COM3"}});
        QCOMPARE(combo->currentData().toString(), QString("COM17"));
        QVERIFY(combo->currentText().contains("Updated friendly name"));
        selector.setSerialPorts({{"COM3", "USB-SERIAL CH340", {}, "COM3"}});
        QCOMPARE(combo->currentData().toString(), QString("COM17"));
        QVERIFY(combo->currentText().contains(QStringLiteral("不可用")));
        QVERIFY(!selector.hasValidConfiguration());
        selector.setSerialPorts({{"COM17", "USB Serial Port", {}, "COM17"}});
        QVERIFY(selector.hasValidConfiguration());
        // 手动刷新按钮已移除：扫描由组合根的定时器驱动，服务侧行为见 serialDiscoveryPublishesSnapshot。
    }

    void serialDiscoveryPublishesSnapshot()
    {
        PortDiscoveryService discovery;
        QSignalSpy snapshots(&discovery, &PortDiscoveryService::portsChanged);
        QSignalSpy busy(&discovery, &PortDiscoveryService::refreshingChanged);
        discovery.refresh();
        discovery.refresh(); // Coalesce repeated clicks while discovery is running.
        QCOMPARE(busy.count(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(snapshots.count(), 1, 10000);
        QCOMPARE(busy.count(), 2);
        QVERIFY(!busy.last().first().toBool());
        const auto ports = qvariant_cast<QList<SerialPortDescriptor>>(snapshots.first().first());
        QSet<QString> names;
        for (const auto &port : ports) {
            QVERIFY(!port.portName.isEmpty());
            QVERIFY(!names.contains(port.portName));
            names.insert(port.portName);
        }
    }

    void addressHistoryPersistsAndIsBounded()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto fileName = directory.filePath("preferences.ini");
        {
            PreferencesStore store(nullptr, fileName);
            store.rememberAddress("bad address", false);
            store.rememberAddress("999.1.1.1", false);
            store.rememberAddress("192.168.", true);
            QVERIFY(store.addressHistory(false).isEmpty());
            QVERIFY(store.addressHistory(true).isEmpty());
            for (int i = 1; i <= 25; ++i) store.rememberAddress(QString("10.0.0.%1").arg(i), false);
            QCOMPARE(store.addressHistory(false).size(), 20);
            QVERIFY(!store.addressHistory(false).contains("10.0.0.1"));
            store.rememberAddress("10.0.0.10", false);
            QCOMPARE(store.addressHistory(false).first(), QString("10.0.0.10"));
            QCOMPARE(store.addressHistory(false).size(), 20);
            store.rememberAddress("Device.Example", false);
            store.rememberAddress("DEVICE.EXAMPLE", false);
            QCOMPARE(store.addressHistory(false).count("device.example"), 1);
            store.rememberAddress("0.0.0.0", true);
            store.rememberAddress("device.example", true);
        }
        PreferencesStore reloaded(nullptr, fileName);
        QCOMPARE(reloaded.addressHistory(false).first(), QString("device.example"));
        QCOMPARE(reloaded.addressHistory(false).size(), 20);
        QCOMPARE(reloaded.addressHistory(true), QStringList{"0.0.0.0"});
    }

    void addressEditsCommitOnCompletion()
    {
        QTemporaryDir directory;
        PreferencesStore store(nullptr, directory.filePath("preferences.ini"));
        PortSelectorWidget selector;
        selector.setAttribute(Qt::WA_DontShowOnScreen);
        selector.resize(980, 96);
        const auto update = [&] { selector.setAddressHistory(store.addressHistory(false), store.addressHistory(true)); };
        connect(&selector, &PortSelectorWidget::addressCommitted, &store, &PreferencesStore::rememberAddress);
        connect(&store, &PreferencesStore::addressHistoryChanged, &selector, update);
        PortDraft port;
        port.kind = 1;
        selector.setDraft(port);
        selector.show();
        auto *client = selector.findChild<QComboBox *>("client.host");
        auto *udp = selector.findChild<QComboBox *>("udp.target");
        auto *edit = client->lineEdit();
        edit->setFocus();
        edit->selectAll();
        QTest::keyClicks(edit, "192.168.8.10");
        QVERIFY(store.addressHistory(false).isEmpty());
        QTest::keyClick(edit, Qt::Key_Return);
        QCOMPARE(store.addressHistory(false).first(), QString("192.168.8.10"));
        QVERIFY(udp->findText("192.168.8.10") >= 0);
        edit->selectAll();
        QTest::keyClicks(edit, "device.example");
        QTest::keyClick(edit, Qt::Key_Return);
        QVERIFY(client->findText("device.example") >= 0);
        QCOMPARE(udp->findText("device.example"), -1); // UDP only offers IPv4 destinations.
        edit->selectAll();
        QTest::keyClicks(edit, "10.20.30.40");
        selector.setDraft(port); // Switching endpoints also commits the last complete edit.
        QCOMPARE(store.addressHistory(false).first(), QString("10.20.30.40"));
        edit->selectAll();
        QTest::keyClicks(edit, "999.1.1.1");
        selector.commitAddressHistory();
        QVERIFY(!store.addressHistory(false).contains("999.1.1.1"));
        QCOMPARE(store.addressHistory(true).size(), 0);
        PreferencesStore reloaded(nullptr, directory.filePath("preferences.ini"));
        PortSelectorWidget reopened;
        reopened.setAddressHistory(reloaded.addressHistory(false), reloaded.addressHistory(true));
        QVERIFY(reopened.findChild<QComboBox *>("client.host")->findText("10.20.30.40") >= 0);
    }

    void trafficWrapSwitchAdjustsView()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        window.loadExample();
        // 摘除组合根注入的偏好存储：本用例改显示开关，不该写进真实用户的 preferences.ini。
        window.findChild<TrafficPanel *>()->setPreferences(nullptr);
        // 文本显示模式默认开启，表格被收进堆栈且跳过布局；本用例读表格行高，先切回表格。
        window.findChild<QAction *>(QStringLiteral("textMode"))->setChecked(false);
        auto *wrapAction = window.findChild<QAction *>(QStringLiteral("wrapTraffic"));
        auto *table = window.findChild<TrafficLogView *>(QStringLiteral("trafficTable"));
        auto *model = window.findChild<TrafficModel *>();
        QVERIFY(wrapAction && table && model);
        QCOMPARE(wrapAction->isChecked(), false);
        QCOMPARE(table->wordWrap(), false);
        QCOMPARE(table->defaultRowHeight(), 33);

        // 换行行高按各条内容的实际行数给：先让长报文进模型，才能看出行高是否跟着内容走。
        const QUuid endpointId = window.findChild<QPushButton *>(QStringLiteral("showPortA"))
                                     ->property("endpoint").toUuid();
        QByteArray longRaw(1024, Qt::Uninitialized);
        for (int i = 0; i < longRaw.size(); ++i)
            longRaw[i] = char(i & 0xFF);
        Traffic::Message longMessage;
        longMessage.id = 999001;
        longMessage.direction = Traffic::Direction::Receive;
        longMessage.source.endpointId = endpointId;
        longMessage.source.endpointIndex = 0;
        longMessage.source.endpointKind = PortDraft::Serial;
        longMessage.raw = longRaw;
        longMessage.rawLength = longRaw.size();

        wrapAction->setChecked(true);
        QTRY_COMPARE(table->wordWrap(), true);
        const int lineHeight = table->fontMetrics().height();
        // 单行高度 = 一行文本 + 绘制余量；余量从"单行基线"里反推，测试不复制实现常数。
        const int lineUnit = table->defaultRowHeight();
        const int padding = lineUnit - lineHeight;
        QVERIFY2(lineUnit > 0 && padding >= 0, "换行模式的行高基线应是一行文本");

        auto *proxy = qobject_cast<QSortFilterProxyModel *>(table->model());
        QVERIFY(proxy);
        // 行高 = 该行内容需要的行数 × 行高单位 + 绘制余量。逐行独立算，
        // 不是全表一个常数——这正是"短报文不再占着长报文高度"的判据。
        const auto expectedHeight = [&](int viewRow) {
            const int sourceRow = proxy->mapToSource(proxy->index(viewRow, 0)).row();
            return model->wrappedLines(sourceRow) * lineHeight + padding;
        };

        const int shortRows = table->model()->rowCount();
        QVERIFY(shortRows > 0);
        const int shortSourceRow = proxy->mapToSource(proxy->index(0, 0)).row();
        const int shortLines = model->wrappedLines(shortSourceRow);

        // 一条 1KB 报文（3071 字符 HEX）必须占更多行，且被折行上限钳住。
        window.findChild<TrafficPanel *>()->appendMessages({longMessage});
        QTRY_COMPARE(model->rowCount(), shortRows + 1);
        const int longViewRow = table->model()->rowCount() - 1;
        const int longSourceRow = proxy->mapToSource(proxy->index(longViewRow, 0)).row();
        QVERIFY2(model->wrappedLines(longSourceRow) > shortLines,
                 "1KB 报文必须比十几字节的示例报文占更多行");
        QVERIFY2(model->wrappedLines(longSourceRow) <= 6, "折行行数必须被上限钳住");

        // 列宽要等布局与预算防抖落地，行高才按内容算得出来，所以轮询等待收敛。
        QTRY_COMPARE(table->rowHeight(longViewRow), expectedHeight(longViewRow));
        QVERIFY2(table->rowHeight(longViewRow) > lineUnit,
                 "长报文行高必须高于单行基线");
        // 前面的短报文各行按自己的内容定高，没有被长报文带着一起变高。
        for (int row = 0; row < shortRows; ++row)
            QCOMPARE(table->rowHeight(row), expectedHeight(row));

        // 连续大批追加并触发头部淘汰后，行高仍必须跟内容走。
        // 旧的 QHeaderView section 会在"删头部 + 加尾部"之后丢高度，折行看起来就停了。
        quint64 nextId = 100000;
        for (int round = 0; round < 2; ++round) {
            QList<Traffic::Message> batch;
            batch.reserve(2500);
            for (int i = 0; i < 2499; ++i) {
                auto message = longMessage;
                message.id = nextId++;
                batch.append(message);
            }
            auto tail = longMessage;
            tail.id = nextId++;
            if (round == 1) {
                tail.raw = QByteArray::fromHex("0102");
                tail.rawLength = tail.raw.size();
            }
            batch.append(tail);
            window.findChild<TrafficPanel *>()->appendMessages(batch);
            QTRY_COMPARE(model->record(model->rowCount() - 1).id, nextId - 1);
        }
        QVERIFY2(model->rowCount() < 5000, "字节预算应该已经从头部淘汰旧记录");
        const int floodedRows = table->model()->rowCount();
        const int tailRow = floodedRows - 1;
        const int beforeTail = floodedRows - 2;
        QTRY_COMPARE(table->rowHeight(tailRow), expectedHeight(tailRow));
        QTRY_COMPARE(table->rowHeight(beforeTail), expectedHeight(beforeTail));
        QTRY_COMPARE(table->rowHeight(0), expectedHeight(0));
        QVERIFY2(model->wrappedLines(proxy->mapToSource(proxy->index(tailRow, 0)).row()) == 1,
                 "尾部短报文必须仍是一行");
        QVERIFY2(table->rowHeight(beforeTail) > table->rowHeight(tailRow),
                 "头部淘汰之后，长报文行高不能塌成短报文的高度");

        // 切回单行省略：所有行回到统一 33px。
        wrapAction->setChecked(false);
        QTRY_COMPARE(table->wordWrap(), false);
        QCOMPARE(table->defaultRowHeight(), 33);
        for (int row = 0; row < table->model()->rowCount(); ++row)
            QCOMPARE(table->rowHeight(row), 33);
    }

    // 大报文只在绘制那一份上裁剪，数据本身必须保持完整。
    // 委托会为拿到的整段文本建立折行 / 省略布局，文本长度直接决定每帧成本：
    // 实测 4KB 报文（12287 字符 HEX）不裁剪时单次整视口重绘要 441ms，
    // 按可见行数裁到千字符以内后降到 20ms 上下。
    void hugeMessagesRenderBoundedTextOnly()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        window.findChild<TrafficPanel *>()->setPreferences(nullptr);
        // 本用例验证字符预算对绘制文本的裁剪，预算只在表格可见时计算，先切回表格。
        window.findChild<QAction *>(QStringLiteral("textMode"))->setChecked(false);
        auto *wrapAction = window.findChild<QAction *>(QStringLiteral("wrapTraffic"));
        auto *table = window.findChild<TrafficLogView *>(QStringLiteral("trafficTable"));
        auto *model = window.findChild<TrafficModel *>();
        QVERIFY(wrapAction && table && model);
        QTest::qWait(50); // 等列宽完成布局，字符预算才能按真实列宽算
        wrapAction->setChecked(true);

        // 真实形状的 4KB 报文：每个字节都不同，HEX 渲染后 12287 字符。
        QByteArray raw(4096, Qt::Uninitialized);
        for (int i = 0; i < raw.size(); ++i)
            raw[i] = char(i & 0xFF);
        const auto full = PayloadCodec::render(raw, PayloadCodec::Format::Hex);
        QCOMPARE(full.size(), 12287);

        Traffic::Message message;
        message.id = 1;
        message.direction = Traffic::Direction::Receive;
        message.source.endpointIndex = 0;
        message.source.endpointKind = PortDraft::Serial;
        message.source.endpointId = QUuid::createUuid();
        message.raw = raw;
        message.rawLength = raw.size();
        window.findChild<TrafficPanel *>()->appendMessages({message});
        QTRY_COMPARE(model->rowCount(), 1);

        const auto shownText = [model]() {
            return model->index(0, TrafficModel::DataColumn).data(Qt::DisplayRole).toString();
        };
        QTRY_VERIFY2(shownText().size() < full.size() / 2,
                     qPrintable(QStringLiteral("绘制文本未按列宽裁剪：%1 / %2 字符")
                                    .arg(shownText().size()).arg(full.size())));
        QVERIFY2(shownText().startsWith(full.left(64)), "裁剪必须保留开头，改的是尾部看不见的部分");
        QVERIFY(shownText().endsWith(QStringLiteral("…")));

        // 内存里仍然是完整的结构化数据：原始字节一字未动，真实长度单独保留。
        QCOMPARE(model->record(0).raw.size(), raw.size());
        QCOMPARE(model->record(0).displayBytes(), qint64(raw.size()));
        // 完整内容仍可从悬停提示与导出拿到，裁剪不影响排障。
        QVERIFY(model->index(0, TrafficModel::DataColumn).data(Qt::ToolTipRole).toString().contains(full));
    }

    // 历史上限必须同时限字节和条数。只限字节时，串口遥测那种几十字节的小帧
    // 能把条数堆到六万以上，每条 Message 的元数据（时间、来源、三个 QUuid、
    // 两个 QString）远大于它那点载荷——"限了字节"不等于"限了内存"。
    void historyBudgetAlsoLimitsRowCount()
    {
        TrafficModel model;
        Traffic::Message tiny;
        tiny.direction = Traffic::Direction::Receive;
        tiny.source.endpointIndex = 0;
        tiny.source.endpointKind = PortDraft::Serial;
        tiny.source.endpointId = QUuid::createUuid();
        tiny.raw = QByteArray::fromHex("0102"); // 2 字节：4 MiB 字节预算能装两百万条
        tiny.rawLength = tiny.raw.size();

        quint64 id = 1;
        for (int round = 0; round < 30; ++round) {
            QList<Traffic::Message> batch;
            batch.reserve(1000);
            for (int i = 0; i < 1000; ++i) {
                auto message = tiny;
                message.id = id++;
                batch.append(message);
            }
            model.appendRecords(batch);
        }
        QCOMPARE(model.rowCount(), 20000);
        // 淘汰只从最旧一端发生：最新的一定在，最旧的恰好被挤掉。
        QCOMPARE(model.record(model.rowCount() - 1).id, id - 1);
        QCOMPARE(model.record(0).id, id - 20000);
    }

    void displayPreferencesPersistAcrossPanels()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto fileName = directory.filePath("preferences.ini");
        // 第一组面板：改显示开关即落盘（PreferencesStore::setValue 带 sync）。
        PreferencesStore firstStore(nullptr, fileName);
        TrafficPanel firstTraffic;
        firstTraffic.setPreferences(&firstStore);
        SendPanel firstSender;
        firstSender.setPreferences(&firstStore);
        auto *wrap = firstTraffic.findChild<QAction *>(QStringLiteral("wrapTraffic"));
        auto *rx = firstTraffic.findChild<QAction *>(QStringLiteral("showRx"));
        auto *format = firstTraffic.findChild<QComboBox *>(QStringLiteral("trafficFormat"));
        auto *sendFormat = firstSender.findChild<QComboBox *>(QStringLiteral("sendFormat"));
        QVERIFY(wrap && rx && format && sendFormat);
        wrap->setChecked(true);
        rx->setChecked(false);
        format->setCurrentText(QStringLiteral("UTF-8"));
        sendFormat->setCurrentText(QStringLiteral("GBK"));
        // 第二组面板：从同一份配置读回，用户上次的选择原样恢复。
        PreferencesStore secondStore(nullptr, fileName);
        TrafficPanel secondTraffic;
        secondTraffic.setPreferences(&secondStore);
        SendPanel secondSender;
        secondSender.setPreferences(&secondStore);
        QCOMPARE(secondTraffic.findChild<QAction *>(QStringLiteral("wrapTraffic"))->isChecked(), true);
        QCOMPARE(secondTraffic.findChild<QAction *>(QStringLiteral("showRx"))->isChecked(), false);
        QCOMPARE(secondTraffic.findChild<QComboBox *>(QStringLiteral("trafficFormat"))->currentText(),
                 QStringLiteral("UTF-8"));
        QCOMPARE(secondSender.findChild<QComboBox *>(QStringLiteral("sendFormat"))->currentText(),
                 QStringLiteral("GBK"));
    }

    void logRecordingFollowsDisplayFormat()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto logPath = directory.filePath(QStringLiteral("traffic.log"));
        PreferencesStore store(nullptr, directory.filePath(QStringLiteral("preferences.ini")));
        // 预置路径：记录开关开启时若无路径会弹文件对话框，注入路径绕开交互。
        store.setValue(QStringLiteral("traffic/recordPath"), logPath);
        TrafficPanel panel;
        panel.setPreferences(&store);
        auto *record = panel.findChild<QAction *>(QStringLiteral("recordTraffic"));
        auto *format = panel.findChild<QComboBox *>(QStringLiteral("trafficFormat"));
        QVERIFY(record && format);
        QCOMPARE(format->currentText(), QStringLiteral("HEX"));
        record->setChecked(true);

        Traffic::Message rx;
        rx.direction = Traffic::Direction::Receive;
        rx.source.endpointIndex = 0;
        rx.source.endpointKind = PortDraft::Serial;
        rx.source.endpointId = QUuid::createUuid();
        rx.raw = QByteArray::fromHex("0102");
        Traffic::Message tx;
        tx.direction = Traffic::Direction::Transmit;
        tx.origin = Traffic::Origin::Manual;
        tx.source.endpointIndex = 1;
        tx.source.endpointKind = PortDraft::TcpServer;
        tx.source.endpointId = QUuid::createUuid();
        tx.raw = QByteArray::fromHex("FF");
        panel.appendMessages({rx, tx});
        // 批入模是 16ms 异步的，轮询模型行数即等到了写日志的同一拍。
        auto *model = panel.findChild<TrafficModel *>();
        QVERIFY(model);
        QTRY_COMPARE(model->rowCount(), 2);

        QFile log(logPath);
        QVERIFY(log.open(QIODevice::ReadOnly | QIODevice::Text));
        const auto hexLines = QString::fromUtf8(log.readAll()).trimmed().split(QLatin1Char('\n'));
        QCOMPARE(hexLines.size(), 2);
        QVERIFY(hexLines.at(0).contains(QStringLiteral("A · 串口")));
        QVERIFY(hexLines.at(0).contains(QStringLiteral("RX")));
        QVERIFY(hexLines.at(0).endsWith(QStringLiteral("01 02"))); // HEX 模式记 HEX
        QVERIFY(hexLines.at(1).endsWith(QStringLiteral("FF")));
        log.close();

        // 切换显示格式后新行跟随新模式：所见即所记。
        format->setCurrentText(QStringLiteral("UTF-8"));
        Traffic::Message text;
        text.direction = Traffic::Direction::Receive;
        text.source.endpointIndex = 0;
        text.source.endpointKind = PortDraft::Serial;
        text.source.endpointId = QUuid::createUuid();
        text.raw = "ok";
        panel.appendMessages({text});
        QTRY_COMPARE(model->rowCount(), 3);
        QVERIFY(log.open(QIODevice::ReadOnly | QIODevice::Text));
        const auto all = QString::fromUtf8(log.readAll()).trimmed().split(QLatin1Char('\n'));
        QCOMPARE(all.size(), 3);
        QVERIFY(all.at(2).endsWith(QStringLiteral("ok")));
    }

    void endpointRowsStayVisibleAndInline()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(50);
        // 文本显示模式默认开启，表格被收进堆栈；本用例验证表格列的可见性，先切回表格。
        if (auto *textModeAction = window.findChild<QAction *>(QStringLiteral("textMode")))
            textModeAction->setChecked(false);
        auto *a = window.findChild<PortSelectorWidget *>("endpointA");
        auto *b = window.findChild<PortSelectorWidget *>("endpointB");
        auto *traffic = window.findChild<TrafficLogView *>("trafficTable");
        QVERIFY(a && b && traffic);
        const auto fits = [&window](QWidget *w) {
            return window.rect().contains(QRect(w->mapTo(&window, QPoint()), w->size()));
        };
        // Each endpoint keeps its parameters on one row, both rows reachable without scrolling.
        for (auto *selector : {a, b}) {
            QVERIFY(fits(selector));
            auto *type = selector->findChild<QComboBox *>("portType");
            auto *badge = selector->findChild<QLabel *>("EndpointBadge");
            auto *action = selector->findChild<QPushButton *>("endpointAction");
            QVERIFY(type && badge && action);
            // Same row: the badge and the type selector overlap vertically.
            const int badgeMid = badge->mapTo(&window, QPoint()).y() + badge->height() / 2;
            const int typeTop = type->mapTo(&window, QPoint()).y();
            QVERIFY(badgeMid >= typeTop && badgeMid <= typeTop + type->height());
            QVERIFY(fits(action));
            // 动作按钮的可用性跟随配置校验：草稿有效即可打开（通信后端已接线），
            // 无效则禁用。默认草稿下 A（串口未选设备）无效，B（TCP 服务器默认值）有效。
            QCOMPARE(action->isEnabled(), selector->hasValidConfiguration());
        }
        QVERIFY(a->mapTo(&window, QPoint()).y() < b->mapTo(&window, QPoint()).y());

        // The bridge block sits beside A/B, not on a row of its own below them.
        auto *bridgeBlock = window.findChild<QWidget *>("BridgeRow");
        auto *bar = window.findChild<QWidget *>("EndpointBar");
        QVERIFY(bridgeBlock && bar);
        QVERIFY(bridgeBlock->mapTo(&window, QPoint()).x() > a->mapTo(&window, QPoint()).x() + a->width() - 1);
        const int bridgeTop = bridgeBlock->mapTo(&window, QPoint()).y();
        const int bridgeBottom = bridgeTop + bridgeBlock->height();
        QVERIFY(bridgeTop <= a->mapTo(&window, QPoint()).y() + a->height());
        QVERIFY(bridgeBottom >= b->mapTo(&window, QPoint()).y());
        QVERIFY(fits(bridgeBlock));
        // Collapsed, the bar is just two endpoint rows; a third bridge row would add to this.
        QVERIFY2(bar->height() <= 96, qPrintable(QString::number(bar->height())));
        // No inline error widget at all; the invalid field is marked by its own border.
        QVERIFY(!a->findChild<QLabel *>("portValidationError"));
        QVERIFY(!a->hasValidConfiguration());
        auto *serialField = a->findChild<QComboBox *>("serial.port");
        QCOMPARE(serialField->property("invalid").toBool(), true);
        QVERIFY(serialField->toolTip().contains(QStringLiteral("请选择设备")));

        // No combo may clip its own items: a fixed width breaks under translation.
        for (auto *box : window.findChildren<QComboBox *>()) {
            // serial.port deliberately takes spare row width instead of fitting its items,
            // so a long device name elides rather than widening the window.
            if (box->objectName() == "serial.port") continue;
            if (box->isEditable() || !box->isVisible() || box->count() == 0) continue;
            int widest = 0;
            for (int i = 0; i < box->count(); ++i)
                widest = qMax(widest, box->fontMetrics().horizontalAdvance(box->itemText(i)));
            QVERIFY2(box->width() >= widest + 24,
                     qPrintable(QString("%1: %2 < %3 + 24").arg(box->objectName())
                                    .arg(box->width()).arg(widest)));
        }

        // A and B line up: same type-combo edge and same first-parameter edge for any pair of types.
        const auto leadFieldX = [&window](PortSelectorWidget *s) {
            for (const QString &key : {"serial.port", "client.host", "server.address", "udp.address"}) {
                auto *field = s->findChild<QWidget *>(key);
                if (field && field->isVisible()) return field->mapTo(&window, QPoint()).x();
            }
            return -1;
        };
        for (int kindA = 0; kindA < 4; ++kindA) {
            a->findChild<QComboBox *>("portType")->setCurrentIndex(kindA);
            for (int kindB = 0; kindB < 4; ++kindB) {
                b->findChild<QComboBox *>("portType")->setCurrentIndex(kindB);
                QTest::qWait(20);
                QCOMPARE(b->findChild<QComboBox *>("portType")->mapTo(&window, QPoint()).x(),
                         a->findChild<QComboBox *>("portType")->mapTo(&window, QPoint()).x());
                const int xa = leadFieldX(a), xb = leadFieldX(b);
                QVERIFY(xa > 0 && xb > 0);
                QVERIFY2(xa == xb, qPrintable(QString("kinds %1/%2: %3 vs %4")
                                                  .arg(kindA).arg(kindB).arg(xa).arg(xb)));
            }
        }
        a->findChild<QComboBox *>("portType")->setCurrentIndex(0);
        b->findChild<QComboBox *>("portType")->setCurrentIndex(2);
        QTest::qWait(20);

        auto *splitter = window.findChild<QSplitter *>("dataSplitter");
        QVERIFY(splitter);
        auto *trafficPanel = splitter->widget(0);
        const int trafficTop = trafficPanel->mapTo(&window, QPoint()).y();
        auto *advanced = a->findChild<QToolButton *>("advancedToggle");
        auto *flow = a->findChild<QComboBox *>("serial.flow");
        QVERIFY(advanced && flow && !flow->isVisible());
        // QToolButton draws an arrow instead of its icon, so the disclosure state now
        // lives in the icon and its hint; the arrow must stay off or the glyph vanishes.
        QCOMPARE(advanced->arrowType(), Qt::NoArrow);
        QVERIFY(!advanced->icon().isNull());
        // QIcon lost its comparison operators in Qt 6, so the state cue is asserted
        // through the hint text that flips with the checked state.
        QCOMPARE(advanced->toolTip(), QStringLiteral("展开该类型的高级参数"));
        advanced->click();
        QTest::qWait(30);
        QTRY_VERIFY(flow->isVisible());
        QCOMPARE(advanced->toolTip(), QStringLiteral("收起该类型的高级参数"));
        QVERIFY(!advanced->icon().isNull());
        // Icon only, and sized in code so it lines up with the action button beside it.
        QCOMPARE(advanced->toolButtonStyle(), Qt::ToolButtonIconOnly);
        // The word is kept for the accessible name and as the fallback when no icon loads.
        QCOMPARE(advanced->text(), QStringLiteral("高级"));
        QCOMPARE(advanced->accessibleName(), QStringLiteral("高级参数"));
        auto *endpointAction = a->findChild<QPushButton *>("endpointAction");
        QVERIFY(endpointAction);
        QCOMPARE(advanced->height(), endpointAction->height());
        // Advanced combos carry short values ("8", "1.5", "None"); their fixed width must
        // still show the widest item, otherwise the selected value looks unset.
        // Nothing in the endpoint row may hide its own content while it is expanded.
        const auto clipped = controlsThatClipContent(a);
        QVERIFY2(clipped.isEmpty(), qPrintable(clipped.join("; ")));
        QVERIFY(fits(flow));
        QVERIFY(fits(a->findChild<QComboBox *>("serial.parity")));
        QVERIFY(!a->findChild<QLineEdit *>("portName")); // The name field is gone for good.
        // Expanding advanced parameters pushes the data area down, it must not overflow it.
        // Expanding advanced parameters pushes the data area down; it must still fit.
        QVERIFY(trafficPanel->mapTo(&window, QPoint()).y() > trafficTop);
        QVERIFY(fits(trafficPanel));
        QVERIFY(fits(splitter->widget(1)));
        QVERIFY(fits(window.findChild<QPlainTextEdit *>("sendEditor")));
        window.loadExample(); // With rows present the table replaces the empty state.
        QTest::qWait(30);
        QVERIFY(traffic->isVisible());
        QVERIFY(fits(traffic));
        const auto snapshotDir = qEnvironmentVariable("EASYDEBUG_UI_ARTIFACT_DIR");
        if (!snapshotDir.isEmpty()) {
            QDir().mkpath(snapshotDir);
            QVERIFY(window.grab().save(snapshotDir + "/endpoints-expanded.png"));
        }
        advanced->click();
        QTest::qWait(30);
        QTRY_VERIFY(!flow->isVisible());

        // The combo takes the row's spare width so a typical friendly name reads in full.
        a->setSerialPorts({{"COM3", "USB-SERIAL CH340", {}, "COM3"}});
        auto *serialBox = a->findChild<QComboBox *>("serial.port");
        serialBox->setCurrentIndex(serialBox->findData("COM3"));
        QTest::qWait(20);
        const int textWidth = serialBox->fontMetrics().horizontalAdvance(serialBox->currentText());
        QVERIFY2(serialBox->width() >= textWidth + 24,
                 qPrintable(QString("box %1 vs text %2").arg(serialBox->width()).arg(textWidth)));
        QVERIFY(fits(serialBox));
        // Longer names cannot widen the window; the popup and tooltip keep them readable.
        a->setSerialPorts({{"COM3", "USB-SERIAL CH340 (Prolific adapter)", {}, "COM3"}});
        serialBox->setCurrentIndex(serialBox->findData("COM3"));
        QTest::qWait(20);
        QVERIFY(fits(serialBox));
        QVERIFY(serialBox->view()->minimumWidth()
                >= serialBox->fontMetrics().horizontalAdvance(serialBox->currentText()));

        // Every type keeps its fields inside the row at 1280 width.
        for (int kind = 0; kind < 4; ++kind) {
            b->findChild<QComboBox *>("portType")->setCurrentIndex(kind);
            QTest::qWait(30);
            for (const QString &key : {"serial.port", "serial.baud", "client.host", "client.port",
                                       "server.address", "server.port", "udp.address", "udp.port",
                                       "udp.target", "udp.targetPort"}) {
                auto *field = b->findChild<QWidget *>(key);
                if (field && field->isVisible()) QVERIFY2(fits(field), qPrintable(key));
            }
            if (!snapshotDir.isEmpty())
                QVERIFY(window.grab().save(snapshotDir + QString("/endpoint-kind-%1.png").arg(kind)));
        }
    }

    void portControlsShowTheirOwnContent()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);
        // Both endpoints, all four types, collapsed and expanded: a control that cannot show
        // its own value is a bug wherever it sits in the row.
        for (auto *selector : window.findChildren<PortSelectorWidget *>()) {
            auto *type = selector->findChild<QComboBox *>("portType");
            auto *advanced = selector->findChild<QToolButton *>("advancedToggle");
            QVERIFY(type && advanced);
            for (const bool expanded : {false, true}) {
                if (advanced->isChecked() != expanded) {
                    advanced->click();
                    QTest::qWait(20);
                }
                for (int kind = 0; kind < 4; ++kind) {
                    type->setCurrentIndex(kind);
                    QTest::qWait(20);
                    const auto clipped = controlsThatClipContent(selector);
                    QVERIFY2(clipped.isEmpty(),
                             qPrintable(QString("%1 %2: %3").arg(selector->objectName(),
                                         expanded ? QStringLiteral("expanded")
                                                  : QStringLiteral("collapsed"),
                                         clipped.join("; "))));
                }
            }
        }
    }

    void sendPreviewRejectsInvalidInput()
    {
        MainWindow window;
        auto *editor = window.findChild<QPlainTextEdit *>("sendEditor");
        auto *length = window.findChild<QLabel *>("ByteCount");
        QVERIFY(editor && length);
        // 存为命令随快捷命令页一起撤下：命令列表回归前，发送区不留没有落点的按钮。
        QVERIFY(!window.findChild<QPushButton *>("saveCommand"));
        // 预览阶段发送按钮恒为禁用（尚未连接端口），与输入合法性无关。
        for (auto *button : window.findChildren<QPushButton *>("PrimaryButton"))
            QVERIFY(!button->isEnabled());
        auto *preview = window.findChild<QLabel *>("payloadPreview");
        QVERIFY(preview);
        editor->setPlainText("01 GG");
        QCOMPARE(length->text(), QStringLiteral("输入错误"));
        QVERIFY(preview->text().contains(QStringLiteral("无效 HEX"))); // 错误原因展示在预览位置
        editor->setPlainText("00 FF");
        QCOMPARE(length->text(), QStringLiteral("2 字节"));
        QCOMPARE(preview->text(), QStringLiteral("00 FF"));
        // 格式/末尾切换的接线：预览字节数必须跟随变化。
        editor->setPlainText("01 03 00 00 00 02 C4 0B");
        QCOMPARE(length->text(), QStringLiteral("8 字节"));
        window.findChild<QComboBox *>("sendSuffix")->setCurrentIndex(3); // CRLF
        QCOMPARE(length->text(), QStringLiteral("10 字节"));
        window.findChild<QComboBox *>("sendSuffix")->setCurrentIndex(0);
        window.findChild<QComboBox *>("sendFormat")->setCurrentIndex(3); // UTF-8：正文按文本编码
        QCOMPARE(length->text(), QStringLiteral("23 字节"));
        window.findChild<QComboBox *>("sendFormat")->setCurrentIndex(0); // 恢复默认，不留跨用例状态
    }

    void sendTargetsMatchDisplayToggles()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);
        // 快捷命令页签整体撤下，发送区回到单页，不再有页签容器。
        QVERIFY(!window.findChild<QWidget *>("sendTabs"));
        QVERIFY(!window.findChild<QTabWidget *>());
        auto *panel = window.findChild<SendPanel *>();
        auto *portA = window.findChild<QPushButton *>("showPortA");
        QVERIFY(panel && portA);
        // 目标只有 A／B／AB 三个按钮：端口默认目标下拉框已删除。
        // 格式/末尾是发送区仅有的两个下拉框（编码并入格式列表，来自共享 PayloadCodec）。
        QCOMPARE(panel->findChildren<QComboBox *>().size(), 2);
        // 发送区格式下拉框逐项来自共享 PayloadCodec（顺序与名称的权威断言在 renderFormats）。
        auto *sendFormat = window.findChild<QComboBox *>("sendFormat");
        QVERIFY(sendFormat);
        const auto formats = PayloadCodec::formats();
        QCOMPARE(sendFormat->count(), formats.size());
        for (int i = 0; i < formats.size(); ++i)
            QCOMPARE(sendFormat->itemText(i), PayloadCodec::formatName(formats.at(i)));
        QVERIFY(panel->findChildren<QRadioButton *>().isEmpty());
        const QList<QPushButton *> targets{window.findChild<QPushButton *>("sendTargetA"),
                                           window.findChild<QPushButton *>("sendTargetB"),
                                           window.findChild<QPushButton *>("sendTargetAB")};
        for (auto *button : targets) {
            QVERIFY(button && button->isCheckable());
            // 与数据显示区端点开关同款形态：同为可勾选按钮、同样高度，
            // 勾选态因此共用同一条 QPushButton:checked 样式，不受系统深色调色板影响。
            QCOMPARE(button->sizeHint().height(), portA->sizeHint().height());
        }
        // 三选一：点新目标即取消旧目标；重复点当前目标不会把目标清空。
        QVERIFY(targets.at(0)->isChecked());
        targets.at(1)->click();
        QVERIFY(!targets.at(0)->isChecked() && targets.at(1)->isChecked() && !targets.at(2)->isChecked());
        targets.at(2)->click();
        QVERIFY(!targets.at(0)->isChecked() && !targets.at(1)->isChecked() && targets.at(2)->isChecked());
        targets.at(2)->click();
        int checked = 0;
        for (auto *button : targets) checked += button->isChecked() ? 1 : 0;
        QCOMPARE(checked, 1);
        // 格式/末尾贴在正文下方，正文上方不再有任何一行。
        auto *editor = window.findChild<QPlainTextEdit *>("sendEditor");
        QVERIFY(editor);
        const int editorTop = editor->mapTo(&window, QPoint()).y();
        const int editorBottom = editorTop + editor->height();
        for (const QString &name : {"sendFormat", "sendSuffix"}) {
            auto *box = window.findChild<QComboBox *>(name);
            QVERIFY2(box && box->isVisibleTo(&window), qPrintable(name));
            QVERIFY2(box->mapTo(&window, QPoint()).y() >= editorBottom, qPrintable(name));
        }
        // 目标按钮在发送按钮旁边：同一行、位于其左侧，且整体落到正文下方。
        auto *send = window.findChild<QPushButton *>("PrimaryButton");
        QVERIFY(send);
        const int targetMid = targets.at(0)->mapTo(&window, QPoint()).y() + targets.at(0)->height() / 2;
        const int sendTop = send->mapTo(&window, QPoint()).y();
        QVERIFY2(targetMid >= editorBottom,
                 qPrintable(QString("target mid %1 vs editor bottom %2").arg(targetMid).arg(editorBottom)));
        QVERIFY2(targetMid >= sendTop && targetMid <= sendTop + send->height(),
                 qPrintable(QString("target mid %1 vs send row %2..%3")
                                .arg(targetMid).arg(sendTop).arg(sendTop + send->height())));
        QVERIFY2(targets.at(2)->mapTo(&window, QPoint()).x() + targets.at(2)->width()
                     <= send->mapTo(&window, QPoint()).x(),
                 qPrintable(QString("AB right edge %1 vs send left %2")
                                .arg(targets.at(2)->mapTo(&window, QPoint()).x() + targets.at(2)->width())
                                .arg(send->mapTo(&window, QPoint()).x())));
        const auto artifactDir = qEnvironmentVariable("EASYDEBUG_UI_ARTIFACT_DIR");
        if (!artifactDir.isEmpty()) {
            QDir().mkpath(artifactDir);
            QVERIFY(window.grab().save(artifactDir + "/send-panel.png"));
        }
    }

    void bridgeUsesOneFixedPair()
    {
        MainWindow window;
        auto *a = window.findChild<PortSelectorWidget *>("endpointA");
        auto *direction = window.findChild<QComboBox *>("routeDirection");
        auto *bridge = window.findChild<QPushButton *>("bridgeButton");
        QVERIFY(a && direction && bridge);
        // The title and the pair label are gone: the direction options name the pair, so all
        // three Forward/Reverse/Bidirectional variants must be selectable.
        QVERIFY(!window.findChild<QLabel *>("BridgeTitle"));
        QVERIFY(!window.findChild<QLabel *>("BridgePair"));
        QCOMPARE(direction->count(), 3);
        QCOMPARE(direction->itemText(0), QStringLiteral("A → B"));
        QCOMPARE(direction->itemText(1), QStringLiteral("B → A"));
        QCOMPARE(direction->itemText(2), QStringLiteral("A ↔ B"));
        QCOMPARE(direction->currentIndex(), 2);
        QVERIFY(!bridge->icon().isNull());
        // Each option carries its own pictogram so the direction stays readable.
        for (int i = 0; i < direction->count(); ++i)
            QVERIFY(!direction->itemIcon(i).isNull());
        // No standing hint line: an unmet precondition shows up as a disabled button.
        QVERIFY(!window.findChild<QLabel *>("BridgeStatus"));
        // A starts as a serial endpoint with no device chosen, so the bridge cannot start.
        QVERIFY(!a->hasValidConfiguration());
        QVERIFY(!bridge->isEnabled());
        a->setSerialPorts({{"COM3", "USB-SERIAL CH340", {}, "COM3"}});
        auto *serial = a->findChild<QComboBox *>("serial.port");
        serial->setCurrentIndex(serial->findData("COM3"));
        QVERIFY(a->hasValidConfiguration());
        QVERIFY(bridge->isEnabled());
        // The full endpoint names live in the tooltip now that the pair label is gone.
        QVERIFY(bridge->toolTip().contains(QStringLiteral("A · 串口")));
        QVERIFY(bridge->toolTip().contains(QStringLiteral("B · TCP 服务器")));
        bridge->click();
        QCOMPARE(bridge->text(), QStringLiteral("停止桥接"));
        QCOMPARE(window.findChild<QLabel *>("RouteStatus")->text(), QStringLiteral("桥接  已建立 · 预览"));
        QVERIFY(!direction->isEnabled());
        bridge->click();
        QCOMPARE(bridge->text(), QStringLiteral("建立桥接"));
        QVERIFY(direction->isEnabled());
        // Editing an endpoint while bridged stops the preview link.
        bridge->click();
        QCOMPARE(bridge->text(), QStringLiteral("停止桥接"));
        a->findChild<QComboBox *>("serial.baud")->setEditText("921600");
        QCOMPARE(bridge->text(), QStringLiteral("建立桥接"));
        QVERIFY(direction->isEnabled());
        // A scan that removes the selected device must reach the bridge, not just the field.
        bridge->click();
        QCOMPARE(bridge->text(), QStringLiteral("停止桥接"));
        a->setSerialPorts({});
        QVERIFY(!a->hasValidConfiguration());
        QCOMPARE(bridge->text(), QStringLiteral("建立桥接"));
        QVERIFY(!bridge->isEnabled());
        window.showRoutes(); // The --routes entry point focuses the always-visible action.
        QVERIFY(bridge->hasFocus() || !bridge->isEnabled());
        // Direction combo and bridge button share the panel width once the layout has run.
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.show();
        QTest::qWait(30);
        QCOMPARE(direction->width(), bridge->width());
        // The bridge block repeats the endpoint rows' rhythm, so its controls line up with the
        // connect and listen buttons instead of drifting by the row gap.
        auto *b = window.findChild<PortSelectorWidget *>("endpointB");
        QVERIFY(b);
        QCOMPARE(direction->mapTo(&window, QPoint(0, 0)).y(),
                 a->findChild<QPushButton *>("endpointAction")->mapTo(&window, QPoint(0, 0)).y());
        QCOMPARE(bridge->mapTo(&window, QPoint(0, 0)).y(),
                 b->findChild<QPushButton *>("endpointAction")->mapTo(&window, QPoint(0, 0)).y());
    }

    void endpointActionOpensAndCloses()
    {
        // 端到端接线：点击动作按钮 → 运行时监听 → 状态回到按钮文案；
        // 再次点击关闭。全程不需要真实硬件（TCP 服务器走本机监听）。
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);
        auto *b = window.findChild<PortSelectorWidget *>("endpointB");
        QVERIFY(b);
        QVERIFY(b->hasValidConfiguration()); // TCP 服务器默认 127.0.0.1:9000
        auto *action = b->findChild<QPushButton *>("endpointAction");
        QCOMPARE(action->text(), QStringLiteral("监听"));
        action->click();
        QTRY_COMPARE(action->text(), QStringLiteral("停止"));
        // 打开期间配置被锁定：不能边连边改。
        QVERIFY(!b->findChild<QComboBox *>("portType")->isEnabled());
        action->click();
        QTRY_COMPARE(action->text(), QStringLiteral("监听"));
        QVERIFY(b->findChild<QComboBox *>("portType")->isEnabled());
    }

    void trafficColumnsShowTheirOwnContent()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);
        window.loadExample();
        auto *table = window.findChild<TrafficLogView *>("trafficTable");
        QVERIFY(table);
        auto *model = table->model();
        QCOMPARE(model->rowCount(), 12);

        // 文本区就是列宽减去视图实际使用的单元格内边距。对端列用等宽字体，
        // 必须按那套字体量，否则会把"列宽够了"误判成绘制也放得下。
        const auto pad = table->cellPadding();
        for (int row = 0; row < model->rowCount(); ++row) {
            for (const int column : {TrafficModel::SourceColumn, TrafficModel::PeerColumn}) {
                const auto index = model->index(row, column);
                const auto text = index.data().toString();
                QFont font = table->font();
                const QVariant fontData = index.data(Qt::FontRole);
                if (fontData.isValid()) font = fontData.value<QFont>();
                const int advance = QFontMetrics(font).horizontalAdvance(text);
                const int textAreaWidth = table->columnWidth(column) - pad.left() - pad.right();
                QVERIFY2(advance <= textAreaWidth,
                         qPrintable(QStringLiteral("第 %1 行第 %2 列被截断：%3（文本区 %4px，字宽 %5px）")
                                        .arg(row).arg(column).arg(text)
                                        .arg(textAreaWidth)
                                        .arg(advance)));
            }
        }
    }

    void compactWindowAndExamples()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);
        window.loadExample();
        auto *table = window.findChild<TrafficLogView *>("trafficTable");
        auto *editor = window.findChild<QPlainTextEdit *>("sendEditor");
        QVERIFY(table && editor);
        // 显示格式下拉框逐项来自共享 PayloadCodec，与发送区同一套列表与规则。
        auto *trafficFormat = window.findChild<QComboBox *>("trafficFormat");
        QVERIFY(trafficFormat);
        const auto formats = PayloadCodec::formats();
        QCOMPARE(trafficFormat->count(), formats.size());
        for (int i = 0; i < formats.size(); ++i)
            QCOMPARE(trafficFormat->itemText(i), PayloadCodec::formatName(formats.at(i)));
        QCOMPARE(trafficFormat->currentText(), QStringLiteral("HEX"));
        auto *splitterProbe = window.findChild<QSplitter *>("dataSplitter");
        QVERIFY(splitterProbe);
        qInfo("SPLITPROBE sizes=%d/%d mins=%d/%d hints=%d/%d",
              splitterProbe->sizes().value(0), splitterProbe->sizes().value(1),
              splitterProbe->widget(0)->minimumSizeHint().height(),
              splitterProbe->widget(1)->minimumSizeHint().height(),
              splitterProbe->widget(0)->sizeHint().height(),
              splitterProbe->widget(1)->sizeHint().height());
        QCOMPARE(table->model()->rowCount(), 12);
        // 对端独占一列并按 Source.peerAddress 渲染。串口行没有对端概念，显示占位破折号；
        // 挤进端口列的第二行会被 33px 行高裁掉，所以它必须是独立的一列。
        // 来源列已并入方向（TX·手动/定时）：RX 恒为自动来源，独立列与方向高度绑定。
        QCOMPARE(TrafficModel::ColumnCount, 6);
        QCOMPARE(table->model()->index(0, TrafficModel::SourceColumn).data().toString(),
                 QStringLiteral("A · 串口"));
        QCOMPARE(table->model()->index(0, TrafficModel::PeerColumn).data().toString(),
                 QStringLiteral("—"));
        const int tcpServerRow = [&] {
            for (int row = 0; row < table->model()->rowCount(); ++row)
                if (table->model()->index(row, TrafficModel::SourceColumn).data().toString()
                    == QStringLiteral("B · TCP 服务器"))
                    return row;
            return -1;
        }();
        QVERIFY(tcpServerRow >= 0);
        QCOMPARE(table->model()->index(tcpServerRow, TrafficModel::PeerColumn).data().toString(),
                 QStringLiteral("127.0.0.1:52001"));
        // 显示选项收进"显示设置"菜单后仍可过滤：关掉 RX 只剩 TX 记录。
        auto *rx = window.findChild<QAction *>("showRx");
        QVERIFY(rx && rx->isChecked());
        rx->setChecked(false);
        QCOMPARE(table->model()->rowCount(), 4);
        rx->setChecked(true);
        QCOMPARE(table->model()->rowCount(), 12);
        // A/B 端点开关在顶栏选项栏里：点哪个看哪个，都点看全部。
        auto *portA = window.findChild<QPushButton *>("showPortA");
        auto *portB = window.findChild<QPushButton *>("showPortB");
        QVERIFY(portA && portB && portA->isChecked() && portB->isChecked());
        portB->setChecked(false);
        QCOMPARE(table->model()->rowCount(), 6);
        portA->setChecked(false);
        QCOMPARE(table->model()->rowCount(), 0);
        portA->setChecked(true);
        portB->setChecked(true);
        QCOMPARE(table->model()->rowCount(), 12);
        QVERIFY(window.rect().contains(QRect(editor->mapTo(&window, QPoint()), editor->size())));
        // 切换显示格式：表格渲染跟着变（HEX 基线 → DEC），原始字节不变。
        // 第 0 条是字节流片段，内容列在渲染文本后带 ⏎ 续接标记——边界是消息的事实，
        // 不能因为格式切换或只看完整消息的开关而消失。
        const QModelIndex firstData = table->model()->index(0, TrafficModel::DataColumn);
        QCOMPARE(firstData.data().toString(), QStringLiteral("01 03 00 00 00 02 C4 0B ⏎"));
        trafficFormat->setCurrentIndex(1); // DEC
        QCOMPARE(table->model()->index(0, TrafficModel::DataColumn).data().toString(),
                 QStringLiteral("1 3 0 0 0 2 196 11 ⏎"));
        // 搜索匹配当前格式的渲染文本：DEC 文本也能命中（C4 0B → "196 11"）。
        auto *search = window.findChild<QLineEdit *>("trafficSearch");
        search->setText("196 11");
        QCOMPARE(table->model()->rowCount(), 1);
        trafficFormat->setCurrentIndex(0);
        search->clear();
        QCOMPARE(table->model()->rowCount(), 12);
        search->setText("not-present-in-samples");
        QCOMPARE(table->model()->rowCount(), 0);
        search->clear();
        window.findChild<QPushButton *>("clearTraffic")->click();
        QCOMPARE(table->model()->rowCount(), 0);
    }

    void receiveFramingValueSemantics()
    {
        // 接收组帧是端点配置值类型：默认参数可直接使用，非法组合必须被 isValid 拦下。
        using Framing = ReceiveFraming;
        const Framing defaults;
        QVERIFY(defaults.isValid());
        QCOMPARE(defaults.mode, Framing::Passthrough); // 默认仅兜底输出，不改逐包语义
        QCOMPARE(defaults.idleTimeoutMs, 100);
        QCOMPARE(defaults.bufferThresholdBytes, 300);
        QVERIFY(!defaults.includeDelimiter);           // 结束符默认剥离
        QVERIFY(defaults.includeLengthPrefix);         // 长度前缀默认保留帧头

        // Delimiter 模式必须有非空结束符；超时与阈值必须为正。
        Framing delimiter;
        delimiter.mode = Framing::Delimiter;
        QVERIFY(!delimiter.isValid());
        delimiter.delimiter = QByteArray::fromHex("0D0A");
        QVERIFY(delimiter.isValid());
        Framing badTimeout = delimiter;
        badTimeout.idleTimeoutMs = 0;
        QVERIFY(!badTimeout.isValid());
        Framing badThreshold = delimiter;
        badThreshold.bufferThresholdBytes = -1;
        QVERIFY(!badThreshold.isValid());

        // 帧内容开关参与相等性：组帧器据此决定输出帧携带什么。
        Framing stripped = delimiter;
        stripped.includeDelimiter = true;
        QVERIFY(stripped != delimiter);        // 唯一差异是开关，必须判为不等
        Framing same = stripped;
        QVERIFY(same == stripped);             // 其余字段逐项相同则相等

        // 与参考实现的分级对齐：警告是独立方向，有自己的显示名，非数据消息不进统计。
        Traffic::Message warning;
        warning.direction = Traffic::Direction::Warning;
        QVERIFY(!warning.isData());
        QCOMPARE(Traffic::directionName(warning.direction), QStringLiteral("警告"));
    }

    // 文本显示模式的三条底线：高速追加后跟随钉底；模型触顶淘汰后文本历史原样保留；
    // 停止跟随滚回顶部，第 1 条仍可回滚显示——这正是 JCom 34 万行不卡的行为基线。
    void textViewFollowsRetainsAndRollsBack()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        window.findChild<TrafficPanel *>()->setPreferences(nullptr);
        auto *textModeAction = window.findChild<QAction *>(QStringLiteral("textMode"));
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        auto *model = window.findChild<TrafficModel *>();
        auto *follow = window.findChild<QPushButton *>(QStringLiteral("followTraffic"));
        QVERIFY(textModeAction && text && model && follow);
        textModeAction->setChecked(true);

        const QUuid endpointId = window.findChild<QPushButton *>(QStringLiteral("showPortA"))
                                     ->property("endpoint").toUuid();
        // 25000 条 8 字节小帧：越过模型 20000 条的条数上限，文本历史必须原样保留。
        const int total = 25000;
        for (int batchStart = 0; batchStart < total; batchStart += 500) {
            QList<Traffic::Message> batch;
            batch.reserve(500);
            for (int i = 0; i < 500; ++i) {
                Traffic::Message message;
                message.id = batchStart + i + 1;
                message.timestamp = QDateTime::currentDateTime();
                message.direction = (batchStart + i) % 2 ? Traffic::Direction::Transmit
                                                         : Traffic::Direction::Receive;
                message.source.endpointId = endpointId;
                message.source.endpointIndex = 0;
                message.source.endpointKind = PortDraft::Serial;
                message.raw = QByteArray::fromHex("0102030405060708");
                message.rawLength = message.raw.size();
                batch.append(message);
            }
            window.findChild<TrafficPanel *>()->appendMessages(batch);
            // 等攒批逐批落地：同步连灌会让 50 批合成一个 25000 条巨批，
            // 而"先裁后插"裁不到单批自身——单批不超预算是调用方的既有约定。
            QTest::qWait(20);
        }
        // 攒批是 16ms 异步的；空表上 record(-1) 是未定义行为，先等首条落地。
        QTRY_VERIFY(model->rowCount() > 0);
        QTRY_COMPARE(model->record(model->rowCount() - 1).id, quint64(total));

        // 跟随开启：追加后钉在底部。
        QVERIFY(follow->isChecked());
        QCOMPARE(text->verticalScrollBar()->value(), text->verticalScrollBar()->maximum());

        // 模型触顶淘汰，文本历史一行不少：这是"文本模式能看完整历史"的判据。
        QVERIFY2(model->rowCount() <= 20000, qPrintable(QStringLiteral("模型应已淘汰：当前 %1 条").arg(model->rowCount())));
        QCOMPARE(text->lineCount(), total);
        QCOMPARE(text->document()->blockCount(), total);

        // 向上翻阅自动解除跟随；滚回顶部，第 1 条仍然在第一行。
        text->verticalScrollBar()->setValue(text->verticalScrollBar()->maximum() - 100);
        QVERIFY(!follow->isChecked());
        text->verticalScrollBar()->setValue(0);
        QCOMPARE(text->verticalScrollBar()->value(), 0);
        const QString firstLine = text->document()->firstBlock().text();
        QVERIFY2(firstLine.contains(QStringLiteral("01 02 03 04 05 06 07 08")),
                 qPrintable(QStringLiteral("第 1 行应是首条记录：") + firstLine));
        QVERIFY(firstLine.trimmed().startsWith(QStringLiteral("1 ")));

        // 重新跟随：一键回底，继续钉住最新记录。
        follow->setChecked(true);
        QCOMPARE(text->verticalScrollBar()->value(), text->verticalScrollBar()->maximum());

        // 清空是会话边界：行号从 1 重新计数。
        window.findChild<QPushButton *>("clearTraffic")->click();
        QCOMPARE(text->lineCount(), 1); // 空文档保底的一个空块
        QList<Traffic::Message> afterClear;
        for (int i = 0; i < 3; ++i) {
            Traffic::Message message;
            message.id = i + 1;
            message.timestamp = QDateTime::currentDateTime();
            message.direction = Traffic::Direction::Receive;
            message.source.endpointId = endpointId;
            message.source.endpointIndex = 0;
            message.source.endpointKind = PortDraft::Serial;
            message.raw = QByteArray::fromHex("AA");
            message.rawLength = message.raw.size();
            afterClear.append(message);
        }
        window.findChild<TrafficPanel *>()->appendMessages(afterClear);
        QTRY_COMPARE(text->lineCount(), 3); // 首行复用空文档保底块，3 条即 3 块
        QVERIFY(text->document()->firstBlock().text().trimmed().startsWith(QStringLiteral("1 ")));
    }

    // 报文里的 \r\n 必须还原成真实换行（按文本本来的样子显示），而不是转义字面量；
    // 行号时间只标首行，续行紧随其后不带前缀——与 JCom 的多行报文显示一致。
    void textViewKeepsRealLineBreaks()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        window.findChild<TrafficPanel *>()->setPreferences(nullptr);
        window.findChild<QAction *>(QStringLiteral("textMode"))->setChecked(true);
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        QVERIFY(text);
        // 换行语义属于文本类渲染：HEX/DEC 本身没有换行概念，切到 UTF-8 验证。
        window.findChild<QComboBox *>(QStringLiteral("trafficFormat"))->setCurrentText(QStringLiteral("UTF-8"));

        const QUuid endpointId = window.findChild<QPushButton *>(QStringLiteral("showPortA"))
                                     ->property("endpoint").toUuid();
        Traffic::Message message;
        message.id = 1;
        message.timestamp = QDateTime::currentDateTime();
        message.direction = Traffic::Direction::Receive;
        message.source.endpointId = endpointId;
        message.source.endpointIndex = 0;
        message.source.endpointKind = PortDraft::Serial;
        message.raw = QByteArray("OK\r\nnext");
        message.rawLength = message.raw.size();
        window.findChild<TrafficPanel *>()->appendMessages({message});

        // QTRY 重算的是内联表达式，局部变量会被冻结——先等块数落地，再取文本。
        QTRY_COMPARE(text->document()->blockCount(), 2);
        const QStringList lines = text->toPlainText().split(QLatin1Char('\n'));
        QCOMPARE(lines.size(), 2);
        QVERIFY2(lines.at(0).trimmed().endsWith(QStringLiteral("OK")),
                 qPrintable(QStringLiteral("首行应以 OK 结尾：") + lines.at(0)));
        QVERIFY2(lines.at(0).trimmed().startsWith(QStringLiteral("1 ")),
                 qPrintable(QStringLiteral("首行应有行号：") + lines.at(0)));
        QCOMPARE(lines.at(1), QStringLiteral("next")); // 续行不带行号
        QVERIFY2(!text->toPlainText().contains(QStringLiteral("\\r\\n")),
                 "换行不能再以转义字面量出现");
    }
};

int main(int argc, char **argv)
{
    // UI smoke tests must run outside Qt Creator and on headless CI agents.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setOrganizationName("easyDebugTests");
    app.setApplicationName("easyDebugUiTests");
    QStandardPaths::setTestModeEnabled(true);
    UiSmokeTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_ui.moc"
