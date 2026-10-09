QT += core testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TARGET = QQCom_update_tests
msvc: QMAKE_CXXFLAGS += /utf-8
INCLUDEPATH += $$PWD/../../src/update
SOURCES += tst_update.cpp \
    ../../src/update/updatemanifest.cpp
HEADERS += ../../src/update/updatemanifest.h
