QT       += core gui
TARGET = QQCom

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++17
msvc: QMAKE_CXXFLAGS += /utf-8

include(version/version.pri)

win32 {
    VERSION_UPDATE_SCRIPT = $$shell_path($$PWD/scripts/update_version.ps1)
    VERSION_UPDATE_ROOT = $$shell_path($$PWD)
    !system(powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$$VERSION_UPDATE_SCRIPT" -Root "$$VERSION_UPDATE_ROOT" -Mode build) {
        error("Unable to update version metadata before build")
    }
}

include(src/ui/ui.pri)
include(src/services.pri)
include(src/update/update.pri)
RESOURCES += resources/resources.qrc

# 程序主图标：RC_ICONS 让 qmake 自动生成 .rc 并编译进 exe（资源管理器/任务栏/快捷方式用）。
RC_ICONS = resources/icons/app.ico

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    main.cpp \
    mainwindow.cpp

HEADERS += \
    mainwindow.h

FORMS += \
    mainwindow.ui

TRANSLATIONS += \
    QQCom_zh_CN.ts
CONFIG += lrelease
CONFIG += embed_translations

include(scripts/deploy_qt_windows.pri)

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
