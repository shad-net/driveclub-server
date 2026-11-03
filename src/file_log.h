// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QList>
#include <QString>

bool startFileLog(const QString& path, qint64 maxBytes, QString* error);

// Keeps the last warnings and errors logged (any category) for the admin overview. Call once at
// startup, before startFileLog.
struct LoggedWarning {
    qint64 time = 0; // ms since epoch
    QString category;
    QString message;
};
void startWarningCapture();
QList<LoggedWarning> recentWarnings(); // oldest first, at most 50
