// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "api_client.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QMainWindow>

class QCheckBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTabWidget;
class QTimer;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    void connectTo(const QString& url, const QString& key);

private:
    QWidget* buildOverviewTab();
    QWidget* buildPlayersTab();
    QWidget* buildLeaderboardsTab();
    QWidget* buildTrafficTab();

    void connectToServer();
    void refresh();
    void refreshTraffic();
    void showStatus(const QJsonObject& status);
    void showSessions(const QJsonArray& sessions);
    void showTokens(const QJsonArray& tokens);
    void showBoards(const QJsonArray& boards);
    void loadBoardEntries();
    void showEntries(const QJsonArray& entries);
    void appendTraffic(const QJsonArray& requests);

    void kickSession();
    void revokeToken();
    void addEntries();
    void removeEntry();
    void clearBoard();

    void updateButtons();
    void setStatus(const QString& text, bool error = false);
    void reportError(const QString& action, const ApiClient::Reply& reply);
    QString selectedBoard() const;

    ApiClient api_;
    QJsonArray sessions_;
    QJsonArray tokens_;
    QJsonArray boards_;
    QJsonArray entries_;
    double lastRequestId_ = 0;
    bool connected_ = false;

    QLineEdit* url_;
    QLineEdit* key_;
    QPushButton* connect_;
    QPushButton* refresh_;
    QCheckBox* autoRefresh_;
    QTimer* timer_;
    QTimer* trafficTimer_;
    QTabWidget* tabs_;

    QFormLayout* overviewForm_;
    QLabel* server_;

    QTableWidget* sessionTable_;
    QTableWidget* tokenTable_;
    QPushButton* kick_;
    QPushButton* revoke_;

    QTableWidget* boardTable_;
    QTableWidget* entryTable_;
    QLabel* boardTitle_;
    QPushButton* addEntry_;
    QPushButton* removeEntry_;
    QPushButton* clearBoard_;

    QTableWidget* trafficTable_;
    QCheckBox* liveTraffic_;
    QCheckBox* errorsOnly_;
};
