#include "mainwindow.h"
#include "version/version.h"

#include <QApplication>
#include <QLocale>
#include <QTranslator>
#include <QCommandLineParser>
#include <QFile>
#include <QTimer>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    a.setApplicationName("easyDebug");
    a.setOrganizationName("easyDebug");
    a.setApplicationVersion(QStringLiteral(EASYDEBUG_VERSION_STR));
    a.setStyle("Fusion");
    QFile stylesheet(":/styles/workbench.qss");
    if (stylesheet.open(QIODevice::ReadOnly))
        a.setStyleSheet(QString::fromUtf8(stylesheet.readAll()));

    QCommandLineParser parser;
    parser.setApplicationDescription("easyDebug UI preview");
    parser.addHelpOption();
    parser.addOption({"sample", "Show clearly labelled example traffic."});
    parser.addOption({"routes", "Show the forwarding rule editor."});
    parser.addOption({"snapshot", "Render the window to a PNG and exit.", "path"});
    parser.addOption({"compact", "Preview the layout at 1280 x 720."});
    parser.process(a);

    QTranslator translator;
    const QStringList uiLanguages = QLocale::system().uiLanguages();
    for (const QString &locale : uiLanguages) {
        const QString baseName = "easyDebug_" + QLocale(locale).name();
        if (translator.load(":/i18n/" + baseName)) {
            a.installTranslator(&translator);
            break;
        }
    }
    MainWindow w;
    if (parser.isSet("snapshot")) w.setAttribute(Qt::WA_DontShowOnScreen);
    if (parser.isSet("compact")) w.resize(1280, 720);
    if (parser.isSet("sample")) w.loadExample();
    if (parser.isSet("routes")) w.showRoutes();
    w.show();
    if (parser.isSet("snapshot")) {
        QTimer::singleShot(400, &w, [&a, &w, &parser] {
            a.exit(w.grab().save(parser.value("snapshot")) ? 0 : 1);
        });
    }
    return a.exec();
}
