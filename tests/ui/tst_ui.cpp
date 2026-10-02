#include "mainwindow.h"
#include "src/ui/common/payloadcodec.h"
#include "src/ui/panels/sending/sendpanel.h"
#include "src/ui/panels/traffic/trafficpanel.h"
#include "src/discovery/portdiscoveryservice.h"
#include "src/persistence/preferencesstore.h"

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
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QStyleOptionSpinBox>
#include <QTableView>
#include <QTabWidget>
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
        model.replaceRecords({{QDateTime(), a, "A", false, QByteArray::fromHex("007fff")},
                              {QDateTime(), b, "B", true, "hello"}});
        filter.setSourceModel(&model);
        // 端点为独立开关：勾选集合内的才显示，一个都不勾则不显示。
        filter.setOptions({a, b}, true, true, "7F FF");
        QCOMPARE(filter.rowCount(), 1);
        // 显示格式只影响渲染，模型里的原始字节始终不变。
        model.setFormat(PayloadCodec::Format::Utf8);
        QCOMPARE(model.record(0).payload, QByteArray::fromHex("007fff"));
        filter.setOptions({a}, true, false, {});
        QCOMPARE(filter.rowCount(), 1);
        filter.setOptions({b}, false, true, "HELLO");
        QCOMPARE(filter.rowCount(), 1);
        filter.setOptions({b}, false, false, {});
        QCOMPARE(filter.rowCount(), 0);
        filter.setOptions({}, false, false, {});
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

    void endpointRowsStayVisibleAndInline()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(50);
        auto *a = window.findChild<PortSelectorWidget *>("endpointA");
        auto *b = window.findChild<PortSelectorWidget *>("endpointB");
        auto *traffic = window.findChild<QTableView *>("trafficTable");
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
            QVERIFY(!action->isEnabled()); // Preview only; no transport is wired yet.
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

    void compactWindowAndExamples()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);
        window.loadExample();
        auto *table = window.findChild<QTableView *>("trafficTable");
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
        const QModelIndex firstData = table->model()->index(0, 4);
        QCOMPARE(firstData.data().toString(), QStringLiteral("01 03 00 00 00 02 C4 0B"));
        trafficFormat->setCurrentIndex(1); // DEC
        QCOMPARE(table->model()->index(0, 4).data().toString(),
                 QStringLiteral("1 3 0 0 0 2 196 11"));
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
