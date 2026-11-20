#include "mainwindow.h"
#include "version/version.h"
#include "domain/networkaddress.h"
#include "update/updateservice.h"
#include "automation/clirunner.h"

#include "ui/common/apptheme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QIcon>
#include <QSettings>
#include <QTimer>
#include <QTextStream>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace {

// 解析 --auto-open 的端点描述（压测/QA 专用，test-only）：
//   tcp-server:<port> | udp:<port> | tcp-client:<host>:<port> | serial:<name>
// 返回是否解析成功；端口越界/主机非法/空串口名都算失败，由调用方 qWarning 后忽略。
// 解析即按 UI 同源校验（isValidNetworkAddress / 端口范围），保证落进选择控件后
// hasValidConfiguration() 必过——否则 toggleEndpoint 会静默跳过打开，连错误都不弹。
bool parseAutoOpenEndpoint(const QString &spec, PortDraft *out)
{
    if (!out) return false;
    const int colon = spec.indexOf(QLatin1Char(':'));
    if (colon <= 0) return false;
    const QString scheme = spec.left(colon);
    const QString rest = spec.mid(colon + 1);
    const auto parsePort = [](const QString &text, int minPort, int maxPort, int *port) {
        bool ok = false;
        const int value = text.toInt(&ok);
        if (!ok || value < minPort || value > maxPort) return false;
        *port = value;
        return true;
    };
    PortDraft draft;
    if (scheme == QLatin1String("tcp-server")) {
        int port = 0;
        if (!parsePort(rest, 0, 65535, &port)) return false; // 0 = 内核选端口，与 UI 同语义
        draft.kind = PortDraft::TcpServer;
        draft.values.insert(QStringLiteral("server.port"), port);
    } else if (scheme == QLatin1String("udp")) {
        int port = 0;
        if (!parsePort(rest, 0, 65535, &port)) return false;
        draft.kind = PortDraft::Udp;
        draft.values.insert(QStringLiteral("udp.port"), port);
        draft.values.insert(QStringLiteral("udp.target"), QString()); // 仅接收：目标留空
    } else if (scheme == QLatin1String("tcp-client")) {
        const int hostColon = rest.lastIndexOf(QLatin1Char(':'));
        if (hostColon <= 0) return false;
        const QString host = rest.left(hostColon);
        int port = 0;
        if (host.isEmpty() || !isValidNetworkAddress(host, true)
            || !parsePort(rest.mid(hostColon + 1), 1, 65535, &port))
            return false;
        draft.kind = PortDraft::TcpClient;
        draft.values.insert(QStringLiteral("client.host"), host);
        draft.values.insert(QStringLiteral("client.port"), port);
    } else if (scheme == QLatin1String("serial")) {
        if (rest.isEmpty()) return false;
        draft.kind = PortDraft::Serial;
        draft.values.insert(QStringLiteral("serial.port"), rest);
    } else {
        return false;
    }
    *out = draft;
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    // Native CLI uses QCoreApplication: no window, theme, font or platform plugin
    // initialization is needed to discover capabilities or call a running window.
    if (argc > 1 && QByteArray(argv[1]) == "--cli") {
        QCoreApplication cli(argc, argv);
        cli.setApplicationName("QQCom-plus");
        cli.setOrganizationName("QQCom-plus");
        cli.setApplicationVersion(QStringLiteral(QQCOM_VERSION_STR));
        return Cli::run(cli.arguments().mid(2), QStringLiteral(QQCOM_VERSION_STR));
    }
    QApplication a(argc, argv);
    a.setApplicationName("QQCom-plus");
    a.setOrganizationName("QQCom-plus");
    a.setApplicationVersion(QStringLiteral(QQCOM_VERSION_STR));
    a.setStyle("Fusion");
    // 样式表写死了颜色，必须按明暗换整份。用户选过亮色或暗色时先钉住，再加载对应样式表，
    // 避免先按系统画一遍再跳过去。没选过则跟随系统。
    {
        QSettings appearance(QSettings::IniFormat, QSettings::UserScope,
            QStringLiteral("QQCom-plus"), QStringLiteral("QQCom-plus"));
        const QString saved = appearance.value(QStringLiteral("window/colorScheme")).toString();
        if (saved == QLatin1String("light") || saved == QLatin1String("dark"))
            AppTheme::setMode(AppTheme::modeFromKey(saved));
    }
    AppTheme::install(&a);
    // 程序主图标（用户钦定 ActionResponse.ico）：exe 内嵌资源（RC_ICONS）管资源管理器，
    // 这行管运行中的窗口与任务栏——两处必须指向同一个图标，否则对不上。
    a.setWindowIcon(QIcon(":/icons/app.ico"));

    QCommandLineParser parser;
    parser.setApplicationDescription("QQCom-plus communication workbench");
    parser.addHelpOption();
    parser.addOption({"cli", "Native JSON command interface (must be the first option); use --cli --help."});
    parser.addOption({"sample", "Show clearly labelled example traffic."});
    parser.addOption({"routes", "Show the forwarding rule editor."});
    parser.addOption({"snapshot", "Render the window to a PNG and exit.", "path"});
    parser.addOption({"compact", "Preview the layout at 1280 x 720."});
    parser.addOption({"latency-probe",
        "Sample GUI-thread event-loop latency and append aggregates to <path> "
        "(stress-test tooling; never enabled without this flag).", "path"});
    parser.addOption({"auto-open",
        "Open endpoint A from the command line (test-only). Syntax: "
        "tcp-server:<port> | udp:<port> | tcp-client:<host>:<port> | serial:<name>.", "endpoint"});
    parser.addOption({"local-update-source",
        "Use a loopback HTTP update fixture for this process only (test-only). "
        "Syntax: http://127.0.0.1:<port>", "origin"});
    parser.addOption({"local-update-current-version",
        "Compare updates against this version instead of the built-in version (test-only).",
        "version"});
    parser.addOption({"accept-update",
        "Download an available update without asking (test-only)."});
    parser.process(a);
    if (parser.isSet("cli")) {
        QTextStream(stderr) << "--cli must be the first option; use QQCom-plus.exe --cli --help\n";
        return 2;
    }
    MainWindow w;
    if (parser.isSet("snapshot")) w.setAttribute(Qt::WA_DontShowOnScreen);
    if (parser.isSet("compact")) w.resize(1280, 720);
    if (parser.isSet("sample")) w.loadExample();
    if (parser.isSet("routes")) w.showRoutes();
    w.show();

    // --auto-open（test-only）：构造并 show 之后、偏好恢复之后，把端点 A 的草稿换成
    // 命令行给的配置并触发打开。解析失败只 qWarning 后忽略，绝不阻断启动；不传此参
    // 数零行为（分支整体跳过）。打开走 toggleEndpoint 同款路径，错误照常弹窗/进 Error 行。
    if (parser.isSet("auto-open")) {
        PortDraft draft;
        if (parseAutoOpenEndpoint(parser.value("auto-open"), &draft)) {
            w.autoOpenEndpoint(draft);
        } else {
            qWarning() << "auto-open: 无法解析端点描述" << parser.value("auto-open")
                       << "——忽略并继续启动";
        }
    }

#ifdef Q_OS_WIN
    // 自动升级只在正常可见运行时触发；快照和延迟探针不能访问网络或弹窗。
    // QQCOM_DISABLE_UPDATE=1 只关掉启动后的自动检查，状态栏「检查更新」仍然可用。
    // --local-update-source 只服务本次进程的回环自测，不改正式清单地址，也不写配置。
    if (!parser.isSet("snapshot") && !parser.isSet("latency-probe")) {
        const QString simulatedVersion = parser.value("local-update-current-version");
        const QString currentVersion = simulatedVersion.isEmpty()
                                           ? QStringLiteral(QQCOM_VERSION_STR)
                                           : simulatedVersion;
        auto *updates = new UpdateService(currentVersion, &w);
        QUrl manifestUrl(QStringLiteral(
            "https://api.github.com/repos/hyb-oyqq/QQCom-plus/contents/ota/stable.json?ref=master"));
        const QString localSource = parser.value("local-update-source");
        if (!localSource.isEmpty()) {
            const QUrl origin(localSource);
            setLoopbackUpdateOrigin(origin);
            if (!loopbackUpdateOriginActive()) {
                qWarning() << "local-update-source: 只接受 http://127.0.0.1:<端口>，已忽略";
            } else {
                manifestUrl = QUrl(origin.toString(QUrl::RemovePath | QUrl::RemoveQuery)
                                   + QStringLiteral("/stable.json"));
            }
        } else {
            const QString overrideUrl = qEnvironmentVariable("QQCOM_OTA_MANIFEST_URL");
            if (!overrideUrl.isEmpty()) manifestUrl = QUrl(overrideUrl);
        }
        if (parser.isSet("accept-update")) updates->setAcceptUpdatesWithoutPrompt(true);
        QObject::connect(&w, &MainWindow::updateCheckRequested, updates, [updates, manifestUrl] {
            updates->checkForUpdates(manifestUrl, UpdateService::CheckMode::Manual);
        });
        QObject::connect(updates, &UpdateService::advisory, &w,
                         [&w](const QString &text, int ms) { w.showStatusNote(text, ms); });
        if (qEnvironmentVariableIsEmpty("QQCOM_DISABLE_UPDATE") || !localSource.isEmpty()) {
            const int delayMs = localSource.isEmpty() ? 3000 : 400;
            QTimer::singleShot(delayMs, &w, [updates, manifestUrl] {
                updates->checkForUpdates(manifestUrl, UpdateService::CheckMode::Automatic);
            });
        }
    }
#endif

    // 事件循环延迟探针（压测专用，用户钦定"先实测出上限，不做常驻性能提示"）：
    // 20ms 周期记"这一拍比理论时刻晚了多少"（µs，自校正：以上一拍实际时刻为基准，
    // 不累积漂移），每 500ms（25 拍）聚合一行追加到文件，供 QA 量化卡顿。
    // 行尾再带 <总条数> <RX字节>：压测脚本的 sent_packets/sent_bytes 与这两个数直接
    // 对账，免去截图读状态栏（透传组帧下条数≠包数，对账以 RX 字节为准）。
    // 探针自身必须便宜：每 20ms 一次 O(1) 记账 + 每 500ms 一次小写入；
    // 文件打不开就 qWarning 后忽略（探针失败不阻断启动）；不启用时一个定时器都不开。
    if (parser.isSet("latency-probe")) {
        constexpr int ProbeTickMs = 20;
        constexpr int ProbeWindowTicks = 25; // 25 × 20ms = 500ms 聚合一行
        struct ProbeState {
            QElapsedTimer clock;      // nsecsElapsed 才有亚毫秒精度
            QFile file;
            std::vector<qint64> samplesUs; // 每个采样的偏差（µs）
        };
        auto probe = std::make_shared<ProbeState>();
        probe->file.setFileName(parser.value("latency-probe"));
        if (!probe->file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            qWarning() << "latency-probe: 无法打开输出文件" << parser.value("latency-probe")
                       << probe->file.errorString() << "——忽略探针，正常启动";
        } else {
            probe->clock.start();
            auto *tick = new QTimer(&w);
            tick->setTimerType(Qt::PreciseTimer); // 压测要亚 20ms 的时间分辨率
            tick->setInterval(ProbeTickMs);
            QObject::connect(tick, &QTimer::timeout, tick, [probe, &w] {
                const qint64 ns = probe->clock.nsecsElapsed();
                probe->clock.restart();
                // 这一拍的偏差 = 实际间隔 − 理论周期。事件循环被长任务占住时定时器
                // 迟到，迟到量就是这一拍感知到的卡顿；负值（提前触发）夹到 0。
                probe->samplesUs.push_back(
                    std::max<qint64>(0, ns / 1000 - ProbeTickMs * 1000LL));
                if (int(probe->samplesUs.size()) < ProbeWindowTicks) return;
                // 聚合写一行：25 个样本排序取分位（代价可忽略），毫秒一位小数。
                auto samples = probe->samplesUs;
                std::sort(samples.begin(), samples.end());
                const auto percentile = [&samples](int p) {
                    const int at = std::max(0, int(std::ceil(p / 100.0 * samples.size())) - 1);
                    return samples.at(std::min<int>(at, int(samples.size()) - 1));
                };
                const auto ms1 = [](qint64 us) {
                    return QString::number(us / 1000.0, 'f', 1);
                };
                const QString line = QStringLiteral("%1 %2 %3 %4 %5 %6 %7 %8\n")
                                         .arg(QDateTime::currentMSecsSinceEpoch())
                                         .arg(samples.size())
                                         .arg(ms1(percentile(50)), ms1(percentile(95)),
                                             ms1(percentile(99)), ms1(samples.back()))
                                         .arg(w.sessionRecordCount())
                                         .arg(w.sessionRxBytes());
                probe->file.write(line.toUtf8());
                probe->file.flush(); // 行级落盘：Git Bash 里 GUI 日志不可见，只能读文件
                probe->samplesUs.clear();
            });
            tick->start();
        }
    }

    if (parser.isSet("snapshot")) {
        QTimer::singleShot(400, &w, [&a, &w, &parser] {
            a.exit(w.grab().save(parser.value("snapshot")) ? 0 : 1);
        });
    }
    return a.exec();
}
