QT += serialport concurrent

SOURCES += \
    $$PWD/domain/networkaddress.cpp \
    $$PWD/discovery/portdiscoveryservice.cpp \
    $$PWD/persistence/preferencesstore.cpp

HEADERS += \
    $$PWD/domain/networkaddress.h \
    $$PWD/domain/portdraft.h \
    $$PWD/domain/serialportdescriptor.h \
    $$PWD/discovery/portdiscoveryservice.h \
    $$PWD/persistence/preferencesstore.h
