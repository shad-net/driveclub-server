// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "proto_dump.h"

#include <algorithm>
#include <cstring>

namespace {

constexpr int MaxDepth = 8;

bool readVarint(const QByteArray& data, qsizetype* pos, quint64* out) {
    quint64 value = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (*pos >= data.size()) {
            return false;
        }
        const auto b = static_cast<unsigned char>(data[(*pos)++]);
        value |= quint64(b & 0x7f) << shift;
        if (!(b & 0x80)) {
            *out = value;
            return true;
        }
    }
    return false;
}

bool printable(const QByteArray& s) {
    return std::all_of(s.begin(), s.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u >= 0x20 || c == '\n' || c == '\r' || c == '\t';
    });
}

} // namespace

bool dumpProto(const QByteArray& data, QStringList* lines, int depth) {
    const QString indent(depth * 4, ' ');
    QStringList out;
    qsizetype pos = 0;
    while (pos < data.size()) {
        quint64 key = 0;
        if (!readVarint(data, &pos, &key)) {
            return false;
        }
        const quint64 field = key >> 3;
        const int wire = static_cast<int>(key & 7);
        if (field == 0) {
            return false;
        }
        switch (wire) {
        case 0: {
            quint64 v = 0;
            if (!readVarint(data, &pos, &v)) {
                return false;
            }
            const auto s = static_cast<qint64>(v);
            out << QStringLiteral("%1%2: %3%4")
                       .arg(indent)
                       .arg(field)
                       .arg(v)
                       .arg(s < 0 ? QStringLiteral(" (%1)").arg(s) : QString());
            break;
        }
        case 1: {
            if (pos + 8 > data.size()) {
                return false;
            }
            quint64 v = 0;
            std::memcpy(&v, data.constData() + pos, 8);
            double d = 0;
            std::memcpy(&d, &v, 8);
            pos += 8;
            out << QStringLiteral("%1%2: 0x%3 (fixed64, double %4)")
                       .arg(indent)
                       .arg(field)
                       .arg(v, 16, 16, QChar('0'))
                       .arg(d);
            break;
        }
        case 2: {
            quint64 len = 0;
            if (!readVarint(data, &pos, &len) || len > quint64(data.size() - pos)) {
                return false;
            }
            const QByteArray sub = data.mid(pos, static_cast<qsizetype>(len));
            pos += static_cast<qsizetype>(len);
            QStringList nested;
            if (!sub.isEmpty() && depth < MaxDepth && !printable(sub) &&
                dumpProto(sub, &nested, depth + 1)) {
                out << QStringLiteral("%1%2 {").arg(indent).arg(field);
                out << nested;
                out << indent + '}';
            } else if (printable(sub)) {
                out << QStringLiteral("%1%2: \"%3\"")
                           .arg(indent)
                           .arg(field)
                           .arg(QString::fromUtf8(sub));
            } else {
                out << QStringLiteral("%1%2: bytes[%3] %4")
                           .arg(indent)
                           .arg(field)
                           .arg(sub.size())
                           .arg(QString::fromLatin1(sub.left(64).toHex(' ')));
            }
            break;
        }
        case 5: {
            if (pos + 4 > data.size()) {
                return false;
            }
            quint32 v = 0;
            std::memcpy(&v, data.constData() + pos, 4);
            float f = 0;
            std::memcpy(&f, &v, 4);
            pos += 4;
            out << QStringLiteral("%1%2: 0x%3 (fixed32, float %4)")
                       .arg(indent)
                       .arg(field)
                       .arg(v, 8, 16, QChar('0'))
                       .arg(f);
            break;
        }
        default:
            return false;
        }
    }
    *lines << out;
    return true;
}
