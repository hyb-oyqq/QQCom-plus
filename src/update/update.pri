# OTA 客户端：读取静态清单、下载 Release zip，并交给独立 updater 覆盖安装。
INCLUDEPATH += $$PWD $$PWD/.. $$PWD/../..

SOURCES += $$PWD/updateservice.cpp

HEADERS += $$PWD/updateservice.h
