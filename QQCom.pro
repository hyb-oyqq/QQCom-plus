QT       += core gui
TARGET = QQCom

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++17
msvc: QMAKE_CXXFLAGS += /utf-8

include(version/version.pri)

# qmake 配置时只刷新 version.h 里的构建时间。版本号改头文件里的 MAJOR/MINOR/PATCH。
# 每次点「构建」刷新时间戳不能写在这里：Qt Creator 的构建步骤只有 qmake 和 Make，
# 且 qmake 只在 .pro 变化时重跑。日常构建用 version\stamp_version.bat，加到 Make 之前。
win32 {
    VERSION_UPDATE_SCRIPT = $$shell_path($$PWD/version/update_version.ps1)
    VERSION_UPDATE_ROOT = $$shell_path($$PWD)
    !system(powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$$VERSION_UPDATE_SCRIPT" -Root "$$VERSION_UPDATE_ROOT") {
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
    mainwindow.cpp \
    src/ui/automation/windowautomation.cpp

HEADERS += \
    mainwindow.h

FORMS += \
    mainwindow.ui

include(scripts/deploy_qt_windows.pri)

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
