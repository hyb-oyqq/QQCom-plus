#include "mainwindow.h"
#include "src/ui/payloadpreview.h"
#include "src/ui/trafficpanel.h"

#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableView>
#include <QTableWidget>
#include <QTabWidget>
#include <QtTest>

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
        QTest::addColumn<QString>("input");
        QTest::addColumn<int>("mode");
        QTest::addColumn<int>("encoding");
        QTest::addColumn<int>("suffix");
        QTest::addColumn<QByteArray>("expected");
        QTest::newRow("hex") << "00 7F 80 FF" << 0 << 0 << 0 << QByteArray::fromHex("007f80ff");
        QTest::newRow("compact") << "007F80FF" << 0 << 0 << 0 << QByteArray::fromHex("007f80ff");
        QTest::newRow("prefix") << "0x01 0xA0" << 0 << 0 << 0 << QByteArray::fromHex("01a0");
        QTest::newRow("raw-high-byte") << "A\\r\\n\\0\\xFF" << 2 << 0 << 0 << QByteArray::fromHex("410d0a00ff");
        QTest::newRow("utf8") << QStringLiteral("中") << 1 << 0 << 0 << QByteArray::fromHex("e4b8ad");
        QTest::newRow("suffix") << "AT" << 1 << 0 << 3 << QByteArray("AT\r\n");
        QTest::newRow("backslash") << "a\\\\b" << 2 << 0 << 0 << QByteArray("a\\b");
    }

    void payload()
    {
        QFETCH(QString, input);
        QFETCH(int, mode);
        QFETCH(int, encoding);
        QFETCH(int, suffix);
        QFETCH(QByteArray, expected);
        const auto result = encodePreview(input, mode, encoding, suffix);
        QVERIFY2(result.valid(), qPrintable(result.error));
        QCOMPARE(result.bytes, expected);
    }

    void invalidPayload_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<int>("mode");
        QTest::addColumn<int>("encoding");
        QTest::newRow("odd") << "A" << 0 << 0;
        QTest::newRow("illegal") << "01 GG" << 0 << 0;
        QTest::newRow("prefix-short") << "0x1" << 0 << 0;
        QTest::newRow("comma") << "01,02" << 0 << 0;
        QTest::newRow("short-escape") << "\\xF" << 2 << 0;
        QTest::newRow("unknown-escape") << "\\q" << 2 << 0;
        QTest::newRow("trailing-escape") << "a\\" << 2 << 0;
        QTest::newRow("unrepresentable") << QStringLiteral("中") << 1 << 1;
    }

    void invalidPayload()
    {
        QFETCH(QString, input);
        QFETCH(int, mode);
        QFETCH(int, encoding);
        const auto result = encodePreview(input, mode, encoding, 0);
        QVERIFY(!result.valid());
        QVERIFY(result.bytes.isEmpty());
    }

    void payloadSizeIncludesSuffix()
    {
        QVERIFY(encodePreview(QString(65536, 'a'), 1, 0, 0).valid());
        QVERIFY(!encodePreview(QString(65536, 'a'), 1, 0, 3).valid());
    }

    void filtersPreserveRawData()
    {
        TrafficModel model;
        TrafficFilter filter;
        const auto a = QUuid::createUuid(), b = QUuid::createUuid();
        model.replaceRecords({{QDateTime(), a, "A", false, QByteArray::fromHex("007fff")},
                              {QDateTime(), b, "B", true, "hello"}});
        filter.setSourceModel(&model);
        filter.setOptions(a, true, true, "7F FF");
        QCOMPARE(filter.rowCount(), 1);
        model.setHex(false);
        QCOMPARE(model.record(0).payload, QByteArray::fromHex("007fff"));
        filter.setOptions({}, true, false, {});
        QCOMPARE(filter.rowCount(), 1);
        filter.setOptions({}, false, true, "HELLO");
        QCOMPARE(filter.rowCount(), 1);
        filter.setOptions({}, false, false, {});
        QCOMPARE(filter.rowCount(), 0);
    }

    void portDraftsRemainIndependent()
    {
        MainWindow window;
        auto *ports = window.findChild<QListWidget *>("portList");
        auto *name = window.findChild<QLineEdit *>("portName");
        auto *serial = window.findChild<QLineEdit *>("serial.port");
        auto *udpPort = window.findChild<QSpinBox *>("udp.port");
        QVERIFY(ports && name && serial && udpPort);
        QCOMPARE(ports->count(), 4);
        ports->setCurrentRow(0);
        name->setText("Device A");
        serial->setText("COM17");
        ports->setCurrentRow(3);
        udpPort->setValue(23456);
        ports->setCurrentRow(0);
        QCOMPARE(name->text(), QString("Device A"));
        QCOMPARE(serial->text(), QString("COM17"));
        ports->setCurrentRow(3);
        QCOMPARE(udpPort->value(), 23456);
    }

    void sendPreviewRejectsInvalidInput()
    {
        MainWindow window;
        auto *editor = window.findChild<QPlainTextEdit *>("sendEditor");
        auto *save = window.findChild<QPushButton *>("saveCommand");
        QVERIFY(editor && save);
        editor->setPlainText("01 GG");
        QVERIFY(!save->isEnabled());
        editor->setPlainText("00 FF");
        QVERIFY(save->isEnabled());
        for (auto *button : window.findChildren<QPushButton *>("PrimaryButton"))
            QVERIFY(!button->isEnabled());
    }

    void ruleDraftsRejectSelfAndDuplicate()
    {
        MainWindow window;
        window.showRoutes();
        auto *source = window.findChild<QComboBox *>("routeSource");
        auto *target = window.findChild<QComboBox *>("routeTarget");
        auto *add = window.findChild<QPushButton *>("addRoute");
        auto *table = window.findChild<QTableWidget *>("routeTable");
        QVERIFY(source && target && add && table);
        target->setCurrentIndex(source->currentIndex());
        QVERIFY(!add->isEnabled());
        target->setCurrentIndex(1);
        QVERIFY(add->isEnabled());
        add->click();
        QCOMPARE(table->rowCount(), 1);
        add->click();
        QCOMPARE(table->rowCount(), 1);
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
        QCOMPARE(table->model()->rowCount(), 12);
        QVERIFY(window.rect().contains(QRect(editor->mapTo(&window, QPoint()), editor->size())));
        auto *search = window.findChild<QLineEdit *>("trafficSearch");
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
    UiSmokeTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_ui.moc"
