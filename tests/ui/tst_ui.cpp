#include "mainwindow.h"
#include "app/commandmanager.h"
#include "domain/commandset.h"
#include "domain/receiveframing.h"
#include "ui/common/endpointformat.h"
#include "ui/common/payloadcodec.h"
#include "ui/panels/commands/commandpanel.h"
#include "ui/panels/commands/commandtablewidget.h"
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
#include <QDirIterator>
#include <QFile>
#include <QFrame>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QInputDialog>
#include <QInputMethodEvent>
#include <QLineEdit>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPoint>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
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
#include <QTimer>
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

// 窗口不再有内容下限：边框可以拖到比顶栏更窄，窄了允许裁切。
// 顶栏"不重叠"只在放得下它的宽度上检查。

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

        // 正则支持已移除（条件一律按纯文本子串匹配）：`heart.*` 里的 `.*` 不再是通配，
        // 整串原样去找，因此一条也命中不了——这条断言就是"没人悄悄把它加回来"的看门人。
        options.caseSensitive = false;
        options.keywords = {QStringLiteral("heart.*")};
        QCOMPARE(hits(options), 0);
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
        // 正则开关已移除（条件一律按纯文本匹配），"区分大小写"是条上唯一的匹配选项。
        const QStringList named{QStringLiteral("filterCaseSensitive"),
            QStringLiteral("filterRebuild"), QStringLiteral("filterReset")};
        for (const auto &name : named) {
            auto *button = bar->findChild<QPushButton *>(name);
            QVERIFY2(button, qPrintable(QStringLiteral("缺按钮：") + name));
            QVERIFY2(!button->icon().isNull(), qPrintable(QStringLiteral("缺字形：") + name));
        }
        // 匹配选项是条上的按钮，不是下拉列表：既没有菜单，也没有失去"一眼可见"的入口。
        auto *caseButton = bar->findChild<QPushButton *>(QStringLiteral("filterCaseSensitive"));
        QVERIFY(caseButton && caseButton->isCheckable());
        QVERIFY(!caseButton->menu());
        QVERIFY(!bar->findChild<QPushButton *>(QStringLiteral("filterMore")));
        // 正则开关已整体移除：控件、菜单项、动作都不该以任何形式回来。
        QVERIFY(!bar->findChild<QPushButton *>(QStringLiteral("filterRegex")));
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

    // 顶栏第一行的开关组（用户钦定）：方向开关（RX / TX）与显示开关（纯数据模式 /
    // 同时显示 HEX / 自动换行）常驻，**不在**可折叠的条件条里——条件条收起来时它们
    // 仍在原位，而且每个都是"图标 + 文案"，不必悬停才知道自己在点的是什么。
    // 事件开关（状态 / 警告 / 错误）让出顶栏位置，收进显示设置菜单，但仍必须找得到：
    // 它们是"只看错误日志"的唯一入口。
    // 代价压在顶栏这一行的宽度上，所以末尾还要把窗口压到最小宽度量一次。
    void topRowSwitchGroupsStayPut()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);

        auto *flags = window.findChild<QWidget *>(QStringLiteral("filterFlags"));
        auto *views = window.findChild<QWidget *>(QStringLiteral("viewToggles"));
        auto *bar = window.findChild<QWidget *>(QStringLiteral("filterBar"));
        auto *toggle = window.findChild<QPushButton *>(QStringLiteral("filterToggle"));
        QVERIFY(flags && views && bar && toggle);
        // 两组开关同住第一行：并排出现才叫"常驻顶栏"，谁被挪进第二行都会随条件条一起消失。
        QCOMPARE(views->parentWidget(), flags->parentWidget());

        const QStringList directions{QStringLiteral("filterRx"), QStringLiteral("filterTx")};
        const QStringList viewSwitches{QStringLiteral("viewPureData"), QStringLiteral("viewHexEcho"),
            QStringLiteral("viewLineWrap")};
        for (const auto &name : directions) {
            auto *button = flags->findChild<QPushButton *>(name);
            QVERIFY2(button, qPrintable(QStringLiteral("缺方向开关：") + name));
            QVERIFY2(button->isCheckable(), qPrintable(QStringLiteral("方向开关必须是开/关：") + name));
        }
        for (const auto &name : viewSwitches) {
            auto *button = views->findChild<QPushButton *>(name);
            QVERIFY2(button, qPrintable(QStringLiteral("缺显示开关：") + name));
            QVERIFY2(button->isCheckable(), qPrintable(QStringLiteral("显示开关必须是开/关：") + name));
            QVERIFY2(!button->icon().isNull(), qPrintable(QStringLiteral("缺字形：") + name));
            QVERIFY2(!button->text().isEmpty(), qPrintable(QStringLiteral("缺文案：") + name));
        }
        for (const auto &name : directions + viewSwitches) {
            // 同一个开关不能既在顶栏又在条件条里：条件分散在两个控件之间，
            // "重置一次清干净"全靠这条边界清楚。
            QVERIFY2(!bar->findChild<QPushButton *>(name),
                qPrintable(QStringLiteral("开关不应出现在条件条里：") + name));
            // 也不该再有第二个同名按钮藏在别处（顶栏只有一份）。
            QCOMPARE(window.findChildren<QPushButton *>(name).size(), 1);
        }

        // 事件开关：顶栏上已经让位，但菜单里必须还在，且是勾选型的 QAction。
        for (const auto &name : {QStringLiteral("filterState"), QStringLiteral("filterWarning"),
                 QStringLiteral("filterError")}) {
            QVERIFY2(!flags->findChild<QPushButton *>(name),
                qPrintable(QStringLiteral("事件开关不该留在顶栏：") + name));
            QVERIFY2(!views->findChild<QPushButton *>(name),
                qPrintable(QStringLiteral("事件开关不该挪进显示开关组：") + name));
        }
        for (const auto &name : {QStringLiteral("showStates"), QStringLiteral("showWarnings"),
                 QStringLiteral("showErrors")}) {
            auto *action = window.findChild<QAction *>(name);
            QVERIFY2(action && action->isCheckable(),
                qPrintable(QStringLiteral("缺菜单里的事件开关：") + name));
        }

        // 收起条件条后这排开关照旧可见——它们管的是"哪些行进来"与"行长什么样"，
        // 不属于被折叠的内容条件。
        toggle->setChecked(false);
        QVERIFY(!bar->isVisibleTo(&window));
        for (const auto &name : directions)
            QVERIFY2(flags->findChild<QPushButton *>(name)->isVisibleTo(&window),
                qPrintable(QStringLiteral("折叠后方向开关应仍可见：") + name));
        for (const auto &name : viewSwitches)
            QVERIFY2(views->findChild<QPushButton *>(name)->isVisibleTo(&window),
                qPrintable(QStringLiteral("折叠后显示开关应仍可见：") + name));
        toggle->setChecked(true);

        // 顶栏按钮与菜单项是同一个命令的两处门面（ViewToggles::bind）：任一侧改动、
        // 另一侧立刻跟上。两侧都不许"各记一份"，否则用户在菜单里看到的与顶栏按下的
        // 会是两个状态。
        auto *pure = views->findChild<QPushButton *>(QStringLiteral("viewPureData"));
        auto *hexEcho = views->findChild<QPushButton *>(QStringLiteral("viewHexEcho"));
        auto *lineWrap = views->findChild<QPushButton *>(QStringLiteral("viewLineWrap"));
        auto *pureAction = window.findChild<QAction *>(QStringLiteral("pureData"));
        auto *hexAction = window.findChild<QAction *>(QStringLiteral("showHex"));
        auto *wrapAction = window.findChild<QAction *>(QStringLiteral("wrapTraffic"));
        QVERIFY(pure && hexEcho && lineWrap && pureAction && hexAction && wrapAction);
        const QList<QPair<QPushButton *, QAction *>> mirrored{
            {pure, pureAction}, {hexEcho, hexAction}, {lineWrap, wrapAction}};
        for (const auto &pair : mirrored) {
            // 文案取自命令本身：菜单与顶栏说的是同一件事，不在这里另抄一份。
            QCOMPARE(pair.first->text(), pair.second->text());
            QCOMPARE(pair.first->isChecked(), pair.second->isChecked());
            // 按钮 → 命令。
            const bool flipped = !pair.second->isChecked();
            pair.first->click();
            QCOMPARE(pair.second->isChecked(), flipped);
            // 命令 → 按钮（程序侧：恢复偏好、联动置灰走这条路）。
            pair.second->setChecked(!flipped);
            QCOMPARE(pair.first->isChecked(), !flipped);
        }

        // 纯数据模式会把"行前缀"那批开关置灰：顶栏按钮必须同步置灰，
        // 否则留下一个"看着能点、点了一下没反应"的死开关。
        if (pure->isChecked()) pure->click();
        QVERIFY(!pure->isChecked());
        auto *number = window.findChild<QAction *>(QStringLiteral("showLineNumber"));
        QVERIFY(number && number->isEnabled() && hexEcho->isEnabled());
        pure->click();
        QVERIFY(pure->isChecked());
        QCOMPARE(number->isEnabled(), false);
        QCOMPARE(hexEcho->isEnabled(), false);
        pure->click();
        QVERIFY(number->isEnabled());

        // '过滤 · N' 仍是两处条件的合计：只关掉 RX（条件条里一条内容条件都没有）也必须算进去。
        auto *rx = flags->findChild<QPushButton *>(QStringLiteral("filterRx"));
        // 只关掉 RX 时重置按钮必须是亮的，否则那半套条件没有回到默认的入口。
        auto *reset = bar->findChild<QPushButton *>(QStringLiteral("filterReset"));
        QVERIFY(reset && !reset->isEnabled());
        rx->setChecked(false);
        QVERIFY2(toggle->text().contains(QStringLiteral("· 1")), qPrintable(toggle->text()));
        QVERIFY(reset->isEnabled());
        // 菜单里的事件开关同样是条件：只关掉"警告"也要计数。
        auto *warnings = window.findChild<QAction *>(QStringLiteral("showWarnings"));
        warnings->setChecked(false);
        QVERIFY2(toggle->text().contains(QStringLiteral("· 2")), qPrintable(toggle->text()));
        // 重置一次清干净：顶栏方向开关、菜单事件开关、条件条内容条件一起回到不过滤。
        bar->findChild<QLineEdit *>(QStringLiteral("filterKeywords"))->setText(QStringLiteral("心跳"));
        QVERIFY(toggle->text().contains(QStringLiteral("· 3")));
        reset->click();
        QVERIFY(rx->isChecked());
        QVERIFY(warnings->isChecked());
        // 「清除过滤」改为**保留输入框内容、只停用过滤**（用户反馈"又得重新输入，很麻烦"）：
        // 复位的是方向/事件开关与"生效的过滤"；条件留在框里，点「应用过滤」即可恢复。
        QCOMPARE(bar->findChild<QLineEdit *>(QStringLiteral("filterKeywords"))->text(),
                 QStringLiteral("心跳"));
        QVERIFY2(toggle->text().contains(QStringLiteral("过滤 ")), qPrintable(toggle->text()));

        // 窗口没有内容下限：布局激活之后仍然是 0，并且能停在比内容更小的尺寸上。
        auto *panel = window.findChild<TrafficPanel *>();
        QVERIFY(panel);
        qInfo() << "window minimum width" << window.minimumWidth() << "traffic minimum"
                << panel->minimumSizeHint().width();
        QCOMPARE(window.minimumWidth(), 0);
        QCOMPARE(window.minimumHeight(), 0);
        window.resize(400, 280);
        QApplication::processEvents();
        QTest::qWait(50);
        QVERIFY2(window.width() < 700,
            qPrintable(QStringLiteral("窗口宽度被弹回 %1，最小宽度 %2")
                           .arg(window.width()).arg(window.minimumWidth())));
        QVERIFY2(window.height() < 500,
            qPrintable(QStringLiteral("窗口高度被弹回 %1，最小高度 %2")
                           .arg(window.height()).arg(window.minimumHeight())));
        QCOMPARE(window.minimumWidth(), 0);
        QCOMPARE(window.minimumHeight(), 0);
        // 拉回放得下顶栏的宽度，这一行仍不重叠、不越右缘。
        window.resize(1280, 660);
        QTest::qWait(30);
        QWidget *row = flags->parentWidget();
        QVERIFY(row);
        for (const auto &name : {QStringLiteral("showPortA"), QStringLiteral("showPortB"),
                 QStringLiteral("trafficFormat"), QStringLiteral("filterToggle"),
                 QStringLiteral("filterFlags"), QStringLiteral("viewToggles"),
                 QStringLiteral("displaySettings"), QStringLiteral("exportTraffic"),
                 QStringLiteral("followTraffic"), QStringLiteral("clearTraffic"),
                 QStringLiteral("commandPanelToggle")})
            QVERIFY2(row->findChild<QWidget *>(name), qPrintable(QStringLiteral("缺顶栏元素：") + name));
        QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("exportTraffic"))->icon().isNull());
        QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("clearTraffic"))->icon().isNull());
        const auto crowded = overlapInRow(row);
        QVERIFY2(crowded.isEmpty(), qPrintable(crowded));
    }

    // ★ 编辑一行**不得重建整表**——这是"拖拽崩 / 选结尾符崩"和"列表越用越卡"
    // 两个用户可见问题的共同病根。
    //
    // 病根链条：行内控件改一下 → commandsChanged → 面板 saveCurrentSet() → 落库；
    // 而落库用的 CommandManager::syncSet() 曾经转手调用 updateSet()，后者无条件
    // emit setsChanged()，于是 setsChanged → 面板重建下拉框 → 表格 setCommandSet()
    // → refreshTable() 把**正在派发信号的那个控件**一起销毁 → 回到它内部就崩。
    // 同一件事也让每次勾选/改延时都付一次全表重建的代价。
    //
    // 这条用例同时钉两件事：① 编辑后行内控件必须**还是原来那些对象**；
    // ② 编辑不得广播 setsChanged（"静默回写"必须真的静默）。
    void editingARowKeepsItsWidgetsAndStaysSilent()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1400, 800);
        window.show();
        QTest::qWait(30);

        auto *table = window.findChild<CommandTableWidget *>();
        QVERIFY(table);

        CommandSet set;
        for (int i = 0; i < 3; ++i) {
            CommandItem item;
            item.content = QStringLiteral("+CR,%1;").arg(i);
            item.description = QStringLiteral("指令%1").arg(i);
            set.commands.append(item);
        }
        table->setCommandSet(set);
        QTRY_COMPARE(table->rowCount(), 3);
        QTest::qWait(20);

        // 行内控件三族：结尾符下拉（每行 1）、延时框（每行 1）、勾选框（每行 2——启用 + Hex）。
        // 它们的**对象身份**就是判据，所以下面的断言全部写成与遍历顺序无关的形式。
        const auto tailsBefore = table->findChildren<QComboBox *>();
        const auto checksBefore = table->findChildren<QCheckBox *>();
        const auto delaysBefore = table->findChildren<QSpinBox *>();
        QCOMPARE(tailsBefore.size(), 3);
        QCOMPARE(checksBefore.size(), 6);
        QCOMPARE(delaysBefore.size(), 3);

        QSignalSpy broadcast(CommandManager::instance(), &CommandManager::setsChanged);

        // 逐个编辑三类控件，覆盖三条不同的信号路径：toggled / valueChanged / activated 之外
        // 的 index 变更。全部勾选框都点一遍：这样"哪一行"就不必推断，断言可以按整体写。
        for (auto *box : checksBefore) box->click();
        for (auto *spin : delaysBefore) spin->setValue(123);
        for (auto *combo : tailsBefore) combo->setCurrentIndex(3);
        QCoreApplication::processEvents();

        // ① 行内控件必须还是原来那些对象——重建过一次就说明"编辑 → 落库 → 重建"这条
        //    use-after-free 链条又回来了。
        QCOMPARE(table->findChildren<QComboBox *>(), tailsBefore);
        QCOMPARE(table->findChildren<QCheckBox *>(), checksBefore);
        QCOMPARE(table->findChildren<QSpinBox *>(), delaysBefore);

        // ② 编辑必须落到模型（"静默"不等于"不生效"）。默认 enabled=true / isHex=false，
        //    全部点一遍之后每一行都该反过来。
        const CommandSet stored = table->commandSet();
        QCOMPARE(stored.commands.size(), 3);
        for (const auto &command : stored.commands) {
            QCOMPARE(command.enabled, false);
            QCOMPARE(command.isHex, true);
            QCOMPARE(command.delayMs, 123);
            QCOMPARE(command.tail, QStringLiteral("CR+LF"));
        }

        // ③ 整段编辑不得广播一次 setsChanged——正是它把表格重建推给了"正在发信号的控件"。
        QCOMPARE(broadcast.count(), 0);
    }

    void renamingACommandSetKeepsItsContents()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1400, 800);
        window.show();
        QTest::qWait(30);

        auto *panel = window.findChild<CommandPanel *>();
        auto *selector = panel ? panel->findChild<QComboBox *>() : nullptr;
        auto *rename = panel ? panel->findChild<QPushButton *>(QStringLiteral("renameCommandSet"))
                             : nullptr;
        QVERIFY(panel && selector && rename);
        QCOMPARE(rename->text(), QStringLiteral("重命名"));

        const QUuid setId = selector->currentData().toUuid();
        QVERIFY(!setId.isNull());
        CommandSet before = CommandManager::instance()->getSet(setId);
        before.name = QStringLiteral("原分组名称");
        before.notes = QStringLiteral("保留的备注");
        CommandItem command;
        command.content = QStringLiteral("AT+RST");
        command.description = QStringLiteral("重启");
        before.commands = {command};
        CommandManager::instance()->updateSet(setId, before);
        QTRY_COMPARE(selector->currentText(), before.name);

        const QString renamed = QStringLiteral("生产线 A");
        bool dialogAccepted = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            dialog->setTextValue(renamed);
            dialog->accept();
            dialogAccepted = true;
        });
        rename->click();
        QVERIFY(dialogAccepted);

        const CommandSet after = CommandManager::instance()->getSet(setId);
        QCOMPARE(after.id, before.id);
        QCOMPARE(after.name, renamed);
        QCOMPARE(after.notes, before.notes);
        QCOMPARE(after.commands.size(), 1);
        QCOMPARE(after.commands.first().content, command.content);
        QCOMPARE(after.commands.first().description, command.description);
        QCOMPARE(selector->currentText(), renamed);
    }

    // ★ 快捷指令的发送必须真的到达运行时。
    //
    // 早先 MainWindow::handleCommandSend 调的是 EndpointRuntime 的 "write" 槽，
    // 而运行时**根本没有这个槽**——invokeMethod 找不到方法就什么也不做，
    // 于是"快捷面板点了没反应"，且没有任何报错（静默失败）。
    // 顺带钉住两件同源的事：目标取发送区当前选中（指令里不存目标）、
    // 结尾符在**解析之后**按字节追加（否则 HEX 指令的结尾符会被 fromHex 静默吃掉）。
    void quickPanelSendReachesTheRuntime()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1400, 800);
        window.show();
        QTest::qWait(30);

        auto *table = window.findChild<CommandTableWidget *>();
        auto *model = window.findChild<TrafficModel *>();
        auto *sendA = window.findChild<QPushButton *>(QStringLiteral("sendTargetA"));
        auto *sendB = window.findChild<QPushButton *>(QStringLiteral("sendTargetB"));
        QVERIFY(table && model && sendA && sendB);

        CommandSet set;
        CommandItem text;
        text.content = QStringLiteral("+CR,GO;");
        text.description = QStringLiteral("文本指令");
        text.delayMs = 0; // 用例不必等延时
        text.tail = QStringLiteral("CR+LF");
        CommandItem hex;
        hex.content = QStringLiteral("AA 55");
        hex.description = QStringLiteral("HEX指令");
        hex.delayMs = 0;
        hex.isHex = true;
        hex.tail = QStringLiteral("CR+LF");
        set.commands << text << hex;
        table->setCommandSet(set);
        QTRY_COMPARE(table->rowCount(), 2);
        QTest::qWait(20);

        // 行内发送按钮：靠 "row" 属性认出来（底栏那几个按钮没有这个属性）。
        QList<QPushButton *> rowSends;
        for (auto *button : table->findChildren<QPushButton *>())
            if (button->property("row").isValid()) rowSends.append(button);
        QCOMPARE(rowSends.size(), 2);

        // 发送区目标是 A、B 两个独立开关，当前选中的就是唯一目标来源。
        // 默认只勾着 A；先勾上 B 再取消 A，只留 B，验证指令发去了发送区选的那个口。
        if (!sendB->isChecked()) sendB->click();
        if (sendA->isChecked()) sendA->click();
        QVERIFY(!sendA->isChecked() && sendB->isChecked());

        int rowsSeen = model->rowCount();
        rowSends.at(0)->click(); // 文本指令 → B
        QTRY_VERIFY(model->rowCount() > rowsSeen);
        auto sent = model->record(model->rowCount() - 1);
        QCOMPARE(sent.direction, Traffic::Direction::Transmit);
        QCOMPARE(sent.source.endpointIndex, 1); // 发去了 B，不是写死的 A
        QCOMPARE(sent.raw, QByteArray("+CR,GO;\r\n")); // 结尾符必须真的在末尾

        rowsSeen = model->rowCount();
        rowSends.at(1)->click(); // HEX 指令
        QTRY_VERIFY(model->rowCount() > rowsSeen);
        sent = model->record(model->rowCount() - 1);
        QCOMPARE(sent.direction, Traffic::Direction::Transmit);
        // HEX 内容 + 结尾符字节：AA 55 0D 0A。若把结尾符当文本拼进内容再交给
        // fromHex，那两个控制字符不是十六进制数字会被跳过，这条断言就会失败。
        QCOMPARE(sent.raw, QByteArray::fromHex("AA550D0A"));
    }

    // ★ 源码里引用的每一个 :/icons/... 都必须真的在资源里。
    //
    // 这个类别的错**不报错、不崩溃**，只是图标变成空白或纯色块——本次的
    // `image: url(:/icons/check-white.svg)` 与 `chevron-double-*.svg` 就是这样：
    // 文件躺在 resources/icons/ 里，但没登记进 resources.qrc，于是
    // ① QSS 的 image 静默失败 → 勾选态只剩一块蓝底、看不到白勾；
    // ② 图标引擎只留下一条 QWARN，跑测试时混在几十行警告里没人看。
    //
    // 判据取"源码里出现过这个路径"而不是手抄一份清单：手抄的清单会和源码一起过期，
    // 那正是 bug 的成因。扫描根目录由 __FILE__ 推出（编译期绝对路径）。
    void everyReferencedIconIsBundled()
    {
        // 项目根从 exe 所在目录逐级向上找：同时含 `src/ui/ui.pri` 与 `resources/resources.qrc`
        // 的那一级就是根。**不靠 __FILE__、也不靠工作目录**——编译器给的 __FILE__ 是相对
        // 路径，而相对谁取决于构建方式，配出来的绝对路径会指到 build/ 而不是项目根。
        QDir probe(QCoreApplication::applicationDirPath());
        QString rootPath;
        while (!probe.isRoot()) {
            if (probe.exists(QStringLiteral("src/ui/ui.pri"))
                && probe.exists(QStringLiteral("resources/resources.qrc"))) {
                rootPath = probe.absolutePath();
                break;
            }
            probe.cdUp();
        }
        QVERIFY2(!rootPath.isEmpty(),
            qPrintable(QStringLiteral("从 %1 向上找不到项目根").arg(QCoreApplication::applicationDirPath())));
        const QDir root(rootPath);

        // 只扫代码与样式表——文档里提到图标名不代表运行时要加载它。
        const QStringList filters{"*.cpp", "*.h", "*.qss"};
        // 必须带扩展名：不加这一条，本文件里那行模式字符串自身的字面量 ":/icons/"
        // 会被当成一个图标引用（`...` 被 [A-Za-z0-9_.-]+ 吃掉），报出一堆假缺失。
        // 真实引用一律带 .svg / .xpm / .png。
        static const QRegularExpression pattern(
            QStringLiteral(":/icons/[A-Za-z0-9_-]+\\.[A-Za-z0-9]+"));
        QSet<QString> referenced;
        QDirIterator it(rootPath, filters, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            // 构建产物（build*/）里是生成物与拷贝件，不是"源码引用了什么"的依据。
            // Windows 下 QDir 遍历给原生反斜杠，两种分隔符都要排除——
            // 只查 "/build" 的话，build/ 下生成物里的旧图标引用会误报"被源码引用"。
            if (QDir::fromNativeSeparators(path).contains(QStringLiteral("/build"))) continue;
            // rcc 生成物（qrc_*.cpp）内嵌的是打包时刻的资源引用，不是"源码引用了什么"；
            // 源码树里早期 qmake 散落的 release/ 目录因此被扫到过，统一跳过生成物。
            if (QFileInfo(path).fileName().startsWith(QStringLiteral("qrc_"))) continue;
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
            const QString text = QString::fromUtf8(file.readAll());
            auto match = pattern.globalMatch(text);
            while (match.hasNext()) referenced.insert(match.next().captured());
        }
        QVERIFY2(referenced.size() > 10,
            qPrintable(QStringLiteral("在 %1 下只扫到 %2 个图标引用，扫描范围大概不对")
                           .arg(rootPath).arg(referenced.size())));

        QStringList missing;
        for (const auto &path : referenced) {
            QFile icon(path);
            if (!icon.exists()) missing << path;
        }
        missing.sort();
        QVERIFY2(missing.isEmpty(),
            qPrintable(QStringLiteral("这些图标被源码引用但不在资源里（补进 resources.qrc）：\n  ")
                + missing.join(QStringLiteral("\n  "))));
    }

    // ★ 批量发送要有进度标识（用户钦定）：正在发的那一条勾选框转琥珀，
    // 这一轮发过的留绿色，一眼看出"发到哪条了"。
    void batchSendMarksProgressOnRows()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1400, 800);
        window.show();
        QTest::qWait(30);

        auto *table = window.findChild<CommandTableWidget *>();
        // 批量发送按钮的 objectName 是 primaryButton（QSS 按它画强调底，见实现里的说明）。
        auto *batch = window.findChild<QPushButton *>(QStringLiteral("primaryButton"));
        QVERIFY(table && batch);

        CommandSet set;
        for (int i = 0; i < 3; ++i) {
            CommandItem item;
            item.content = QStringLiteral("+CR,%1;").arg(i);
            item.description = QStringLiteral("指令%1").arg(i);
            item.delayMs = 0; // 用例不等延时，只验进度标识
            set.commands.append(item);
        }
        table->setCommandSet(set);
        QTRY_COMPARE(table->rowCount(), 3);
        QTest::qWait(20);

        // 起点干净：没有任何行带着发送痕迹。
        for (int row = 0; row < 3; ++row) QVERIFY(table->sendStateOf(row).isEmpty());

        // 光验属性还不够——属性对了、QSS 没覆盖到（选择器写错、被更具体的规则压掉、
        // 动态属性改了但没重新 polish）都是一样的白干，而屏幕上就是没变色。
        // 所以这里**真的把它画出来看**：临时装上应用样式表，取勾选框指示器的像素。
        // （测试进程默认不带样式表，因为 QSS 是 main.cpp 装的。）
        QFile qss(QStringLiteral(":/styles/workbench.qss"));
        QVERIFY(qss.open(QIODevice::ReadOnly));
        const QString previousStyle = qApp->styleSheet();
        qApp->setStyleSheet(QString::fromUtf8(qss.readAll()));

        // 指示器在勾选框左侧 16×16；按"这个区域里最饱和的那个像素"取色，
        // 比盯死某个坐标稳（不同 DPI 下居中留白会变）。
        const auto indicatorColor = [&table](int row) {
            const QString state = table->sendStateOf(row);
            if (state.isEmpty()) return QColor();
            // 走公开接口拿不到控件，按"动态属性值对得上"定位（每行至多一个带标记的勾选框）。
            QCheckBox *target = nullptr;
            for (auto *box : table->findChildren<QCheckBox *>())
                if (box->property("sendState").toString() == state) { target = box; break; }
            if (!target) return QColor();
            const QImage shot = target->grab().toImage();
            QColor best;
            int bestScore = -1;
            for (int y = 0; y < qMin(18, shot.height()); ++y) {
                for (int x = 0; x < qMin(18, shot.width()); ++x) {
                    const QColor c = shot.pixelColor(x, y);
                    const int score = qMax(qMax(c.red(), c.green()), c.blue())
                        - qMin(qMin(c.red(), c.green()), c.blue());
                    if (score > bestScore) { bestScore = score; best = c; }
                }
            }
            return best;
        };

        table->beginSendRun();
        table->setSendCursor(0);
        QTest::qWait(20);
        const QColor sending = indicatorColor(0);
        table->setSendCursor(1); // 0 号转为"已发送"
        QTest::qWait(20);
        const QColor sent = indicatorColor(0);
        table->clearSendRun();
        QTest::qWait(20);

        qApp->setStyleSheet(previousStyle); // 复原，别影响其他用例

        // 琥珀（正在发）：红高、绿中、蓝很低。绿（已发）：绿最高、红最低。
        QVERIFY2(sending.isValid() && sending.red() > 190 && sending.green() > 110
                && sending.green() < 210 && sending.blue() < 90,
            qPrintable(QStringLiteral("正在发送的勾选框应为琥珀色，实测 rgb(%1,%2,%3)")
                           .arg(sending.red()).arg(sending.green()).arg(sending.blue())));
        QVERIFY2(sent.isValid() && sent.green() > 110 && sent.red() < 90,
            qPrintable(QStringLiteral("已发送的勾选框应为绿色，实测 rgb(%1,%2,%3)")
                           .arg(sent.red()).arg(sent.green()).arg(sent.blue())));

        // 游标语义单独验一遍（批量发送跑得快，中间态不适合抢时间窗去抓）。
        table->beginSendRun();
        table->setSendCursor(1);
        QCOMPARE(table->sendStateOf(1), QStringLiteral("sending"));
        QVERIFY(table->sendStateOf(0).isEmpty());
        table->setSendCursor(2); // 游标前移：1 变成"已发送"
        QCOMPARE(table->sendStateOf(1), QStringLiteral("sent"));
        QCOMPARE(table->sendStateOf(2), QStringLiteral("sending"));
        table->endSendRun();
        QCOMPARE(table->sendStateOf(2), QStringLiteral("sent"));
        QVERIFY(table->sendStateOf(0).isEmpty());
        table->clearSendRun();
        for (int row = 0; row < 3; ++row) QVERIFY(table->sendStateOf(row).isEmpty());

        // 端到端：勾选 → 点「按序发送勾选项」→ 三条最终都留下"已发送"标记。
        // 整条链（表格取行号 → 面板排时序 → 回写进度 → 收尾）一次跑通。
        //
        // 等待要盯**收尾完成**这个条件本身（最后一行由 sending 转 sent），不能只等
        // 第一行变 sent 就往下断言：游标是逐条推进的，第一行早早就是 sent 了，
        // 而最后一行还停在 sending —— 那时断言等于在抢时间窗。
        for (auto *box : table->findChildren<QCheckBox *>()) box->setChecked(true);
        batch->click();
        QTRY_COMPARE_WITH_TIMEOUT(table->sendStateOf(2), QStringLiteral("sent"), 3000);
        QCOMPARE(table->sendStateOf(0), QStringLiteral("sent"));
        QCOMPARE(table->sendStateOf(1), QStringLiteral("sent"));

        // 重建表格必须把痕迹清干净：行号会被重排，留着高亮只会指到别的指令上。
        table->setCommandSet(set);
        for (int row = 0; row < 3; ++row) QVERIFY(table->sendStateOf(row).isEmpty());
    }

    // ★ 展开/收起指令面板时，**数据区宽度原地不动、窗口宽度跟着变**（用户钦定）。
    //
    // 早先的行为是"收起就把空出来的宽度全给数据区"，于是收起那一下监视区突然变宽、
    // 展开又缩回去——折行位置与列宽跟着跳，正看着的内容会整屏重排。
    // 这条用例把"数据区宽度是锚点"钉死：两种切换下数据区都必须原地不动，
    // 而窗口宽度必须**恰好**增减面板那一段（只验"变了"是不够的）。
    void togglingPanelKeepsDataAreaWidth()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1600, 900);
        window.show();
        QTest::qWait(50);

        auto *splitter = window.findChild<QSplitter *>(QStringLiteral("horizontalSplitter"));
        auto *toggle = window.findChild<QPushButton *>(QStringLiteral("commandPanelToggle"));
        QVERIFY(splitter && toggle);
        QCOMPARE(splitter->count(), 2);

        const int expandedData = splitter->widget(0)->width();
        const int expandedPanel = splitter->widget(1)->width();
        const int expandedWindow = window.width();
        qInfo() << "EXPANDED data" << expandedData << "panel" << expandedPanel
                << "window" << expandedWindow << "splitter" << splitter->width()
                << "handle" << splitter->handleWidth();
        QVERIFY2(expandedData > 0 && expandedPanel > 0,
            qPrintable(QStringLiteral("展开态几何不对：数据区 %1 面板 %2")
                           .arg(expandedData).arg(expandedPanel)));

        // ★ 端口条必须**止步于分栏线**，不许横贯到指令面板下方。
        //
        // 用户反馈："点击扩展时端口设置区域跟着拉长"。根因是端口条原本挂在**根布局**里
        // 横贯全宽：面板展开把窗口加宽，横贯的端口条就跟着被拉长了。端口条改为左侧列的
        // 成员之后，它的右边缘应当不超过面板的左边缘——这条断言直接编码用户的诉求
        //（"让快捷面板占据整个右侧"），比只验宽度更有意义。
        auto *endpointBar = window.findChild<QFrame *>(QStringLiteral("EndpointBar"));
        QVERIFY(endpointBar);
        const int expandedBarWidth = endpointBar->width();
        const int barRight = endpointBar->mapTo(&window, QPoint()).x() + expandedBarWidth;
        const int panelLeft = splitter->widget(1)->mapTo(&window, QPoint()).x();
        QVERIFY2(barRight <= panelLeft,
            qPrintable(QStringLiteral("端口条越过了分栏线（应止步于指令面板左侧）："
                                      "右缘 %1 vs 面板左缘 %2")
                           .arg(barRight).arg(panelLeft)));

        // 收起：数据区原地不动，面板归零，窗口宽度恰好减去面板那一段。
        toggle->setChecked(false);
        QTest::qWait(50);
        const int collapsedData = splitter->widget(0)->width();
        const int collapsedWindow = window.width();
        qInfo() << "COLLAPSED data" << collapsedData << "window" << collapsedWindow
                << "splitter" << splitter->width() << "shrunk" << (expandedWindow - collapsedWindow);
        // 数据区是锚点：两种状态下的宽度必须**逐像素相等**。
        QCOMPARE(collapsedData, expandedData);
        QCOMPARE(splitter->widget(1)->width(), 0);
        // 端口条同理：它在左列里，左列宽度不变，它就不该变——这是用户报的
        // "端口设置区域跟着拉长"的直接反面。
        QCOMPARE(endpointBar->width(), expandedBarWidth);
        QVERIFY2(collapsedWindow < expandedWindow,
            qPrintable(QStringLiteral("收起后窗口应变窄：%1 → %2")
                           .arg(expandedWindow).arg(collapsedWindow)));
        // 窗口收窄量应当约等于面板宽度（那一段还给了桌面）。给一点余量：
        // 分栏对手柄怎么记账是 Qt 的实现细节（手柄在收起时归谁、QSS 画多宽），
        // 钉死几像素只会让用例在无关改动上红——**关键不变式是上面那条"数据区逐像素不变"**。
        const int shrunk = expandedWindow - collapsedWindow;
        QVERIFY2(qAbs(shrunk - expandedPanel) <= 12,
            qPrintable(QStringLiteral("窗口收窄 %1px，应约等于面板宽度 %2px")
                           .arg(shrunk).arg(expandedPanel)));

        // 展开：数据区仍然原地不动（核心不变式），面板重新出现，窗口加宽回去。
        toggle->setChecked(true);
        QTest::qWait(50);
        QCOMPARE(splitter->widget(0)->width(), expandedData);
        QVERIFY2(splitter->widget(1)->width() > 0, qPrintable(QStringLiteral("面板应重新出现")));
        const int onceExpandedWindow = window.width();
        QVERIFY2(onceExpandedWindow > collapsedWindow,
            qPrintable(QStringLiteral("展开后窗口应变宽：%1 → %2")
                           .arg(collapsedWindow).arg(onceExpandedWindow)));

        // ★ 幂等：再收起再展开一轮，几何必须与上一轮**完全相同**。
        // 这条比"回到最初那个数字"更有意义——最初那个宽度是初始布局给的（面板拿到的
        // 实际宽度可能略小于它自己的最小宽度），把它当基准会钉在一个偶然值上。
        // 而"反复切换不漂移"才是用户真正会遇到的：连点几次扩展，窗口不该越点越宽。
        toggle->setChecked(false);
        QTest::qWait(50);
        QCOMPARE(splitter->widget(0)->width(), expandedData);
        QCOMPARE(window.width(), collapsedWindow);
        toggle->setChecked(true);
        QTest::qWait(50);
        QCOMPARE(splitter->widget(0)->width(), expandedData);
        QCOMPARE(window.width(), onceExpandedWindow);
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
                 "filter/warnings", "filter/errors", "filter/expanded",
                 // 显示开关同样会落盘：某个用例把"同时显示 HEX"打开后不关，
                 // 下一个用例就会在"行数多一倍"的前提下断言——与过滤条件同一类串扰。
                 "traffic/pure", "traffic/hex", "traffic/wrap",
                 // 编码格式与发送格式也会落盘，且后果最隐蔽：某个用例把发送格式切成
                 // GBK/UTF-8 后不切回来，下一个用例就会在"输入按文本编码"的前提下断言——
                 // 实测 `sendPreviewRejectsInvalidInput` 因此间歇失败，报的是
                 // `"01 GG"` 被解析成 7 字节（5 字符 + CRLF）而不是"输入错误"。
                 // 这类用例大多**显式设置** format，不依赖残留值，所以清掉是安全的。
                 "traffic/format", "send/format", "send/suffix",
                 "file/mode", "file/endpoint", "file/block", "file/direction",
                 "file/beforeId", "file/afterId", "file/beforeDelay", "file/afterDelay",
                 "file/send/beforeHex", "file/send/afterHex",
                 "file/receive/beforeHex", "file/receive/afterHex",
                 // 窗口几何与指令面板展开状态同样会落盘，而且后果最直接：某个用例把面板
                 // 收起 / 把窗口拖小之后不还原，下一个用例就在"面板已收起"的前提下断言。
                 // （commandPanel/expanded 由 CommandPanel::savePreferences 写入，属既有遗漏。）
                 "window/geometry", "window/width", "window/height", "window/stayOnTop",
                 "commandPanel/expanded"})
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
        QVERIFY(!address->isEditable());
        address->setCurrentIndex(address->findData(QStringLiteral("0.0.0.0")));
        QVERIFY(selector.hasValidConfiguration());
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

    void editableBaudRateUsesTheWholeComboEditField()
    {
        PortSelectorWidget selector;
        PortDraft port;
        port.values.insert(QStringLiteral("serial.port"), QStringLiteral("COM3"));
        port.values.insert(QStringLiteral("serial.baud"), QStringLiteral("2147483647"));
        selector.setDraft(port);
        selector.show();

        auto *baud = selector.findChild<QComboBox *>(QStringLiteral("serial.baud"));
        QVERIFY(baud);
        auto *edit = baud->lineEdit();
        QVERIFY(edit);
        edit->setFocus();
        edit->setCursorPosition(edit->text().size());
        QCoreApplication::processEvents();

        const auto editFieldWidth = [baud] {
            QStyleOptionComboBox option;
            option.initFrom(baud);
            option.currentText = baud->currentText();
            return baud->style()->subControlRect(
                QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, baud).width();
        };
        QVERIFY2(editFieldWidth() >= edit->fontMetrics().horizontalAdvance(edit->text()),
                 "The editable baud-rate field must fit the longest legal value");

        // A preset loaded after a longer custom value must reset the embedded editor's
        // horizontal scroll. The first digit of 921600 was previously hidden in this path.
        port.values.insert(QStringLiteral("serial.baud"), QStringLiteral("2000000"));
        selector.setDraft(port);
        QCoreApplication::processEvents();
        QCOMPARE(edit->text(), QStringLiteral("2000000"));
        QVERIFY2(editFieldWidth() >= edit->fontMetrics().horizontalAdvance(edit->text()),
                 "The editable baud-rate field must fit 2000000");

        port.values.insert(QStringLiteral("serial.baud"), QStringLiteral("921600"));
        selector.setDraft(port);
        QCoreApplication::processEvents();
        QCOMPARE(edit->text(), QStringLiteral("921600"));
        QCOMPARE(edit->cursorPosition(), 0);

        // The embedded editor must not retain the generic line-edit frame/padding. Otherwise
        // Qt may scroll the text left even when the combo still has visibly unused space.
        QCOMPARE(edit->textMargins(), QMargins());
        QVERIFY(edit->contentsRect().width() >= edit->fontMetrics().horizontalAdvance(edit->text()));
    }

    void localBindingsUseInterfacePickerAndAutoPort()
    {
        PortSelectorWidget selector;
        PortDraft port;
        port.kind = PortDraft::Udp;
        port.values.insert("udp.target", "");
        selector.setDraft(port);

        auto *bindAddress = selector.findChild<QComboBox *>("udp.address");
        auto *bindPort = selector.findChild<QSpinBox *>("udp.port");
        auto *target = selector.findChild<QComboBox *>("udp.target");
        QVERIFY(bindAddress && bindPort && target);
        QVERIFY(!bindAddress->isEditable());
        QVERIFY(bindAddress->findData(QStringLiteral("0.0.0.0")) >= 0);
        QCOMPARE(bindAddress->currentData().toString(), QStringLiteral("0.0.0.0"));
        QCOMPARE(bindPort->minimum(), 0);
        QCOMPARE(bindPort->value(), 0);
        QVERIFY(target->isEditable());
        QVERIFY(target->lineEdit()->placeholderText().contains(QStringLiteral("仅接收")));
        QVERIFY(bindAddress->toolTip().contains(QStringLiteral("本机 IPv4 网卡")));
        QVERIFY(selector.hasValidConfiguration());
        QCOMPARE(selector.draft().values.value("udp.address").toString(), QStringLiteral("0.0.0.0"));

        port = PortDraft{};
        port.kind = PortDraft::TcpServer;
        selector.setDraft(port);
        auto *serverBind = selector.findChild<QComboBox *>("server.address");
        QVERIFY(serverBind);
        QVERIFY(!serverBind->isEditable());
        QVERIFY(serverBind->findData(QStringLiteral("0.0.0.0")) >= 0);
        QCOMPARE(serverBind->currentData().toString(), QStringLiteral("0.0.0.0"));
        QVERIFY(serverBind->toolTip().contains(QStringLiteral("本机 IPv4 网卡")));
        QCOMPARE(selector.draft().values.value("server.address").toString(), QStringLiteral("0.0.0.0"));

        for (int i = 1; i < serverBind->count(); ++i) {
            const QString address = serverBind->itemData(i).toString();
            QVERIFY(serverBind->itemText(i).contains(QStringLiteral(" · ")));
            QVERIFY(serverBind->itemText(i).endsWith(address));
            QVERIFY(serverBind->itemData(i, Qt::ToolTipRole).toString().contains(QStringLiteral("网卡：")));
        }
    }

    void udpSourcesAreVisibleWithoutChangingTheSendTarget()
    {
        // 每条 UDP 接收记录都在监视区的 RX 后带上实际来源。
        TrafficTextView view;
        TrafficTextView::DisplayOptions options;
        options.number = false;
        options.timestamp = false;
        Traffic::Message message;
        message.id = 1;
        message.timestamp = QDateTime::currentDateTime();
        message.direction = Traffic::Direction::Receive;
        message.framing = Traffic::Framing::Datagram;
        message.source.endpointIndex = 0;
        message.source.endpointKind = PortDraft::Udp;
        message.source.endpointId = QUuid::createUuid();
        message.source.peerAddress = QStringLiteral("127.0.0.1:9001");
        message.raw = QByteArray("ping");
        message.rawLength = message.raw.size();
        view.appendMessages({message}, PayloadCodec::Format::Utf8, options);
        QCOMPARE(view.toPlainText(), QStringLiteral("RX ← 127.0.0.1:9001: ping"));

        // 纯数据模式依旧不添加任何元信息。
        TrafficTextView pureView;
        options.pure = true;
        pureView.appendMessages({message}, PayloadCodec::Format::Utf8, options);
        QCOMPARE(pureView.toPlainText(), QStringLiteral("ping"));

        PortSelectorWidget selector;
        PortDraft draft;
        draft.kind = PortDraft::Udp;
        draft.values.insert(QStringLiteral("udp.target"), QStringLiteral("192.168.1.10"));
        selector.setDraft(draft);
        auto *hint = selector.findChild<QLabel *>(QStringLiteral("udpPeerHint"));
        QVERIFY(hint);
        selector.setPeerList({QStringLiteral("127.0.0.1:9001"), QStringLiteral("192.168.1.20:8000")});
        QCOMPARE(hint->text(), QStringLiteral("本次已收到来自 2 个来源的数据报"));
        QCOMPARE(hint->toolTip(), QStringLiteral("127.0.0.1:9001\n192.168.1.20:8000"));
        QCOMPARE(selector.draft().values.value(QStringLiteral("udp.target")).toString(),
                 QStringLiteral("192.168.1.10"));
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
        const auto artifactDir = qEnvironmentVariable("QQCOM_UI_ARTIFACT_DIR");
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

    void historyBudgetHandlesOversizedBatch()
    {
        TrafficModel model;
        model.setBudget(5, 3);
        QList<Traffic::Message> batch;
        for (int i = 1; i <= 10; ++i) {
            Traffic::Message message;
            message.id = i;
            message.raw = "xy";
            batch.append(message);
        }
        QCOMPARE(model.appendRecords(batch), 8);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.rawBytes(), qint64(4));
        QCOMPARE(model.record(0).id, quint64(9));
        QCOMPARE(model.record(1).id, quint64(10));
        model.setBudget(100, 1);
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.record(0).id, quint64(10));
        Traffic::Message oversized;
        oversized.raw = QByteArray(8192, 'x');
        QCOMPARE(model.appendRecords({oversized}), 2);
        QCOMPARE(model.rowCount(), 0);
        QCOMPARE(model.rawBytes(), qint64(0));
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

    void pendingTrafficIsLoggedOnDestruction()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        PreferencesStore store(nullptr, directory.filePath("preferences.ini"));
        store.setValue("traffic/recordPath", directory.filePath("tail.log"));
        store.setValue("traffic/record", true);
        store.setValue("traffic/format", "UTF-8");
        {
            TrafficPanel panel;
            panel.setPreferences(&store);
            Traffic::Message message;
            message.timestamp = QDateTime::currentDateTime();
            message.raw = "pending-at-exit";
            message.rawLength = message.raw.size();
            panel.appendMessage(message);
            // 不推进事件循环，确保这一批尚未进入显示刷新。
        }
        const auto paths = QDir(directory.path()).entryList({"tail-*.log"}, QDir::Files);
        QCOMPARE(paths.size(), 1);
        QFile file(directory.filePath(paths.first()));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.readAll().contains("pending-at-exit"));
    }

    void logRecordingFollowsDisplayFormat()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // 用户选定的是"模板路径"：实际文件名每次带时间戳另起，模板本身不写。
        const auto templatePath = directory.filePath(QStringLiteral("traffic.log"));
        PreferencesStore store(nullptr, directory.filePath(QStringLiteral("preferences.ini")));
        // 预置路径：没手动指定时会写到程序目录下的 log，这里改到临时目录。
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
        rx.source.peerAddress = QStringLiteral("127.0.0.1:9001");
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
        // 元信息头后是固定的列名：日志里的 Peer 字段有明确含义。
        QCOMPARE(hexLines.size(), 4);
        QVERIFY2(hexLines.at(0).startsWith(QStringLiteral("# QQCom")), qPrintable(hexLines.at(0)));
        QVERIFY(hexLines.at(0).contains(QStringLiteral("格式=HEX")));
        QCOMPARE(hexLines.at(1), QStringLiteral("# Time\tPort\tPeer\tDirection\tData"));
        QVERIFY(hexLines.at(2).contains(QStringLiteral("A · 串口")));
        QVERIFY(hexLines.at(2).contains(QStringLiteral("127.0.0.1:9001")));
        QVERIFY(hexLines.at(2).contains(QStringLiteral("RX")));
        QVERIFY(hexLines.at(2).endsWith(QStringLiteral("01 02"))); // HEX 模式记 HEX
        QVERIFY(hexLines.at(3).endsWith(QStringLiteral("FF")));
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
        // 元信息 + 列名 + 2 数据 + 格式切换行 + 1 数据 = 6 行
        QCOMPARE(all.size(), 6);
        QVERIFY(all.at(4).contains(QStringLiteral("格式切换")));
        QVERIFY(all.at(4).contains(QStringLiteral("UTF-8")));
        QVERIFY(all.at(5).endsWith(QStringLiteral("ok")));
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

        // ---- C2：新文件首行是元信息头 # QQCom ... 格式=... ----
        QStringList lines = readLines(firstLog);
        QVERIFY(!lines.isEmpty());
        QVERIFY2(lines.at(0).startsWith(QStringLiteral("# QQCom ")), qPrintable(lines.at(0)));
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
        QCOMPARE(lines.size(), 4); // 元信息 + 列名 + 2 数据
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
        QCOMPARE(metaLines, 2); // 元信息与列名各占一行

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
        QCOMPARE(secondLines.size(), 2); // 元信息 + 列名，清空前的 2 条数据不在其中
        QVERIFY2(secondLines.at(0).startsWith(QStringLiteral("# QQCom ")), qPrintable(secondLines.at(0)));
        QCOMPARE(secondLines.at(1), QStringLiteral("# Time\tPort\tPeer\tDirection\tData"));

        // ---- C5：切换显示格式 → 当前文件追加一行 # 格式切换 → ... ----
        format->setCurrentText(QStringLiteral("UTF-8"));
        const QStringList afterSwitch = readLines(secondLog);
        QCOMPARE(afterSwitch.size(), 3); // 元信息 + 列名 + 格式切换
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

    // 没手动指定目录时，日志写到程序目录下的 log，不再弹保存对话框。
    // 指定过的目录留在偏好里，显示设置里的「日志位置」和「恢复默认位置」可以再改。
    void logDirectoryDefaultsToProgramFolderAndCanChange()
    {
        const QString programLog = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("log"));
        QStringList created;
        struct Guard {
            QStringList *files;
            ~Guard()
            {
                for (const auto &path : *files) QFile::remove(path);
            }
        } guard{&created};
        const auto currentLogPath = [](QAction *record, const QString &directory) {
            const QString marker = QStringLiteral("正在写 ");
            const int at = record->toolTip().indexOf(marker);
            if (at < 0) return QString();
            return QDir(directory).filePath(record->toolTip().mid(at + marker.size()).trimmed());
        };

        QTemporaryDir iniDir;
        QVERIFY(iniDir.isValid());
        PreferencesStore store(nullptr, iniDir.filePath(QStringLiteral("preferences.ini")));
        store.setValue(QStringLiteral("traffic/record"), true);
        TrafficPanel panel;
        panel.setPreferences(&store);
        auto *record = panel.findChild<QAction *>(QStringLiteral("recordTraffic"));
        auto *choose = panel.findChild<QAction *>(QStringLiteral("chooseLogDirectory"));
        auto *reset = panel.findChild<QAction *>(QStringLiteral("resetLogDirectory"));
        QVERIFY(record && choose && reset);
        QVERIFY(record->isChecked());
        QVERIFY(!reset->isEnabled());
        QCOMPARE(QDir::cleanPath(panel.logDirectory()), QDir::cleanPath(programLog));
        QVERIFY(choose->toolTip().contains(QDir::toNativeSeparators(programLog)));
        const QString first = currentLogPath(record, programLog);
        QVERIFY2(!first.isEmpty(), qPrintable(record->toolTip()));
        QVERIFY(QFileInfo(first).fileName().startsWith(QStringLiteral("QQCom-log-")));
        QVERIFY(QFileInfo(first).fileName().endsWith(QStringLiteral(".txt")));
        QVERIFY(QFile::exists(first));
        QVERIFY(!QFile::exists(QDir(programLog).filePath(QStringLiteral("QQCom-log.txt"))));
        QVERIFY(store.value(QStringLiteral("traffic/recordPath")).toString().isEmpty());
        created.append(first);

        QTemporaryDir custom;
        QVERIFY(custom.isValid());
        panel.setLogDirectory(custom.path());
        QVERIFY(reset->isEnabled());
        QVERIFY(record->isChecked());
        QCOMPARE(QDir::cleanPath(panel.logDirectory()), QDir::cleanPath(custom.path()));
        QCOMPARE(QDir::cleanPath(store.value(QStringLiteral("traffic/recordPath")).toString()),
                 QDir::cleanPath(QDir(custom.path()).filePath(QStringLiteral("QQCom-log.txt"))));
        const QString moved = currentLogPath(record, custom.path());
        QVERIFY(QFile::exists(moved));
        QVERIFY(QFileInfo(moved).fileName().startsWith(QStringLiteral("QQCom-log-")));
        QVERIFY(QFile::exists(first)); // 换目录另起新文件，旧文件留在原地

        panel.resetLogDirectory();
        QVERIFY(!reset->isEnabled());
        QVERIFY(record->isChecked());
        QCOMPARE(QDir::cleanPath(panel.logDirectory()), QDir::cleanPath(programLog));
        QVERIFY(store.value(QStringLiteral("traffic/recordPath")).toString().isEmpty());
        const QString restored = currentLogPath(record, programLog);
        QVERIFY(restored != first);
        QVERIFY(QFile::exists(restored));
        created.append(restored);

        // 换目录只改文件夹，基名和扩展名留着。
        QTemporaryDir namedIni;
        QTemporaryDir namedDest;
        QVERIFY(namedIni.isValid() && namedDest.isValid());
        PreferencesStore namedStore(nullptr, namedIni.filePath(QStringLiteral("preferences.ini")));
        namedStore.setValue(QStringLiteral("traffic/recordPath"),
                            namedIni.filePath(QStringLiteral("session.log")));
        TrafficPanel named;
        named.setPreferences(&namedStore);
        QVERIFY(named.findChild<QAction *>(QStringLiteral("resetLogDirectory"))->isEnabled());
        named.setLogDirectory(namedDest.path());
        QCOMPARE(QDir::cleanPath(namedStore.value(QStringLiteral("traffic/recordPath")).toString()),
                 QDir::cleanPath(namedDest.filePath(QStringLiteral("session.log"))));
        named.findChild<QAction *>(QStringLiteral("recordTraffic"))->setChecked(true);
        const auto produced = QDir(namedDest.path()).entryList({QStringLiteral("session-*.log")}, QDir::Files);
        QCOMPARE(produced.size(), 1);
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
    void portOpenFailureSurfacesErrorRowAndStatus()
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
        QVERIFY(blocker.listen(QHostAddress::AnyIPv4, 0));
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
        QVERIFY2(text->toPlainText().contains(QStringLiteral("B · ")),
            qPrintable(QStringLiteral("Error 行应带端点标识：") + text->toPlainText()));
        QCOMPARE(errorRows(), 1);

        // 当前交互通过日志和状态栏报错，不弹窗打断调试。
        QVERIFY(window.findChildren<QMessageBox *>().isEmpty());

        // 状态栏短消息同样带端点标识——读状态栏右端的临时提示格 StatusNote，
        // 不再是 statusBar()->currentMessage()：后者（showMessage）会隐藏 addWidget
        // 挂的统计行，已按项目铁律改用 MainWindow::showStatusNote（见其说明）。
        auto *note = window.findChild<QLabel *>(QStringLiteral("StatusNote"));
        QVERIFY(note);
        QVERIFY2(note->text().contains(QStringLiteral("B · ")), qPrintable(note->text()));

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
    // （长压测中断开/重连风暴会把弹窗叠成轰炸）。用 localhost TCP Server：QQCom
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
        // 先确认 QQCom 已接受并收到了这条数据，再 abort——否则连接还没从 backlog
        // 接进来，断开事件无 peer 可挂。
        QTRY_VERIFY2(text->toPlainText().contains(QStringLiteral("ping")),
            qPrintable(text->toPlainText()));

        client.abort(); // 模拟对端异常掉线
        QTRY_VERIFY2(text->toPlainText().contains(QStringLiteral("已断开")),
            qPrintable(text->toPlainText()));
        QVERIFY2(text->toPlainText().contains(QStringLiteral("B · ")),
            qPrintable(QStringLiteral("Error 行应带端点标识：") + text->toPlainText()));
        // 与"打开失败"路径同理：读临时提示格 StatusNote，而不是 statusBar()->currentMessage()。
        auto *note = window.findChild<QLabel *>(QStringLiteral("StatusNote"));
        QVERIFY(note);
        QTRY_VERIFY2(note->text().contains(QStringLiteral("已断开")), qPrintable(note->text()));
        QVERIFY2(note->text().contains(QStringLiteral("B · ")), qPrintable(note->text()));
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
        QVERIFY(editor);
        // 存为命令随快捷命令页一起撤下：命令列表回归前，发送区不留没有落点的按钮。
        QVERIFY(!window.findChild<QPushButton *>("saveCommand"));
        // 预览阶段发送按钮恒为禁用（尚未连接端口），与输入合法性无关。
        for (auto *button : window.findChildren<QPushButton *>("PrimaryButton"))
            QVERIFY(!button->isEnabled());
        auto *preview = window.findChild<QLabel *>("payloadPreview");
        QVERIFY(preview);
        // 字节数与预览数据**已合并进同一个标签**（富文本），所以都从 preview->text() 上看；
        // 字节数在 span 里，用 ">2B<" 这种带标记边界的写法定位，避免误命中 HEX 里恰好出现的 "2B"。
        editor->setPlainText("01 GG");
        QVERIFY2(preview->text().contains(QStringLiteral("输入错误")),
            qPrintable(QStringLiteral("非法输入应显示字节数为「输入错误」：") + preview->text()));
        QVERIFY(preview->text().contains(QStringLiteral("无效 HEX"))); // 错误原因展示在预览位置
        editor->setPlainText("00 FF");
        QVERIFY2(preview->text().contains(QStringLiteral(">2B<")),
            qPrintable(QStringLiteral("字节数应为 2B：") + preview->text()));
        QVERIFY(preview->text().contains(QStringLiteral("00 FF")));
        // 格式/末尾切换的接线：预览字节数必须跟随变化。
        editor->setPlainText("01 03 00 00 00 02 C4 0B");
        QVERIFY2(preview->text().contains(QStringLiteral(">8B<")), qPrintable(preview->text()));
        window.findChild<QComboBox *>("sendSuffix")->setCurrentIndex(3); // CRLF
        QVERIFY2(preview->text().contains(QStringLiteral(">10B<")), qPrintable(preview->text()));
        window.findChild<QComboBox *>("sendSuffix")->setCurrentIndex(0);
        window.findChild<QComboBox *>("sendFormat")->setCurrentIndex(3); // UTF-8：正文按文本编码
        QVERIFY2(preview->text().contains(QStringLiteral(">23B<")), qPrintable(preview->text()));
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
        // 手动发送、发送文件、接收文件分成三个标签。快捷指令不再放进发送区。
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("sendModeTabs"));
        QVERIFY(tabs);
        QCOMPARE(tabs->count(), 3);
        QCOMPARE(tabs->tabText(0), QStringLiteral("手动发送"));
        QCOMPARE(tabs->tabText(1), QStringLiteral("发送文件"));
        QCOMPARE(tabs->tabText(2), QStringLiteral("接收文件"));
        QCOMPARE(window.findChildren<QTabWidget *>().size(), 1);
        auto *panel = window.findChild<SendPanel *>();
        auto *portA = window.findChild<QPushButton *>("showPortA");
        QVERIFY(panel && portA);
        // 目标只有 A、B 两个可同时选中的开关，没有单独的 AB。
        // 手动发送页的格式/末尾仍是那两个下拉框；文件页另有模式和前后指令下拉框。
        QCOMPARE(panel->findChildren<QComboBox *>(QStringLiteral("sendFormat")).size(), 1);
        QCOMPARE(panel->findChildren<QComboBox *>(QStringLiteral("sendSuffix")).size(), 1);
        // 发送区格式下拉框逐项来自共享 PayloadCodec（顺序与名称的权威断言在 renderFormats）。
        auto *sendFormat = window.findChild<QComboBox *>("sendFormat");
        QVERIFY(sendFormat);
        const auto formats = PayloadCodec::formats();
        QCOMPARE(sendFormat->count(), formats.size());
        for (int i = 0; i < formats.size(); ++i)
            QCOMPARE(sendFormat->itemText(i), PayloadCodec::formatName(formats.at(i)));
        QVERIFY(panel->findChildren<QRadioButton *>().isEmpty());
        QVERIFY(!window.findChild<QPushButton *>("sendTargetAB"));
        const QList<QPushButton *> targets{window.findChild<QPushButton *>("sendTargetA"),
                                           window.findChild<QPushButton *>("sendTargetB")};
        for (auto *button : targets) {
            QVERIFY(button && button->isCheckable());
            // 与数据显示区端点开关同款形态：同为可勾选按钮、同样高度，
            // 勾选态因此共用同一条 QPushButton:checked 样式，不受系统深色调色板影响。
            QCOMPARE(button->sizeHint().height(), portA->sizeHint().height());
        }
        // A、B 各自独立：可以同时选中，两个都选中的掩码是 3。
        // 不能把最后一个也取消，发送必须有目标。
        QVERIFY(targets.at(0)->isChecked());
        QVERIFY(!targets.at(1)->isChecked());
        QCOMPARE(panel->targetMask(), 1);
        targets.at(1)->click();
        QVERIFY(targets.at(0)->isChecked() && targets.at(1)->isChecked());
        QCOMPARE(panel->targetMask(), 3);
        targets.at(0)->click();
        QVERIFY(!targets.at(0)->isChecked() && targets.at(1)->isChecked());
        QCOMPARE(panel->targetMask(), 2);
        targets.at(1)->click();
        QVERIFY(targets.at(1)->isChecked());
        QCOMPARE(panel->targetMask(), 2);
        // 新布局（用户钦定"发送按钮放到发送框的右边，定时发送、编码选择也集成到右边"）：
        // 编码 / 定时 / 目标 / 发送全部落在正文**右侧一列**，不再铺在正文下方。
        auto *editor = window.findChild<QPlainTextEdit *>("sendEditor");
        QVERIFY(editor);
        const int editorRight = editor->mapTo(&window, QPoint()).x() + editor->width();
        for (const QString &name : {"sendFormat", "sendSuffix"}) {
            auto *box = window.findChild<QComboBox *>(name);
            QVERIFY2(box && box->isVisibleTo(&window), qPrintable(name));
            QVERIFY2(box->mapTo(&window, QPoint()).x() >= editorRight,
                qPrintable(QStringLiteral("%1 应落在正文右侧：x=%2，正文右缘=%3")
                               .arg(name)
                               .arg(box->mapTo(&window, QPoint()).x())
                               .arg(editorRight)));
        }
        // 定时发送同属右侧列。
        auto *repeat = window.findChild<QCheckBox *>("repeatSending");
        QVERIFY(repeat);
        QVERIFY2(repeat->mapTo(&window, QPoint()).x() >= editorRight,
            qPrintable(QStringLiteral("定时发送应在正文右侧：x=%1，正文右缘=%2")
                           .arg(repeat->mapTo(&window, QPoint()).x())
                           .arg(editorRight)));
        // 发送按钮在正文右侧；目标 A/B 与它同一行、且位于它左侧
        //（这一排的语义就是"发给谁 + 发出去"，拆行会读不出关系）。
        auto *send = window.findChild<QPushButton *>("PrimaryButton");
        QVERIFY(send);
        QVERIFY2(send->mapTo(&window, QPoint()).x() >= editorRight,
            qPrintable(QStringLiteral("发送按钮应在正文右侧：x=%1，正文右缘=%2")
                           .arg(send->mapTo(&window, QPoint()).x())
                           .arg(editorRight)));
        const int sendMid = send->mapTo(&window, QPoint()).y() + send->height() / 2;
        const int targetMid = targets.at(0)->mapTo(&window, QPoint()).y() + targets.at(0)->height() / 2;
        QVERIFY2(qAbs(targetMid - sendMid) <= 2,
                 qPrintable(QStringLiteral("目标与发送应在同一行：目标中心 %1 vs 发送中心 %2")
                                .arg(targetMid).arg(sendMid)));
        QVERIFY2(targets.at(1)->mapTo(&window, QPoint()).x() + targets.at(1)->width()
                     <= send->mapTo(&window, QPoint()).x(),
                 qPrintable(QStringLiteral("B right edge %1 vs send left %2")
                                .arg(targets.at(1)->mapTo(&window, QPoint()).x() + targets.at(1)->width())
                                .arg(send->mapTo(&window, QPoint()).x())));
        QCOMPARE(send->height(), targets.at(0)->height());
        QCOMPARE(targets.at(0)->height(), targets.at(1)->height());
        QVERIFY2(send->width() > targets.at(0)->width(),
                 qPrintable(QStringLiteral("发送按钮应比目标开关更宽：发送 %1，目标 %2")
                                .arg(send->width()).arg(targets.at(0)->width())));
        // 格式、末尾、间隔、A/B、发送是同一条控制带：同高，右缘对齐，
        // 三行之间的空隙相等，首行贴着正文上沿，末行与预览同一中线。
        auto *suffixBox = window.findChild<QComboBox *>(QStringLiteral("sendSuffix"));
        auto *intervalBox = window.findChild<QSpinBox *>(QStringLiteral("repeatInterval"));
        auto *preview = window.findChild<QLabel *>(QStringLiteral("payloadPreview"));
        QVERIFY(suffixBox && intervalBox && preview);
        const auto centerY = [](const QWidget *widget, const QWidget *origin) {
            return widget->mapTo(origin, QPoint()).y() + widget->height() / 2;
        };
        const auto edgeRight = [](const QWidget *widget, const QWidget *origin) {
            return widget->mapTo(origin, QPoint()).x() + widget->width();
        };
        QCOMPARE(sendFormat->height(), send->height());
        QCOMPARE(suffixBox->height(), send->height());
        QCOMPARE(intervalBox->height(), send->height());
        QVERIFY2(qAbs(sendFormat->mapTo(&window, QPoint()).y() - editor->mapTo(&window, QPoint()).y()) <= 2,
            "格式行应贴着正文上沿");
        QVERIFY2(qAbs(centerY(preview, &window) - centerY(send, &window)) <= 2,
            "预览应与发送按钮同一高度");
        QVERIFY2(qAbs(edgeRight(suffixBox, &window) - edgeRight(send, &window)) <= 2,
            "末尾下拉框应与发送按钮右缘对齐");
        QVERIFY2(qAbs(edgeRight(intervalBox, &window) - edgeRight(suffixBox, &window)) <= 2,
            "间隔框应与末尾下拉框右缘对齐");
        QVERIFY2(qAbs(intervalBox->width() - suffixBox->width()) <= 2,
            "间隔框应与末尾下拉框同宽");
        const int formatGap = intervalBox->mapTo(&window, QPoint()).y()
            - (sendFormat->mapTo(&window, QPoint()).y() + sendFormat->height());
        const int actionGap = send->mapTo(&window, QPoint()).y()
            - (intervalBox->mapTo(&window, QPoint()).y() + intervalBox->height());
        QVERIFY2(qAbs(formatGap - actionGap) <= 2,
            qPrintable(QStringLiteral("右栏行距不均：格式到定时 %1，定时到发送 %2")
                           .arg(formatGap).arg(actionGap)));
        // 三页都是内容撑开的高度，而且一样高。切换标签不再改发送区高度。
        auto *manualPage = window.findChild<QWidget *>(QStringLiteral("manualSendPage"));
        auto *fileSendPage = window.findChild<QWidget *>(QStringLiteral("fileSendPage"));
        auto *fileReceivePage = window.findChild<QWidget *>(QStringLiteral("fileReceivePage"));
        QVERIFY(manualPage && fileSendPage && fileReceivePage);
        QVERIFY2(qAbs(manualPage->sizeHint().height() - fileSendPage->sizeHint().height()) <= 2,
            qPrintable(QStringLiteral("手动发送 %1，发送文件 %2")
                           .arg(manualPage->sizeHint().height())
                           .arg(fileSendPage->sizeHint().height())));
        QVERIFY2(qAbs(manualPage->sizeHint().height() - fileReceivePage->sizeHint().height()) <= 2,
            qPrintable(QStringLiteral("手动发送 %1，接收文件 %2")
                           .arg(manualPage->sizeHint().height())
                           .arg(fileReceivePage->sizeHint().height())));
        const int paneH = panel->height();
        const int preferred = panel->preferredHeight();
        const auto artifactDir = qEnvironmentVariable("QQCOM_UI_ARTIFACT_DIR");
        if (!artifactDir.isEmpty()) {
            QDir().mkpath(artifactDir);
            QVERIFY(window.grab().save(artifactDir + "/send-panel.png"));
        }
        tabs->setCurrentIndex(1);
        QTest::qWait(30);
        QCOMPARE(panel->height(), paneH);
        QCOMPARE(panel->preferredHeight(), preferred);
        if (!artifactDir.isEmpty())
            QVERIFY(window.grab().save(artifactDir + QStringLiteral("/file-send.png")));
        auto *stats = window.findChild<QLabel *>(QStringLiteral("fileTransferStats"));
        QVERIFY(!window.findChild<QComboBox *>(QStringLiteral("fileTransferBeforeTail")));
        QVERIFY(!window.findChild<QComboBox *>(QStringLiteral("fileTransferAfterTail")));
        QVERIFY(!window.findChild<QComboBox *>(QStringLiteral("fileTransferBefore")));
        QVERIFY(stats && stats->isVisibleTo(&window));
        QVERIFY2(!stats->text().contains(QStringLiteral("B/s")), qPrintable(stats->text()));
        QVERIFY(!stats->text().contains(QStringLiteral("重试")));
        QVERIFY(window.findChild<QComboBox *>(QStringLiteral("fileTransferMode")));
        auto *beforeText = window.findChild<QLineEdit *>(QStringLiteral("fileTransferBeforeText"));
        auto *chunk = window.findChild<QSpinBox *>(QStringLiteral("fileTransferChunk"));
        auto *sendMode = window.findChild<QComboBox *>(QStringLiteral("fileTransferMode"));
        auto *sendPath = window.findChild<QLineEdit *>(QStringLiteral("fileTransferPath"));
        QVERIFY(beforeText && beforeText->isVisibleTo(&window));
        QCOMPARE(beforeText->placeholderText(), QStringLiteral("AT+START\\r\\n"));
        auto *beforeFormat = window.findChild<QComboBox *>(QStringLiteral("fileTransferBeforeFormat"));
        QVERIFY(beforeFormat);
        beforeFormat->setCurrentIndex(1);
        QTest::qWait(30);
        QCOMPARE(beforeText->placeholderText(), QStringLiteral("0D 0A，留空不发"));
        beforeFormat->setCurrentIndex(0);
        QTest::qWait(30);
        QCOMPARE(beforeText->placeholderText(), QStringLiteral("AT+START\\r\\n"));
        QVERIFY(chunk && sendMode && sendPath);
        const auto midY = [](const QWidget *widget, const QWidget *origin) {
            return widget->mapTo(origin, QPoint()).y() + widget->height() / 2;
        };
        if (sendMode->currentData().toString() != QLatin1String("raw"))
            sendMode->setCurrentIndex(sendMode->findData(QStringLiteral("raw")));
        QTest::qWait(30);
        QVERIFY2(chunk->isVisibleTo(&window), "RAW 发送要能设置分包");
        auto *chunkGap = window.findChild<QSpinBox *>(QStringLiteral("fileTransferChunkGap"));
        QVERIFY(chunkGap);
        QCOMPARE(chunkGap->suffix(), QStringLiteral(" ms"));
        QCOMPARE(chunkGap->minimum(), 0);
        QCOMPARE(chunkGap->buttonSymbols(), QAbstractSpinBox::NoButtons);
        QVERIFY2(chunkGap->isVisibleTo(&window), "RAW 发送要能设置包间隔");
        QVERIFY2(qAbs(midY(chunk, &window) - midY(chunkGap, &window)) <= 2,
            "分包大小与包间隔应在同一行");
        const int tabBottom = tabs->tabBar()->mapTo(&window, QPoint(0, tabs->tabBar()->height())).y();
        // 页顶边距是 4。间距小于它，说明页签和第一行贴在一起了。
        QVERIFY2(sendMode->mapTo(&window, QPoint()).y() - tabBottom >= 4,
            qPrintable(QStringLiteral("页签与下方内容间距过小：%1")
                           .arg(sendMode->mapTo(&window, QPoint()).y() - tabBottom)));
        QCOMPARE(chunk->buttonSymbols(), QAbstractSpinBox::NoButtons);
        auto *beforeDelay = window.findChild<QSpinBox *>(QStringLiteral("fileTransferBeforeDelay"));
        auto *afterDelay = window.findChild<QSpinBox *>(QStringLiteral("fileTransferAfterDelay"));
        QVERIFY(beforeDelay && afterDelay);
        QCOMPARE(beforeDelay->buttonSymbols(), QAbstractSpinBox::NoButtons);
        QCOMPARE(afterDelay->buttonSymbols(), QAbstractSpinBox::NoButtons);
        auto *block128 = window.findChild<QPushButton *>(QStringLiteral("fileTransferBlock128"));
        QVERIFY(block128 && !block128->isVisibleTo(&window));
        sendMode->setCurrentIndex(sendMode->findData(QStringLiteral("ymodem")));
        QTest::qWait(30);
        QVERIFY(block128->isVisibleTo(&window));
        QVERIFY(!chunk->isVisibleTo(&window));
        QVERIFY(!chunkGap->isVisibleTo(&window));
        sendMode->setCurrentIndex(sendMode->findData(QStringLiteral("raw")));
        QTest::qWait(30);
        QVERIFY(chunk->isVisibleTo(&window));
        QVERIFY(chunkGap->isVisibleTo(&window));
        QVERIFY(!block128->isVisibleTo(&window));
        QVERIFY2(qAbs(midY(sendMode, &window) - midY(sendPath, &window)) <= 2,
            "端点/方式与文件路径应在同一行");
        auto *afterText = window.findChild<QLineEdit *>(QStringLiteral("fileTransferAfterText"));
        QVERIFY(afterText && afterText->isVisibleTo(&window));
        QVERIFY2(qAbs(midY(beforeText, &window) - midY(afterText, &window)) <= 2,
            "前置指令与后置指令应在同一行");
        auto *fileStart = window.findChild<QPushButton *>(QStringLiteral("fileTransferStart"));
        auto *fileProgress = window.findChild<QProgressBar *>(QStringLiteral("fileTransferProgress"));
        QVERIFY(fileStart && fileProgress && fileStart->isVisibleTo(&window));
        QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("fileTransferStop")));
        QVERIFY2(fileStart->mapTo(&window, QPoint()).x() > fileProgress->mapTo(&window, QPoint()).x(),
            "发送按钮应在进度条右侧");
        QCOMPARE(fileStart->height(), fileProgress->height());
        auto *fileStatus = window.findChild<QLabel *>(QStringLiteral("fileTransferStatus"));
        QVERIFY(fileStatus);
        sendPath->clear();
        QCOMPARE(fileStatus->text(), QStringLiteral("未选择"));
        QVERIFY2(stats->text().contains(QStringLiteral("大小")), qPrintable(stats->text()));
        QVERIFY(!stats->text().contains(QStringLiteral("CRC16")));
        QTemporaryDir digestDir;
        QVERIFY(digestDir.isValid());
        const QString digestPath = digestDir.filePath(QStringLiteral("payload.bin"));
        QFile digestFile(digestPath);
        QVERIFY(digestFile.open(QIODevice::WriteOnly));
        // 与传输层同一组校验：CRC16-XMODEM 31C3，CRC32-IEEE CBF43926。
        QCOMPARE(digestFile.write(QByteArray("123456789")), qint64(9));
        digestFile.close();
        sendPath->setText(QDir::cleanPath(digestPath));
        QTRY_VERIFY_WITH_TIMEOUT(stats->text().contains(QStringLiteral("CRC16 31C3")), 3000);
        QVERIFY2(stats->text().contains(QStringLiteral("CRC32 CBF43926")), qPrintable(stats->text()));
        QVERIFY(stats->text().contains(QStringLiteral("payload.bin")));
        QVERIFY(stats->text().contains(QStringLiteral("9 B")));
        QCOMPARE(fileStatus->text(), QStringLiteral("待发送"));
        QVERIFY(!stats->text().contains(QStringLiteral("B/s")));
        tabs->setCurrentIndex(2);
        QTest::qWait(30);
        QCOMPARE(panel->height(), paneH);
        QCOMPARE(panel->preferredHeight(), preferred);
        auto *receiveMode = window.findChild<QComboBox *>(QStringLiteral("fileReceiveMode"));
        auto *receivePath = window.findChild<QLineEdit *>(QStringLiteral("fileReceivePath"));
        QVERIFY(receiveMode && receivePath && receivePath->isVisibleTo(&window));
        receiveMode->setCurrentIndex(receiveMode->findData(QStringLiteral("raw")));
        receivePath->clear();
        receiveMode->setCurrentIndex(receiveMode->findData(QStringLiteral("ymodem")));
        receiveMode->setCurrentIndex(receiveMode->findData(QStringLiteral("raw")));
        QTest::qWait(30);
        QVERIFY2(qAbs(midY(receiveMode, &window) - midY(receivePath, &window)) <= 2,
            "接收页端点/方式与保存路径应在同一行");
        QVERIFY2(receivePath->text().contains(QStringLiteral("raw_"))
                && receivePath->text().endsWith(QStringLiteral(".bin")),
            qPrintable(receivePath->text()));
        auto *receiveStats = window.findChild<QLabel *>(QStringLiteral("fileReceiveStats"));
        auto *receiveStatus = window.findChild<QLabel *>(QStringLiteral("fileReceiveStatus"));
        QVERIFY(receiveStats && receiveStatus);
        QCOMPARE(receiveStatus->text(), QStringLiteral("待接收"));
        QVERIFY2(receiveStats->text().contains(QStringLiteral("新文件")), qPrintable(receiveStats->text()));
        QVERIFY(!receiveStats->text().contains(QStringLiteral("CRC16")));
        auto *receiveBeforeDelay = window.findChild<QSpinBox *>(QStringLiteral("fileReceiveBeforeDelay"));
        auto *receiveAfterDelay = window.findChild<QSpinBox *>(QStringLiteral("fileReceiveAfterDelay"));
        QVERIFY(receiveBeforeDelay && receiveAfterDelay);
        QCOMPARE(receiveBeforeDelay->buttonSymbols(), QAbstractSpinBox::NoButtons);
        QCOMPARE(receiveAfterDelay->buttonSymbols(), QAbstractSpinBox::NoButtons);
        receiveMode->setCurrentIndex(receiveMode->findData(QStringLiteral("ymodem")));
        QTest::qWait(30);
        QVERIFY2(QFileInfo(receivePath->text()).isDir(),
            qPrintable(QStringLiteral("YMODEM 应使用目录：%1").arg(receivePath->text())));
        QVERIFY(!receivePath->text().endsWith(QStringLiteral(".bin")));
        QVERIFY2(receiveStats->text().contains(QStringLiteral("对端")), qPrintable(receiveStats->text()));
        QVERIFY(!receiveStats->text().contains(QStringLiteral("CRC16")));
        QCOMPARE(receiveStatus->text(), QStringLiteral("待接收"));
        const QString customDir = QDir(QDir::tempPath()).filePath(QStringLiteral("qqcom-ymodem-dir"));
        QDir().mkpath(customDir);
        receivePath->setText(QDir::cleanPath(customDir));
        receiveMode->setCurrentIndex(receiveMode->findData(QStringLiteral("raw")));
        receiveMode->setCurrentIndex(receiveMode->findData(QStringLiteral("ymodem")));
        QTest::qWait(30);
        QCOMPARE(QDir::cleanPath(receivePath->text()), QDir::cleanPath(customDir));
        tabs->setCurrentIndex(0);
    }

    // 目标 A、B 宽度取齐且不截断文字；发送按钮比它们更宽更高，文字同样不被截断。
    void sendActionButtonsShareOneWidth()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);

        QVERIFY(!window.findChild<QPushButton *>("sendTargetAB"));
        auto *targetA = window.findChild<QPushButton *>("sendTargetA");
        auto *targetB = window.findChild<QPushButton *>("sendTargetB");
        auto *send = window.findChild<QPushButton *>("PrimaryButton");
        QVERIFY2(targetA && targetB && send && targetA->isVisibleTo(&window)
                     && targetB->isVisibleTo(&window) && send->isVisibleTo(&window),
                 "发送区按钮缺失");
        QCOMPARE(targetA->width(), targetB->width());
        for (auto *button : {targetA, targetB, send}) {
            // 宽度必须撑得下完整文字：QPushButton 放不下会在文字中间插省略号，
            // 用 sizeHint()（含图标/内边距的完整需求）当判据。
            QVERIFY2(button->width() >= button->sizeHint().width(),
                qPrintable(QStringLiteral("按钮文字被截断：%1 需要 %2，实得 %3")
                               .arg(button->text())
                               .arg(button->sizeHint().width())
                               .arg(button->width())));
        }
        QCOMPARE(send->height(), targetA->height());
        QCOMPARE(targetA->height(), targetB->height());
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("sendModeTabs"));
        QVERIFY(tabs);
        QCOMPARE(tabs->count(), 3);
        for (int i = 0; i < tabs->count(); ++i)
            QVERIFY2(!tabs->tabIcon(i).isNull(), qPrintable(tabs->tabText(i)));
        QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("fileTransferBrowse"))->icon().isNull());
        QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("fileReceiveBrowse"))->icon().isNull());
        QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("fileTransferStart"))->icon().isNull());
        QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("fileReceiveStart"))->icon().isNull());
        QVERIFY2(send->width() > targetA->width(),
            qPrintable(QStringLiteral("发送按钮应比目标开关更宽：发送 %1，目标 %2")
                           .arg(send->width()).arg(targetA->width())));
        // 正文跟着文件页一起自适应，至少留得下一行，不能被压成一条缝。
        auto *editor = window.findChild<QPlainTextEdit *>("sendEditor");
        QVERIFY(editor);
        const int lineSpacing = editor->fontMetrics().lineSpacing();
        QVERIFY2(editor->height() >= lineSpacing * 2,
            qPrintable(QStringLiteral("正文只有 %1 行").arg(editor->height() / qMax(1, lineSpacing))));
    }

    // 展开右侧指令面板**不能**把数据区的头两行压变形（用户反馈：展开后过滤栏按钮
    // 被过度挤压）。约束是双向的：过滤条上的按钮保持自己的最小宽度，数据区不被压到
    // 它的宽度下限之下。这里在最小窗宽下量几何。
    // ★ 发送区的「发送」必须真的把内容发出去（用户反馈："实现发送区的发送功能"）。
    //
    // 这条链上曾断过一环：发送按钮的可用性取决于 `SendPanel::m_openFlags`，而唯一能
    // 喂它的 `SendPanel::setEndpointOpen()` **全项目没有任何调用点** —— 于是
    // m_openFlags 恒为 false、按钮永远禁用，「发送」看上去就是个摆设。
    //
    // 所以用例刻意**从 MainWindow 的真实路径走**（点端点的动作按钮让它真的监听），
    // 而不是直接调 `setEndpointOpen()`：直接调的话，接线又断了也照样绿。
    void sendPanelSendsThroughRealEndpointState()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);

        auto *send = window.findChild<QPushButton *>(QStringLiteral("PrimaryButton"));
        auto *editor = window.findChild<QPlainTextEdit *>(QStringLiteral("sendEditor"));
        auto *model = window.findChild<TrafficModel *>();
        auto *panel = window.findChild<SendPanel *>();
        auto *targetA = window.findChild<QPushButton *>(QStringLiteral("sendTargetA"));
        auto *targetB = window.findChild<QPushButton *>(QStringLiteral("sendTargetB"));
        auto *b = window.findChild<PortSelectorWidget *>(QStringLiteral("endpointB"));
        auto *repeat = window.findChild<QCheckBox *>(QStringLiteral("repeatSending"));
        QVERIFY(send && editor && model && panel && targetA && targetB && b && repeat);

        // 没有端点连上时按钮必须是禁用的——"可用性"是双向契约，
        // 修 bug 时很容易顺手改成无条件启用，那等于把"能不能发"变成"点了才知道"。
        QVERIFY2(!send->isEnabled(), "未连接端点时发送按钮不该可用");

        // 目标是发送区当前选中的端点。默认只勾着 A（串口）。这里改成只勾 B
        //（TCP 服务器，不需要真实硬件），顺带验证目标切换会刷新可用性。
        if (!targetB->isChecked()) targetB->click();
        if (targetA->isChecked()) targetA->click();
        QVERIFY(!targetA->isChecked() && targetB->isChecked());
        QCOMPARE(panel->targetMask(), 2);

        // 打开 B：走端点动作按钮，与用户操作同一条路 → runtime 监听 → 状态回传。
        auto *action = b->findChild<QPushButton *>(QStringLiteral("endpointAction"));
        QVERIFY(action);
        action->click();
        QTRY_COMPARE(action->text(), QStringLiteral("停止"));

        // ★ 核心断言：端点打开后发送按钮必须启用。这一条正是"接线缺失"的探测器。
        QTRY_VERIFY2(send->isEnabled(),
            "端点已监听，但发送按钮仍禁用——检查 handleEndpointState 是否把状态喂给了 SendPanel");

        // 内容与末尾都显式设定，不依赖共享 preferences 的残留（用例间会串扰）。
        editor->setPlainText(QStringLiteral("+CR,GO;"));
        auto *format = window.findChild<QComboBox *>(QStringLiteral("sendFormat"));
        auto *suffix = window.findChild<QComboBox *>(QStringLiteral("sendSuffix"));
        QVERIFY(format && suffix);
        format->setCurrentText(QStringLiteral("UTF-8"));
        suffix->setCurrentIndex(3); // +CR+LF
        QTRY_VERIFY(send->isEnabled());

        // 手动发送：消息必须真的进模型（TCP 服务器暂无客户端，写入可能失败，
        // 但"发出了一条 TX"这件事必须成立——失败也要留下可见记录）。
        const int rowsBefore = model->rowCount();
        send->click();
        QTRY_VERIFY(model->rowCount() > rowsBefore);
        auto sent = model->record(model->rowCount() - 1);
        QCOMPARE(sent.direction, Traffic::Direction::Transmit);
        QCOMPARE(sent.source.endpointIndex, 1);       // 发去了选中的 B，不是默认的 A
        QCOMPARE(sent.origin, Traffic::Origin::Manual); // 手点 = 手动
        QCOMPARE(sent.raw, QByteArray("+CR,GO;\r\n")); // 末尾真的追加在字节末尾

        // 两个都选中：同一拍发到 A 和 B。A 没打开，也要留下一条失败记录。
        targetA->click();
        QCOMPARE(panel->targetMask(), 3);
        const int rowsBeforeBoth = model->rowCount();
        send->click();
        QTRY_VERIFY(model->rowCount() >= rowsBeforeBoth + 2);
        bool sawA = false;
        bool sawB = false;
        for (int row = rowsBeforeBoth; row < model->rowCount(); ++row) {
            const auto record = model->record(row);
            if (record.direction != Traffic::Direction::Transmit) continue;
            if (record.source.endpointIndex == 0) sawA = true;
            if (record.source.endpointIndex == 1) sawB = true;
        }
        QVERIFY2(sawA && sawB, "两个都选中时应同时发到 A 和 B");
        targetA->click();
        QCOMPARE(panel->targetMask(), 2);

        // 定时发送：勾选即发第一拍，且**来源标记必须是 Scheduled**。
        // 这个参数以前在 sendRequested 的 lambda 里被丢掉了（形参只声明了两个），
        // 于是每一拍都被记成"手动"，屏幕上显示 `TX·手动` 而不是 `TX·定时`。
        const int rowsBeforeTick = model->rowCount();
        repeat->setChecked(true);
        QTRY_VERIFY(model->rowCount() > rowsBeforeTick);
        bool sawScheduled = false;
        for (int row = model->rowCount() - 1; row >= rowsBeforeTick; --row) {
            if (model->record(row).origin == Traffic::Origin::Scheduled) { sawScheduled = true; break; }
        }
        repeat->setChecked(false); // 立刻停，别让定时器继续往模型里灌
        QVERIFY2(sawScheduled, "定时发送的报文来源标记应为 Scheduled（界面显示 TX·定时）");

        // 关闭端点后按钮必须回到禁用：可用性跟着连接状态走，不是一次性的。
        action->click();
        QTRY_COMPARE(action->text(), QStringLiteral("监听"));
        QTRY_VERIFY2(!send->isEnabled(), "端点已关闭，发送按钮应回到禁用");
    }

    // ★ 打开/关闭必须在监视区留下**读得出结果**的迁移记录，且不能顶掉状态栏统计。
    //
    // 两个用户可见问题的回归守卫：
    // ① 状态行正文曾经直接取传输层的 detail（`COM2 @115200`），只有端口参数、
    //    没有"已打开/已关闭"这个状态词——屏幕上根本读不出连接成没成。
    // ② 统计行是用 `addWidget()` 挂的**普通**部件，而 `QStatusBar::showMessage()`
    //    会隐藏所有普通部件：打开端点时那句参数摘要一出现，RX/TX/条数/桥接整行
    //    就消失，5 秒后才回来；反复连接每次都重置那个计时器，看起来就是
    //    "统计数据不显示了、很久才恢复"。
    void endpointTransitionsAreLoggedAndKeepStatsVisible()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);

        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        auto *b = window.findChild<PortSelectorWidget *>(QStringLiteral("endpointB"));
        QVERIFY(text && b);
        auto *action = b->findChild<QPushButton *>(QStringLiteral("endpointAction"));
        QVERIFY(action);

        const auto lastLine = [text] { return text->document()->lastBlock().text(); };

        // 打开：状态行必须同时含"状态"与"已打开"两个要素——
        // 只有端口参数（旧行为）就说明状态词没拼上。
        action->click();
        QTRY_COMPARE(action->text(), QStringLiteral("停止"));
        QTRY_VERIFY(lastLine().contains(QStringLiteral("已打开")));
        QVERIFY2(lastLine().contains(QStringLiteral("状态")),
            qPrintable(QStringLiteral("状态行应带方向词：") + lastLine()));
        const QString openedLine = lastLine();

        // ★ 打开之后统计行必须**仍然可见**。旧实现里 showMessage 会把它整行藏掉，
        //   所以这条断言正是那个 bug 的探测器。
        for (const auto *name : {"RxStatus", "TxStatus", "TrafficStatus", "RouteStatus"}) {
            auto *label = window.findChild<QLabel *>(QString::fromLatin1(name));
            QVERIFY2(label && label->isVisibleTo(&window),
                qPrintable(QStringLiteral("打开端点后统计标签被隐藏：%1").arg(name)));
        }

        // 关闭：同样要留下迁移记录，且统计行照旧可见。
        action->click();
        QTRY_COMPARE(action->text(), QStringLiteral("监听"));
        QTRY_VERIFY(lastLine().contains(QStringLiteral("已关闭")));
        QVERIFY2(lastLine() != openedLine,
            qPrintable(QStringLiteral("关闭后状态行没更新：") + lastLine()));
        // 关闭来源必须写进正文：用户主动停止与对端断开/设备错误是不同事件，
        // 一条没有来源的"已关闭"让人无从判断是谁关的（用户反馈"不知道谁关闭了"）。
        QVERIFY2(lastLine().contains(QStringLiteral("用户停止")),
            qPrintable(QStringLiteral("关闭来源应写进正文：") + lastLine()));
        // 而且必须说清**是哪个端口**关了：只说"已关闭 · 用户停止"仍然答不全是 A 还是 B、
        // 是哪个口（用户反馈"需要清晰表明是那个端口关闭"）。身份取草稿的真实配置——
        // 这条用例里 B 是 TCP 服务器，身份形如 "B · 0.0.0.0:<端口>"。
        const QString identity = QStringLiteral("B · 0.0.0.0:%1")
            .arg(b->findChild<QSpinBox *>(QStringLiteral("server.port"))->value());
        QVERIFY2(lastLine().contains(identity),
            qPrintable(QStringLiteral("关闭行应带端点身份「%1」：%2").arg(identity, lastLine())));
        // 把渲染出来的关闭行原样记进测试日志：这里的**文案本身就是契约**
        //（用户要求"清晰表明是那个端口关闭"），出问题时能直接看到实际长什么样，
        // 不必反推。与 SPLITPROBE / window minimum width 那几条诊断同一用途。
        qInfo("CLOSEROW %s", qPrintable(lastLine()));
        for (const auto *name : {"RxStatus", "TxStatus", "TrafficStatus"}) {
            auto *label = window.findChild<QLabel *>(QString::fromLatin1(name));
            QVERIFY2(label && label->isVisibleTo(&window),
                qPrintable(QStringLiteral("关闭端点后统计标签被隐藏：%1").arg(name)));
        }
    }

    // 一次失败只留**一条**日志（用户反馈"错误跟状态感觉重复了"）。传输层对失败成对
    // 发出 errorOccurred + stateChanged(Closed)（两条都是底层真实事件，core 测试亦依赖），
    // 合并发生在 EndpointRuntime：错误行保留完整原因，紧随的 Closed 不再单独成 State 行。
    // 用 TCP 服务器监听一个已被占用的端口制造同步失败（不依赖网络栈超时，确定性最强）。
    void endpointFailureLeavesExactlyOneLogLine()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);
        auto *model = window.findChild<TrafficModel *>();
        auto *b = window.findChild<PortSelectorWidget *>(QStringLiteral("endpointB"));
        QVERIFY(model && b); // B 默认 TCP 服务器
        QTcpServer blocker;  // 占住端口的"另一个进程"：保持监听直到本用例结束
        QVERIFY(blocker.listen(QHostAddress::AnyIPv4, 0));
        b->findChild<QSpinBox *>(QStringLiteral("server.port"))->setValue(blocker.serverPort());
        auto *action = b->findChild<QPushButton *>(QStringLiteral("endpointAction"));
        QVERIFY(action);

        action->click(); // 监听一个已被占用的端口 → listen() 同步失败
        QTRY_COMPARE(model->rowCount(), 1);
        QCOMPARE(model->record(0).direction, Traffic::Direction::Error);
        // 那条唯一的日志必须能读出失败原因（而不是零信息的"打开失败"）。
        QVERIFY2(model->record(0).statusText.contains(QStringLiteral("失败")),
            qPrintable(QStringLiteral("错误行正文应含原因：") + model->record(0).statusText));
        // 同一次失败不再另成一条 State 行——这正是"感觉重复了"的根源。
        int stateRows = 0;
        for (int i = 0; i < model->rowCount(); ++i)
            if (model->record(i).direction == Traffic::Direction::State) ++stateRows;
        QCOMPARE(stateRows, 0);
    }

    // 失败播报必须走 showStatusNote，**不得**用 QStatusBar::showMessage——后者会隐藏所有
    // addWidget 挂的普通部件，把 RX/TX/条数统计整行顶掉 5 秒。旧实现两处残留的 showMessage
    // 正是这么干的，偏偏"打开失败常常连续发生"，用户于是看到统计行反复消失。
    //
    // 为什么必须**独立成例**：同类的端口断言分别住在 portOpenFailureSurfacesErrorRowAndStatus
    // 与 runtimeDisconnectReportsErrorRowWithoutPopup 里，而这两条用例在本机因**预存失败**
    // （断言要求日志行带端点名前缀，属过时用例）会提前中止——挂在它们后面的 StatusNote /
    // 统计行断言**永远执行不到**。独立用例才真正跑到。
    //
    // 两个断言必须**同一次失败、同一时刻**成立：
    //   B（核心）统计行未被顶掉——旧 showMessage 会让 RxStatus/TxStatus/TrafficStatus 不可见；
    //   A（信息不丢）失败原因仍被播报——m_statusNote 文本含"失败"。
    // 把 B 放在前：这样一旦有人把 showStatusNote 改回 showMessage，本用例会**先**在 B 变红，
    // 明确指向"统计行被顶掉"这个回归点，而不是被 A 的连锁失败掩盖。
    void errorBroadcastKeepsStatsVisible()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);
        auto *model = window.findChild<TrafficModel *>();
        auto *note = window.findChild<QLabel *>(QStringLiteral("StatusNote"));
        auto *b = window.findChild<PortSelectorWidget *>(QStringLiteral("endpointB"));
        QVERIFY(model && note && b); // B 默认 TCP 服务器
        QTcpServer blocker;          // 占住端口的"另一个进程"：保持监听直到本用例结束
        QVERIFY(blocker.listen(QHostAddress::AnyIPv4, 0));
        b->findChild<QSpinBox *>(QStringLiteral("server.port"))->setValue(blocker.serverPort());
        auto *action = b->findChild<QPushButton *>(QStringLiteral("endpointAction"));
        QVERIFY(action);

        action->click(); // 监听一个已被占用的端口 → listen() 同步失败（确定性，不依赖超时）
        // 等这次失败处理完毕：模型里出现那条 Error 行，且播报已落到 StatusNote。
        QTRY_COMPARE(model->rowCount(), 1);
        QCOMPARE(model->record(0).direction, Traffic::Direction::Error);

        // —— 断言 B（本次补修的核心）——
        // 用 addWidget 挂的统计行必须**仍然可见**。旧 showMessage 会把它整行隐藏 5 秒。
        for (const auto *name : {"RxStatus", "TxStatus", "TrafficStatus"}) {
            auto *label = window.findChild<QLabel *>(QString::fromLatin1(name));
            QVERIFY2(label && label->isVisibleTo(&window),
                qPrintable(QStringLiteral("失败播报把统计行顶掉了：%1 不可见").arg(name)));
        }
        // —— 断言 A（信息不丢）——
        // 改走 showStatusNote 后，失败原因仍必须被播报出来。
        QVERIFY2(note->text().contains(QStringLiteral("失败")),
            qPrintable(QStringLiteral("失败播报丢失：StatusNote 文本不含失败原因：") + note->text()));
    }

    // 正向锁定（QA 补充）：失败合并的**边界**。合并锚点 m_pendingFailureGeneration 会一直挂到
    // 下一次 stateChanged 才被消费，所以必须证明它不会"残留到下一代"或"该合却没合"：
    //   C1 连续两次失败（用户连点）——每次各自恰好一条 Error 行，且不残留任何 State 行；
    //   C4 失败后再点连接＝换代——openEndpoint 清锚点，第二次失败仍恰好一条；
    //   A  失败信息不能丢——端点必须仍被上报回 Closed（动作按钮回到"监听"，不是卡在"停止"）；
    //   C2 打开成功后再主动停止——这次关闭必须留行，且能读出"已关闭 · 用户停止"。
    // 若把 endpointruntime.cpp 的合并判断改回"从不合并"，本用例会在 C1/C4 处变红。
    void endpointFailureMergeIsSingleLineAcrossConsecutiveFailures()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);
        window.findChild<TrafficPanel *>()->setPreferences(nullptr);
        auto *model = window.findChild<TrafficModel *>();
        auto *b = window.findChild<PortSelectorWidget *>(QStringLiteral("endpointB"));
        QVERIFY(model && b); // B 默认 TCP 服务器
        auto *portBox = b->findChild<QSpinBox *>(QStringLiteral("server.port"));
        auto *action = b->findChild<QPushButton *>(QStringLiteral("endpointAction"));
        QVERIFY(portBox && action);

        const auto countRows = [model](Traffic::Direction direction) {
            int n = 0;
            for (int i = 0; i < model->rowCount(); ++i)
                if (model->record(i).direction == direction) ++n;
            return n;
        };

        // C1 + C4：连点两次都命中"监听一个已被占用端口"的确定性同步失败。
        QTcpServer blocker;
        QVERIFY(blocker.listen(QHostAddress::AnyIPv4, 0));
        portBox->setValue(blocker.serverPort());

        action->click(); // 失败 #1
        QTRY_COMPARE(countRows(Traffic::Direction::Error), 1);
        // A：端点最终回到 Closed 这件事不能丢——动作按钮必须回到"监听"。
        QTRY_COMPARE(action->text(), QStringLiteral("监听"));
        // 一次失败只留一条日志：不得出现零信息的 State 行。
        QCOMPARE(countRows(Traffic::Direction::State), 0);

        action->click(); // C4：失败后再点连接（换代）→ 仍恰好一条 Error
        QTRY_COMPARE(countRows(Traffic::Direction::Error), 2);
        QTRY_COMPARE(action->text(), QStringLiteral("监听"));
        // ★ 锚点若残留，第二次失败会多出一条 State 行，这里就会红。
        QCOMPARE(countRows(Traffic::Direction::State), 0);
        // 每条失败都读得出原因（错误行正文非空）。
        for (int i = 0; i < model->rowCount(); ++i)
            if (model->record(i).direction == Traffic::Direction::Error)
                QVERIFY2(!model->record(i).statusText.isEmpty(),
                    qPrintable(QStringLiteral("错误行丢失了原因：") + model->record(i).statusText));

        // 收掉失败弹窗，别让它干扰后续路径。
        for (auto *box : window.findChildren<QMessageBox *>())
            box->close();
        QTest::qWait(30);

        // C2：换到空闲端口打开成功，再主动停止——这次关闭必须留行，且读出"用户停止"。
        blocker.close();
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        const quint16 freePort = probe.serverPort();
        probe.close();
        portBox->setValue(freePort);

        action->click(); // 打开成功 → 留下一条"已打开" State 行，并消费掉可能残留的锚点
        QTRY_COMPARE(action->text(), QStringLiteral("停止"));
        // 用 QTRY 而不是"读到即断言"：按钮文案与状态行是**两条独立路径**（前者由
        // handleEndpointState 更新，后者经模型通知视图重投影），先后没有保证。
        // 全量跑（机器负载高）时状态行会晚一拍，此刻读到的还是上一行（实测偶发失败）。
        QTRY_VERIFY2(countRows(Traffic::Direction::State) >= 1,
            qPrintable(QStringLiteral("打开成功应留下状态行，实际 ")
                + QString::number(countRows(Traffic::Direction::State))));

        action->click(); // 用户主动停止
        QTRY_COMPARE(action->text(), QStringLiteral("监听"));
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        QVERIFY(text);
        // 同上：等**关闭行真的落到视图里**再断言，不去赌按钮与状态行同拍。
        QTRY_VERIFY2(text->document()->lastBlock().text().contains(QStringLiteral("已关闭 · 用户停止")),
            qPrintable(QStringLiteral("关闭行应读出用户停止：")
                + text->document()->lastBlock().text()));
    }

    // C3（QA 补充）：打开成功 → 运行中**对端断开** = errorOccurred + stateChanged(Closed)
    // 成对路径，必须合并成一条，且留下的那条读得出是什么错。走 TCP 客户端真实路径：
    // 连上本地监听、再让对端 abort，客户端应只多出一条 Error 行（含"断开"），且不多出 State 行。
    // 若把合并判断改回"从不合并"，State 行会多出 1 条 → 变红。
    void endpointRuntimeDisconnectAlsoCollapsesToOneLogLine()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);
        window.findChild<TrafficPanel *>()->setPreferences(nullptr);
        auto *model = window.findChild<TrafficModel *>();
        auto *b = window.findChild<PortSelectorWidget *>(QStringLiteral("endpointB"));
        QVERIFY(model && b);
        auto *type = b->findChild<QComboBox *>(QStringLiteral("portType"));
        QVERIFY(type);
        type->setCurrentIndex(PortDraft::TcpClient); // 0 串口 / 1 TCP 客户端 / 2 TCP 服务器 / 3 UDP
        QTest::qWait(20);

        const auto countRows = [model](Traffic::Direction direction) {
            int n = 0;
            for (int i = 0; i < model->rowCount(); ++i)
                if (model->record(i).direction == direction) ++n;
            return n;
        };

        QTcpServer listener;
        QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
        auto *host = b->findChild<QComboBox *>(QStringLiteral("client.host"));
        auto *port = b->findChild<QSpinBox *>(QStringLiteral("client.port"));
        auto *action = b->findChild<QPushButton *>(QStringLiteral("endpointAction"));
        QVERIFY(host && port && action);
        host->setCurrentText(QStringLiteral("127.0.0.1"));
        port->setValue(listener.serverPort());

        action->click(); // 连接
        QTRY_COMPARE(action->text(), QStringLiteral("断开")); // TCP 客户端打开态文案
        QTRY_VERIFY2(listener.hasPendingConnections(), "客户端应已连上本地监听");
        auto *peer = listener.nextPendingConnection();
        QVERIFY(peer);

        const int errorsBefore = countRows(Traffic::Direction::Error);
        const int statesBefore = countRows(Traffic::Direction::State);

        peer->abort(); // 模拟对端异常掉线（运行中出错关闭）

        QTRY_COMPARE(countRows(Traffic::Direction::Error), errorsBefore + 1);
        // ★ 成对的 Closed 被并进那条 Error：不得再多出一条 State 行。
        QCOMPARE(countRows(Traffic::Direction::State), statesBefore);
        // 留下的一条读得出是什么错。
        QString lastError;
        for (int i = 0; i < model->rowCount(); ++i)
            if (model->record(i).direction == Traffic::Direction::Error)
                lastError = model->record(i).statusText;
        QVERIFY2(lastError.contains(QStringLiteral("断开")),
            qPrintable(QStringLiteral("错误行应读出断开原因：") + lastError));
        // 端点回到 Closed：动作按钮可再次"连接"。
        QTRY_COMPARE(action->text(), QStringLiteral("连接"));
        for (auto *box : window.findChildren<QMessageBox *>())
            box->close();
    }

    void expandingCommandPanelKeepsFilterRowIntact()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);

        auto *bar = window.findChild<QWidget *>(QStringLiteral("filterBar"));
        auto *toggleExpand = window.findChild<QPushButton *>("commandPanelToggle");
        auto *commandPanel = window.findChild<CommandPanel *>();
        QVERIFY(bar && toggleExpand && commandPanel);

        // 每条按钮记录"它自己声明的最小宽度"，展开后不许跌破。
        // 图标按钮（重置）没有文字，尺寸由 iconSize + QSS 内边距决定。
        QList<QPushButton *> barButtons = bar->findChildren<QPushButton *>();
        QVERIFY(!barButtons.isEmpty());
        QHash<QPushButton *, int> minimums;
        for (auto *button : barButtons) minimums.insert(button, button->minimumSizeHint().width());

        // 指令面板已默认展开；先收起再展开，走一遍完整的状态切换。
        toggleExpand->setChecked(false);
        QTest::qWait(30);
        QVERIFY2(!bar->findChild<QPushButton *>("filterRebuild")->isVisibleTo(&window) == false,
            "过滤条本身不随指令面板收起");
        toggleExpand->setChecked(true);
        QTest::qWait(30);
        QVERIFY2(commandPanel->isVisibleTo(&window), "指令面板应展开");

        // 展开后：过滤条按钮不能被压到最小宽度以下。
        for (auto *button : barButtons) {
            QVERIFY2(button->width() >= minimums.value(button),
                qPrintable(QStringLiteral("过滤条按钮被挤压：%1 宽 %2 < 最小 %3")
                               .arg(button->objectName())
                               .arg(button->width())
                               .arg(minimums.value(button))));
        }
        // 第二行仍必须一行排得下（不重叠、不越右缘）——展开不该破坏这条铁律。
        const auto problem = overlapInRow(bar);
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
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
        QCOMPARE(window.findChild<QLabel *>("RouteStatus")->text(), QStringLiteral("已建立 · 预览"));
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
        QVERIFY(b->hasValidConfiguration()); // TCP 服务器默认监听所有 IPv4 网卡的 9000 端口
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
        QCOMPARE(defaults.idleTimeoutMs, 10);
        QCOMPARE(defaults.bufferThresholdBytes, 2048);
        QVERIFY(!defaults.includeDelimiter);           // 结束符默认剥离
        QVERIFY(defaults.includeLengthPrefix);         // 长度前缀默认保留帧头

        // Delimiter 模式必须有非空结束符；超时与阈值必须为正。
        Framing delimiter;
        delimiter.mode = Framing::Delimiter;
        QVERIFY(!delimiter.isValid());
        delimiter.delimiter = QByteArray::fromHex("0D0A");
        QVERIFY(delimiter.isValid());
        Framing idle;
        idle.mode = Framing::IdleTimeout;
        QVERIFY(idle.isValid());
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
        QVERIFY2(view.document()->toPlainText().contains(QRegularExpression(QStringLiteral("(?m)^60 .*line one"))),
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
            // 分批推进，模拟实际到达节奏；单批超限另有独立用例。
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
        text->verticalScrollBar()->triggerAction(QAbstractSlider::SliderPageStepSub);
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
        // 事件开关住在显示设置菜单里（QAction），不在顶栏。
        window.findChild<QAction *>(QStringLiteral("showErrors"))->setChecked(false);
        panel->appendMessages({makeMessage(2, Traffic::Direction::Error, "boom")});
        QTRY_COMPARE(model->rowCount(), 2);
        QCOMPARE(text->lineCount(), 1);
        QCOMPARE(text->document()->lastBlock().text(), baseline);
        // 重新开启：后续错误事件恢复显示。
        window.findChild<QAction *>(QStringLiteral("showErrors"))->setChecked(true);
        panel->appendMessages({makeMessage(3, Traffic::Direction::Error, "boom2")});
        QTRY_COMPARE(text->lineCount(), 2);
        QVERIFY2(text->document()->lastBlock().text().endsWith(QStringLiteral("boom2")),
            qPrintable(QStringLiteral("错误行实况：")
                + text->document()->lastBlock().text()));
    }

    // 外观重投影不得改变"哪些行在屏幕上"（设计稿 §5 决策表）：改显示设置 / 编码格式只换
    // 每行长什么样，未点「应用过滤」的关键词绝不能被顺手套到历史行上——否则屏幕会莫名变少、
    // 过滤输入框支持回车应用（用户钦定）：焦点在关键词/排除框里按 Enter，等同于点
    // 「应用过滤」。两条入口必须指向同一个出口——按钮禁用时回车同样无效、
    // 待应用记号与"重复重放"的判断都只在一处维护，否则两条路会各自漂移。
    void filterInputAppliesOnReturn()
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
        auto *excludes = window.findChild<QLineEdit *>(QStringLiteral("filterExcludes"));
        auto *apply = window.findChild<QPushButton *>(QStringLiteral("filterRebuild"));
        QVERIFY(text && model && keywords && excludes && apply);
        // 提示文案（用户要求"给出回车生效的提示"）：placeholder 与 tooltip 都要提到回车。
        // 两者覆盖不同时机——placeholder 在空框时可见（输入后消失），tooltip 在悬停时可见；
        // 用断言锁住，免得以后调文案时把这条提示弄丢。
        for (auto *input : {keywords, excludes}) {
            QVERIFY2(input->placeholderText().contains(QStringLiteral("回车")),
                qPrintable(QStringLiteral("placeholder 应提示回车：") + input->placeholderText()));
            QVERIFY2(input->toolTip().contains(QStringLiteral("回车")),
                qPrintable(QStringLiteral("tooltip 应提示回车：") + input->toolTip()));
        }
        // 注意断言对象是**视图行数**不是模型行数：过滤是"投影"，模型始终保留全部记录，
        // 过滤只决定哪些行进视图（见 r5 契约）。拿 model->rowCount() 去断言会永远看到 6。
        const auto visibleRows = [text] { return text->lineCount(); };

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
        QList<Traffic::Message> batch;
        for (quint64 i = 0; i < 3; ++i) {
            batch.append(makeMessage(i * 2 + 1, "AABB"));
            batch.append(makeMessage(i * 2 + 2, "CCDD"));
        }
        panel->appendMessages(batch);
        QTRY_COMPARE(visibleRows(), 6);

        // 打上关键词但**不按回车**：视图不动（"条件不自动回溯"的既有契约仍然成立）。
        keywords->setText(QStringLiteral("aa bb"));
        QCOMPARE(visibleRows(), 6);

        // ★ 回车 = 应用：只有命中的 3 条留下。
        QTest::keyClick(keywords, Qt::Key_Return);
        QTRY_COMPARE(visibleRows(), 3);
        QCOMPARE(apply->text(), QStringLiteral("应用过滤")); // 应用后待应用记号消失

        // ★ 排除框同样支持回车，且排除优先于关键词：两个条件都命中同一批 → 全部隐藏
        //（全隐藏时视图仍是保底的一个空块，所以是 1 而不是 0）。
        excludes->setText(QStringLiteral("aa bb"));
        QCOMPARE(visibleRows(), 3); // 未回车前不动
        QTest::keyClick(excludes, Qt::Key_Return);
        QTRY_COMPARE(visibleRows(), 1);

        // 按钮禁用时（清空后无记录）回车也必须无效——两条入口行为一致，不能一条绕开。
        keywords->clear();
        excludes->clear();
        window.findChild<QPushButton *>(QStringLiteral("clearTraffic"))->click();
        QTRY_COMPARE(model->rowCount(), 0);
        QTRY_VERIFY(!apply->isEnabled());
        keywords->setText(QStringLiteral("aa"));
        QTest::keyClick(keywords, Qt::Key_Return);
        QTest::qWait(30); // 给可能被误触发的重放留出窗口
        QCOMPARE(visibleRows(), 1); // 仍是保底空块，没有被"凭空"重放出内容
    }

    // 窗口几何与「扩展」按钮状态都要记忆（用户钦定）：退出时落盘，下次启动恢复。
    // 两者必须**自洽**——上次是"面板收起 + 窄窗口"，重开就不能是"面板收起 + 宽窗口"。
    void windowGeometryAndPanelStateAreRemembered()
    {
        int savedWidth = 0;
        int savedHeight = 0;
        {
            MainWindow window;
            window.setAttribute(Qt::WA_DontShowOnScreen);
            window.resize(1320, 800);
            window.show();
            QTest::qWait(30);
            auto *toggle = window.findChild<QPushButton *>(QStringLiteral("commandPanelToggle"));
            QVERIFY(toggle);
            toggle->setChecked(false); // 收起指令面板（会顺带把窗口就地收窄）
            QTest::qWait(50);
            savedWidth = window.width();
            savedHeight = window.height();
            QVERIFY(savedWidth > 0 && savedHeight > 0);
            auto *pin = window.findChild<QToolButton *>(QStringLiteral("stayOnTop"));
            QVERIFY(pin);
            QVERIFY(pin->text().isEmpty());
            QVERIFY(!pin->icon().isNull());
            QCOMPARE(pin->toolButtonStyle(), Qt::ToolButtonIconOnly);
            QVERIFY(!pin->isChecked());
            QVERIFY(!window.windowFlags().testFlag(Qt::WindowStaysOnTopHint));
            const int widthBeforePin = window.width();
            const int heightBeforePin = window.height();
            pin->click();
            QVERIFY(pin->isChecked());
            QVERIFY(window.windowFlags().testFlag(Qt::WindowStaysOnTopHint));
            // 置顶会重建原生窗口，大小必须留在原地。
            QCOMPARE(window.width(), widthBeforePin);
            QCOMPARE(window.height(), heightBeforePin);
            window.close(); // 触发 closeEvent → 几何落盘
            QTest::qWait(30);
        }

        // 新建窗口：应当恢复上次的几何，并保持"收起"这个状态。
        MainWindow again;
        again.setAttribute(Qt::WA_DontShowOnScreen);
        again.show();
        QTest::qWait(50);
        auto *toggle = again.findChild<QPushButton *>(QStringLiteral("commandPanelToggle"));
        auto *panel = again.findChild<CommandPanel *>();
        QVERIFY(toggle && panel);
        QVERIFY2(!panel->isExpanded(),
            qPrintable(QStringLiteral("上次是收起状态，重开应仍是收起；实际 expanded=")
                + QString::number(panel->isExpanded())));
        QVERIFY(!toggle->isChecked());
        auto *pinAgain = again.findChild<QToolButton *>(QStringLiteral("stayOnTop"));
        QVERIFY(pinAgain && pinAgain->isChecked());
        QVERIFY(again.windowFlags().testFlag(Qt::WindowStaysOnTopHint));
        // 默认尺寸是 1280×860，且不再有 900×720 的下限；恢复后必须贴近"上次"，
        // 而不是退回默认——这是本用例要守的东西。
        //
        // 容差不一致是**故意的**：宽度逐像素（实测只差 2px），高度给 ±32。
        // 原因是离屏窗口（WA_DontShowOnScreen）没有真实标题栏：saveGeometry 保存时框架高度
        // 算作 0，restoreGeometry 恢复时却按"有标题栏"折算，于是客户区差出约 26px
        //（实测 800 → 774，正是标题栏那一段）。这是测试环境的固有换算差异，不是功能缺陷
        // ——同一份几何用 saveGeometry 再存一遍，只有 1px 出入（1222/798 vs 1223/799）。
        // 默认高度 860 与恢复后的 774 差 86，大于 ±32，这个容差不会把"根本没恢复"放过去。
        QVERIFY2(qAbs(again.width() - savedWidth) <= 4,
            qPrintable(QStringLiteral("窗口宽度应恢复到上次：实际 %1，上次 %2")
                           .arg(again.width()).arg(savedWidth)));
        QVERIFY2(qAbs(again.height() - savedHeight) <= 32,
            qPrintable(QStringLiteral("窗口高度应恢复到上次：实际 %1，上次 %2")
                           .arg(again.height()).arg(savedHeight)));
    }

    // 三条用户反馈的契约：
    //   ① 定时发送的间隔框**始终可输入**（不必先勾选），且**只收数字**；
    //   ② 「清除过滤」按钮**带文案**（不再是个只有图标的按钮）；
    //   ③ 「清除过滤」只**停用过滤**、不丢用户敲的条件 —— 输入框内容保留并被持久化，
    //      下次启动还在（用户反馈"关键词过滤与排除是否可以记忆……又得重新输入，很麻烦"）。
    void filterResetKeepsConditionsAndTimerAcceptsInput()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);

        // ---------- ① 定时间隔框 ----------
        auto *repeat = window.findChild<QCheckBox *>(QStringLiteral("repeatSending"));
        auto *interval = window.findChild<QSpinBox *>(QStringLiteral("repeatInterval"));
        QVERIFY(repeat && interval);
        QVERIFY2(!repeat->isChecked(), "默认不勾选定时发送");
        QVERIFY2(interval->isEnabled(),
            "未勾选定时发送时间隔框也必须可输入（用户钦定：方便先设好再勾）");
        // 只收数字：它是 QSpinBox，非数字键会被自带校验器挡掉。
        // 单位是框内后缀，不进入数值。（键盘事件直接发给 spinbox
        // —— 它内部的 lineEdit 是 protected，测试拿不到。）
        QCOMPARE(interval->suffix(), QStringLiteral(" ms"));
        interval->clear();
        QTest::keyClicks(interval, QStringLiteral("abc"));
        QString numeric = interval->text();
        if (numeric.endsWith(interval->suffix()))
            numeric.chop(interval->suffix().size());
        numeric = numeric.trimmed();
        for (const QChar &ch : numeric) {
            QVERIFY2(ch.isDigit(),
                qPrintable(QStringLiteral("间隔框数值只应含数字，实际是「%1」").arg(interval->text())));
        }
        interval->clear();
        QTest::keyClicks(interval, QStringLiteral("250"));
        QVERIFY2(interval->value() == 250,
            qPrintable(QStringLiteral("数字应能输入进去：value=%1 text=%2")
                           .arg(interval->value()).arg(interval->text())));
        // 勾上之后**不**改成禁用（这正是用户抱怨的那一点）。
        repeat->setChecked(true);
        QVERIFY2(interval->isEnabled(), "勾选定时发送后间隔框不应被禁用");
        repeat->setChecked(false);

        // ---------- ② 清除按钮带文案 ----------
        auto *reset = window.findChild<QPushButton *>(QStringLiteral("filterReset"));
        QVERIFY(reset);
        QVERIFY2(!reset->text().isEmpty(), "「清除过滤」按钮必须有文案（用户要求）");

        // ---------- ③ 清除过滤只停用、不丢条件 ----------
        auto *panel = window.findChild<TrafficPanel *>();
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        auto *keywords = window.findChild<QLineEdit *>(QStringLiteral("filterKeywords"));
        auto *excludes = window.findChild<QLineEdit *>(QStringLiteral("filterExcludes"));
        auto *apply = window.findChild<QPushButton *>(QStringLiteral("filterRebuild"));
        QVERIFY(panel && text && keywords && excludes && apply);
        panel->setPreferences(nullptr); // 不碰共享 ini

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
        QList<Traffic::Message> batch;
        for (quint64 i = 0; i < 3; ++i) {
            batch.append(makeMessage(i * 2 + 1, "AABB"));
            batch.append(makeMessage(i * 2 + 2, "CCDD"));
        }
        panel->appendMessages(batch);
        QTRY_COMPARE(text->lineCount(), 6);

        keywords->setText(QStringLiteral("aa bb"));
        apply->click(); // 应用过滤 → 只剩命中的 3 条
        QTRY_COMPARE(text->lineCount(), 3);

        reset->click(); // 清除过滤
        QTRY_COMPARE(text->lineCount(), 6); // 过滤停用、显示全部
        // ★ 条件**原样保留**：用户不用重新敲。
        QCOMPARE(keywords->text(), QStringLiteral("aa bb"));
        // 而且没被丢掉——再点「应用过滤」立刻回到 3 条（若被清空，这里会是 6）。
        apply->click();
        QTRY_COMPARE(text->lineCount(), 3);
    }

    // 状态栏里所有文字必须**同一个字号**（用户反馈"底部状态栏文字大小似乎没有统一"）：
    // 原来只有右侧提示行（StatusNote）写了 12px、其余标签（RX/TX/条数/桥接）继承默认字号，
    // 并排看就是大小不一。这里直接量每个标签的 font().pixelSize()——QSS 用 px 写 font-size 时
    // 它就等于那个值；若某个标签没有明确字号，会是与 12px 不同的值。
    void statusBarTextSizeIsUniform()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);

        auto *bar = window.findChild<QStatusBar *>();
        QVERIFY(bar);
        const auto labels = bar->findChildren<QLabel *>();
        QVERIFY2(labels.size() >= 4,
            qPrintable(QStringLiteral("状态栏应有多个标签，实际 ") + QString::number(labels.size())));

        int expected = -1;
        for (auto *label : labels) {
            const int px = label->font().pixelSize();
            if (expected < 0) expected = px;
            QVERIFY2(px == expected,
                qPrintable(QStringLiteral("状态栏字号不一致：%1 是 %2px，其余是 %3px（应为同一条 QSS 规则）")
                               .arg(label->objectName()).arg(px).arg(expected)));
        }
        // 字族也要一致：RX/TX 原来硬写了 `font-family: "Segoe UI"`，换系统字体或切换语言时
        // 会与旁边标签不一路，同样是"看着没统一"的来源。
        const QString family = labels.first()->font().family();
        for (auto *label : labels) {
            QVERIFY2(label->font().family() == family,
                qPrintable(QStringLiteral("状态栏字族不一致：%1 是 %2，其余是 %3")
                               .arg(label->objectName(), label->font().family(), family)));
        }
    }

    // HEX / 字节数据的显示字体必须统一（用户反馈"hex 数据显示的字体似乎没有统一"）：
    // 全局 QSS 给的是比例字体（Microsoft YaHei UI），而监视区、发送区正文各自设了等宽；
    // **发送区预览行漏设**，于是同一串字节在两处写法不同、宽窄也对不齐。
    // 这里锁住"字节显示一律等宽"：同一面板内三处必须完全一致，跨面板必须同族。
    void hexDisplayFontIsUniform()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);

        auto *editor = window.findChild<QPlainTextEdit *>(QStringLiteral("sendEditor"));
        auto *preview = window.findChild<QLabel *>(QStringLiteral("payloadPreview"));
        auto *trafficText = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        QVERIFY(editor && preview && trafficText);
        // 字节数与预览**合成一个标签**之后，"ByteCount" 这个控件不该再存在
        //（曾是两个控件，底边在分数 DPR 下会错开 1 物理像素）。
        QVERIFY2(!window.findChild<QLabel *>(QStringLiteral("ByteCount")),
            "字节数应与预览合并在同一个标签里，不该再有独立的 ByteCount 控件");

        // 四个控件（发送区正文 / 字节数 / 预览、监视区）的字体必须**完全一致**：
        // 字族、像素大小、字重三者都比。
        // ★ 不能只比 family：全局 QSS 会把所有控件的 family 都设成同一个值，那样"同族"是
        //   恒真的、什么也守不住。真正的差异（也是用户看到的）在**字重**——字节数曾是
        //   DemiBold，与同一行的 HEX 挨着显示就像两种字体。
        const QFont ref = editor->font();
        for (auto *w : {static_cast<QWidget *>(editor), static_cast<QWidget *>(preview),
                 static_cast<QWidget *>(trafficText)}) {
            QVERIFY2(w->font().family() == ref.family(),
                qPrintable(QStringLiteral("字体族不一致：%1 是 %2，基准是 %3")
                               .arg(w->objectName(), w->font().family(), ref.family())));
            QCOMPARE(w->font().pixelSize(), ref.pixelSize());
            QVERIFY2(w->font().weight() == ref.weight(),
                qPrintable(QStringLiteral("字重不一致：%1 是 %2，基准是 %3")
                               .arg(w->objectName())
                               .arg(int(w->font().weight()))
                               .arg(int(ref.weight()))));
        }
        // 而且这些控件确实**用上了字体**：pixelSize 有效（不是 -1 那种"没设"的中间态）。
        QVERIFY2(ref.pixelSize() > 0,
            qPrintable(QStringLiteral("字节显示控件的字号应当有确定值，实际 pixelSize=%1")
                           .arg(ref.pixelSize())));

        // 字节数（标签文本里 span 内那一段）必须是**纯 ASCII、且不加空格**（形如 "25B"）：
        //   · ASCII —— 汉字的字面框天生比数字高（实测「字节」段 16px、纯 HEX 段 12px），
        //     与同一行的 HEX 永远对不齐；
        //   · 无空格 —— "25" 只有两个字符，后面再跟空格会让 B 显得孤零零飘在外面。
        const QString html = preview->text();
        const int s0 = html.indexOf(QLatin1Char('>')) + 1;
        const int s1 = html.indexOf(QStringLiteral("</span>"));
        QVERIFY2(s0 > 0 && s1 > s0,
            qPrintable(QStringLiteral("预览标签应含字节数 span：") + html));
        const QString countText = html.mid(s0, s1 - s0);
        QVERIFY2(!countText.isEmpty(), qPrintable(QStringLiteral("字节数不应为空：") + html));
        for (const QChar &ch : countText) {
            QVERIFY2(ch.unicode() < 128,
                qPrintable(QStringLiteral("字节数应为纯 ASCII，实际是「%1」").arg(countText)));
        }
        QVERIFY2(!countText.contains(QLatin1Char(' ')),
            qPrintable(QStringLiteral("字节数不应含空格（B 要紧贴数字）：「%1」").arg(countText)));
    }

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
        auto *threshold = selector->findChild<QSpinBox *>(QStringLiteral("framingThreshold"));
        auto *keepBoundary = selector->findChild<QCheckBox *>(QStringLiteral("framingKeepBoundary"));
        auto *logHints = selector->findChild<QCheckBox *>(QStringLiteral("framingLogHints"));
        QVERIFY(mode && delimiter && idle && threshold && keepBoundary && logHints);
        QCOMPARE(threshold->minimum(), 16);
        QCOMPARE(threshold->maximum(), 65536);
        QCOMPARE(threshold->value(), 2048);
        QCOMPARE(selector->draft().framing.bufferThresholdBytes, 2048);
        QCOMPARE(selector->findChild<QLabel *>(QStringLiteral("framingThresholdUnit"))->text(),
                 QStringLiteral("字节"));
        // 组帧设置收成一行：开关、判定、兜底参数共用同一个父控件。
        auto *framingControls = selector->findChild<QWidget *>(QStringLiteral("framingControls"));
        QVERIFY(framingControls);
        QCOMPARE(mode->parentWidget(), framingControls);
        QCOMPARE(idle->parentWidget(), framingControls);
        QCOMPARE(threshold->parentWidget(), framingControls);
        QCOMPARE(logHints->parentWidget(), framingControls);
        QCOMPARE(keepBoundary->parentWidget(), framingControls);
        // 标签与输入框同一字号。Muted 是 11px，组帧行不能再用它。
        QFile framingStyle(QStringLiteral(":/styles/workbench.qss"));
        QVERIFY(framingStyle.open(QIODevice::ReadOnly));
        const QString previousFramingStyle = qApp->styleSheet();
        qApp->setStyleSheet(QString::fromUtf8(framingStyle.readAll()));
        const auto framingLabels = selector->findChild<QWidget *>(QStringLiteral("framingPanel"))
                                       ->findChildren<QLabel *>();
        QVERIFY(!framingLabels.isEmpty());
        for (auto *widget : selector->findChildren<QWidget *>())
            widget->ensurePolished();
        const int framingPx = idle->font().pixelSize();
        QVERIFY(framingPx > 0);
        for (auto *framingLabel : framingLabels) {
            QVERIFY2(framingLabel->font().pixelSize() == framingPx,
                qPrintable(QStringLiteral("%1 是 %2px，输入框是 %3px")
                               .arg(framingLabel->objectName())
                               .arg(framingLabel->font().pixelSize())
                               .arg(framingPx)));
        }
        qApp->setStyleSheet(previousFramingStyle);
        auto *enabled = selector->findChild<QCheckBox *>(QStringLiteral("framingEnabled"));
        QVERIFY(enabled);
        QVERIFY(mode->findData(int(ReceiveFraming::IdleTimeout)) >= 0);
        QCOMPARE(idle->value(), 10);
        mode->setCurrentIndex(mode->findData(int(ReceiveFraming::IdleTimeout)));
        QTest::qWait(20);
        QVERIFY(enabled->isChecked());
        QVERIFY(idle->isEnabled());
        QVERIFY(threshold->isEnabled());
        QVERIFY(!keepBoundary->isVisible());
        QVERIFY(selector->hasValidConfiguration());
        const auto valid = [selector](const QString &stage) {
            QVERIFY2(selector->hasValidConfiguration(),
                qPrintable(stage + QStringLiteral("：") + selector->validationError()));
        };
        // 空闲超时模式不需要结束符，输入因此不出现。
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
        const auto artifactDir = qEnvironmentVariable("QQCOM_UI_ARTIFACT_DIR");
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
    app.setOrganizationName("QQComTests");
    app.setApplicationName("QQComUiTests");
    QStandardPaths::setTestModeEnabled(true);
    UiSmokeTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_ui.moc"
