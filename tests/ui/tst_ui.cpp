#include "mainwindow.h"
#include "domain/receiveframing.h"
#include "ui/common/endpointformat.h"
#include "ui/common/payloadcodec.h"
#include "ui/panels/sending/sendpanel.h"
#include "ui/panels/traffic/filterbar.h"
#include "ui/panels/traffic/trafficpanel.h"
#include "ui/panels/traffic/traffictextview.h"
#include "discovery/portdiscoveryservice.h"
#include "persistence/preferencesstore.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QInputMethodEvent>
#include <QLineEdit>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QStyleOptionSpinBox>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextBlock>
#include <QTextFragment>
#include <QToolButton>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

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

// A horizontal strip must stay one row at any window width: no wrapping, no scrollbar.
// Qt never wraps a QHBoxLayout, so the failure mode is overlap or a control pushed past the
// right edge. Anything in the same vertical band that overlaps horizontally is a failure;
// controls in different bands (the second row under the toolbar) are compared separately.
QString overlapInRow(QWidget *container)
{
    QList<QWidget *> items;
    for (auto *child : container->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly))
        if (child->isVisibleTo(container) && child->width() > 1) items.append(child);
    std::sort(items.begin(), items.end(),
        [](QWidget *left, QWidget *right) { return left->x() < right->x(); });
    for (int i = 1; i < items.size(); ++i) {
        const auto *previous = items.at(i - 1);
        const auto *current = items.at(i);
        const bool sameBand = current->y() < previous->y() + previous->height()
            && previous->y() < current->y() + current->height();
        if (sameBand && current->x() < previous->x() + previous->width())
            return QStringLiteral("%1 压住 %2").arg(previous->objectName(), current->objectName());
    }
    for (auto *item : items)
        if (item->x() + item->width() > container->width())
            return QStringLiteral("%1 越过右缘（%2 > %3）")
                       .arg(item->objectName())
                       .arg(item->x() + item->width())
                       .arg(container->width());
    return QString();
}

// 取一个块最后一个分片的前景色。数据行的末片是正文色（HEX 对照行整行一个分片），
// 用来比对"哪一行更浅"。QColor::value() 是 HSV 明度，正好当"深浅"的判据。
QColor lastFragmentColor(const QTextBlock &block)
{
    QColor color;
    for (auto it = block.begin(); !it.atEnd(); ++it)
        color = it.fragment().charFormat().foreground().color();
    return color;
}

// 外观（编码格式 / 显示设置）改动会经防抖（trafficpanel.cpp 的 ReprojectDebounceMs = 50ms）
// 排队一次**设计上必然发生**的重投影：断言前必须显式等掉它，不能把用例建立在
// "它恰好还没触发"这种时序运气上（那正是这两条用例此前 flaky 的根因）。
// 90 = 50ms 防抖窗口 + 40ms 余量。
constexpr int AppearanceReprojectWaitMs = 90;

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
        options.keywords = {QStringLiteral("7F FF")};
        QCOMPARE(accepted(options), 1);
        // 原始字节是事实依据，任何过滤与格式设置都改不了模型里的记录。
        QCOMPARE(model.record(0).raw, QByteArray::fromHex("007fff"));
        options.keywords.clear();
        options.endpoints = {a};
        options.tx = false;
        QCOMPARE(accepted(options), 1);
        options.endpoints = {b};
        options.rx = false;
        options.tx = true;
        options.keywords = {QStringLiteral("HELLO")};
        QCOMPARE(accepted(options), 1);
        options.keywords.clear();
        options.tx = false;
        QCOMPARE(accepted(options), 0);
        options.endpoints.clear();
        QCOMPARE(accepted(options), 0);
    }

    // 内容条件的分组语义：关键词组内 OR、排除词压过关键词、大小写开关、
    // 非法正则**不参与判定**（退化为不过滤，绝不静默吞掉整屏数据）。
    void contentConditionsCombineAsGroups()
    {
        PortDraft port;
        port.kind = PortDraft::Serial;
        const auto source = Traffic::makeSource(port, 0);
        const auto make = [&source](const QByteArray &raw) {
            return Traffic::makeSampleMessage(1, QDateTime(), source, Traffic::Direction::Receive, raw);
        };
        const auto heartbeat = make(QByteArray("heartbeat ok"));
        const auto temperature = make(QStringLiteral("温度 25.0").toUtf8());
        const auto upper = make(QByteArray("HEARTBEAT"));

        const auto hits = [&](const TrafficFilter::Options &options) {
            int count = 0;
            for (const auto &record : {heartbeat, temperature, upper})
                if (TrafficFilter::accepts(record, options, PayloadCodec::Format::Utf8)) ++count;
            return count;
        };

        TrafficFilter::Options options;
        options.endpoints = {source.endpointId};
        // 组内 OR：两个词命中任一即显示，大小写默认不敏感。
        options.keywords = {QStringLiteral("heartbeat"), QStringLiteral("温度")};
        QCOMPARE(hits(options), 3);
        // 排除词优先于关键词：命中即隐藏，压过已满足的关键词。
        options.excludes = {QStringLiteral("温度")};
        QCOMPARE(hits(options), 2);
        options.excludes.clear();
        options.caseSensitive = true;
        QCOMPARE(hits(options), 2); // 区分大小写后 HEARTBEAT 不再命中

        options.caseSensitive = false;
        options.regex = true;
        options.keywords = {QStringLiteral("heart.*")};
        QCOMPARE(hits(options), 2); // 合法正则按正则解释
        // 非法正则这条条件被丢掉，等价于"没写关键词"——不是"什么都不显示"。
        options.keywords = {QStringLiteral("heart[")};
        QCOMPARE(hits(options), 3);
    }

    // 形态铁律：条件条恒为一行（不因条件增多或窗口变窄而换行）、每个按钮都带字形、
    // 匹配选项就摆在条上而不是收进下拉、折叠只隐藏第二行而不清条件。
    void filterBarKeepsToASingleRow()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        auto *bar = window.findChild<QWidget *>(QStringLiteral("filterBar"));
        auto *toggle = window.findChild<QPushButton *>(QStringLiteral("filterToggle"));
        QVERIFY(bar && toggle);
        // 默认展开，且条子高度就是一行控件——"两行头"的全部意义都在这里。
        QVERIFY(toggle->isChecked());
        QVERIFY(bar->isVisibleTo(&window));
        const int rowHeight = window.findChild<QComboBox *>(QStringLiteral("trafficFormat"))->height();
        QVERIFY2(bar->height() <= rowHeight + 12,
            qPrintable(QStringLiteral("过滤条高 %1 超过一行 %2").arg(bar->height()).arg(rowHeight)));

        // 条子上的每个按钮都必须带 Material 字形：只有文字说明的按钮在窄宽度下会先被牺牲。
        const QStringList named{QStringLiteral("filterCaseSensitive"), QStringLiteral("filterRegex"),
            QStringLiteral("filterRebuild"), QStringLiteral("filterReset")};
        for (const auto &name : named) {
            auto *button = bar->findChild<QPushButton *>(name);
            QVERIFY2(button, qPrintable(QStringLiteral("缺按钮：") + name));
            QVERIFY2(!button->icon().isNull(), qPrintable(QStringLiteral("缺字形：") + name));
        }
        // 匹配选项是条上的按钮，不是下拉列表：既没有菜单，也没有失去"一眼可见"的入口。
        auto *caseButton = bar->findChild<QPushButton *>(QStringLiteral("filterCaseSensitive"));
        auto *regexButton = bar->findChild<QPushButton *>(QStringLiteral("filterRegex"));
        QVERIFY(caseButton->isCheckable() && regexButton->isCheckable());
        QVERIFY(!caseButton->menu() && !regexButton->menu());
        QVERIFY(!bar->findChild<QPushButton *>(QStringLiteral("filterMore")));
        QVERIFY(!window.findChild<QAction *>(QStringLiteral("filterRegex")));

        // 关键词不做 chip：输入框本身就是条件唯一的样子，不产生第二份视图；
        // 输入框也不带清除按钮（✕），"全选重输"比瞄准小叉更快。
        auto *keywords = bar->findChild<QLineEdit *>(QStringLiteral("filterKeywords"));
        auto *excludes = bar->findChild<QLineEdit *>(QStringLiteral("filterExcludes"));
        QVERIFY(keywords && excludes);
        QVERIFY(!keywords->isClearButtonEnabled() && !excludes->isClearButtonEnabled());
        keywords->setText(QStringLiteral("心跳, 0D0A ,,心跳"));
        QVERIFY(bar->findChildren<QPushButton *>(QStringLiteral("filterChip")).isEmpty());

        // 条件是逗号分隔的，且 trim / 去空 / 去重：HEX 条件里的空格因此能原样保留。
        const auto conditions = FilterBar::splitConditions(QStringLiteral("心跳, AA 55 ,,心跳,error"));
        QCOMPARE(conditions, QStringList({QStringLiteral("心跳"), QStringLiteral("AA 55"),
                                   QStringLiteral("error")}));
        QVERIFY(FilterBar::splitConditions(QString()).isEmpty());

        // 折叠只隐藏第二行：条件、判定器与记忆状态一律不动。
        toggle->setChecked(false);
        QVERIFY(!bar->isVisibleTo(&window));
        QVERIFY2(toggle->text().contains(QStringLiteral("▸")), qPrintable(toggle->text()));
        QCOMPARE(keywords->text(), QStringLiteral("心跳, 0D0A ,,心跳"));
        toggle->setChecked(true);
        QVERIFY(bar->isVisibleTo(&window));

        // 条上加过好几个控件了：最小窗口宽下第二行仍必须一行排得下（不重叠、不越右缘）。
        window.resize(1120, 660);
        QTest::qWait(30);
        const auto problem = overlapInRow(bar);
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
    }

    // "重新过滤"：把当前条件应用到**已经收到**的记录上（显式回溯）。
    // 到达时判定是控制台语义，改条件不会动旧行；这个按钮是唯一的显式回溯入口，
    // 因此它必须真的重放、必须重排行号、必须自己说清"只回溯模型保留范围"。
    void rebuildAppliesConditionsToExistingRecords()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        auto *panel = window.findChild<TrafficPanel *>();
        panel->setPreferences(nullptr); // 条件不落盘，避免污染共享配置
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        auto *model = window.findChild<TrafficModel *>();
        auto *bar = window.findChild<QWidget *>(QStringLiteral("filterBar"));
        auto *rebuild = window.findChild<QPushButton *>(QStringLiteral("filterRebuild"));
        QVERIFY(text && model && bar && rebuild);
        window.findChild<QComboBox *>(QStringLiteral("trafficFormat"))
            ->setCurrentText(QStringLiteral("UTF-8"));
        // 还没收到任何记录时无从回溯，按钮应该是灰的。
        QVERIFY(!rebuild->isEnabled());

        const QUuid endpointId = window.findChild<QPushButton *>(QStringLiteral("showPortA"))
                                     ->property("endpoint").toUuid();
        QList<Traffic::Message> batch;
        for (int i = 0; i < 6; ++i) {
            Traffic::Message message;
            message.id = i + 1;
            message.timestamp = QDateTime::currentDateTime();
            message.direction = Traffic::Direction::Receive;
            message.source.endpointId = endpointId;
            message.source.endpointIndex = 0;
            message.source.endpointKind = PortDraft::Serial;
            message.raw = (i % 2 == 0) ? QByteArray("心跳 ok") : QByteArray("temp 25");
            message.rawLength = message.raw.size();
            batch.append(message);
        }
        // 先按"只收心跳"到达：半数被挡在屏幕之外。
        auto *keywords = bar->findChild<QLineEdit *>(QStringLiteral("filterKeywords"));
        keywords->setText(QStringLiteral("心跳"));
        panel->appendMessages(batch);
        QTRY_COMPARE(model->rowCount(), 6);
        QTRY_COMPARE(text->document()->blockCount(), 3); // 三条心跳
        QVERIFY(rebuild->isEnabled());

        // 放宽条件后旧行不会自己回来（控制台语义），屏幕上仍是 3 条。
        keywords->setText(QStringLiteral("心跳,temp"));
        QCOMPARE(text->document()->blockCount(), 3);

        // 点"重新过滤"才把新条件应用到已有记录：六条全中，行号从 1 连续重排。
        rebuild->click();
        QTRY_COMPARE(text->document()->blockCount(), 6);
        QVERIFY2(text->document()->firstBlock().text().startsWith(QStringLiteral("1 ")),
            qPrintable(text->document()->firstBlock().text()));
        QVERIFY2(text->document()->lastBlock().text().startsWith(QStringLiteral("6 ")),
            qPrintable(text->document()->lastBlock().text()));

        // 反向也成立：条件收窄后旧行同样要等这个按钮才消失。
        keywords->setText(QStringLiteral("temp"));
        QCOMPARE(text->document()->blockCount(), 6);
        rebuild->click();
        QTRY_COMPARE(text->document()->blockCount(), 3);
        for (const auto &block : text->document()->toPlainText().split(QLatin1Char('\n')))
            QVERIFY(!block.isEmpty());
    }

    // 方向与事件开关常驻顶栏（用户钦定）：它们**不在**可折叠的条件条里——
    // 条件条收起来时这排开关仍在原位，而且每个都是"图标 + 文案"，
    // 不必悬停才知道这排按钮在做什么。代价压在顶栏这一行的宽度上，所以末尾还要量尺寸。
    void filterFlagsStayOnTopRow()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);

        auto *flags = window.findChild<QWidget *>(QStringLiteral("filterFlags"));
        auto *bar = window.findChild<QWidget *>(QStringLiteral("filterBar"));
        auto *toggle = window.findChild<QPushButton *>(QStringLiteral("filterToggle"));
        QVERIFY(flags && bar && toggle);

        const QStringList named{QStringLiteral("filterRx"), QStringLiteral("filterTx"),
            QStringLiteral("filterState"), QStringLiteral("filterWarning"), QStringLiteral("filterError")};
        for (const auto &name : named) {
            auto *button = flags->findChild<QPushButton *>(name);
            QVERIFY2(button, qPrintable(QStringLiteral("缺开关：") + name));
            QVERIFY2(!button->icon().isNull(), qPrintable(QStringLiteral("缺字形：") + name));
            QVERIFY2(!button->text().isEmpty(), qPrintable(QStringLiteral("缺文案：") + name));
            // 同一个开关不能既在顶栏又在条件条里：条件分散在两个控件之间，
            // "重置一次清干净"全靠这条边界清楚。
            QVERIFY2(!bar->findChild<QPushButton *>(name),
                qPrintable(QStringLiteral("开关不应出现在条件条里：") + name));
        }

        // 收起条件条后这排开关照旧可见——它们只管"哪些行进来"，不属于被折叠的内容条件。
        toggle->setChecked(false);
        QVERIFY(!bar->isVisibleTo(&window));
        for (const auto &name : named)
            QVERIFY2(flags->findChild<QPushButton *>(name)->isVisibleTo(&window),
                qPrintable(QStringLiteral("折叠后开关应仍可见：") + name));
        toggle->setChecked(true);

        // "过滤 · N" 是两处的合计：只关掉 RX（条件条里一条内容条件都没有）也必须算进去。
        auto *rx = flags->findChild<QPushButton *>(QStringLiteral("filterRx"));
        // 只关掉 RX 时重置按钮必须是亮的，否则那半套条件没有回到默认的入口。
        auto *reset = bar->findChild<QPushButton *>(QStringLiteral("filterReset"));
        QVERIFY(reset && !reset->isEnabled());
        rx->setChecked(false);
        QVERIFY2(toggle->text().contains(QStringLiteral("· 1")), qPrintable(toggle->text()));
        QVERIFY(reset->isEnabled());
        // 重置一次清干净：顶栏开关与条件条内容条件一起回到不过滤。
        bar->findChild<QLineEdit *>(QStringLiteral("filterKeywords"))->setText(QStringLiteral("心跳"));
        QVERIFY(toggle->text().contains(QStringLiteral("· 2")));
        reset->click();
        QVERIFY(rx->isChecked());
        QVERIFY(bar->findChild<QLineEdit *>(QStringLiteral("filterKeywords"))->text().isEmpty());
        QVERIFY2(toggle->text().contains(QStringLiteral("过滤 ")), qPrintable(toggle->text()));

        // 顶栏必须排得下：开关常驻的代价就是这一行的宽度。窗口最小宽 1120，
        // 一行放不下的表现是控件重叠或右端控件被切掉，两种都要挡住。
        window.resize(1120, 660);
        QTest::qWait(30);
        QWidget *row = flags->parentWidget();
        QVERIFY(row);
        for (const auto &name : {QStringLiteral("showPortA"), QStringLiteral("showPortB"),
                 QStringLiteral("trafficFormat"), QStringLiteral("filterToggle"),
                 QStringLiteral("filterFlags"), QStringLiteral("displaySettings"),
                 QStringLiteral("exportTraffic"), QStringLiteral("followTraffic"),
                 QStringLiteral("clearTraffic")})
            QVERIFY2(row->findChild<QWidget *>(name), qPrintable(QStringLiteral("缺顶栏元素：") + name));
        const auto crowded = overlapInRow(row);
        QVERIFY2(crowded.isEmpty(), qPrintable(crowded));
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
        options.keywords = {QStringLiteral("52344")};
        QCOMPARE(accepted(options), 2);
        options.keywords.clear();

        // 错误/警告日志各自有独立开关：关闭后 Warning/Error 事件不进显示，数据不受影响。
        const auto warning = Traffic::makeSampleMessage(
            4, QDateTime(), b, Traffic::Direction::Warning, {});
        const auto error = Traffic::makeSampleMessage(
            5, QDateTime(), b, Traffic::Direction::Error, {});
        model.replaceRecords({stream, datagram, message, warning, error});
        QCOMPARE(accepted(options), 5);
        options.warnings = false;
        options.errors = false;
        QCOMPARE(accepted(options), 3);
        options.warnings = true;
        options.errors = true;
    }

    // 端点草稿现在随配置即时持久化（endpoint/A、endpoint/B）：共享 ini 里
    // 前序用例留下的端点状态会污染"默认端点"假设，依赖该假设的用例先清场。
    // 用 remove 而不是写 QVariant()：后者留下 @Invalid()，不是"没有这个键"。
    void clearEndpointDrafts()
    {
        const QString ini = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
            + QStringLiteral("/preferences.ini");
        PreferencesStore sweep(nullptr, ini);
        sweep.remove(QStringLiteral("endpoint/A"));
        sweep.remove(QStringLiteral("endpoint/B"));
    }

    // 每个用例跑完清掉共享 ini 里的过滤条件。过滤条会把条件写进共享配置，
    // 前一个用例留下的关键词会让后面的用例"一条都收不到"——与端点草稿同一类串扰，
    // 只是后果更隐蔽：屏幕全空看起来像面板坏了，而不是配置被污染了。
    // 放在 cleanup() 而不是各用例开头，是为了让用例中途断言失败也照样清场。
    void cleanup()
    {
        const QString ini = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
            + QStringLiteral("/preferences.ini");
        PreferencesStore sweep(nullptr, ini);
        for (const auto *key : {"filter/keywords", "filter/excludes", "filter/regex",
                 "filter/caseSensitive", "filter/rx", "filter/tx", "filter/states",
                 "filter/warnings", "filter/errors", "filter/expanded"})
            sweep.remove(QString::fromLatin1(key));
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
    // 能把条数堆到十几万，每条 Message 的元数据（时间、来源、三个 QUuid、
    // 两个 QString）远大于它那点载荷——"限了字节"不等于"限了内存"。
    // 默认预算是**安全阀**（512 MiB / 2000 万条，正常压测碰不到），用例要验的是
    // 淘汰机制本身，所以显式把预算压小——被测对象从"那个数字"变成"这套语义"。
    void historyBudgetAlsoLimitsRowCount()
    {
        TrafficModel model;
        const int rowBudget = 5000;
        model.setBudget(64LL * 1024 * 1024, rowBudget);
        Traffic::Message tiny;
        tiny.direction = Traffic::Direction::Receive;
        tiny.source.endpointIndex = 0;
        tiny.source.endpointKind = PortDraft::Serial;
        tiny.source.endpointId = QUuid::createUuid();
        tiny.raw = QByteArray::fromHex("0102"); // 2 字节：64 MiB 字节预算能装三千万条
        tiny.rawLength = tiny.raw.size();

        const int rounds = rowBudget / 1000 + 10;
        quint64 id = 1;
        for (int round = 0; round < rounds; ++round) {
            QList<Traffic::Message> batch;
            batch.reserve(1000);
            for (int i = 0; i < 1000; ++i) {
                auto message = tiny;
                message.id = id++;
                batch.append(message);
            }
            model.appendRecords(batch);
        }
        QCOMPARE(model.rowCount(), rowBudget);
        // 淘汰只从最旧一端发生：最新的一定在，最旧的恰好被挤掉。
        QCOMPARE(model.record(model.rowCount() - 1).id, id - 1);
        QCOMPARE(model.record(0).id, id - rowBudget);
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
        auto *rx = firstTraffic.findChild<QPushButton *>(QStringLiteral("filterRx"));
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
        QCOMPARE(secondTraffic.findChild<QPushButton *>(QStringLiteral("filterRx"))->isChecked(), false);
        QCOMPARE(secondTraffic.findChild<QComboBox *>(QStringLiteral("trafficFormat"))->currentText(),
                 QStringLiteral("UTF-8"));
        QCOMPARE(secondSender.findChild<QComboBox *>(QStringLiteral("sendFormat"))->currentText(),
                 QStringLiteral("GBK"));
    }

    void logRecordingFollowsDisplayFormat()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // 用户选定的是"模板路径"：实际文件名每次带时间戳另起，模板本身不写。
        const auto templatePath = directory.filePath(QStringLiteral("traffic.log"));
        PreferencesStore store(nullptr, directory.filePath(QStringLiteral("preferences.ini")));
        // 预置路径：记录开关开启时若无路径会弹文件对话框，注入路径绕开交互。
        store.setValue(QStringLiteral("traffic/recordPath"), templatePath);
        TrafficPanel panel;
        panel.setPreferences(&store);
        auto *record = panel.findChild<QAction *>(QStringLiteral("recordTraffic"));
        auto *format = panel.findChild<QComboBox *>(QStringLiteral("trafficFormat"));
        QVERIFY(record && format);
        QCOMPARE(format->currentText(), QStringLiteral("HEX"));

        // 本次实际写入的文件 = 模板基名 + 时间戳 + 扩展名，落在同一目录。
        const auto currentLogFile = [&directory]() -> QString {
            const auto names = QDir(directory.path()).entryList(
                {QStringLiteral("traffic-*.log")}, QDir::Files);
            return names.isEmpty() ? QString() : QDir(directory.path()).filePath(names.first());
        };

        record->setChecked(true);
        const auto logPath = currentLogFile();
        QVERIFY2(!logPath.isEmpty(), "开启记录后应生成本次带时间戳的日志文件");
        QVERIFY(logPath != templatePath); // 绝不覆盖/复用模板路径本身

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
        // 首行是新建文件的元信息头：日期时间 + 当前显示格式；其次才是数据行。
        QCOMPARE(hexLines.size(), 3);
        QVERIFY2(hexLines.at(0).startsWith(QStringLiteral("# easyDebug")), qPrintable(hexLines.at(0)));
        QVERIFY(hexLines.at(0).contains(QStringLiteral("格式=HEX")));
        QVERIFY(hexLines.at(1).contains(QStringLiteral("A · 串口")));
        QVERIFY(hexLines.at(1).contains(QStringLiteral("RX")));
        QVERIFY(hexLines.at(1).endsWith(QStringLiteral("01 02"))); // HEX 模式记 HEX
        QVERIFY(hexLines.at(2).endsWith(QStringLiteral("FF")));
        log.close();

        // 切换显示格式后新行跟随新模式：所见即所记；并追加一行"格式切换"元信息。
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
        // 头 + 2 数据 + 格式切换行 + 1 数据 = 5 行
        QCOMPARE(all.size(), 5);
        QVERIFY(all.at(3).contains(QStringLiteral("格式切换")));
        QVERIFY(all.at(3).contains(QStringLiteral("UTF-8")));
        QVERIFY(all.at(4).endsWith(QStringLiteral("ok")));
    }

    // 三时刻重开：① 启动恢复记录、② 点「清空」、③ 手动勾选「记录到文件」——每次都
    // 新起一个带时间戳的文件，**绝不覆盖、绝不追加到旧文件**。这条用例用 QTemporaryDir
    // 预置偏好绕开文件对话框，独立验收整条语义（工程师改过的 logRecordingFollowsDisplayFormat
    // 只覆盖了"开启记录后跟随显示格式"这一条路径）。
    void logRecordingReopensOnSessionBoundaries()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // 模板路径 = 用户选定的"目录 + 基名 + 扩展名"，它自身不是任何一次记录的目标。
        // 事先写入垃圾内容：任何一次记录都不许改写或追加到这个文件。
        const auto templatePath = directory.filePath(QStringLiteral("traffic.log"));
        const QByteArray seed("SEED-MUST-NOT-BE-TOUCHED\n");
        {
            QFile file(templatePath);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
            QCOMPARE(file.write(seed), qint64(seed.size()));
        }

        PreferencesStore store(nullptr, directory.filePath(QStringLiteral("preferences.ini")));
        store.setValue(QStringLiteral("traffic/recordPath"), templatePath);
        // record=true → 第一时刻：启动恢复记录，直接开新文件、不弹对话框。
        store.setValue(QStringLiteral("traffic/record"), true);

        TrafficPanel panel;
        panel.setPreferences(&store);
        auto *record = panel.findChild<QAction *>(QStringLiteral("recordTraffic"));
        auto *format = panel.findChild<QComboBox *>(QStringLiteral("trafficFormat"));
        auto *clear = panel.findChild<QPushButton *>(QStringLiteral("clearTraffic"));
        auto *model = panel.findChild<TrafficModel *>();
        QVERIFY(record && format && clear && model);
        QCOMPARE(record->isChecked(), true); // 启动恢复：记录开关勾上

        // 目录里本次记录产生的全部文件（traffic-<stamp>[-n].log），按名字排序便于比对。
        const auto logFiles = [&directory]() {
            return QDir(directory.path()).entryList(
                {QStringLiteral("traffic-*.log")}, QDir::Files, QDir::Name);
        };
        // 当前正在写的文件：从「记录到文件」的 tooltip 精确读出，比 glob 更不易误判。
        const auto currentLogPath = [&directory, record]() -> QString {
            const QString marker = QStringLiteral("正在写 ");
            const int at = record->toolTip().indexOf(marker);
            if (at < 0) return QString();
            return QDir(directory.path()).filePath(record->toolTip().mid(at + marker.size()).trimmed());
        };
        const auto readLines = [](const QString &path) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return QStringList();
            QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
            while (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();
            return lines;
        };

        // ---- C1：启动恢复记录 → 实际文件是 traffic-<stamp>.log；模板不被改写/追加 ----
        const QString firstLog = currentLogPath();
        QVERIFY2(!firstLog.isEmpty(), "启动恢复记录后应已有正在写入的日志文件");
        QVERIFY(firstLog != templatePath);
        QVERIFY(QFileInfo(firstLog).fileName().startsWith(QStringLiteral("traffic-")));
        QVERIFY(QFileInfo(firstLog).fileName().endsWith(QStringLiteral(".log")));
        {
            QFile file(templatePath);
            // 写时是 Text 模式（Windows 下 '\n'→"\r\n"），读回也要 Text 模式才能还原，
            // 否则比的是行尾差异而不是"内容有没有被改写"。
            QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
            QCOMPARE(file.readAll(), seed); // 模板内容原样：它只提供目录+基名+扩展名
        }

        // ---- C2：新文件首行是元信息头 # easyDebug ... 格式=... ----
        QStringList lines = readLines(firstLog);
        QVERIFY(!lines.isEmpty());
        QVERIFY2(lines.at(0).startsWith(QStringLiteral("# easyDebug ")), qPrintable(lines.at(0)));
        QVERIFY2(lines.at(0).contains(QStringLiteral("格式=")), qPrintable(lines.at(0)));

        // ---- C3：记录若干条 → 数据行数正确、每行 4 个制表符、且没有额外的 # 行 ----
        Traffic::Message rx;
        rx.direction = Traffic::Direction::Receive;
        rx.source.endpointIndex = 0;
        rx.source.endpointKind = PortDraft::Serial;
        rx.source.endpointId = QUuid::createUuid();
        rx.raw = QByteArray::fromHex("0102");
        rx.rawLength = rx.raw.size();
        Traffic::Message tx = rx;
        tx.direction = Traffic::Direction::Transmit;
        tx.source.endpointIndex = 1;
        tx.source.endpointKind = PortDraft::TcpServer;
        tx.source.endpointId = QUuid::createUuid();
        tx.raw = QByteArray::fromHex("FF");
        tx.rawLength = tx.raw.size();
        panel.appendMessages({rx, tx});
        QTRY_COMPARE(model->rowCount(), 2); // 攒批落地即已写日志

        lines = readLines(firstLog);
        QCOMPARE(lines.size(), 3); // 头 + 2 数据
        int dataLines = 0;
        int metaLines = 0;
        for (const auto &line : lines) {
            if (line.startsWith(QLatin1Char('#'))) {
                ++metaLines;
                continue;
            }
            ++dataLines;
            QCOMPARE(line.count(QLatin1Char('\t')), 4); // 时间\t端点\t对端\t方向\t载荷
        }
        QCOMPARE(dataLines, 2);
        QCOMPARE(metaLines, 1); // 记录消息本身不产生 # 行

        // ---- C4：点「清空视图」是会话边界 → 另起一个文件，从头写、开关保持勾选 ----
        QCOMPARE(logFiles().size(), 1);
        clear->click();
        QCOMPARE(record->isChecked(), true); // 清的是屏幕，记录任务保持勾选
        QCOMPARE(logFiles().size(), 2);      // 换新文件；旧文件原样留在磁盘上
        const QString secondLog = currentLogPath();
        QVERIFY(!secondLog.isEmpty());
        QVERIFY(secondLog != firstLog);
        // 同秒撞名按约定退到 -2。用时间戳是否相同决定断言哪一支，避免跨秒抖动。
        const QString firstBase = QFileInfo(firstLog).completeBaseName();
        const QString secondBase = QFileInfo(secondLog).completeBaseName();
        if (secondBase == firstBase)
            QVERIFY2(secondBase == firstBase + QStringLiteral("-2"), qPrintable(secondBase));
        else
            QVERIFY(secondBase.startsWith(QStringLiteral("traffic-"))); // 跨秒：新时间戳
        const QStringList secondLines = readLines(secondLog);
        QCOMPARE(secondLines.size(), 1); // 只有头行，清空前的 2 条数据不在其中
        QVERIFY2(secondLines.at(0).startsWith(QStringLiteral("# easyDebug ")), qPrintable(secondLines.at(0)));

        // ---- C5：切换显示格式 → 当前文件追加一行 # 格式切换 → ... ----
        format->setCurrentText(QStringLiteral("UTF-8"));
        const QStringList afterSwitch = readLines(secondLog);
        QCOMPARE(afterSwitch.size(), 2); // 头 + 格式切换
        QVERIFY2(afterSwitch.last().startsWith(QStringLiteral("# 格式切换 → ")), qPrintable(afterSwitch.last()));
        QVERIFY(afterSwitch.last().contains(QStringLiteral("UTF-8")));

        // ================= D：独立反证（不重跑工程师走过的路径） =================
        // D②：同一秒内重开撞名 → 基名-stamp-2.后缀，而不是覆盖。系统时钟无法冻结，
        // 于是把"未来若干秒的基础名"全部用占位文件预先占住：无论触发落在哪一秒，代码
        // 都必须避开已存在的同名文件、退到 -2；且占位文件一个都不许被覆盖。
        {
            QTemporaryDir dir2;
            QVERIFY(dir2.isValid());
            const auto template2 = dir2.filePath(QStringLiteral("traffic.log"));
            PreferencesStore store2(nullptr, dir2.filePath(QStringLiteral("preferences.ini")));
            store2.setValue(QStringLiteral("traffic/recordPath"), template2);
            TrafficPanel panel2;
            panel2.setPreferences(&store2); // record 默认 false：此刻不应有文件
            auto *record2 = panel2.findChild<QAction *>(QStringLiteral("recordTraffic"));
            QVERIFY(record2);
            QVERIFY2(record2->toolTip().indexOf(QStringLiteral("正在写 ")) < 0,
                     "未开启记录时不应声称正在写文件");

            const QDateTime now = QDateTime::currentDateTime();
            QList<QString> decoys;
            for (int s = 0; s < 12; ++s) {
                const QString stamp = now.addSecs(s).toString(QStringLiteral("yyyyMMdd-HHmmss"));
                const QString path = QDir(dir2.path()).filePath(QStringLiteral("traffic-%1.log").arg(stamp));
                QFile decoy(path);
                QVERIFY(decoy.open(QIODevice::WriteOnly | QIODevice::Text));
                decoy.write("DECOY");
                decoy.close();
                decoys.append(path);
            }
            record2->setChecked(true); // 第三时刻：手动开启记录，m_logPath 非空 → 不弹对话框
            const QString tip = record2->toolTip();
            const int at = tip.indexOf(QStringLiteral("正在写 "));
            QVERIFY2(at >= 0, "开启记录后 tooltip 应显示正在写的文件");
            const QString produced = tip.mid(at + QStringLiteral("正在写 ").size()).trimmed();
            QVERIFY2(produced.endsWith(QStringLiteral("-2.log")), qPrintable(produced));
            QVERIFY(produced.startsWith(QStringLiteral("traffic-")));
            // 每个占位文件都原封不动：撞名走"改名"，不是"覆盖"。
            for (const auto &path : decoys) {
                QFile decoy(path);
                QVERIFY(decoy.open(QIODevice::ReadOnly | QIODevice::Text));
                QCOMPARE(decoy.readAll(), QByteArray("DECOY"));
            }
        }

        // D③：关闭记录开关后再清空视图 → 不产生新文件（记录任务已结束）。
        {
            QTemporaryDir dir3;
            QVERIFY(dir3.isValid());
            const auto template3 = dir3.filePath(QStringLiteral("traffic.log"));
            PreferencesStore store3(nullptr, dir3.filePath(QStringLiteral("preferences.ini")));
            store3.setValue(QStringLiteral("traffic/recordPath"), template3);
            store3.setValue(QStringLiteral("traffic/record"), true);
            TrafficPanel panel3;
            panel3.setPreferences(&store3);
            auto *record3 = panel3.findChild<QAction *>(QStringLiteral("recordTraffic"));
            auto *clear3 = panel3.findChild<QPushButton *>(QStringLiteral("clearTraffic"));
            QVERIFY(record3 && clear3);
            QCOMPARE(record3->isChecked(), true);
            const auto countFiles = [&dir3]() {
                return QDir(dir3.path()).entryList({QStringLiteral("traffic-*.log")}, QDir::Files).size();
            };
            QCOMPARE(countFiles(), 1); // 启动恢复记录：已有一个文件
            record3->setChecked(false); // 关闭记录 → 文件收口
            clear3->click();            // 清空视图：记录已关，不应再开新文件
            QCOMPARE(record3->isChecked(), false);
            QCOMPARE(countFiles(), 1); // 文件数不变
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

    // 端口打开失败（用户点连接的同步失败）必须主动可见（用户报"无法打开/冲突没有弹窗、
    // 也没有日志提示"）：① 监视区出现 Error 行，正文带端点标识 + 失败原因；② 非模态弹窗
    // （模态会独占 GUI 线程，自动化测试也会被卡死），标题带端点名、正文带原因；③ 同一
    // 端点同类错误 3 秒内只弹一次；④ 记录期间状态栏摘要出现当前日志大小，关掉后消失。
    // 打开失败用 TCP 服务器监听一个已被占用的端口制造：listen() 同步失败、立即返回，
    // 不依赖网络栈对"无人监听端口"的延迟（本机对被拒绝连接可能要有秒级 SYN 重试）。
    void portOpenFailureSurfacesErrorRowAndNonModalPopup()
    {
        clearEndpointDrafts();
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        PreferencesStore store(nullptr, directory.filePath(QStringLiteral("preferences.ini")));
        const auto templatePath = directory.filePath(QStringLiteral("traffic.log"));
        store.setValue(QStringLiteral("traffic/recordPath"), templatePath);
        store.setValue(QStringLiteral("traffic/record"), true);

        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        auto *panel = window.findChild<TrafficPanel *>();
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        auto *model = window.findChild<TrafficModel *>();
        auto *status = window.findChild<QLabel *>(QStringLiteral("TrafficStatus"));
        auto *record = window.findChild<QAction *>(QStringLiteral("recordTraffic"));
        QVERIFY(panel && text && model && status && record);
        panel->setPreferences(&store); // 把日志落到临时目录，验证"日志里查得到原因"
        QVERIFY(record->isChecked());

        auto *b = window.findChild<PortSelectorWidget *>(QStringLiteral("endpointB"));
        QVERIFY(b);
        QTcpServer blocker; // 占住端口的"另一个进程"：保持监听，直到本用例结束
        QVERIFY(blocker.listen(QHostAddress::LocalHost, 0));
        const quint16 conflictPort = blocker.serverPort();
        b->findChild<QSpinBox *>(QStringLiteral("server.port"))->setValue(conflictPort);
        auto *action = b->findChild<QPushButton *>(QStringLiteral("endpointAction"));
        QVERIFY(action);

        const auto errorRows = [model] {
            int count = 0;
            for (int i = 0; i < model->rowCount(); ++i)
                if (model->record(i).direction == Traffic::Direction::Error) ++count;
            return count;
        };

        action->click(); // 监听一个已被占用的端口 → listen() 同步失败
        QTRY_VERIFY2(text->toPlainText().contains(QStringLiteral("监听")),
            qPrintable(text->toPlainText()));
        QVERIFY2(text->toPlainText().contains(QStringLiteral("失败")),
            qPrintable(text->toPlainText()));
        QVERIFY2(text->toPlainText().contains(QStringLiteral("B · TCP 服务器")),
            qPrintable(QStringLiteral("Error 行应带端点标识：") + text->toPlainText()));
        QCOMPARE(errorRows(), 1);

        // 非模态弹窗：标题带端点名、正文带原因；点掉自毁。
        QMessageBox *notice = nullptr;
        QTRY_VERIFY((notice = window.findChild<QMessageBox *>()) != nullptr);
        QVERIFY(!notice->isModal());
        QVERIFY(notice->windowModality() == Qt::NonModal);
        QVERIFY2(notice->windowTitle().contains(QStringLiteral("B · TCP")),
            qPrintable(notice->windowTitle()));
        QVERIFY2(notice->text().contains(QStringLiteral("监听")), qPrintable(notice->text()));
        notice->close();
        delete notice; // 立刻收掉，别让去重窗口（3s）在等待 deleteLater 期间白白流逝
        QTRY_VERIFY(window.findChildren<QMessageBox *>().isEmpty()); // WA_DeleteOnClose 生效

        // 状态栏短消息同样带端点标识。
        QVERIFY2(window.statusBar()->currentMessage().contains(QStringLiteral("B · TCP")),
            qPrintable(window.statusBar()->currentMessage()));

        // 防轰炸：3 秒内同端点同类错误再失败一次，不再弹新窗。
        action->click();
        QTRY_COMPARE(errorRows(), 2);
        QVERIFY(window.findChildren<QMessageBox *>().isEmpty());

        // 日志大小：记录中出现在状态栏摘要里；关掉记录后不再出现。
        QTRY_VERIFY2(status->text().contains(QStringLiteral("日志 ")), qPrintable(status->text()));
        record->setChecked(false);
        const QUuid endpointId = window.findChild<QPushButton *>(QStringLiteral("showPortA"))
                                     ->property("endpoint").toUuid();
        Traffic::Message rx;
        rx.id = 9001;
        rx.timestamp = QDateTime::currentDateTime();
        rx.direction = Traffic::Direction::Receive;
        rx.source.endpointId = endpointId;
        rx.source.endpointIndex = 0;
        rx.source.endpointKind = PortDraft::Serial;
        rx.raw = QByteArray("OK");
        rx.rawLength = 2;
        panel->appendMessage(rx);
        QTRY_VERIFY2(!status->text().contains(QStringLiteral("日志 ")), qPrintable(status->text()));
    }

    // 运行中异常断开（TCP 对端 abort）→ Error 行 + 状态栏播报一句，**不弹窗**
    // （长压测中断开/重连风暴会把弹窗叠成轰炸）。用 localhost TCP Server：easyDebug
    // 监听、脚本/QTcpSocket 连上、先发一字节确认已接受，再 abort 掉。
    // 用户主动停止是正常迁移，走 State 行，同样不弹窗。
    void runtimeDisconnectReportsErrorRowWithoutPopup()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        window.findChild<TrafficPanel *>()->setPreferences(nullptr);
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        QVERIFY(text);
        // 换行/正文断言依赖文本渲染：显式切 UTF-8，不依赖共享配置残留。
        window.findChild<QComboBox *>(QStringLiteral("trafficFormat"))
            ->setCurrentText(QStringLiteral("UTF-8"));

        auto *b = window.findChild<PortSelectorWidget *>(QStringLiteral("endpointB"));
        QVERIFY(b); // 默认 TCP 服务器
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        const quint16 port = probe.serverPort();
        probe.close();
        b->findChild<QSpinBox *>(QStringLiteral("server.port"))->setValue(port);
        auto *action = b->findChild<QPushButton *>(QStringLiteral("endpointAction"));
        QCOMPARE(action->text(), QStringLiteral("监听"));

        action->click();
        QTRY_COMPARE(action->text(), QStringLiteral("停止"));

        QTcpSocket client;
        client.connectToHost(QHostAddress::LocalHost, port);
        QTRY_COMPARE(client.state(), QAbstractSocket::ConnectedState);
        client.write("ping");
        QTRY_COMPARE(client.bytesToWrite(), qint64(0));
        // 先确认 easyDebug 已接受并收到了这条数据，再 abort——否则连接还没从 backlog
        // 接进来，断开事件无 peer 可挂。
        QTRY_VERIFY2(text->toPlainText().contains(QStringLiteral("ping")),
            qPrintable(text->toPlainText()));

        client.abort(); // 模拟对端异常掉线
        QTRY_VERIFY2(text->toPlainText().contains(QStringLiteral("已断开")),
            qPrintable(text->toPlainText()));
        QVERIFY2(text->toPlainText().contains(QStringLiteral("B · TCP 服务器")),
            qPrintable(QStringLiteral("Error 行应带端点标识：") + text->toPlainText()));
        QTRY_VERIFY2(window.statusBar()->currentMessage().contains(QStringLiteral("连接异常断开")),
            qPrintable(window.statusBar()->currentMessage()));
        QVERIFY2(window.statusBar()->currentMessage().contains(QStringLiteral("B · TCP")),
            qPrintable(window.statusBar()->currentMessage()));
        // 运行中异常断开不弹窗（与"打开失败"路径的分野）。
        QVERIFY(window.findChildren<QMessageBox *>().isEmpty());

        // 用户主动停止是正常状态迁移：State 行「已关闭」，不弹窗。
        action->click();
        QTRY_COMPARE(action->text(), QStringLiteral("监听"));
        QVERIFY2(text->toPlainText().contains(QStringLiteral("已关闭")),
            qPrintable(text->toPlainText()));
        QVERIFY(window.findChildren<QMessageBox *>().isEmpty());
    }

    // 性能状态必须"随手可得"而不是"卡了才出现"：状态栏那段文字的 tooltip 里
    // 始终有最近 2 秒的量化值（界面迟滞 / 单批处理 / 缓冲 / 日志占用）。
    // 用户要的是"能判断性能"，所以健康时也要能看到数字。
    void performanceStatusIsAlwaysAvailable()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        auto *status = window.findChild<QLabel *>(QStringLiteral("TrafficStatus"));
        QVERIFY(status);
        QTRY_VERIFY2(status->toolTip().contains(QStringLiteral("界面迟滞 ≤")),
            qPrintable(status->toolTip()));
        QVERIFY(status->toolTip().contains(QStringLiteral("单批处理 ≤")));
        QVERIFY(status->toolTip().contains(QStringLiteral("缓冲 ")));
        QVERIFY(status->toolTip().contains(QStringLiteral("日志 ")));
    }

    // 语义配色：方向令牌按类型分色（RX 饱和绿 / TX 主蓝 / 状态紫 / 警告琥珀 / 错误红），
    // 错误与警告整行同色——这是"一眼分辨消息类型"的落点，改动必须被这里挡住。
    void directionTokensCarrySemanticColors()
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
        // 正文断言依赖文本渲染：显式切 UTF-8，不依赖共享配置里的残留格式。
        window.findChild<QComboBox *>(QStringLiteral("trafficFormat"))
            ->setCurrentText(QStringLiteral("UTF-8"));
        const QUuid endpointId = window.findChild<QPushButton *>(QStringLiteral("showPortA"))
                                     ->property("endpoint").toUuid();
        const auto make = [endpointId](quint64 id, Traffic::Direction direction, const char *body) {
            Traffic::Message message;
            message.id = id;
            message.timestamp = QDateTime::currentDateTime();
            message.direction = direction;
            message.source.endpointId = endpointId;
            message.source.endpointIndex = 0;
            message.source.endpointKind = PortDraft::Serial;
            message.raw = QByteArray(body);
            message.rawLength = message.raw.size();
            return message;
        };
        panel->appendMessages({make(1, Traffic::Direction::Receive, "rx-body"),
            make(2, Traffic::Direction::Transmit, "tx-body"),
            make(3, Traffic::Direction::State, "state-body"),
            make(4, Traffic::Direction::Warning, "warning-body"),
            make(5, Traffic::Direction::Error, "error-body")});
        QTRY_COMPARE(model->rowCount(), 5);
        const auto fragmentColor = [text](int blockIndex, const QString &needle) {
            const QTextBlock block = text->document()->findBlockByNumber(blockIndex);
            for (auto it = block.begin(); !it.atEnd(); ++it)
                if (it.fragment().text().contains(needle))
                    return it.fragment().charFormat().foreground().color();
            return QColor();
        };
        QCOMPARE(fragmentColor(0, QStringLiteral("RX")), QColor(0x04, 0x6C, 0x4E));
        QCOMPARE(fragmentColor(1, QStringLiteral("TX")), QColor(0x1D, 0x4E, 0xD8));
        QCOMPARE(fragmentColor(2, QStringLiteral("状态")), QColor(0x6D, 0x28, 0xD9));
        QCOMPARE(fragmentColor(3, QStringLiteral("警告")), QColor(0x92, 0x40, 0x0E));
        QCOMPARE(fragmentColor(4, QStringLiteral("错误")), QColor(0xC6, 0x28, 0x28));
        // 数据行的正文保持近黑（数据是主体）；错误/警告整行随类型着色。
        QCOMPARE(fragmentColor(0, QStringLiteral("rx-body")), QColor(0x1C, 0x24, 0x30));
        QCOMPARE(fragmentColor(3, QStringLiteral("warning-body")), QColor(0x92, 0x40, 0x0E));
        QCOMPARE(fragmentColor(4, QStringLiteral("error-body")), QColor(0xC6, 0x28, 0x28));
        // 前缀 DemiBold（用户钦定"前缀加粗 + 与正文同色，靠字重区分"）：行号/时间这类
        // 元信息与方向令牌必须同为 DemiBold，且元信息用正文色 #1C2430。这里钉的是字重
        // 与元色，防止以后有人把 Bold 当"够粗"、或把前缀退回更浅的灰。
        const auto fragmentFormat = [text](int blockIndex, const QString &needle) {
            const QTextBlock block = text->document()->findBlockByNumber(blockIndex);
            for (auto it = block.begin(); !it.atEnd(); ++it)
                if (it.fragment().text().contains(needle))
                    return it.fragment().charFormat();
            return QTextCharFormat();
        };
        const auto metaFormat = fragmentFormat(0, QStringLiteral("1 ")); // 行号在前缀最前
        QCOMPARE(metaFormat.fontWeight(), int(QFont::DemiBold));
        QCOMPARE(metaFormat.foreground().color(), QColor(0x1C, 0x24, 0x30));
        QCOMPARE(fragmentFormat(0, QStringLiteral("RX")).fontWeight(), int(QFont::DemiBold));
    }

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
        // 格式切换是"外观"：按契约立刻作用于**已收到/已显示**的行，新到达的消息也按新格式渲染
        // （r5 起不再是"只影响新行"的控制台语义）。切完先等掉那一刻必然发生的防抖重投影。
        // 已知字节流片段（HEX 01 03 00 00 00 02 C4 0B）渲染为 DEC，并带 ⏎ 续接标记——边界是消息的事实。
        trafficFormat->setCurrentIndex(1); // DEC
        QTest::qWait(AppearanceReprojectWaitMs); // 设计上必然发生的一次重投影，等掉它再断言
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
        QTest::qWait(AppearanceReprojectWaitMs); // 切格式必然触发的重投影，先等掉
        // 搜索按到达时过滤：命中的记录进文本历史，未命中的只进模型不进显示。
        auto *search = window.findChild<QLineEdit *>("filterKeywords");
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

    // 文本视图的安全阀触发时同样必须播报：屏幕上看不到的几行，可能是用户唯一想
    // 回看的东西。默认预算是 400 万块（用例填不满），所以显式注入小预算验机制。
    void textViewBudgetReleaseIsReported()
    {
        TrafficTextView view;
        view.setBudget(50, 1024 * 1024);
        QList<Traffic::Message> batch;
        batch.reserve(60);
        for (int i = 0; i < 60; ++i) {
            Traffic::Message message;
            message.id = i + 1;
            message.timestamp = QDateTime::currentDateTime();
            message.direction = Traffic::Direction::Receive;
            message.source.endpointId = QUuid::createUuid();
            message.source.endpointIndex = 0;
            message.source.endpointKind = PortDraft::Serial;
            message.raw = QByteArray("line one\r\nline two\r\n");
            message.rawLength = message.raw.size();
            batch.append(message);
        }
        view.appendMessages(batch, PayloadCodec::Format::Utf8, TrafficTextView::DisplayOptions());

        QVERIFY2(view.lineCount() <= 50,
            qPrintable(QStringLiteral("行数应被压回预算内：%1").arg(view.lineCount())));
        QVERIFY2(view.releasedBlocks() > 0, "触顶释放必须记账——释放不许无声");
        // 释放只从最旧一端发生：第 1 条已被挤掉，最新一条一定还在。
        QVERIFY2(!view.document()->firstBlock().text().startsWith(QStringLiteral("1 ")),
            qPrintable(QStringLiteral("首块应已不是第 1 条：") + view.document()->firstBlock().text()));
        QVERIFY2(view.document()->toPlainText().contains(QStringLiteral("60 2")),
                 "最新一条必须还在");
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
        // 模型的安全阀默认 2000 万条，本用例要验的是"模型淘汰、文本原样保留"，
        // 所以显式压小模型预算。文本视图的安全阀比这大两个数量级，一行都不该少。
        const int rowBudget = 5000;
        model->setBudget(64LL * 1024 * 1024, rowBudget);
        const int total = rowBudget + 5000;
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
            // 等攒批逐批落地：同步连灌会让所有批合成一个巨批落地，
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
        QVERIFY2(model->rowCount() <= rowBudget,
            qPrintable(QStringLiteral("模型应已淘汰：当前 %1 条").arg(model->rowCount())));
        QCOMPARE(text->lineCount(), total);
        QCOMPARE(text->document()->blockCount(), total);
        // 淘汰**必须播报**：缓冲区已满要进状态栏摘要，用户才不会以为是自己看错了。
        // 这正是压测时"拖到最前面发现少了"唯一能被察觉的出口。
        auto *status = window.findChild<QLabel *>(QStringLiteral("TrafficStatus"));
        QVERIFY(status);
        QTRY_VERIFY2(status->text().contains(QStringLiteral("缓冲区已满")),
            qPrintable(QStringLiteral("摘要应播报缓冲区已满：") + status->text()));

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

    // 显示设置是"每行长什么样"的视图属性，不是"只对新数据生效"的到达时快照。
    // 切开关必须立刻作用到**已经收到**的那批行上（用户明确要求：包括已经接收到的数据）。
    // 这条用例同时钉住两半：外观改了就重排已有行；而行号在重投影后仍从 1 连续，
    // 以及 HEX 对照行比数据行浅——它是附注，不是主体。
    void displaySettingsApplyToAlreadyReceivedRows()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        auto *panel = window.findChild<TrafficPanel *>();
        panel->setPreferences(nullptr); // 开关变化不落盘，避免污染共享配置
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        auto *model = window.findChild<TrafficModel *>();
        auto *clock = window.findChild<QAction *>(QStringLiteral("showTimestamp"));
        auto *hexEcho = window.findChild<QAction *>(QStringLiteral("showHex"));
        QVERIFY(text && model && clock && hexEcho);
        window.findChild<QComboBox *>(QStringLiteral("trafficFormat"))
            ->setCurrentText(QStringLiteral("UTF-8"));
        QVERIFY2(clock->isChecked(), "默认应带时间戳，否则本用例的前提不成立");
        QVERIFY2(!hexEcho->isChecked(), "默认不应带 HEX 对照，否则本用例的前提不成立");

        const QUuid endpointId = window.findChild<QPushButton *>(QStringLiteral("showPortA"))
                                     ->property("endpoint").toUuid();
        constexpr int Count = 5;
        QList<Traffic::Message> batch;
        for (int i = 0; i < Count; ++i) {
            Traffic::Message message;
            message.id = i + 1;
            message.timestamp = QDateTime::currentDateTime();
            message.direction = Traffic::Direction::Receive;
            message.source.endpointId = endpointId;
            message.source.endpointIndex = 0;
            message.source.endpointKind = PortDraft::Serial;
            message.raw = QByteArray("OK");
            message.rawLength = 2;
            batch.append(message);
        }
        panel->appendMessages(batch);
        QTRY_COMPARE(model->rowCount(), Count);
        QTRY_COMPARE(text->document()->blockCount(), Count);
        const QRegularExpression stamp(QStringLiteral(R"(\d{2}:\d{2}:\d{2}\.\d{3})"));
        QVERIFY2(text->document()->firstBlock().text().contains(stamp),
            qPrintable(text->document()->firstBlock().text()));

        // ① 关掉时间戳：**已经在这儿的 5 行**必须一起失去时间戳。
        //    这一条正是"显示设置只对新数据生效"那个缺陷的反面。
        clock->setChecked(false);
        QTRY_VERIFY2(!text->document()->toPlainText().contains(stamp),
            qPrintable(QStringLiteral("已有行没有跟随显示设置重排：")
                + text->document()->firstBlock().text()));
        QCOMPARE(text->document()->blockCount(), Count); // 只改样子，行数不变
        QVERIFY2(text->document()->firstBlock().text().startsWith(QStringLiteral("1 ")),
            qPrintable(text->document()->firstBlock().text()));

        // ② 打开 HEX 对照：已有行要长出对照行——它们不是"新数据"，是重新渲染出来的。
        hexEcho->setChecked(true);
        QTRY_COMPARE(text->document()->blockCount(), Count * 2);
        // 行号在重投影后从 1 连续重排：第 5 条的数据行是倒数第 2 块。
        QVERIFY2(text->document()->findBlockByNumber(Count * 2 - 2).text()
                     .startsWith(QStringLiteral("5 ")),
            qPrintable(text->document()->findBlockByNumber(Count * 2 - 2).text()));
        // HEX 对照行是附注而不是数据本身，必须比数据行浅：上下两行一眼能分出主次。
        const QColor dataLine = lastFragmentColor(text->document()->firstBlock());
        const QColor hexLine = lastFragmentColor(text->document()->firstBlock().next());
        QVERIFY2(hexLine.value() > dataLine.value(),
            qPrintable(QStringLiteral("HEX 对照行应当比数据行浅：对照 %1 / 数据 %2")
                           .arg(hexLine.value())
                           .arg(dataLine.value())));

        // 还原默认开关，不把本用例的状态带给后续用例。
        hexEcho->setChecked(false);
        clock->setChecked(true);
    }

    // 显示格式为 HEX 时不重复显示 HEX 对照行：数据本身就是 HEX，再来一行一模一样。
    // 编码格式是"外观"：切换会立刻作用于**已显示的行**（r5 契约，旧行也重画），
    // 所以下面的断言都建立在"等掉那次必然发生的防抖重投影"之上，而不是某一条的到达状态。
    void hexEchoSuppressedInHexFormat()    {
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
        // 切 UTF-8：对照行恢复出现，且**已显示的那一行**也要一起重画为 UTF-8 + 对照行。
        window.findChild<QComboBox *>(QStringLiteral("trafficFormat"))
            ->setCurrentText(QStringLiteral("UTF-8"));
        QTest::qWait(AppearanceReprojectWaitMs); // 设计上必然发生的一次重投影，等掉它
        QCOMPARE(text->document()->blockCount(), 2); // 旧行已重画为：数据行 + HEX 对照行
        message.id = 2;
        panel->appendMessages({message});
        QTRY_COMPARE(model->rowCount(), 2);
        QCOMPARE(text->document()->blockCount(), 4); // 两行各带一个 HEX 对照行
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
        // 先建立一条可见数据行。
        panel->appendMessages({makeMessage(1, Traffic::Direction::Receive, "data")});
        QTRY_COMPARE(model->rowCount(), 1);
        // 外观（编码格式）改动会立刻作用于已收到的行（r5 契约）：切 UTF-8 后旧的 "data"
        // 行也应从 HEX 变为 UTF-8。这一步必然触发一次防抖重投影，显式等掉它再取基线——
        // 不能把基线建立在"重投影还没发生"上（旧断言正是这么过期的：它赌的是 HEX 还没重画）。
        window.findChild<QComboBox *>(QStringLiteral("trafficFormat"))
            ->setCurrentText(QStringLiteral("UTF-8"));
        QTest::qWait(AppearanceReprojectWaitMs); // 设计上必然发生的一次重投影，等掉它
        const auto baseline = text->document()->lastBlock().text();
        QVERIFY2(baseline.endsWith(QStringLiteral("data")), // 不再是 HEX 的 …64 61 74 61
            qPrintable(QStringLiteral("切格式后基线行应已变 UTF-8：") + baseline));
        // 关闭错误日志：错误事件进模型不进显示，数据行纹丝不动。
        window.findChild<QPushButton *>(QStringLiteral("filterError"))->setChecked(false);
        panel->appendMessages({makeMessage(2, Traffic::Direction::Error, "boom")});
        QTRY_COMPARE(model->rowCount(), 2);
        QCOMPARE(text->lineCount(), 1);
        QCOMPARE(text->document()->lastBlock().text(), baseline);
        // 重新开启：后续错误事件恢复显示。
        window.findChild<QPushButton *>(QStringLiteral("filterError"))->setChecked(true);
        panel->appendMessages({makeMessage(3, Traffic::Direction::Error, "boom2")});
        QTRY_COMPARE(text->lineCount(), 2);
        QVERIFY2(text->document()->lastBlock().text().endsWith(QStringLiteral("boom2")),
            qPrintable(QStringLiteral("错误行实况：")
                + text->document()->lastBlock().text()));
    }

    // 外观重投影不得改变"哪些行在屏幕上"（设计稿 §5 决策表）：改显示设置 / 编码格式只换
    // 每行长什么样，未点「应用过滤」的关键词绝不能被顺手套到历史行上——否则屏幕会莫名变少、
    // 待应用提示同时消失。这条用例把该契约钉死。
    void appearanceReprojectKeepsRowSetAndPendingHint()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        auto *panel = window.findChild<TrafficPanel *>();
        panel->setPreferences(nullptr);
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        auto *model = window.findChild<TrafficModel *>();
        auto *keywords = window.findChild<QLineEdit *>(QStringLiteral("filterKeywords"));
        auto *apply = window.findChild<QPushButton *>(QStringLiteral("filterRebuild"));
        auto *timestamp = window.findChild<QAction *>(QStringLiteral("showTimestamp"));
        QVERIFY(text && model && keywords && apply && timestamp);
        QVERIFY(timestamp->isChecked()); // 默认显示时间戳，下面用它当"外观"开关

        const QUuid endpointId = window.findChild<QPushButton *>(QStringLiteral("showPortA"))
                                     ->property("endpoint").toUuid();
        const auto makeMessage = [endpointId](quint64 id, const char *hex) {
            Traffic::Message message;
            message.id = id;
            message.timestamp = QDateTime::currentDateTime();
            message.direction = Traffic::Direction::Receive;
            message.source.endpointId = endpointId;
            message.source.endpointIndex = 0;
            message.source.endpointKind = PortDraft::Serial;
            message.raw = QByteArray::fromHex(hex);
            message.rawLength = message.raw.size();
            return message;
        };
        // 6 条记录：3 条命中关键词 "aa bb"，3 条不命中。先全部显示（此时还没有关键词）。
        QList<Traffic::Message> batch;
        for (quint64 i = 0; i < 3; ++i) {
            batch.append(makeMessage(i * 2 + 1, "AABB"));
            batch.append(makeMessage(i * 2 + 2, "CCDD"));
        }
        panel->appendMessages(batch);
        QTRY_COMPARE(model->rowCount(), 6);
        QTRY_COMPARE(text->lineCount(), 6);

        // 敲一个只命中部分记录的关键词，**不点**应用过滤：行集不动，只是提示待应用。
        keywords->setText(QStringLiteral("aa bb"));
        QCOMPARE(text->lineCount(), 6);
        QVERIFY2(apply->text() != QStringLiteral("应用过滤"),
            qPrintable(QStringLiteral("改条件后应显示待应用提示：") + apply->text()));

        // 改外观（关掉时间戳）：只换每行长什么样，行集必须一字不变，待应用提示保持。
        timestamp->setChecked(false);
        QTest::qWait(AppearanceReprojectWaitMs); // 设计上必然发生的一次外观重投影，等掉它
        QCOMPARE(text->lineCount(), 6); // 未应用的关键词没有被顺手应用到历史行上
        QVERIFY2(apply->text() != QStringLiteral("应用过滤"),
            qPrintable(QStringLiteral("外观重投影后待应用提示必须仍在：") + apply->text()));

        // 点「应用过滤」：这才把关键词补到历史行上，行集按关键词收窄（只有 3 条命中）。
        apply->click();
        QCOMPARE(text->lineCount(), 3);
        QCOMPARE(apply->text(), QStringLiteral("应用过滤")); // 应用后提示消失

        // ---- 清空之后的同类不变量：清空是会话边界，以当前条件为新会话的已应用基线。----
        keywords->clear(); // 回到无内容条件，再清空
        window.findChild<QPushButton *>(QStringLiteral("clearTraffic"))->click();
        QCOMPARE(model->rowCount(), 0);
        QCOMPARE(text->lineCount(), 1); // 空文档保底的一个空块
        // 清空 + 首批数据到达后，按钮文字必须是干净的「应用过滤」：这批数据本就是按当前
        // 条件收的，不该被判成"待应用"（旧实现靠 !m_everProjected 会把 pending 误判为真）。
        QList<Traffic::Message> again;
        for (quint64 i = 0; i < 3; ++i) {
            again.append(makeMessage(100 + i * 2 + 1, "AABB"));
            again.append(makeMessage(100 + i * 2 + 2, "CCDD"));
        }
        panel->appendMessages(again);
        QTRY_COMPARE(model->rowCount(), 6);
        QTRY_COMPARE(text->lineCount(), 6);
        QCOMPARE(apply->text(), QStringLiteral("应用过滤")); // 首批数据不该触发待应用提示

        // 敲一个只命中部分记录的关键词，**不点**应用；再改外观：行集必须一字不变、提示保持
        // ——清空后的会话同样走这条不变量（外观路径用已应用基线重放，不写 m_projectedOptions）。
        keywords->setText(QStringLiteral("aa bb"));
        QCOMPARE(text->lineCount(), 6);
        timestamp->setChecked(true); // 换个外观值（上一段关掉了时间戳）
        QTest::qWait(AppearanceReprojectWaitMs); // 设计上必然发生的一次外观重投影，等掉它
        QCOMPARE(text->lineCount(), 6); // 未应用的关键词没有被顺手应用
        QVERIFY2(apply->text() != QStringLiteral("应用过滤"),
            qPrintable(QStringLiteral("清空后的外观重投影仍应保持待应用提示：") + apply->text()));

        // 点「应用过滤」：行集按关键词收窄。
        apply->click();
        QCOMPARE(text->lineCount(), 3);
        QCOMPARE(apply->text(), QStringLiteral("应用过滤"));
    }

    // QA 独立验收（第二轮）。与工程师的 appearanceReprojectKeepsRowSetAndPendingHint 互补：
    // 这里专门盯"外观路径绝不写 m_projectedOptions"这一条实现约束，逐组合给出
    // "若实现退回旧语义，这条断言会怎么挂"。
    // 关键词选 "aa bb"：hex 紧凑/带空格候选与编码格式无关，用来把变量隔离在"行集"上。
    void appearanceReprojectIndependenceQa()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        auto *panel = window.findChild<TrafficPanel *>();
        panel->setPreferences(nullptr);
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        auto *model = window.findChild<TrafficModel *>();
        auto *keywords = window.findChild<QLineEdit *>(QStringLiteral("filterKeywords"));
        auto *apply = window.findChild<QPushButton *>(QStringLiteral("filterRebuild"));
        auto *timestamp = window.findChild<QAction *>(QStringLiteral("showTimestamp"));
        auto *format = window.findChild<QComboBox *>(QStringLiteral("trafficFormat"));
        auto *clear = window.findChild<QPushButton *>(QStringLiteral("clearTraffic"));
        QVERIFY(text && model && keywords && apply && timestamp && format && clear);

        // 归一化到确定起点，避免被同一次运行里更早的用例留下的共享偏好影响。
        keywords->clear();
        format->setCurrentText(QStringLiteral("HEX"));
        timestamp->setChecked(true);
        window.findChild<QPushButton *>(QStringLiteral("showPortA"))->setChecked(true);
        if (auto *rx = window.findChild<QPushButton *>(QStringLiteral("filterRx")))
            rx->setChecked(true);
        clear->click();
        QCOMPARE(model->rowCount(), 0);

        const QUuid endpointId = window.findChild<QPushButton *>(QStringLiteral("showPortA"))
                                     ->property("endpoint").toUuid();
        const auto makeMessage = [endpointId](quint64 id, const char *hex) {
            Traffic::Message message;
            message.id = id;
            message.timestamp = QDateTime::currentDateTime();
            message.direction = Traffic::Direction::Receive;
            message.source.endpointId = endpointId;
            message.source.endpointIndex = 0;
            message.source.endpointKind = PortDraft::Serial;
            message.raw = QByteArray::fromHex(hex);
            message.rawLength = message.raw.size();
            return message;
        };
        // 6 条：3 条含 "AA BB"，3 条不含。起点无关键词 → 全显示。
        QList<Traffic::Message> batch;
        for (quint64 i = 0; i < 3; ++i) {
            batch.append(makeMessage(i * 2 + 1, "AABB"));
            batch.append(makeMessage(i * 2 + 2, "CCDD"));
        }
        panel->appendMessages(batch);
        QTRY_COMPARE(model->rowCount(), 6);
        QTRY_COMPARE(text->lineCount(), 6);
        QCOMPARE(apply->text(), QStringLiteral("应用过滤")); // 起点：没有待应用

        // ---- C1：敲关键词不应用 + 改外观 → 行集不变、提示仍在 ----
        keywords->setText(QStringLiteral("aa bb"));
        QCOMPARE(text->lineCount(), 6); // 只改条件没点应用，屏幕不动
        QVERIFY2(apply->text() != QStringLiteral("应用过滤"), "改条件后应出现待应用提示");
        timestamp->setChecked(false); // 外观变化：必然触发一次防抖重投影
        QTest::qWait(AppearanceReprojectWaitMs);
        QCOMPARE(text->lineCount(), 6); // 未应用的关键词没有被外观重投影顺手应用
        QVERIFY2(apply->text() != QStringLiteral("应用过滤"), "外观重投影后提示必须仍在");
        // 旧语义下：外观路径用当前条件过滤 → 行集 6→3；且 markReprojected 把
        // m_projectedOptions 记成当前条件 → 提示被清成"应用过滤"。两条断言都会挂。

        // ---- C2：连点两次外观开关（时间戳 + 编码格式同一轮）→ 行集与提示都不变 ----
        timestamp->setChecked(true);                     // 第一次外观变更
        format->setCurrentText(QStringLiteral("UTF-8")); // 第二次外观变更（防抖窗口内）
        QTest::qWait(AppearanceReprojectWaitMs);
        QCOMPARE(text->lineCount(), 6);
        QVERIFY2(apply->text() != QStringLiteral("应用过滤"), "两次外观变更后提示仍应在");
        // 这一条专门证明**外观路径不写 m_projectedOptions**：若它写了，第一次重投影
        // 之后提示就会被清掉，断言即挂。

        // ---- C3：点应用过滤 → 行集按关键词收窄、提示回到干净；之后再改外观稳定 ----
        apply->click();
        QCOMPARE(text->lineCount(), 3);                       // 只有 3 条命中 "aa bb"
        QCOMPARE(apply->text(), QStringLiteral("应用过滤"));   // 已应用 → 提示清干净
        timestamp->setChecked(false); // 再次改外观（当前为 true）
        QTest::qWait(AppearanceReprojectWaitMs);
        QCOMPARE(text->lineCount(), 3);                       // 已应用基线已含关键词 → 仍 3
        QCOMPARE(apply->text(), QStringLiteral("应用过滤"));   // 提示保持干净
        // 若 apply 没更新 m_projectedOptions，或外观路径用当前条件却又不更新签名，
        // 会出现"行集对但提示错"或反之，两条断言至少挂一条。

        // ---- C4：清空 + 首批到达 → 按钮文字干净；随后"清空→敲词不应用→改外观" ----
        keywords->clear();
        clear->click();
        QCOMPARE(model->rowCount(), 0);
        QList<Traffic::Message> again;
        for (quint64 i = 0; i < 3; ++i) {
            again.append(makeMessage(100 + i * 2 + 1, "AABB"));
            again.append(makeMessage(100 + i * 2 + 2, "CCDD"));
        }
        panel->appendMessages(again);
        QTRY_COMPARE(model->rowCount(), 6);
        QTRY_COMPARE(text->lineCount(), 6);
        // 清空是会话边界，以当前条件为新基线：首批数据不该被判成"待应用"。
        QCOMPARE(apply->text(), QStringLiteral("应用过滤"));
        // 旧实现（清空后 m_everProjected=false）会让 refreshApplyHint 判 pending=true
        // → 文字变成"应用过滤 •"，此处断言挂。

        keywords->setText(QStringLiteral("aa bb"));
        QCOMPARE(text->lineCount(), 6);
        timestamp->setChecked(true); // 换外观值（当前为 false）
        QTest::qWait(AppearanceReprojectWaitMs);
        QCOMPARE(text->lineCount(), 6); // 清空后的会话同样不允许外观重投影套上未应用条件
        QVERIFY2(apply->text() != QStringLiteral("应用过滤"), "清空后的外观重投影仍应保留提示");
        // 旧实现的关键回归点：清空后无基线（m_everProjected=false），外观路径会退回
        // "用当前条件"过滤 → 行集 6→3，此条挂。

        // ---- D1：证实"已知残余"与 trafficpanel.cpp:1027-1031 注释一致 ----
        apply->click(); // 建立"已应用基线 = aa bb"
        QCOMPARE(text->lineCount(), 3);
        QCOMPARE(apply->text(), QStringLiteral("应用过滤"));
        keywords->setText(QStringLiteral("cc dd")); // 改条件，不应用（已应用基线仍是 aa bb）
        QCOMPARE(text->lineCount(), 3);              // 改条件不影响已有行
        QVERIFY(apply->text() != QStringLiteral("应用过滤"));
        // 新行到达：增量路径用**当前**条件（cc dd）收流，于是这 2 条 CCDD 先显示出来。
        panel->appendMessages({makeMessage(200, "CCDD"), makeMessage(201, "CCDD")});
        QTRY_COMPARE(model->rowCount(), 8);
        QTRY_COMPARE(text->lineCount(), 5); // 只命中新条件的行先显示——注释所述现象
        // 一次外观重投影（按已应用基线 aa bb 重放）把它们**收回**，与注释一致。
        timestamp->setChecked(false);
        QTest::qWait(AppearanceReprojectWaitMs);
        QCOMPARE(text->lineCount(), 3);
        QVERIFY2(apply->text() != QStringLiteral("应用过滤"), "提示仍在（条件尚未应用）");
    }

    // QA 独立验收（第二轮）D2：外观路径的已应用判定器用**当前编码格式**重建
    // （Matcher(options, currentDisplayFormat())），而候选视图含"按当前格式渲染的文本"。
    // 这里实测"只在 DEC 渲染视图里命中"的条件在切到 HEX 后行集是否变化，并固化判定。
    void appearanceReprojectFormatDependentCandidatesQa()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        auto *panel = window.findChild<TrafficPanel *>();
        panel->setPreferences(nullptr);
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        auto *model = window.findChild<TrafficModel *>();
        auto *keywords = window.findChild<QLineEdit *>(QStringLiteral("filterKeywords"));
        auto *apply = window.findChild<QPushButton *>(QStringLiteral("filterRebuild"));
        auto *format = window.findChild<QComboBox *>(QStringLiteral("trafficFormat"));
        auto *clear = window.findChild<QPushButton *>(QStringLiteral("clearTraffic"));
        QVERIFY(text && model && keywords && apply && format && clear);

        keywords->clear();
        clear->click();
        format->setCurrentText(QStringLiteral("DEC")); // DEC 渲染 = 十进制字节值，如 "0 255"
        window.findChild<QPushButton *>(QStringLiteral("showPortA"))->setChecked(true);
        if (auto *rx = window.findChild<QPushButton *>(QStringLiteral("filterRx")))
            rx->setChecked(true);
        QCOMPARE(model->rowCount(), 0);

        const QUuid endpointId = window.findChild<QPushButton *>(QStringLiteral("showPortA"))
                                     ->property("endpoint").toUuid();
        const auto makeMessage = [endpointId](quint64 id, const char *hex) {
            Traffic::Message message;
            message.id = id;
            message.timestamp = QDateTime::currentDateTime();
            message.direction = Traffic::Direction::Receive;
            message.source.endpointId = endpointId;
            message.source.endpointIndex = 0;
            message.source.endpointKind = PortDraft::Serial;
            message.raw = QByteArray::fromHex(hex);
            message.rawLength = message.raw.size();
            return message;
        };
        // 3 条都含 0xFF：DEC 渲染（"0 255"/"255 1"/"2 255"）都有 "255"；
        // HEX 渲染（"00 FF" 等）与 hex 候选里都没有 "255"。
        panel->appendMessages({makeMessage(1, "00FF"), makeMessage(2, "FF01"), makeMessage(3, "02FF")});
        QTRY_COMPARE(model->rowCount(), 3);
        QTRY_COMPARE(text->lineCount(), 3);

        keywords->setText(QStringLiteral("255")); // 只在 DEC 渲染视图里命中
        apply->click();
        QCOMPARE(text->lineCount(), 3);                     // 应用后 3 条都命中
        QCOMPARE(apply->text(), QStringLiteral("应用过滤"));

        // 切到 HEX：已应用条件 "255" 不变，但外观路径按**当前格式 HEX** 重建判定器，
        // DEC 渲染视图不再是候选。若"外观不改行集"严格成立，这里应仍是 3 行。
        format->setCurrentText(QStringLiteral("HEX"));
        QTest::qWait(AppearanceReprojectWaitMs);
        // 实测固化：格式相关候选（DEC 渲染）消失后，3 条全部不再命中 → 只剩空文档保底块。
        // 这与"未应用条件被套上"不同：条件 "255" 早已 apply（m_projectedOptions 就是它），
        // 提示也干净，属于**同一条件在新渲染下的重新求值**。
        QCOMPARE(text->lineCount(), 1);
        QCOMPARE(apply->text(), QStringLiteral("应用过滤")); // 提示干净：不是待应用机制在动行集
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
