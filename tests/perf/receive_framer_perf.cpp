#include "app/receiveframer.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTextStream>
#include <algorithm>
#include <array>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    ReceiveFraming config;
    config.mode = ReceiveFraming::Delimiter;
    config.delimiter = "\r\n";
    config.bufferThresholdBytes = 2 * 1024 * 1024;
    const QByteArray chunk(64, 'x');
    out << "scenario,bytes,median_ms\n";
    for (const int bytes : {64 * 1024, 256 * 1024, 1024 * 1024}) {
        std::array<qint64, 7> times;
        for (auto &elapsed : times) {
            ReceiveFramer framer(config);
            QElapsedTimer clock;
            clock.start();
            for (int at = 0; at < bytes; at += chunk.size())
                if (!framer.feed(chunk).isEmpty()) return 1;
            elapsed = clock.nsecsElapsed();
            if (framer.pendingBytes() != bytes) return 2;
            const auto frames = framer.feed(config.delimiter);
            if (frames.size() != 1 || frames.first().bytes.size() != bytes) return 3;
        }
        std::sort(times.begin(), times.end());
        out << "fragmented," << bytes << ',' << double(times[3]) / 1e6 << '\n';
    }
    const QByteArray batch = QByteArray("payload\r\n").repeated(20000);
    std::array<qint64, 7> times;
    for (auto &elapsed : times) {
        ReceiveFramer framer(config);
        QElapsedTimer clock;
        clock.start();
        const auto frames = framer.feed(batch);
        elapsed = clock.nsecsElapsed();
        if (frames.size() != 20000 || framer.pendingBytes() != 0) return 4;
    }
    std::sort(times.begin(), times.end());
    out << "coalesced," << batch.size() << ',' << double(times[3]) / 1e6 << '\n';
}
