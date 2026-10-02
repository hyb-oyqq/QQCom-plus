#ifndef SERIALPORTDESCRIPTOR_H
#define SERIALPORTDESCRIPTOR_H

#include <QList>
#include <QMetaType>
#include <QString>

struct SerialPortDescriptor
{
    QString portName;
    QString description;
    QString manufacturer;
    QString systemLocation;

    // 轮询场景用于比较两次快照，列表不变时不发布。
    bool operator==(const SerialPortDescriptor &other) const
    {
        return portName == other.portName && description == other.description
            && manufacturer == other.manufacturer && systemLocation == other.systemLocation;
    }
};

Q_DECLARE_METATYPE(SerialPortDescriptor)
Q_DECLARE_METATYPE(QList<SerialPortDescriptor>)

#endif
