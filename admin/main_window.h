// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "api_client.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QMainWindow>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTabWidget;
class QTimer;

// driveclub-admin: overview, players (EGS sessions and OTG tokens), leaderboards and live
// traffic of a running driveclub-server, through its admin API.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

    // Fills the connection fields and connects (used by the command line).
    void connectTo(const QString& url, const QString& key);

private:
    QWidget* buildOverviewTab();
    QWidget* buildPlayersTab();
    QWidget* buildLeaderboardsTab();
    QWidget* buildCountersTab();
    QWidget* buildGhostsTab();
    QWidget* buildClubsTab();
    QWidget* buildStatsTab();
    QWidget* buildChallengesTab();
    QWidget* buildTrafficTab();

    void connectToServer();
    void refresh();
    void refreshTraffic();
    void showStatus(const QJsonObject& status);
    void showSessions(const QJsonArray& sessions);
    void showTokens(const QJsonArray& tokens);
    void showParties(const QJsonArray& parties);
    void setPlayerTabCount(int index, const QString& name, qsizetype count);
    void addTableTools(QWidget* root);
    void exportCsv(QTableWidget* table);
    void showPlayers(const QJsonArray& players);
    void showCounters(const QJsonArray& counters);
    void showGhosts(const QJsonArray& ghosts);
    void showClubs(const QJsonArray& clubs);
    void showClubDetails();
    void deleteClub();
    void showClubReports(const QJsonArray& reports);
    void dismissClubReport();
    void makeClubOwner();
    void showGhostUses();
    void showStatsPlayers(const QJsonObject& stats);
    void loadStatsTable();
    void showChallenges(const QJsonArray& challenges);
    void loadChallengeTimes();
    void applyBoardFilter();
    QString playerName(const QString& accountId) const;
    void showBoards(const QJsonArray& boards);
    void loadBoardEntries();
    void showEntries(const QJsonArray& entries);
    void appendTraffic(const QJsonArray& requests);

    void kickSession();
    void revokeToken();
    void setFame();
    void setClubFame();
    void renameClub();
    void removeClubMember();
    void kickPartyMember();
    void disbandParty();
    void showPlayerPage();
    void showActivity(const QJsonObject& activity);
    void resetCounter();
    void featureChallenge(const QString& list);
    void unfeatureChallenge();
    void saveGhost();
    void deleteGhosts(const QStringList& ids, const QString& question);
    void deleteGhost();
    void deleteUnusedGhosts();
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
    QJsonArray players_;
    QJsonArray counters_;
    QJsonArray ghosts_;
    QJsonArray clubs_;
    QJsonArray parties_;
    QJsonArray clubReports_;
    QJsonArray statsPlayers_;
    QJsonArray challenges_;
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
    QTableWidget* partyTable_;
    QTabWidget* playerTabs_;
    QPushButton* kick_;
    QPushButton* revoke_;
    QTableWidget* playerTable_;
    QPushButton* setFame_;

    QTableWidget* challengeTable_;
    QTableWidget* challengeTimes_;
    QLabel* challengeTitle_;
    QPushButton* featureCommunity_;
    QPushButton* featureEvo_;
    QPushButton* unfeature_;
    QComboBox* boardFilter_;

    QTableWidget* counterTable_;
    QPushButton* resetCounter_;

    QTableWidget* ghostTable_;
    QTableWidget* clubTable_;
    QTableWidget* clubMemberTable_;
    QLabel* clubTitle_;
    QPushButton* deleteClub_;
    QPushButton* makeOwner_;
    QPushButton* removeClubMember_;
    QPushButton* setClubFame_;
    QPushButton* renameClub_;
    QPushButton* playerPage_;
    QPushButton* kickPartyMember_;
    QPushButton* disbandParty_;
    QLabel* activity_;
    QTableWidget* warningTable_;
    QTableWidget* clubReportTable_;
    QPushButton* dismissReport_;
    QTableWidget* ghostUses_;
    QLabel* ghostTitle_;
    QPushButton* saveGhost_;
    QPushButton* deleteGhost_;
    QPushButton* deleteUnused_;

    QTableWidget* statsPlayerTable_;
    QComboBox* statsSet_;
    QLabel* statsTitle_;
    QTableWidget* statsView_;

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
