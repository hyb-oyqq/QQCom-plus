# OTA 客户端：读取静态清单、下载 Release zip，并交给独立 updater 覆盖安装。
QT += network widgets
INCLUDEPATH += $$PWD $$PWD/.. $$PWD/../..

SOURCES += \
    $$PWD/updatemanifest.cpp \
    $$PWD/updateservice.cpp

HEADERS += \
    $$PWD/updatemanifest.h \
    $$PWD/updateservice.h
