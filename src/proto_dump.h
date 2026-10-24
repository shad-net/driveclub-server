// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QByteArray>
#include <QStringList>

// Decodes any protobuf message without a schema
bool dumpProto(const QByteArray& data, QStringList* lines, int depth = 0);
