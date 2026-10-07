QT = core
CONFIG += console c++17 release
CONFIG -= app_bundle debug
TARGET = QQCom_framer_perf
msvc: QMAKE_CXXFLAGS += /utf-8
# 可指定审查前源码目录，使用同一探针测量两个实现。
isEmpty(FRAMER_SOURCE_ROOT): FRAMER_SOURCE_ROOT = $$PWD/../../src
INCLUDEPATH += $$FRAMER_SOURCE_ROOT
SOURCES += receive_framer_perf.cpp $$FRAMER_SOURCE_ROOT/app/receiveframer.cpp
HEADERS += $$FRAMER_SOURCE_ROOT/app/receiveframer.h $$FRAMER_SOURCE_ROOT/domain/receiveframing.h
