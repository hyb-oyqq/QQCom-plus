#include "mainwindow.h"
#include "domain/receiveframing.h"
#include "ui/common/endpointformat.h"
#include "ui/common/payloadcodec.h"
#include "ui/panels/sending/sendpanel.h"
#include "ui/panels/traffic/trafficpanel.h"
#include "ui/panels/traffic/traffictextview.h"
#include "discovery/portdiscoveryservice.h"
#include "persistence/preferencesstore.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
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
        // 用例统一按 HEX 格式判定（面板默认格式），断言走静态 accepts。
        const auto accepted = [&model](const TrafficFilter::Options &options) {
            int count = 0;
            for (int i = 0; i < model.rowCount(); ++i)
                if (TrafficFilter::accepts(model.record(i), options, PayloadCodec::Format::Hex)) ++count;
            return count;
        };
        // 端点为独立开关：勾选集合内的才显示，一个都不勾则不显示。
        TrafficFilter::Options options;
        options.endpoints = {a, b};
        options.search = QStringLiteral("7F FF");
        QCOMPARE(accepted(options), 1);
        // 原始字节是事实依据，任何过滤与格式设置都改不了模型里的记录。
        QCOMPARE(model.record(0).raw, QByteArray::fromHex("007fff"));
        options.search.clear();
        options.endpoints = {a};
        options.tx = false;
        QCOMPARE(accepted(options), 1);
        options.endpoints = {b};
        options.rx = false;
        options.tx = true;
        options.search = QStringLiteral("HELLO");
        QCOMPARE(accepted(options), 1);
        options.search.clear();
        options.tx = false;
        QCOMPARE(accepted(options), 0);
        options.endpoints.clear();
        QCOMPARE(accepted(options), 0);
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

        const auto accepted = [&model](const TrafficFilter::Options &options) {
            int count = 0;
            for (int i = 0; i < model.rowCount(); ++i)
                if (TrafficFilter::accepts(model.record(i), options, PayloadCodec::Format::Hex)) ++count;
            return count;
        };
        TrafficFilter::Options options;
        options.endpoints = {a.endpointId, b.endpointId};
        QCOMPARE(accepted(options), 3);

        // 对端地址是来源的结构化字段，能被关键词直接命中，不必先渲染成显示文本。
        options.search = QStringLiteral("52344");
        QCOMPARE(accepted(options), 2);
        options.search.clear();

        // 错误/警告日志有独立开关：关闭后 Warning/Error 事件不进显示，数据不受影响。
        const auto warning = Traffic::makeSampleMessage(
            4, QDateTime(), b, Traffic::Direction::Warning, {});
        const auto error = Traffic::makeSampleMessage(
            5, QDateTime(), b, Traffic::Direction::Error, {});
        model.replaceRecords({stream, datagram, message, warning, error});
        QCOMPARE(accepted(options), 5);
        options.events = false;
        QCOMPARE(accepted(options), 3);
        options.events = true;
    }

    // 端点草稿现在随配置即时持久化（endpoint/A、endpoint/B）：共享 ini 里
    // 前序用例留下的端点状态会污染"默认端点"假设，依赖该假设的用例先清场。
    void clearEndpointDrafts()
    {
        const QString ini = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
            + QStringLiteral("/preferences.ini");
        PreferencesStore sweep(nullptr, ini);
        sweep.setValue(QStringLiteral("endpoint/A"), QVariant());
        sweep.setValue(QStringLiteral("endpoint/B"), QVariant());
    }

    void portDraftsRemainIndependent()
    {
        clearEndpointDrafts();
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
        clearEndpointDrafts();
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

    // A（串口）与 B（TCP 服务器）的参数列必须落在同一条竖线上：主字段右缘一致，
    // 尾部标签右缘一致、尾部字段左缘与宽度一致。原先各页的尾部组宽窄不同
    // （波特率 vs 监听端口）且串口页多一个尾部 stretch，行间看着参差。
    void endpointParameterColumnsAlign()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);
        auto *a = window.findChild<PortSelectorWidget *>("endpointA");
        auto *b = window.findChild<PortSelectorWidget *>("endpointB");
        QVERIFY(a && b);
        const auto leftInWindow = [&window](QWidget *w) {
            return w->mapTo(&window, QPoint(0, 0)).x();
        };
        const auto rightInWindow = [&window](QWidget *w) {
            return w->mapTo(&window, QPoint(w->width(), 0)).x();
        };
        // 主字段（串口选择 / 本地地址）与尾部组（波特率 / 监听端口）逐列比对。
        auto *aMain = a->findChild<QComboBox *>("serial.port");
        auto *bMain = b->findChild<QComboBox *>("server.address");
        auto *aTrailLabel = a->findChild<QLabel *>("serial.baudLabel");
        auto *bTrailLabel = b->findChild<QLabel *>("server.portLabel");
        auto *aTrail = a->findChild<QComboBox *>("serial.baud");
        auto *bTrail = b->findChild<QSpinBox *>("server.port");
        QVERIFY(aMain && bMain && aTrailLabel && bTrailLabel && aTrail && bTrail);
        QCOMPARE(leftInWindow(aMain), leftInWindow(bMain));
        QCOMPARE(rightInWindow(aMain), rightInWindow(bMain));
        QCOMPARE(rightInWindow(aTrailLabel), rightInWindow(bTrailLabel));
        QCOMPARE(leftInWindow(aTrail), leftInWindow(bTrail));
        QCOMPARE(aTrail->width(), bTrail->width());
    }

    void sendPreviewRejectsInvalidInput()
    {
        clearEndpointDrafts();
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
        clearEndpointDrafts();
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
        clearEndpointDrafts();
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
        clearEndpointDrafts();
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


    void compactWindowAndExamples()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);
        window.loadExample();
        auto *text = window.findChild<TrafficTextView *>("trafficText");
        auto *model = window.findChild<TrafficModel *>();
        auto *editor = window.findChild<QPlainTextEdit *>("sendEditor");
        QVERIFY(text && model && editor);
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
        QCOMPARE(model->rowCount(), 12);
        // 条数摘要搬进了主窗口状态栏：示例加载完成后状态栏同步播报会话累计。
        QCOMPARE(window.findChild<QLabel *>(QStringLiteral("TrafficStatus"))->text(),
                 QStringLiteral("共 12 条"));
        QVERIFY(window.rect().contains(QRect(editor->mapTo(&window, QPoint()), editor->size())));
        // 格式切换只影响新行（控制台语义）：切到 DEC 后，新到达的消息按 DEC 渲染固化进文本视图。
        // 已知字节流片段（HEX 01 03 00 00 00 02 C4 0B）渲染为 DEC，并带 ⏎ 续接标记——边界是消息的事实。
        trafficFormat->setCurrentIndex(1); // DEC
        const QUuid endpointId = window.findChild<QPushButton *>("showPortA")
                                     ->property("endpoint").toUuid();
        Traffic::Message dec;
        dec.id = 100000;
        dec.timestamp = QDateTime::currentDateTime();
        dec.direction = Traffic::Direction::Receive;
        dec.source.endpointId = endpointId;
        dec.source.endpointIndex = 0;
        dec.source.endpointKind = PortDraft::Serial;
        dec.framing = Traffic::Framing::Stream;
        dec.hasMoreFragments = true;
        dec.raw = QByteArray::fromHex("010300000002c40b");
        dec.rawLength = dec.raw.size();
        window.findChild<TrafficPanel *>()->appendMessages({dec});
        QTRY_COMPARE(model->rowCount(), 13);
        QCOMPARE(text->lineCount(), 13);
        const QString decLine = text->document()->lastBlock().text();
        QVERIFY2(decLine.endsWith(QStringLiteral("1 3 0 0 0 2 196 11 ⏎")),
                 qPrintable(QStringLiteral("DEC 渲染行不符：") + decLine));
        trafficFormat->setCurrentIndex(0); // 恢复 HEX，后续消息按 HEX 渲染与搜索
        // 搜索按到达时过滤：命中的记录进文本历史，未命中的只进模型不进显示。
        auto *search = window.findChild<QLineEdit *>("trafficSearch");
        Traffic::Message hit;
        hit.id = 100001;
        hit.timestamp = QDateTime::currentDateTime();
        hit.direction = Traffic::Direction::Receive;
        hit.source.endpointId = endpointId;
        hit.source.endpointIndex = 0;
        hit.source.endpointKind = PortDraft::Serial;
        hit.raw = QByteArray::fromHex("AABB");
        hit.rawLength = hit.raw.size();
        window.findChild<TrafficPanel *>()->appendMessages({hit});
        QTRY_COMPARE(model->rowCount(), 14);
        QCOMPARE(text->lineCount(), 14);
        // 搜索按到达时生效：设一个只命中 HEX "AA BB" 的关键词，后续未命中记录不进显示。
        search->setText(QStringLiteral("aa bb"));
        Traffic::Message miss = hit;
        miss.id = 100002;
        miss.raw = QByteArray::fromHex("CCDD");
        miss.rawLength = miss.raw.size();
        window.findChild<TrafficPanel *>()->appendMessages({miss});
        QTRY_COMPARE(model->rowCount(), 15); // 模型照单全收：导出与记录不依赖显示过滤
        QCOMPARE(text->lineCount(), 14);     // 搜索未命中 → 不进显示
        search->clear();
        window.findChild<QPushButton *>("clearTraffic")->click();
        QCOMPARE(model->rowCount(), 0);
        QCOMPARE(text->lineCount(), 1); // 空文档保底的一个空块
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
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        auto *model = window.findChild<TrafficModel *>();
        auto *follow = window.findChild<QPushButton *>(QStringLiteral("followTraffic"));
        QVERIFY(text && model && follow);

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

    // 超长报文在控制台完整显示：单行不再封顶（完整显示是显式需求），
    // 内存由行数与字符两个总预算兜底。模型侧仍按 4096 字节做预览截断
    // （truncated 标记），原始字节事实保留在结构化存储里，导出与记录拿得到。
    void hugePayloadDisplayedFullyInTextView()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        window.findChild<TrafficPanel *>()->setPreferences(nullptr);
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        auto *model = window.findChild<TrafficModel *>();
        QVERIFY(text && model);

        const QUuid endpointId = window.findChild<QPushButton *>(QStringLiteral("showPortA"))
                                     ->property("endpoint").toUuid();
        // 8KB 可打印 ASCII：模型预览截到 4096 字节，控制台必须一字节不少地展开。
        // 显示格式显式切到 UTF-8，不依赖共享配置里的残留值；HEX 对照行同样关掉，
        // 保证最后一行就是数据行，断言才有稳定锚点。
        window.findChild<QComboBox *>(QStringLiteral("trafficFormat"))
            ->setCurrentText(QStringLiteral("UTF-8"));
        window.findChild<QAction *>(QStringLiteral("showHex"))->setChecked(false);
        QByteArray raw(8192, Qt::Uninitialized);
        for (int i = 0; i < raw.size(); ++i)
            raw[i] = char(0x20 + (i % 0x5F));
        Traffic::Message message;
        message.id = 1;
        message.timestamp = QDateTime::currentDateTime();
        message.direction = Traffic::Direction::Receive;
        message.source.endpointId = endpointId;
        message.source.endpointIndex = 0;
        message.source.endpointKind = PortDraft::Serial;
        message.raw = raw;
        message.rawLength = raw.size();
        window.findChild<TrafficPanel *>()->appendMessages({message});
        QTRY_COMPARE(model->rowCount(), 1);

        const QString line = text->toPlainText();
        // 控制台完整显示：8192 字节一字节不少（单行封顶已移除）。
        const auto full = PayloadCodec::render(raw, PayloadCodec::Format::Utf8);
        QVERIFY2(line.endsWith(full),
            qPrintable(QStringLiteral("控制台行必须完整展开载荷：实际 %1 字符，载荷 %2 字符")
                           .arg(line.size()).arg(full.size())));
        // 模型侧仍有 4096 字节的预览截断：存储的 raw 被截到上限，rawLength 仍是
        // 真实大小并带 truncated 标记——结构化存储（导出/记录）与显示各有各的预算。
        QCOMPARE(model->record(0).raw.size(), 4096);
        QCOMPARE(model->record(0).displayBytes(), qint64(raw.size()));
        QVERIFY(model->record(0).truncated);
    }

    // 行内容开关按控制台语义生效：只影响新行，已固化的行不重排。
    // 行号恒在；纯数据模式去掉行号/时间/方向/长度/HEX 等一切前缀与附注。
    void displayTogglesShapeNewLines()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        auto *panel = window.findChild<TrafficPanel *>();
        panel->setPreferences(nullptr); // 开关变化不落盘，避免污染共享配置
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        auto *model = window.findChild<TrafficModel *>();
        QVERIFY(text && model);
        window.findChild<QComboBox *>(QStringLiteral("trafficFormat"))
            ->setCurrentText(QStringLiteral("UTF-8"));
        const QUuid endpointId = window.findChild<QPushButton *>(QStringLiteral("showPortA"))
                                     ->property("endpoint").toUuid();
        Traffic::Message message;
        message.id = 1;
        message.timestamp = QDateTime::currentDateTime();
        message.direction = Traffic::Direction::Receive;
        message.source.endpointId = endpointId;
        message.source.endpointIndex = 0;
        message.source.endpointKind = PortDraft::Serial;
        message.raw = QByteArray("OK");
        message.rawLength = 2;

        // 关时间戳/方向，开长度与 HEX 对照：长度用真实字节数；
        // HEX 独占一行（不是接在数据行尾），便于上下比对字节。
        window.findChild<QAction *>(QStringLiteral("showTimestamp"))->setChecked(false);
        window.findChild<QAction *>(QStringLiteral("showDirection"))->setChecked(false);
        window.findChild<QAction *>(QStringLiteral("showLength"))->setChecked(true);
        window.findChild<QAction *>(QStringLiteral("showHex"))->setChecked(true);
        panel->appendMessages({message});
        QTRY_COMPARE(model->rowCount(), 1);
        QCOMPARE(text->document()->blockCount(), 2); // 数据行 + HEX 对照行
        // 行号顶格左对齐：右对齐补空格在自动换行下像首行缩进，与续行错位。
        // 前缀以冒号收尾，与数据的分界一眼可辨。
        QCOMPARE(text->document()->firstBlock().text(), QStringLiteral("1 [2 B]: OK"));
        QCOMPARE(text->document()->lastBlock().text(), QStringLiteral("4F 4B"));

        // 行号是独立开关：关掉后只剩长度与数据。
        window.findChild<QAction *>(QStringLiteral("showHex"))->setChecked(false);
        window.findChild<QAction *>(QStringLiteral("showLineNumber"))->setChecked(false);
        message.id = 2;
        panel->appendMessages({message});
        QTRY_COMPARE(model->rowCount(), 2);
        QCOMPARE(text->document()->lastBlock().text(), QStringLiteral("[2 B]: OK"));

        // 纯数据模式：一切前缀与附注消失，只剩数据本身。
        window.findChild<QAction *>(QStringLiteral("pureData"))->setChecked(true);
        message.id = 3;
        panel->appendMessages({message});
        QTRY_COMPARE(model->rowCount(), 3);
        QCOMPARE(text->document()->lastBlock().text(), QStringLiteral("OK"));
        // 还原默认开关，不把本用例的状态带给后续用例。
        window.findChild<QAction *>(QStringLiteral("pureData"))->setChecked(false);
        window.findChild<QAction *>(QStringLiteral("showLineNumber"))->setChecked(true);
        window.findChild<QAction *>(QStringLiteral("showLength"))->setChecked(false);
    }

    // 显示格式为 HEX 时不重复显示 HEX 对照行：数据本身就是 HEX，再来一行一模一样。
    // 已固化的旧行不回溯（控制台语义），此断言锁定的是"新行"的行为。
    void hexEchoSuppressedInHexFormat()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        auto *panel = window.findChild<TrafficPanel *>();
        panel->setPreferences(nullptr);
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        auto *model = window.findChild<TrafficModel *>();
        QVERIFY(text && model);
        window.findChild<QComboBox *>(QStringLiteral("trafficFormat"))
            ->setCurrentText(QStringLiteral("HEX"));
        // 关掉时间戳/方向，行内容只剩行号 + 数据，断言更聚焦。
        window.findChild<QAction *>(QStringLiteral("showTimestamp"))->setChecked(false);
        window.findChild<QAction *>(QStringLiteral("showDirection"))->setChecked(false);
        window.findChild<QAction *>(QStringLiteral("showHex"))->setChecked(true);
        const QUuid endpointId = window.findChild<QPushButton *>(QStringLiteral("showPortA"))
                                     ->property("endpoint").toUuid();
        Traffic::Message message;
        message.id = 1;
        message.timestamp = QDateTime::currentDateTime();
        message.direction = Traffic::Direction::Receive;
        message.source.endpointId = endpointId;
        message.source.endpointIndex = 0;
        message.source.endpointKind = PortDraft::Serial;
        message.raw = QByteArray("OK");
        message.rawLength = 2;
        panel->appendMessages({message});
        QTRY_COMPARE(model->rowCount(), 1);
        QCOMPARE(text->document()->blockCount(), 1); // 只有数据行，没有 HEX 对照行
        QCOMPARE(text->document()->lastBlock().text(), QStringLiteral("1: 4F 4B"));
        // UTF-8 格式下对照行恢复出现。
        window.findChild<QComboBox *>(QStringLiteral("trafficFormat"))
            ->setCurrentText(QStringLiteral("UTF-8"));
        message.id = 2;
        panel->appendMessages({message});
        QTRY_COMPARE(model->rowCount(), 2);
        QCOMPARE(text->document()->blockCount(), 3);
        QCOMPARE(text->document()->lastBlock().text(), QStringLiteral("4F 4B"));
        window.findChild<QAction *>(QStringLiteral("showHex"))->setChecked(false);
        window.findChild<QAction *>(QStringLiteral("showTimestamp"))->setChecked(true);
        window.findChild<QAction *>(QStringLiteral("showDirection"))->setChecked(true);
    }

    // 错误/警告日志的独立开关：关闭后事件不进显示、数据不受影响；开启后恢复。
    void errorEventsFollowTheirOwnToggle()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        auto *panel = window.findChild<TrafficPanel *>();
        panel->setPreferences(nullptr);
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        auto *model = window.findChild<TrafficModel *>();
        QVERIFY(text && model);
        const QUuid endpointId = window.findChild<QPushButton *>(QStringLiteral("showPortA"))
                                     ->property("endpoint").toUuid();
        const auto makeMessage = [endpointId](quint64 id, Traffic::Direction direction, const char *text) {
            Traffic::Message message;
            message.id = id;
            message.timestamp = QDateTime::currentDateTime();
            message.direction = direction;
            message.source.endpointId = endpointId;
            message.source.endpointIndex = 0;
            message.source.endpointKind = PortDraft::Serial;
            message.raw = QByteArray(text);
            message.rawLength = message.raw.size();
            if (direction == Traffic::Direction::Error)
                message.statusText = QStringLiteral("写入失败");
            return message;
        };
        // 先建立一条可见数据行作基线。
        panel->appendMessages({makeMessage(1, Traffic::Direction::Receive, "data")});
        QTRY_COMPARE(model->rowCount(), 1);
        const auto baseline = text->document()->lastBlock().text();
        // 关闭错误日志：错误事件进模型不进显示，数据行纹丝不动。
        window.findChild<QComboBox *>(QStringLiteral("trafficFormat"))
            ->setCurrentText(QStringLiteral("UTF-8"));
        window.findChild<QAction *>(QStringLiteral("showErrorLog"))->setChecked(false);
        panel->appendMessages({makeMessage(2, Traffic::Direction::Error, "boom")});
        QTRY_COMPARE(model->rowCount(), 2);
        QCOMPARE(text->lineCount(), 1);
        QCOMPARE(text->document()->lastBlock().text(), baseline);
        // 重新开启：后续错误事件恢复显示。
        window.findChild<QAction *>(QStringLiteral("showErrorLog"))->setChecked(true);
        panel->appendMessages({makeMessage(3, Traffic::Direction::Error, "boom2")});
        QTRY_COMPARE(text->lineCount(), 2);
        QVERIFY2(text->document()->lastBlock().text().endsWith(QStringLiteral("boom2")),
            qPrintable(QStringLiteral("错误行实况：")
                + text->document()->lastBlock().text()));
    }

    // ANSI 颜色标识：SGR 序列渲染为前景色分片，转义序列本身不进显示文本，
    // 换行还原保持既有语义。命令行式的彩色日志因此原样可读。
    void ansiColorsRenderStripped()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        auto *panel = window.findChild<TrafficPanel *>();
        panel->setPreferences(nullptr);
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        auto *model = window.findChild<TrafficModel *>();
        QVERIFY(text && model);
        window.findChild<QComboBox *>(QStringLiteral("trafficFormat"))
            ->setCurrentText(QStringLiteral("UTF-8"));
        const QUuid endpointId = window.findChild<QPushButton *>(QStringLiteral("showPortA"))
                                     ->property("endpoint").toUuid();
        Traffic::Message message;
        message.id = 1;
        message.timestamp = QDateTime::currentDateTime();
        message.direction = Traffic::Direction::Receive;
        message.source.endpointId = endpointId;
        message.source.endpointIndex = 0;
        message.source.endpointKind = PortDraft::Serial;
        message.raw = QByteArray("\x1b[31mRED\x1b[0m plain\r\nnext \x1b[92mgreen");
        message.rawLength = message.raw.size();
        panel->appendMessages({message});
        QTRY_COMPARE(model->rowCount(), 1);
        QCOMPARE(text->document()->blockCount(), 2);
        const auto first = text->document()->firstBlock();
        QVERIFY2(first.text().endsWith(QStringLiteral("RED plain")),
                 qPrintable(QStringLiteral("转义序列应被剥离：") + first.text()));
        QVERIFY2(text->document()->lastBlock().text() == QStringLiteral("next green"),
                 qPrintable(QStringLiteral("续行应无残留序列：")
                     + text->document()->lastBlock().text()));
        // "RED" 分片按 SGR 31 的前景色渲染。
        bool foundRed = false;
        for (auto it = first.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (fragment.text() == QStringLiteral("RED"))
                foundRed = fragment.charFormat().foreground().color() == QColor(0xC6, 0x28, 0x28);
        }
        QVERIFY(foundRed);
    }

    // 接收组帧是端点配置：模式联动显隐、结束符可校验、开关随草稿下发到运行时。
    void receiveFramingControlsFeedTheDraft()
    {
        // 独立选择器即可：组帧是端点配置，不依赖主窗口，也避开串口扫描的时序。
        PortSelectorWidget selectorWidget;
        selectorWidget.show();
        auto *selector = &selectorWidget;
        // 先给串口选个设备：未选设备本身就不合法，会盖住组帧的校验结果。
        selector->setSerialPorts({{"COM17", "USB Serial", {}, "COM17"}});
        selector->findChild<QComboBox *>(QStringLiteral("serial.port"))
            ->setCurrentIndex(selector->findChild<QComboBox *>(QStringLiteral("serial.port"))
                                  ->findData(QStringLiteral("COM17")));
        auto *advanced = selector->findChild<QToolButton *>(QStringLiteral("advancedToggle"));
        QVERIFY(advanced);
        advanced->click(); // 展开高级参数：组帧行挂在类型专属参数之下
        QTest::qWait(20);
        auto *mode = selector->findChild<QComboBox *>(QStringLiteral("framingMode"));
        auto *delimiter = selector->findChild<QLineEdit *>(QStringLiteral("framingDelimiter"));
        auto *idle = selector->findChild<QSpinBox *>(QStringLiteral("framingIdle"));
        auto *logHints = selector->findChild<QCheckBox *>(QStringLiteral("framingLogHints"));
        QVERIFY(mode && delimiter && idle && logHints);
        const auto valid = [selector](const QString &stage) {
            QVERIFY2(selector->hasValidConfiguration(),
                qPrintable(stage + QStringLiteral("：") + selector->validationError()));
        };
        // 默认仅兜底输出：不需要结束符，输入因此不出现。
        QVERIFY(!delimiter->isVisible());
        valid(QStringLiteral("默认模式"));

        // 结束符模式：结束符必填（HEX），缺失即校验失败。
        mode->setCurrentIndex(mode->findData(int(ReceiveFraming::Delimiter)));
        QTest::qWait(20);
        QVERIFY(delimiter->isVisible());
        QVERIFY2(!selector->hasValidConfiguration(), qPrintable(selector->validationError()));
        delimiter->setText(QStringLiteral("0D 0A"));
        valid(QStringLiteral("填好结束符"));
        idle->setValue(250);
        logHints->setChecked(true);
        const auto draft = selector->draft();
        QCOMPARE(draft.framing.mode, ReceiveFraming::Delimiter);
        QCOMPARE(draft.framing.delimiter, QByteArray::fromHex("0D0A"));
        QCOMPARE(draft.framing.idleTimeoutMs, 250);
        QVERIFY(draft.framing.logHints);

        // 长度前缀模式：结束符输入隐去，不再要求结束符。
        mode->setCurrentIndex(mode->findData(int(ReceiveFraming::LengthPrefixU16LE)));
        QTest::qWait(20);
        QVERIFY(!delimiter->isVisible());
        QVERIFY(selector->hasValidConfiguration());
        QCOMPARE(selector->draft().framing.mode, ReceiveFraming::LengthPrefixU16LE);

        // 截图产物：结束符模式下的组帧行（模式 + 结束符 + 兜底设置 + 说明文字），
        // 供目视检查排版与文案清晰度。
        mode->setCurrentIndex(mode->findData(int(ReceiveFraming::Delimiter)));
        QTest::qWait(20);
        const auto artifactDir = qEnvironmentVariable("EASYDEBUG_UI_ARTIFACT_DIR");
        if (!artifactDir.isEmpty()) {
            QDir().mkpath(artifactDir);
            QVERIFY(selector->grab().save(artifactDir + "/framing-row.png"));
        }
    }

    // 自动换行开关：勾选后按视口宽度折行（横向滚动随之失效），取消后恢复水平滚动。
    void wrapToggleSwitchesLineWrapMode()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        QVERIFY(text);
        QCOMPARE(text->lineWrapMode(), QPlainTextEdit::NoWrap);
        window.findChild<QAction *>(QStringLiteral("wrapTraffic"))->setChecked(true);
        QCOMPARE(text->lineWrapMode(), QPlainTextEdit::WidgetWidth);
        window.findChild<QAction *>(QStringLiteral("wrapTraffic"))->setChecked(false);
        QCOMPARE(text->lineWrapMode(), QPlainTextEdit::NoWrap);
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
