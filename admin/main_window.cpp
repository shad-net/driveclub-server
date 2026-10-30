// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "main_window.h"

#include "version.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimeZone>
#include <QTimer>
#include <QToolBar>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>
#include <memory>

namespace {

constexpr int MaxTrafficRows = 2000;

QString when(double ms) {
    if (ms <= 0) {
        return QStringLiteral("-");
    }
    return QDateTime::fromMSecsSinceEpoch(qint64(ms))
        .toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"));
}

QString ago(double ms) {
    if (ms <= 0) {
        return QStringLiteral("-");
    }
    const qint64 secs = (QDateTime::currentMSecsSinceEpoch() - qint64(ms)) / 1000;
    if (secs < 60) {
        return QObject::tr("%1 s ago").arg(secs);
    }
    if (secs < 3600) {
        return QObject::tr("%1 min ago").arg(secs / 60);
    }
    return QObject::tr("%1 h ago").arg(secs / 3600);
}

QString duration(qint64 secs) {
    return QStringLiteral("%1:%2:%3")
        .arg(secs / 3600)
        .arg((secs / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(secs % 60, 2, 10, QLatin1Char('0'));
}

QTableWidget* makeTable(const QStringList& headers) {
    auto* t = new QTableWidget(0, int(headers.size()));
    t->setHorizontalHeaderLabels(headers);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setSelectionMode(QAbstractItemView::SingleSelection);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setAlternatingRowColors(true);
    t->verticalHeader()->hide();
    t->horizontalHeader()->setStretchLastSection(true);
    t->horizontalHeader()->setSortIndicator(0, Qt::AscendingOrder);
    t->setSortingEnabled(true);
    return t;
}

QTableWidgetItem* cell(const QString& text, int index = -1) {
    auto* item = new QTableWidgetItem(text);
    if (index >= 0) {
        item->setData(Qt::UserRole, index);
    }
    return item;
}

QString byteSize(double bytes) {
    if (bytes < 1024 * 1024) {
        return bytes < 1024 ? QObject::tr("%1 B").arg(qint64(bytes))
                            : QObject::tr("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    }
    return QObject::tr("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
}

QTableWidgetItem* numberCell(double value) {
    auto* item = new QTableWidgetItem;
    item->setData(Qt::DisplayRole, qint64(value));
    return item;
}

int selectedIndex(const QTableWidget* table) {
    const auto rows = table->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        return -1;
    }
    return table->item(rows.first().row(), 0)->data(Qt::UserRole).toInt();
}

QString yesNo(bool v) {
    return v ? QObject::tr("yes") : QObject::tr("no");
}

constexpr quint64 TimeMask = 0x0FFFFFFFFFFFFFFFull;

QString formatMs(quint64 ms) {
    return QStringLiteral("%1:%2.%3")
        .arg(ms / 60000)
        .arg((ms / 1000) % 60, 2, 10, QLatin1Char('0'))
        .arg(ms % 1000, 3, 10, QLatin1Char('0'));
}

QString scoreTime(quint64 score) {
    if ((score >> 60) != 0) {
        return {};
    }
    const quint64 ms = TimeMask - score;
    if (ms >= quint64(24) * 3600 * 1000) {
        return {};
    }
    return formatMs(ms);
}

QString scoreValue(quint64 score) {
    const quint64 kind = score >> 60;
    const quint64 value = score & TimeMask;
    if (kind == 0) {
        const QString time = scoreTime(score);
        return time.isEmpty() ? QObject::tr("%L1 (as time)").arg(TimeMask - value) : time;
    }
    if (kind == 1) {
        return QObject::tr("%L1 (drift)").arg(value);
    }
    if (kind == 2) {
        return QStringLiteral("%L1").arg(value);
    }
    return QObject::tr("invalid kind %1").arg(kind);
}

qint64 parseTime(const QString& text) {
    const QString t = text.trimmed();
    static const QRegularExpression re(QStringLiteral("^(?:(\\d+):)?(\\d+)(?:\\.(\\d{1,3}))?$"));
    const QRegularExpressionMatch m = re.match(t);
    if (!m.hasMatch()) {
        return -1;
    }
    if (m.captured(1).isEmpty() && m.captured(3).isEmpty()) {
        return t.toLongLong(); // plain milliseconds
    }
    const qint64 minutes = m.captured(1).toLongLong();
    const qint64 seconds = m.captured(2).toLongLong();
    const qint64 millis = m.captured(3).leftJustified(3, QLatin1Char('0')).toLongLong();
    return (minutes * 60 + seconds) * 1000 + millis;
}

QTableWidgetItem* scoreCell(quint64 score) {
    auto* item = new QTableWidgetItem;
    item->setData(Qt::DisplayRole, score);
    item->setToolTip(QObject::tr("shown in the game as %1").arg(scoreValue(score)));
    return item;
}

QString enc(const QString& s) {
    return QString::fromLatin1(QUrl::toPercentEncoding(s));
}

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(tr("Driveclub Admin %1").arg(Driveclub::Version()));
    resize(1100, 700);
    auto* versionLabel = new QLabel(tr("driveclub-admin %1").arg(Driveclub::Version()));
    versionLabel->setToolTip(tr("Built %1").arg(Driveclub::BuildTimestamp().replace('T', ' ')));
    statusBar()->addPermanentWidget(versionLabel);

    QSettings settings;
    url_ = new QLineEdit(settings.value("url", QStringLiteral("http://127.0.0.1:8081")).toString());
    url_->setPlaceholderText(QStringLiteral("http://127.0.0.1:8081"));
    key_ = new QLineEdit(settings.value("key").toString());
    key_->setEchoMode(QLineEdit::Password);
    key_->setPlaceholderText(tr("admin.key from config.json"));
    connect_ = new QPushButton(tr("Connect"));
    connect_->setDefault(true);
    refresh_ = new QPushButton(tr("Refresh"));
    refresh_->setShortcut(QKeySequence::Refresh);
    autoRefresh_ = new QCheckBox(tr("Auto refresh"));
    autoRefresh_->setChecked(settings.value("autoRefresh", true).toBool());

    auto* bar = new QToolBar(tr("Connection"));
    bar->setMovable(false);
    auto* barWidget = new QWidget;
    auto* barLayout = new QHBoxLayout(barWidget);
    barLayout->setContentsMargins(4, 2, 4, 2);
    barLayout->addWidget(new QLabel(tr("Server")));
    barLayout->addWidget(url_, 3);
    barLayout->addWidget(new QLabel(tr("Key")));
    barLayout->addWidget(key_, 2);
    barLayout->addWidget(connect_);
    barLayout->addWidget(refresh_);
    barLayout->addWidget(autoRefresh_);
    bar->addWidget(barWidget);
    addToolBar(bar);

    tabs_ = new QTabWidget;
    tabs_->addTab(buildOverviewTab(), tr("Overview"));
    tabs_->addTab(buildPlayersTab(), tr("Players"));
    tabs_->addTab(buildLeaderboardsTab(), tr("Leaderboards"));
    tabs_->addTab(buildChallengesTab(), tr("Challenges"));
    tabs_->addTab(buildCountersTab(), tr("Counters"));
    tabs_->addTab(buildGhostsTab(), tr("Ghosts"));
    tabs_->addTab(buildStatsTab(), tr("Stats"));
    tabs_->addTab(buildTrafficTab(), tr("Traffic"));
    setCentralWidget(tabs_);

    timer_ = new QTimer(this);
    timer_->setInterval(5000);
    trafficTimer_ = new QTimer(this);
    trafficTimer_->setInterval(1500);

    connect(connect_, &QPushButton::clicked, this, &MainWindow::connectToServer);
    connect(key_, &QLineEdit::returnPressed, this, &MainWindow::connectToServer);
    connect(refresh_, &QPushButton::clicked, this, &MainWindow::refresh);
    connect(timer_, &QTimer::timeout, this, &MainWindow::refresh);
    connect(trafficTimer_, &QTimer::timeout, this, &MainWindow::refreshTraffic);
    connect(autoRefresh_, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue("autoRefresh", on);
        if (on && connected_) {
            timer_->start();
        } else {
            timer_->stop();
        }
    });
    connect(liveTraffic_, &QCheckBox::toggled, this, [this](bool on) {
        if (on && connected_) {
            trafficTimer_->start();
        } else {
            trafficTimer_->stop();
        }
    });

    updateButtons();
    setStatus(tr("Not connected"));
}

void MainWindow::connectTo(const QString& url, const QString& key) {
    if (!url.isEmpty()) {
        url_->setText(url);
    }
    if (!key.isEmpty()) {
        key_->setText(key);
    }
    connectToServer();
}

QWidget* MainWindow::buildOverviewTab() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    server_ = new QLabel(tr("Connect to a driveclub-server admin API to see its state."));
    server_->setWordWrap(true);
    layout->addWidget(server_);
    auto* formWidget = new QWidget;
    overviewForm_ = new QFormLayout(formWidget);
    overviewForm_->setLabelAlignment(Qt::AlignRight);
    layout->addWidget(formWidget);
    layout->addStretch();
    return page;
}

QWidget* MainWindow::buildPlayersTab() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);

    layout->addWidget(new QLabel(tr("<b>EGS sessions</b> (signed-in players)")));
    sessionTable_ = makeTable({tr("Name"), tr("Verified"), tr("shadNet account"), tr("Account ID"),
                               tr("Client"), tr("PS Plus"), tr("Signed in"), tr("Last seen"),
                               tr("Requests"), tr("Transaction")});
    layout->addWidget(sessionTable_, 3);
    kick_ = new QPushButton(tr("Kick (drop session)"));
    kick_->setToolTip(tr("The game's next signed request no longer matches; it has to log in "
                         "again."));
    auto* sessionButtons = new QHBoxLayout;
    sessionButtons->addStretch();
    sessionButtons->addWidget(kick_);
    layout->addLayout(sessionButtons);

    layout->addWidget(new QLabel(tr("<b>OTG tokens</b> (leaderboard/challenge logins)")));
    tokenTable_ = makeTable({tr("Name"), tr("Verified"), tr("shadNet account"), tr("Created"),
                             tr("Last used"), tr("Requests"), tr("Token")});
    layout->addWidget(tokenTable_, 2);
    revoke_ = new QPushButton(tr("Revoke token"));
    auto* tokenButtons = new QHBoxLayout;
    tokenButtons->addStretch();
    tokenButtons->addWidget(revoke_);
    layout->addLayout(tokenButtons);

    layout->addWidget(new QLabel(
        tr("<b>Known players</b> (everyone who logged in, plus names from friend lookups and "
           "test entries; Fame as the game last reported it)")));
    playerTable_ =
        makeTable({tr("Name"), tr("Account ID"), tr("Logged in"), tr("shadNet account"), tr("Fame"),
                   tr("Stars"), tr("Fame updated"), tr("First seen"), tr("Last login")});
    layout->addWidget(playerTable_, 3);
    setFame_ = new QPushButton(tr("Set Fame..."));
    setFame_->setToolTip(tr("Set the player's Fame exactly (also their Fame leaderboard entry). "
                            "The game reads it at its next getFame; it never lowers its own "
                            "Fame below what it already has."));
    auto* playerButtons = new QHBoxLayout;
    playerButtons->addStretch();
    playerButtons->addWidget(setFame_);
    layout->addLayout(playerButtons);

    connect(sessionTable_, &QTableWidget::itemSelectionChanged, this, &MainWindow::updateButtons);
    connect(tokenTable_, &QTableWidget::itemSelectionChanged, this, &MainWindow::updateButtons);
    connect(playerTable_, &QTableWidget::itemSelectionChanged, this, &MainWindow::updateButtons);
    connect(setFame_, &QPushButton::clicked, this, &MainWindow::setFame);
    connect(kick_, &QPushButton::clicked, this, &MainWindow::kickSession);
    connect(revoke_, &QPushButton::clicked, this, &MainWindow::revokeToken);
    return page;
}

QWidget* MainWindow::buildChallengesTab() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->addWidget(new QLabel(
        tr("<b>Challenges</b>: each one is on its creator's list and on every challenged player's "
           "list; the times set on it are on its own board. End times are in the game's clock.")));
    challengeTable_ = makeTable({tr("Challenge"), tr("Creator"), tr("Challenged"), tr("Ends"),
                                 tr("Times"), tr("Best"), tr("Details")});
    layout->addWidget(challengeTable_, 3);
    challengeTitle_ = new QLabel(tr("Select a challenge to see its times"));
    layout->addWidget(challengeTitle_);
    challengeTimes_ = makeTable(
        {tr("Rank"), tr("Player"), tr("Shown as"), tr("Score"), tr("Attachment"), tr("Updated")});
    layout->addWidget(challengeTimes_, 2);
    connect(challengeTable_, &QTableWidget::itemSelectionChanged, this,
            &MainWindow::loadChallengeTimes);
    return page;
}

QWidget* MainWindow::buildCountersTab() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->addWidget(new QLabel(
        tr("<b>OTG counter stores</b> (<code>…/~gamestate</code>, e.g. ChallengeTimers). The game "
           "reads them after login and sends changes as deltas; a player without one uses the "
           "game's defaults.")));
    counterTable_ =
        makeTable({tr("Resource"), tr("Version"), tr("Counters"), tr("Values"), tr("Updated")});
    layout->addWidget(counterTable_);
    resetCounter_ = new QPushButton(tr("Reset to game defaults"));
    resetCounter_->setToolTip(tr("Delete the stored counters; the game starts again from its "
                                 "built-in defaults."));
    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(resetCounter_);
    layout->addLayout(buttons);
    connect(counterTable_, &QTableWidget::itemSelectionChanged, this, &MainWindow::updateButtons);
    connect(resetCounter_, &QPushButton::clicked, this, &MainWindow::resetCounter);
    return page;
}

QWidget* MainWindow::buildGhostsTab() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->addWidget(new QLabel(
        tr("<b>Ghosts</b> uploaded by the game (storageService.createBob). A score carries its "
           "ghost as <code>ghost:&lt;id&gt;</code> in its attachment; rivals download it from "
           "the leaderboard. Deleting a ghost a score still uses makes that ghost unavailable "
           "in the game.")));
    qobject_cast<QLabel*>(layout->itemAt(0)->widget())->setWordWrap(true);
    ghostTable_ =
        makeTable({tr("Id"), tr("Uploaded by"), tr("Size (bytes)"), tr("Uploaded"), tr("Downloads"),
                   tr("Last download"), tr("Used by scores"), tr("Type"), tr("Checksum (MD5)")});
    layout->addWidget(ghostTable_, 3);
    ghostTitle_ = new QLabel(tr("Select a ghost to see the scores that use it"));
    layout->addWidget(ghostTitle_);
    ghostUses_ = makeTable({tr("Board"), tr("Player"), tr("Score"), tr("Posted")});
    layout->addWidget(ghostUses_, 2);
    saveGhost_ = new QPushButton(tr("Save to file…"));
    saveGhost_->setToolTip(tr("Download the selected ghost's data (not counted as a download)."));
    deleteGhost_ = new QPushButton(tr("Delete ghost"));
    deleteUnused_ = new QPushButton(tr("Delete unused ghosts"));
    deleteUnused_->setToolTip(tr("Delete every ghost no stored score points to (e.g. replaced by "
                                 "a better time)."));
    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(saveGhost_);
    buttons->addWidget(deleteGhost_);
    buttons->addWidget(deleteUnused_);
    layout->addLayout(buttons);
    connect(ghostTable_, &QTableWidget::itemSelectionChanged, this, [this] {
        showGhostUses();
        updateButtons();
    });
    connect(saveGhost_, &QPushButton::clicked, this, &MainWindow::saveGhost);
    connect(deleteGhost_, &QPushButton::clicked, this, &MainWindow::deleteGhost);
    connect(deleteUnused_, &QPushButton::clicked, this, &MainWindow::deleteUnusedGhosts);
    return page;
}

QWidget* MainWindow::buildStatsTab() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    auto* intro = new QLabel(tr(
        "<b>Player stats</b> as the game's profile pages show them (statsService.getAccountStats). "
        "The game keeps them in the counter store <code>stats/V06/player/&lt;id&gt;</code> and "
        "writes it only with <code>OTG_Service_statsWrite</code> in the TSS. Values are the "
        "game's own units."));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    statsPlayerTable_ =
        makeTable({tr("Player"), tr("Account ID"), tr("Counters"), tr("Writes"), tr("Updated")});
    layout->addWidget(statsPlayerTable_, 2);
    auto* bar = new QHBoxLayout;
    bar->addWidget(new QLabel(tr("Set")));
    statsSet_ = new QComboBox;
    statsSet_->addItems({QStringLiteral("Tracks"), QStringLiteral("Vehicles"),
                         QStringLiteral("GameModes"), QStringLiteral("Challenges"),
                         QStringLiteral("Overdrive"), QStringLiteral("FaceOff")});
    bar->addWidget(statsSet_);
    statsTitle_ = new QLabel(tr("Select a player"));
    bar->addWidget(statsTitle_, 1);
    layout->addLayout(bar);
    statsView_ = makeTable({});
    layout->addWidget(statsView_, 3);
    connect(statsPlayerTable_, &QTableWidget::itemSelectionChanged, this,
            &MainWindow::loadStatsTable);
    connect(statsSet_, &QComboBox::currentTextChanged, this, &MainWindow::loadStatsTable);
    return page;
}

QWidget* MainWindow::buildLeaderboardsTab() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    auto* splitter = new QSplitter(Qt::Horizontal);

    auto* left = new QWidget;
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    auto* filterRow = new QHBoxLayout;
    filterRow->addWidget(new QLabel(tr("Show:")));
    boardFilter_ = new QComboBox;
    boardFilter_->addItem(tr("Leaderboards"), QStringLiteral("leaderboard"));
    boardFilter_->addItem(tr("Event history"), QStringLiteral("event_history"));
    boardFilter_->addItem(tr("Challenge lists and boards"), QStringLiteral("challenge"));
    boardFilter_->addItem(tr("Everything"), QString());
    boardFilter_->setToolTip(tr("Challenges are easier to read on the Challenges tab."));
    filterRow->addWidget(boardFilter_);
    filterRow->addStretch();
    leftLayout->addLayout(filterRow);
    boardTable_ = makeTable({tr("Board"), tr("Entries"), tr("Best"), tr("Updated"), tr("Kind")});
    leftLayout->addWidget(boardTable_);
    splitter->addWidget(left);
    connect(boardFilter_, &QComboBox::currentIndexChanged, this, &MainWindow::applyBoardFilter);

    auto* right = new QWidget;
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    boardTitle_ = new QLabel(tr("Select a board"));
    rightLayout->addWidget(boardTitle_);
    entryTable_ = makeTable(
        {tr("Rank"), tr("Entrant"), tr("Score"), tr("Shown as"), tr("Attachment"), tr("Updated")});
    rightLayout->addWidget(entryTable_);
    addEntry_ = new QPushButton(tr("Add entries..."));
    removeEntry_ = new QPushButton(tr("Remove entry"));
    clearBoard_ = new QPushButton(tr("Clear board"));
    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(addEntry_);
    buttons->addWidget(removeEntry_);
    buttons->addWidget(clearBoard_);
    rightLayout->addLayout(buttons);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    layout->addWidget(splitter);

    connect(boardTable_, &QTableWidget::itemSelectionChanged, this, [this] {
        loadBoardEntries();
        updateButtons();
    });
    connect(entryTable_, &QTableWidget::itemSelectionChanged, this, &MainWindow::updateButtons);
    connect(addEntry_, &QPushButton::clicked, this, &MainWindow::addEntries);
    connect(removeEntry_, &QPushButton::clicked, this, &MainWindow::removeEntry);
    connect(clearBoard_, &QPushButton::clicked, this, &MainWindow::clearBoard);
    return page;
}

QWidget* MainWindow::buildTrafficTab() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    auto* options = new QHBoxLayout;
    liveTraffic_ = new QCheckBox(tr("Live"));
    liveTraffic_->setChecked(true);
    errorsOnly_ = new QCheckBox(tr("Errors only (status 400 and up, except expected ones)"));
    auto* clear = new QPushButton(tr("Clear view"));
    options->addWidget(liveTraffic_);
    options->addWidget(errorsOnly_);
    options->addStretch();
    options->addWidget(clear);
    layout->addLayout(options);
    trafficTable_ = makeTable({tr("#"), tr("Time"), tr("Method"), tr("Service"), tr("Status"),
                               tr("ms"), tr("In"), tr("Out"), tr("Path")});
    trafficTable_->setSortingEnabled(false);
    layout->addWidget(trafficTable_);

    connect(clear, &QPushButton::clicked, this, [this] { trafficTable_->setRowCount(0); });
    connect(errorsOnly_, &QCheckBox::toggled, this, [this](bool on) {
        for (int row = 0; row < trafficTable_->rowCount(); ++row) {
            const bool error = trafficTable_->item(row, 4)->data(Qt::UserRole + 1).toBool();
            trafficTable_->setRowHidden(row, on && !error);
        }
    });
    return page;
}

void MainWindow::connectToServer() {
    const QUrl url = QUrl::fromUserInput(url_->text().trimmed());
    if (!url.isValid() || key_->text().isEmpty()) {
        setStatus(tr("Enter the admin address and key"), true);
        return;
    }
    api_.setServer(url, key_->text());
    connected_ = false;
    setStatus(tr("Connecting to %1...").arg(url.toString()));
    api_.get(QStringLiteral("/api/status"), [this, url](const ApiClient::Reply& reply) {
        if (!reply.ok()) {
            reportError(tr("Connect"), reply);
            return;
        }
        QSettings settings;
        settings.setValue("url", url_->text().trimmed());
        settings.setValue("key", key_->text());
        connected_ = true;
        lastRequestId_ = 0;
        trafficTable_->setRowCount(0);
        if (autoRefresh_->isChecked()) {
            timer_->start();
        }
        if (liveTraffic_->isChecked()) {
            trafficTimer_->start();
        }
        setStatus(tr("Connected to %1").arg(url.toString()));
        showStatus(reply.json().object());
        refresh();
        refreshTraffic();
    });
}

void MainWindow::refresh() {
    if (!connected_) {
        return;
    }
    api_.get(QStringLiteral("/api/status"), [this](const ApiClient::Reply& r) {
        if (r.ok()) {
            showStatus(r.json().object());
        } else {
            reportError(tr("Status"), r);
        }
    });
    api_.get(QStringLiteral("/api/sessions"), [this](const ApiClient::Reply& r) {
        if (r.ok()) {
            showSessions(r.json().array());
        }
    });
    api_.get(QStringLiteral("/api/otg/tokens"), [this](const ApiClient::Reply& r) {
        if (r.ok()) {
            showTokens(r.json().array());
        }
    });
    api_.get(QStringLiteral("/api/leaderboards"), [this](const ApiClient::Reply& r) {
        if (r.ok()) {
            showBoards(r.json().array());
        }
    });
    api_.get(QStringLiteral("/api/players"), [this](const ApiClient::Reply& r) {
        if (r.ok()) {
            showPlayers(r.json().array());
        }
    });
    api_.get(QStringLiteral("/api/challenges"), [this](const ApiClient::Reply& r) {
        if (r.ok()) {
            showChallenges(r.json().array());
        }
    });
    api_.get(QStringLiteral("/api/gamestates"), [this](const ApiClient::Reply& r) {
        if (r.ok()) {
            showCounters(r.json().array());
        }
    });
    api_.get(QStringLiteral("/api/ghosts"), [this](const ApiClient::Reply& r) {
        if (r.ok()) {
            showGhosts(r.json().array());
        }
    });
    api_.get(QStringLiteral("/api/stats"), [this](const ApiClient::Reply& r) {
        if (r.ok()) {
            showStatsPlayers(r.json().object());
        }
    });
}

void MainWindow::refreshTraffic() {
    if (!connected_) {
        return;
    }
    api_.get(QStringLiteral("/api/requests?after=%1").arg(qint64(lastRequestId_)),
             [this](const ApiClient::Reply& r) {
                 if (r.ok()) {
                     appendTraffic(r.json().array());
                 }
             });
}

void MainWindow::showStatus(const QJsonObject& s) {
    while (overviewForm_->rowCount() > 0) {
        overviewForm_->removeRow(0);
    }
    const bool checks = !s.value("shadnet_url").toString().isEmpty();
    const QString serverVersion = s.value("version").toString();
    server_->setText(tr("<b>driveclub-server %1</b>, up %2, game port %3")
                         .arg(serverVersion, duration(s.value("uptime").toInteger()),
                              s.value("listen").toString()));
    const auto add = [this](const QString& label, const QString& value) {
        auto* v = new QLabel(value);
        v->setTextInteractionFlags(Qt::TextSelectableByMouse);
        overviewForm_->addRow(label, v);
    };
    QString versions =
        tr("server %1, built %2")
            .arg(serverVersion, s.value("build_timestamp").toString().replace('T', ' '));
    if (!serverVersion.isEmpty() && serverVersion != Driveclub::Version()) {
        versions += tr("  <span style='color:#b36b00'>(this admin tool is %1)</span>")
                        .arg(Driveclub::Version());
    }
    add(tr("Version:"), versions);
    add(tr("Admin tool:"),
        tr("%1, built %2")
            .arg(Driveclub::Version(), Driveclub::BuildTimestamp().replace('T', ' ')));
    add(tr("Signed-in players (EGS):"), QString::number(s.value("sessions").toInteger()));
    add(tr("OTG tokens:"), QString::number(s.value("otg_tokens").toInteger()));
    add(tr("Known players:"), QString::number(s.value("players").toInteger()));
    add(tr("Leaderboards:"), QString::number(s.value("leaderboards").toInteger()));
    add(tr("Ghosts:"), tr("%1 (%2)")
                           .arg(s.value("ghosts").toInteger())
                           .arg(byteSize(s.value("ghost_bytes").toDouble())));
    add(tr("Game requests served:"), QString::number(s.value("requests").toInteger()));
    add(tr("EGS serverIdentifier:"), tr("%1  (every OTG_Service_* TSS value must contain it)")
                                         .arg(s.value("server_identifier").toString()));
    add(tr("OTG zone / domain id:"), s.value("otg_zone").toString() + QStringLiteral(" / ") +
                                         s.value("otg_domain_id").toString());
    add(tr("shadNet checks:"),
        checks ? tr("on, %1").arg(s.value("shadnet_url").toString())
               : tr("<span style='color:#b36b00'>off</span> (shadnet.url empty: logins are "
                    "accepted as sent)"));
    if (checks) {
        const QString client = s.value("shadnet_client_id").toString();
        add(tr("NP client ID:"), client);
        add(tr("Unverified logins:"),
            s.value("allow_unverified").toBool() ? tr("allowed") : tr("refused"));
    }
}

void MainWindow::showSessions(const QJsonArray& sessions) {
    sessions_ = sessions;
    sessionTable_->setSortingEnabled(false);
    sessionTable_->setRowCount(int(sessions.size()));
    for (int i = 0; i < sessions.size(); ++i) {
        const QJsonObject s = sessions.at(i).toObject();
        sessionTable_->setItem(i, 0, cell(s.value("name").toString(), i));
        sessionTable_->setItem(i, 1, cell(yesNo(s.value("verified").toBool())));
        sessionTable_->setItem(i, 2, cell(s.value("shadnet_account").toString()));
        sessionTable_->setItem(i, 3, numberCell(s.value("account_id").toDouble()));
        sessionTable_->setItem(i, 4, cell(s.value("client_version").toString()));
        sessionTable_->setItem(i, 5, cell(yesNo(s.value("ps_plus").toBool())));
        sessionTable_->setItem(i, 6, cell(when(s.value("created").toDouble())));
        sessionTable_->setItem(i, 7, cell(ago(s.value("last_seen").toDouble())));
        sessionTable_->setItem(i, 8, numberCell(s.value("requests").toDouble()));
        sessionTable_->setItem(i, 9, cell(s.value("transaction").toString()));
    }
    sessionTable_->setSortingEnabled(true);
    sessionTable_->resizeColumnsToContents();
    updateButtons();
}

void MainWindow::showTokens(const QJsonArray& tokens) {
    tokens_ = tokens;
    tokenTable_->setSortingEnabled(false);
    tokenTable_->setRowCount(int(tokens.size()));
    for (int i = 0; i < tokens.size(); ++i) {
        const QJsonObject t = tokens.at(i).toObject();
        tokenTable_->setItem(i, 0, cell(t.value("name").toString(), i));
        tokenTable_->setItem(i, 1, cell(yesNo(t.value("verified").toBool())));
        tokenTable_->setItem(i, 2, cell(t.value("shadnet_account").toString()));
        tokenTable_->setItem(i, 3, cell(when(t.value("created").toDouble())));
        tokenTable_->setItem(i, 4, cell(ago(t.value("last_used").toDouble())));
        tokenTable_->setItem(i, 5, numberCell(t.value("requests").toDouble()));
        tokenTable_->setItem(i, 6, cell(t.value("token").toString()));
    }
    tokenTable_->setSortingEnabled(true);
    tokenTable_->resizeColumnsToContents();
    updateButtons();
}

void MainWindow::showPlayers(const QJsonArray& players) {
    players_ = players;
    playerTable_->setSortingEnabled(false);
    playerTable_->setRowCount(int(players.size()));
    for (int i = 0; i < players.size(); ++i) {
        const QJsonObject p = players.at(i).toObject();
        playerTable_->setItem(i, 0, cell(p.value("name").toString(), i));
        playerTable_->setItem(i, 1, numberCell(p.value("account_id").toString().toDouble()));
        playerTable_->setItem(i, 2, cell(yesNo(p.value("logged_in").toBool())));
        playerTable_->setItem(i, 3, cell(p.value("shadnet_account").toString()));
        auto* fame = p.contains("fame") ? numberCell(p.value("fame").toDouble()) : cell(QString());
        playerTable_->setItem(i, 4, fame);
        playerTable_->setItem(
            i, 5, p.contains("stars") ? numberCell(p.value("stars").toDouble()) : cell(QString()));
        playerTable_->setItem(i, 6, cell(ago(p.value("fame_updated").toDouble())));
        playerTable_->setItem(i, 7, cell(when(p.value("first_seen").toDouble())));
        playerTable_->setItem(i, 8,
                              cell(p.value("logged_in").toBool()
                                       ? ago(p.value("last_seen").toDouble())
                                       : QStringLiteral("-")));
    }
    playerTable_->setSortingEnabled(true);
    playerTable_->resizeColumnsToContents();
    updateButtons();
}

void MainWindow::showChallenges(const QJsonArray& challenges) {
    const int previous = selectedIndex(challengeTable_);
    const QString selectedId =
        previous < 0 ? QString() : challenges_.at(previous).toObject().value("id").toString();
    challenges_ = challenges;
    challengeTable_->blockSignals(true);
    challengeTable_->setSortingEnabled(false);
    challengeTable_->setRowCount(int(challenges.size()));
    int reselect = -1;
    for (int i = 0; i < challenges.size(); ++i) {
        const QJsonObject c = challenges.at(i).toObject();
        QStringList challenged;
        for (const QJsonValue& m : c.value("members").toArray()) {
            const QJsonObject o = m.toObject();
            if ((o.value("flags").toInteger() & 1) == 0) {
                challenged << o.value("name").toString();
            }
        }
        challengeTable_->setItem(i, 0, cell(c.value("id").toString(), i));
        challengeTable_->setItem(i, 1, cell(c.value("creator_name").toString()));
        challengeTable_->setItem(i, 2, cell(challenged.join(QStringLiteral(", "))));
        challengeTable_->setItem(i, 3,
                                 cell(QDateTime::fromMSecsSinceEpoch(
                                          qint64(c.value("ends").toDouble()), QTimeZone::utc())
                                          .toString(QStringLiteral("yyyy-MM-dd hh:mm"))));
        challengeTable_->setItem(i, 4, numberCell(c.value("entries").toDouble()));
        const QString best = c.value("best").toString();
        challengeTable_->setItem(
            i, 5,
            cell(best.isEmpty() ? QString()
                                : QStringLiteral("%1 (%2)").arg(scoreValue(best.toULongLong()),
                                                                c.value("best_name").toString())));
        QStringList details;
        for (const QJsonValue& v : c.value("attachment").toArray()) {
            details << v.toString();
        }
        auto* detailCell = cell(details.join(QStringLiteral(", ")));
        detailCell->setToolTip(details.join('\n'));
        challengeTable_->setItem(i, 6, detailCell);
        if (c.value("id").toString() == selectedId) {
            reselect = i;
        }
    }
    challengeTable_->setSortingEnabled(true);
    challengeTable_->resizeColumnsToContents();
    for (int row = 0; reselect >= 0 && row < challengeTable_->rowCount(); ++row) {
        if (challengeTable_->item(row, 0)->data(Qt::UserRole).toInt() == reselect) {
            challengeTable_->selectRow(row);
        }
    }
    challengeTable_->blockSignals(false);
    loadChallengeTimes();
}

void MainWindow::loadChallengeTimes() {
    const int i = selectedIndex(challengeTable_);
    if (i < 0) {
        challengeTitle_->setText(tr("Select a challenge to see its times"));
        challengeTimes_->setRowCount(0);
        return;
    }
    const QJsonObject c = challenges_.at(i).toObject();
    const QString board = c.value("board").toString();
    api_.get(
        QStringLiteral("/api/leaderboards/entries?board=%1").arg(enc(board)),
        [this, c, board](const ApiClient::Reply& r) {
            const int now = selectedIndex(challengeTable_);
            if (now < 0 || challenges_.at(now).toObject().value("board").toString() != board) {
                return;
            }
            challengeTitle_->setText(tr("<b>%1</b> by %2: times set on it")
                                         .arg(c.value("id").toString().toHtmlEscaped(),
                                              c.value("creator_name").toString().toHtmlEscaped()));
            const QJsonArray entries = r.ok() ? r.json().array() : QJsonArray{};
            challengeTimes_->setSortingEnabled(false);
            challengeTimes_->setRowCount(int(entries.size()));
            for (int k = 0; k < entries.size(); ++k) {
                const QJsonObject e = entries.at(k).toObject();
                const quint64 score = e.value("score").toString().toULongLong();
                auto* rank = numberCell(e.value("rank").toDouble());
                rank->setData(Qt::UserRole, k);
                challengeTimes_->setItem(k, 0, rank);
                challengeTimes_->setItem(k, 1, cell(playerName(e.value("entrant").toString())));
                challengeTimes_->setItem(k, 2, cell(scoreValue(score)));
                challengeTimes_->setItem(k, 3, scoreCell(score));
                QStringList attachment;
                for (const QJsonValue& v : e.value("attachment").toArray()) {
                    attachment << v.toString();
                }
                challengeTimes_->setItem(k, 4, cell(attachment.join(QStringLiteral(", "))));
                challengeTimes_->setItem(k, 5, cell(when(e.value("updated").toDouble())));
            }
            challengeTimes_->setSortingEnabled(true);
            challengeTimes_->resizeColumnsToContents();
        });
}

void MainWindow::showCounters(const QJsonArray& counters) {
    counters_ = counters;
    counterTable_->setSortingEnabled(false);
    counterTable_->setRowCount(int(counters.size()));
    for (int i = 0; i < counters.size(); ++i) {
        const QJsonObject c = counters.at(i).toObject();
        const QJsonObject counts = c.value("counts").toObject();
        QStringList values;
        for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
            values << QStringLiteral("%1 = %2").arg(it.key()).arg(it.value().toInteger());
        }
        counterTable_->setItem(i, 0, cell(c.value("resource").toString(), i));
        counterTable_->setItem(i, 1, numberCell(c.value("version").toDouble()));
        counterTable_->setItem(i, 2, numberCell(counts.size()));
        auto* valuesCell = cell(values.join(QStringLiteral(", ")));
        valuesCell->setToolTip(values.join('\n'));
        counterTable_->setItem(i, 3, valuesCell);
        counterTable_->setItem(i, 4, cell(ago(c.value("updated").toDouble())));
    }
    counterTable_->setSortingEnabled(true);
    counterTable_->resizeColumnsToContents();
    updateButtons();
}

namespace {
QString checksumHex(const QString& base64) {
    return QString::fromLatin1(QByteArray::fromBase64(base64.toLatin1()).toHex());
}
} // namespace

void MainWindow::showGhosts(const QJsonArray& ghosts) {
    // Keep the selection across the 5 s refresh.
    const int old = selectedIndex(ghostTable_);
    const QString selected =
        old < 0 ? QString() : ghosts_.at(old).toObject().value("id").toString();
    ghosts_ = ghosts;
    ghostTable_->blockSignals(true);
    ghostTable_->setSortingEnabled(false);
    ghostTable_->clearSelection();
    ghostTable_->setRowCount(int(ghosts.size()));
    int reselect = -1;
    for (int i = 0; i < ghosts.size(); ++i) {
        const QJsonObject g = ghosts.at(i).toObject();
        const QString id = g.value("id").toString();
        const QJsonArray uses = g.value("used_on").toArray();
        auto* idCell = numberCell(id.toDouble());
        idCell->setData(Qt::UserRole, i);
        ghostTable_->setItem(i, 0, idCell);
        auto* who = cell(g.value("name").toString());
        who->setToolTip(g.value("account_id").toString());
        ghostTable_->setItem(i, 1, who);
        auto* size = numberCell(g.value("size").toDouble()); // bytes: sorts as a number
        size->setToolTip(byteSize(g.value("size").toDouble()));
        ghostTable_->setItem(i, 2, size);
        ghostTable_->setItem(i, 3, cell(ago(g.value("created").toDouble())));
        ghostTable_->setItem(i, 4, numberCell(g.value("downloads").toDouble()));
        const double last = g.value("last_download").toDouble();
        ghostTable_->setItem(i, 5, cell(last > 0 ? ago(last) : tr("never")));
        auto* used = cell(uses.isEmpty() ? tr("none (unused)") : QString::number(uses.size()));
        if (uses.isEmpty()) {
            used->setForeground(Qt::gray);
        }
        ghostTable_->setItem(i, 6, used);
        ghostTable_->setItem(i, 7, cell(g.value("mime_type").toString()));
        auto* sum = cell(checksumHex(g.value("checksum").toString()));
        sum->setToolTip(tr("base64: %1").arg(g.value("checksum").toString()));
        ghostTable_->setItem(i, 8, sum);
        if (id == selected) {
            reselect = i;
        }
    }
    ghostTable_->setSortingEnabled(true);
    if (reselect >= 0) {
        for (int row = 0; row < ghostTable_->rowCount(); ++row) {
            if (ghostTable_->item(row, 0)->data(Qt::UserRole).toInt() == reselect) {
                ghostTable_->selectRow(row);
                break;
            }
        }
    }
    ghostTable_->blockSignals(false);
    ghostTable_->resizeColumnsToContents();
    showGhostUses();
    updateButtons();
}

void MainWindow::showGhostUses() {
    const int i = selectedIndex(ghostTable_);
    ghostUses_->setSortingEnabled(false);
    if (i < 0) {
        ghostTitle_->setText(tr("Select a ghost to see the scores that use it"));
        ghostUses_->setRowCount(0);
        return;
    }
    const QJsonObject g = ghosts_.at(i).toObject();
    const QJsonArray uses = g.value("used_on").toArray();
    ghostTitle_->setText(uses.isEmpty()
                             ? tr("Ghost %1: no stored score uses it").arg(g.value("id").toString())
                             : tr("Ghost %1 is used by %n score(s)", nullptr, int(uses.size()))
                                   .arg(g.value("id").toString()));
    ghostUses_->setRowCount(int(uses.size()));
    for (int r = 0; r < uses.size(); ++r) {
        const QJsonObject u = uses.at(r).toObject();
        ghostUses_->setItem(r, 0, cell(u.value("board").toString()));
        const QString entrant = u.value("entrant").toString();
        auto* who = cell(playerName(entrant));
        who->setToolTip(entrant);
        ghostUses_->setItem(r, 1, who);
        ghostUses_->setItem(r, 2, cell(scoreValue(u.value("score").toString().toULongLong())));
        ghostUses_->setItem(r, 3, cell(ago(u.value("updated").toDouble())));
    }
    ghostUses_->setSortingEnabled(true);
    ghostUses_->resizeColumnsToContents();
}

void MainWindow::saveGhost() {
    const int i = selectedIndex(ghostTable_);
    if (i < 0) {
        return;
    }
    const QString id = ghosts_.at(i).toObject().value("id").toString();
    const QString file = QFileDialog::getSaveFileName(this, tr("Save ghost %1").arg(id),
                                                      QStringLiteral("ghost-%1.bin").arg(id));
    if (file.isEmpty()) {
        return;
    }
    api_.get(QStringLiteral("/api/ghosts/data?id=%1").arg(id),
             [this, file, id](const ApiClient::Reply& r) {
                 if (!r.ok()) {
                     reportError(tr("Save ghost"), r);
                     return;
                 }
                 QFile out(file);
                 if (!out.open(QIODevice::WriteOnly) || out.write(r.body) != r.body.size()) {
                     setStatus(tr("Could not write %1: %2").arg(file, out.errorString()), true);
                     return;
                 }
                 setStatus(tr("Ghost %1 saved to %2 (%3 bytes)").arg(id, file).arg(r.body.size()));
             });
}

void MainWindow::deleteGhosts(const QStringList& ids, const QString& question) {
    if (ids.isEmpty() ||
        QMessageBox::question(this, tr("Delete ghosts"), question) != QMessageBox::Yes) {
        return;
    }
    auto left = std::make_shared<int>(int(ids.size()));
    auto failed = std::make_shared<int>(0);
    for (const QString& id : ids) {
        api_.remove(QStringLiteral("/api/ghosts?id=%1").arg(id),
                    [this, left, failed, count = ids.size()](const ApiClient::Reply& r) {
                        if (!r.ok()) {
                            ++*failed;
                            reportError(tr("Delete ghost"), r);
                        }
                        if (--*left == 0) {
                            if (*failed == 0) {
                                setStatus(tr("%n ghost(s) deleted", nullptr, int(count)));
                            }
                            refresh();
                        }
                    });
    }
}

void MainWindow::deleteGhost() {
    const int i = selectedIndex(ghostTable_);
    if (i < 0) {
        return;
    }
    const QJsonObject g = ghosts_.at(i).toObject();
    const qsizetype uses = g.value("used_on").toArray().size();
    QString question =
        tr("Delete ghost %1 of %2?").arg(g.value("id").toString(), g.value("name").toString());
    if (uses > 0) {
        question += QLatin1Char(' ') +
                    tr("%n score(s) still use it: rivals will no longer be able to race it.",
                       nullptr, int(uses));
    }
    deleteGhosts({g.value("id").toString()}, question);
}

void MainWindow::deleteUnusedGhosts() {
    QStringList ids;
    qint64 bytes = 0;
    for (const QJsonValue& v : ghosts_) {
        const QJsonObject g = v.toObject();
        if (g.value("used_on").toArray().isEmpty()) {
            ids << g.value("id").toString();
            bytes += qint64(g.value("size").toDouble());
        }
    }
    if (ids.isEmpty()) {
        setStatus(tr("Every ghost is used by a score"));
        return;
    }
    deleteGhosts(ids, tr("Delete %n unused ghost(s) (%1)? No stored score points to them.", nullptr,
                         int(ids.size()))
                          .arg(byteSize(double(bytes))));
}

void MainWindow::showStatsPlayers(const QJsonObject& stats) {
    const int old = selectedIndex(statsPlayerTable_);
    const QString selected =
        old < 0 ? QString() : statsPlayers_.at(old).toObject().value("account_id").toString();
    const QJsonArray players = stats.value("players").toArray();
    const bool changed = players != statsPlayers_;
    statsPlayers_ = players;
    statsPlayerTable_->blockSignals(true);
    statsPlayerTable_->setSortingEnabled(false);
    statsPlayerTable_->clearSelection();
    statsPlayerTable_->setRowCount(int(players.size()));
    int reselect = -1;
    for (int i = 0; i < players.size(); ++i) {
        const QJsonObject p = players.at(i).toObject();
        statsPlayerTable_->setItem(i, 0, cell(p.value("name").toString(), i));
        statsPlayerTable_->setItem(i, 1, cell(p.value("account_id").toString()));
        statsPlayerTable_->setItem(i, 2, numberCell(p.value("counters").toDouble()));
        statsPlayerTable_->setItem(i, 3, numberCell(p.value("version").toDouble()));
        statsPlayerTable_->setItem(i, 4, cell(ago(p.value("updated").toDouble())));
        if (p.value("account_id").toString() == selected) {
            reselect = i;
        }
    }
    statsPlayerTable_->setSortingEnabled(true);
    if (reselect >= 0) {
        for (int row = 0; row < statsPlayerTable_->rowCount(); ++row) {
            if (statsPlayerTable_->item(row, 0)->data(Qt::UserRole).toInt() == reselect) {
                statsPlayerTable_->selectRow(row);
                break;
            }
        }
    }
    statsPlayerTable_->blockSignals(false);
    statsPlayerTable_->resizeColumnsToContents();
    if (changed || reselect < 0) {
        loadStatsTable();
    }
}

void MainWindow::loadStatsTable() {
    const int i = selectedIndex(statsPlayerTable_);
    if (i < 0) {
        statsTitle_->setText(statsPlayers_.isEmpty() ? tr("No player has stats stored yet")
                                                     : tr("Select a player"));
        statsView_->setRowCount(0);
        statsView_->setColumnCount(0);
        return;
    }
    const QJsonObject p = statsPlayers_.at(i).toObject();
    const QString set = statsSet_->currentText();
    api_.get(
        QStringLiteral("/api/stats/table?account=%1&set=%2")
            .arg(p.value("account_id").toString(), set),
        [this, name = p.value("name").toString(), set](const ApiClient::Reply& r) {
            if (!r.ok()) {
                reportError(tr("Load stats"), r);
                return;
            }
            const QJsonObject t = r.json().object();
            const QJsonArray columns = t.value("columns").toArray();
            const QJsonArray rows = t.value("rows").toArray();
            statsView_->setSortingEnabled(false);
            statsView_->clear();
            if (set == QLatin1String("Overdrive") || set == QLatin1String("FaceOff")) {
                // One row of many columns: shown turned on its side, a stat per line.
                const QJsonArray values = rows.isEmpty()
                                              ? QJsonArray()
                                              : rows.first().toObject().value("values").toArray();
                statsView_->setColumnCount(2);
                statsView_->setHorizontalHeaderLabels({tr("Stat"), tr("Value")});
                statsView_->setRowCount(int(columns.size()));
                for (int c = 0; c < columns.size(); ++c) {
                    statsView_->setItem(c, 0, cell(columns.at(c).toString()));
                    statsView_->setItem(c, 1, numberCell(values.at(c).toDouble()));
                }
            } else {
                QStringList headers{tr("Subject")};
                for (const QJsonValue& c : columns) {
                    headers << c.toString();
                }
                statsView_->setColumnCount(int(headers.size()));
                statsView_->setHorizontalHeaderLabels(headers);
                statsView_->setRowCount(int(rows.size()));
                for (int row = 0; row < rows.size(); ++row) {
                    const QJsonObject o = rows.at(row).toObject();
                    statsView_->setItem(row, 0, cell(o.value("name").toString()));
                    const QJsonArray values = o.value("values").toArray();
                    for (int c = 0; c < values.size(); ++c) {
                        statsView_->setItem(row, c + 1, numberCell(values.at(c).toDouble()));
                    }
                }
            }
            statsView_->setSortingEnabled(true);
            statsView_->resizeColumnsToContents();
            statsTitle_->setText(tr("%1: %2, %n row(s)", nullptr, int(rows.size())).arg(name, set));
        });
}

void MainWindow::showBoards(const QJsonArray& boards) {
    const QString selected = selectedBoard();
    boards_ = boards;
    boardTable_->blockSignals(true);
    boardTable_->setSortingEnabled(false);
    boardTable_->setRowCount(int(boards.size()));
    int reselect = -1;
    for (int i = 0; i < boards.size(); ++i) {
        const QJsonObject b = boards.at(i).toObject();
        boardTable_->setItem(i, 0, cell(b.value("name").toString(), i));
        boardTable_->setItem(i, 1, numberCell(b.value("entries").toDouble()));
        const quint64 best = b.value("best").toString().toULongLong();
        auto* bestCell = scoreCell(best);
        bestCell->setData(Qt::DisplayRole, scoreValue(best));
        bestCell->setData(Qt::UserRole + 1, best);
        boardTable_->setItem(i, 2, bestCell);
        boardTable_->setItem(i, 3, cell(when(b.value("updated").toDouble())));
        const QString kind = b.value("kind").toString();
        const QString kindText = kind == QLatin1String("event_history")     ? tr("event history")
                                 : kind == QLatin1String("challenge_list")  ? tr("challenge list")
                                 : kind == QLatin1String("challenge_board") ? tr("challenge times")
                                                                            : tr("leaderboard");
        auto* kindCell = cell(kindText);
        kindCell->setData(Qt::UserRole + 2, kind);
        boardTable_->setItem(i, 4, kindCell);
        if (b.value("name").toString() == selected) {
            reselect = i;
        }
    }
    boardTable_->setSortingEnabled(true);
    boardTable_->resizeColumnsToContents();
    if (reselect >= 0) {
        for (int row = 0; row < boardTable_->rowCount(); ++row) {
            if (boardTable_->item(row, 0)->data(Qt::UserRole).toInt() == reselect) {
                boardTable_->selectRow(row);
            }
        }
    }
    boardTable_->blockSignals(false);
    applyBoardFilter();
    loadBoardEntries();
    updateButtons();
}

void MainWindow::applyBoardFilter() {
    const QString wanted = boardFilter_->currentData().toString();
    for (int row = 0; row < boardTable_->rowCount(); ++row) {
        const QString kind = boardTable_->item(row, 4)->data(Qt::UserRole + 2).toString();
        const bool show =
            wanted.isEmpty() || kind == wanted ||
            (wanted == QLatin1String("challenge") && kind.startsWith(QLatin1String("challenge")));
        boardTable_->setRowHidden(row, !show);
    }
}

QString MainWindow::playerName(const QString& accountId) const {
    for (const QJsonValue& v : players_) {
        const QJsonObject p = v.toObject();
        if (p.value("account_id").toString() == accountId) {
            return p.value("name").toString();
        }
    }
    return accountId;
}

QString MainWindow::selectedBoard() const {
    const int i = selectedIndex(boardTable_);
    return i < 0 ? QString() : boards_.at(i).toObject().value("name").toString();
}

void MainWindow::loadBoardEntries() {
    const QString board = selectedBoard();
    if (board.isEmpty()) {
        boardTitle_->setText(tr("Select a board"));
        entries_ = {};
        entryTable_->setRowCount(0);
        return;
    }
    api_.get(QStringLiteral("/api/leaderboards/entries?board=%1").arg(enc(board)),
             [this, board](const ApiClient::Reply& r) {
                 if (board != selectedBoard()) {
                     return;
                 }
                 boardTitle_->setText(tr("<b>%1</b>").arg(board.toHtmlEscaped()));
                 if (r.ok()) {
                     showEntries(r.json().array());
                 } else {
                     entryTable_->setRowCount(0);
                 }
             });
}

void MainWindow::showEntries(const QJsonArray& entries) {
    entries_ = entries;
    entryTable_->setSortingEnabled(false);
    entryTable_->setRowCount(int(entries.size()));
    for (int i = 0; i < entries.size(); ++i) {
        const QJsonObject e = entries.at(i).toObject();
        auto* rank = numberCell(e.value("rank").toDouble());
        rank->setData(Qt::UserRole, i);
        entryTable_->setItem(i, 0, rank);
        entryTable_->setItem(i, 1, cell(e.value("entrant").toString()));
        const quint64 score = e.value("score").toString().toULongLong();
        entryTable_->setItem(i, 2, scoreCell(score));
        entryTable_->setItem(i, 3, cell(scoreValue(score)));
        QStringList attachment;
        for (const QJsonValue& v : e.value("attachment").toArray()) {
            attachment << v.toString();
        }
        entryTable_->setItem(i, 4, cell(attachment.join(QStringLiteral(", "))));
        entryTable_->setItem(i, 5, cell(when(e.value("updated").toDouble())));
    }
    entryTable_->setSortingEnabled(true);
    entryTable_->resizeColumnsToContents();
    updateButtons();
}

void MainWindow::appendTraffic(const QJsonArray& requests) {
    for (const QJsonValue& v : requests) {
        const QJsonObject r = v.toObject();
        lastRequestId_ = std::max(lastRequestId_, r.value("id").toDouble());
        const int row = trafficTable_->rowCount();
        trafficTable_->insertRow(row);
        const int status = r.value("status").toInt();
        trafficTable_->setItem(row, 0, numberCell(r.value("id").toDouble()));
        trafficTable_->setItem(
            row, 1,
            cell(QDateTime::fromMSecsSinceEpoch(qint64(r.value("time").toDouble()))
                     .toString(QStringLiteral("hh:mm:ss.zzz"))));
        trafficTable_->setItem(row, 2, cell(r.value("method").toString()));
        const QString label = r.value("label").toString();
        trafficTable_->setItem(row, 3, cell(label.isEmpty() ? tr("(unknown)") : label));
        const bool error = status >= 400 && !r.value("expected").toBool();
        auto* statusItem = cell(QString::number(status));
        statusItem->setData(Qt::UserRole + 1, error);
        if (error) {
            statusItem->setForeground(QColor(0xc0, 0x20, 0x20));
        } else if (status >= 400) {
            statusItem->setForeground(QColor(0x80, 0x80, 0x80));
            statusItem->setToolTip(tr("Expected answer, not an error (for a gamestate: nothing "
                                      "stored yet, the game uses its defaults)"));
        }
        trafficTable_->setItem(row, 4, statusItem);
        trafficTable_->setItem(row, 5, numberCell(r.value("ms").toDouble()));
        trafficTable_->setItem(row, 6, numberCell(r.value("bytes_in").toDouble()));
        trafficTable_->setItem(row, 7, numberCell(r.value("bytes_out").toDouble()));
        trafficTable_->setItem(row, 8, cell(r.value("path").toString()));
        trafficTable_->setRowHidden(row, errorsOnly_->isChecked() && !error);
    }
    while (trafficTable_->rowCount() > MaxTrafficRows) {
        trafficTable_->removeRow(0);
    }
    if (!requests.isEmpty()) {
        trafficTable_->resizeColumnsToContents();
        trafficTable_->scrollToBottom();
    }
}

void MainWindow::kickSession() {
    const int i = selectedIndex(sessionTable_);
    if (i < 0) {
        return;
    }
    const QJsonObject s = sessions_.at(i).toObject();
    if (QMessageBox::question(this, tr("Kick player"),
                              tr("Drop the EGS session of %1? The game has to log in again.")
                                  .arg(s.value("name").toString())) != QMessageBox::Yes) {
        return;
    }
    api_.remove(QStringLiteral("/api/sessions/%1").arg(enc(s.value("key").toString())),
                [this](const ApiClient::Reply& r) {
                    if (!r.ok()) {
                        reportError(tr("Kick"), r);
                        return;
                    }
                    setStatus(tr("Session dropped"));
                    refresh();
                });
}

void MainWindow::setFame() {
    const int i = selectedIndex(playerTable_);
    if (i < 0) {
        return;
    }
    const QJsonObject p = players_.at(i).toObject();
    bool ok = false;
    const int xp =
        QInputDialog::getInt(this, tr("Set Fame"),
                             tr("Fame of %1 (account %2):")
                                 .arg(p.value("name").toString(), p.value("account_id").toString()),
                             p.value("fame").toInt(), 0, std::numeric_limits<int>::max(), 1, &ok);
    if (!ok) {
        return;
    }
    const QJsonObject body{{QStringLiteral("account_id"), p.value("account_id").toString()},
                           {QStringLiteral("xp"), xp}};
    api_.post(QStringLiteral("/api/players/fame"), QJsonDocument(body),
              [this, p, xp](const ApiClient::Reply& r) {
                  if (!r.ok()) {
                      reportError(tr("Set Fame"), r);
                      return;
                  }
                  setStatus(tr("Fame of %1 set to %L2").arg(p.value("name").toString()).arg(xp));
                  refresh();
              });
}

void MainWindow::resetCounter() {
    const int i = selectedIndex(counterTable_);
    if (i < 0) {
        return;
    }
    const QString resource = counters_.at(i).toObject().value("resource").toString();
    if (QMessageBox::question(this, tr("Reset counters"),
                              tr("Delete the counters of %1? The game starts again from its "
                                 "defaults.")
                                  .arg(resource)) != QMessageBox::Yes) {
        return;
    }
    api_.remove(QStringLiteral("/api/gamestates?resource=%1").arg(enc(resource)),
                [this](const ApiClient::Reply& r) {
                    if (!r.ok()) {
                        reportError(tr("Reset counters"), r);
                        return;
                    }
                    setStatus(tr("Counters reset"));
                    refresh();
                });
}

void MainWindow::revokeToken() {
    const int i = selectedIndex(tokenTable_);
    if (i < 0) {
        return;
    }
    const QJsonObject t = tokens_.at(i).toObject();
    api_.remove(QStringLiteral("/api/otg/tokens/%1").arg(enc(t.value("token").toString())),
                [this](const ApiClient::Reply& r) {
                    if (!r.ok()) {
                        reportError(tr("Revoke"), r);
                        return;
                    }
                    setStatus(tr("Token revoked"));
                    refresh();
                });
}

void MainWindow::addEntries() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Add leaderboard entries"));
    auto* form = new QFormLayout(&dialog);

    auto* board = new QComboBox;
    board->setEditable(true);
    QStringList names;
    for (const QJsonValue& b : boards_) {
        names << b.toObject().value("name").toString();
    }
    for (const QString& known : {QStringLiteral("fame/player"), QStringLiteral("fame/club"),
                                 QStringLiteral("challenges/community/player"),
                                 QStringLiteral("challenges/community/club")}) {
        if (!names.contains(known)) {
            names << known;
        }
    }
    board->addItems(names);
    const QString current = selectedBoard();
    board->setCurrentText(current.isEmpty() ? QStringLiteral("fame/player") : current);
    form->addRow(tr("Board:"), board);

    auto* entrant = new QLineEdit(QStringLiteral("1000000001"));
    entrant->setToolTip(tr("The game uses numeric account ids. With several entries, a numeric id "
                           "counts up; any other text gets 1, 2, 3... appended."));
    form->addRow(tr("Entrant:"), entrant);

    auto* name = new QLineEdit(QStringLiteral("Test Driver"));
    name->setToolTip(tr("Name the game shows for these entries (it asks the server for names). "
                        "With several entries, 1, 2, 3... is appended. Leave empty to keep "
                        "existing names."));
    form->addRow(tr("Name:"), name);

    auto* kind = new QComboBox;
    kind->addItem(tr("Points (fame)"));
    kind->addItem(tr("Time (m:ss.mmm)"));
    kind->addItem(tr("Drift points (common/drift/..., drift events)"));
    form->addRow(tr("Kind:"), kind);

    auto* value = new QLineEdit(QStringLiteral("50000"));
    form->addRow(tr("Best value:"), value);
    connect(kind, &QComboBox::currentIndexChanged, &dialog, [value](int k) {
        value->setText(k == 1 ? QStringLiteral("3:30.000") : QStringLiteral("50000"));
    });

    auto* count = new QSpinBox;
    count->setRange(1, 200);
    count->setValue(10);
    count->setToolTip(tr("Each further entry is a little worse than the one before."));
    form->addRow(tr("Entries:"), count);

    auto* attachment = new QLineEdit;
    attachment->setPlaceholderText(
        tr("optional, e.g. veh:761,wet:0.000000,s0:0,s1:0,s2:0,ghost:-1,otherID:0"));
    form->addRow(tr("Attachment:"), attachment);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const QString boardName = board->currentText().trimmed();
    const QString first = entrant->text().trimmed();
    const bool isTime = kind->currentIndex() == 1;
    bool ok = false;
    const qint64 best = isTime ? parseTime(value->text()) : value->text().trimmed().toLongLong(&ok);
    if (boardName.isEmpty() || first.isEmpty() || best < 0 || (!isTime && !ok)) {
        setStatus(tr("Add entries: board, entrant and a valid value are needed"), true);
        return;
    }
    const int n = count->value();
    bool numeric = false;
    const qulonglong base = first.toULongLong(&numeric);
    const qint64 step = isTime ? 1500 : std::max<qint64>(1, best / 20);

    auto remaining = std::make_shared<int>(n);
    auto failed = std::make_shared<int>(0);
    for (int i = 0; i < n; ++i) {
        const QString id = n == 1    ? first
                           : numeric ? QString::number(base + qulonglong(i))
                                     : first + QString::number(i + 1);
        QJsonObject body{{QStringLiteral("entrant"), id},
                         {QStringLiteral("attachment"), attachment->text()}};
        const QString shownName = name->text().trimmed();
        if (!shownName.isEmpty()) {
            body.insert(QStringLiteral("name"),
                        n == 1 ? shownName : QStringLiteral("%1 %2").arg(shownName).arg(i + 1));
        }
        if (isTime) {
            body.insert(QStringLiteral("time_ms"), best + i * step);
        } else if (kind->currentIndex() == 2) {
            const quint64 points = quint64(std::max<qint64>(0, best - i * step));
            body.insert(QStringLiteral("score"), QString::number((quint64(1) << 60) | points));
        } else {
            body.insert(QStringLiteral("points"), std::max<qint64>(0, best - i * step));
        }
        api_.post(QStringLiteral("/api/leaderboards/entries?board=%1").arg(enc(boardName)),
                  QJsonDocument(body),
                  [this, remaining, failed, n, boardName](const ApiClient::Reply& r) {
                      if (!r.ok()) {
                          ++*failed;
                          reportError(tr("Add entry"), r);
                      }
                      if (--*remaining == 0) {
                          if (*failed == 0) {
                              setStatus(tr("Added %n entries to %1", nullptr, n).arg(boardName));
                          }
                          refresh();
                      }
                  });
    }
}

void MainWindow::removeEntry() {
    const QString board = selectedBoard();
    const int i = selectedIndex(entryTable_);
    if (board.isEmpty() || i < 0) {
        return;
    }
    const QString entrant = entries_.at(i).toObject().value("entrant").toString();
    if (QMessageBox::question(this, tr("Remove entry"),
                              tr("Remove %1 from %2?").arg(entrant, board)) != QMessageBox::Yes) {
        return;
    }
    api_.remove(QStringLiteral("/api/leaderboards/entries?board=%1&entrant=%2")
                    .arg(enc(board), enc(entrant)),
                [this](const ApiClient::Reply& r) {
                    if (!r.ok()) {
                        reportError(tr("Remove entry"), r);
                        return;
                    }
                    setStatus(tr("Entry removed"));
                    refresh();
                });
}

void MainWindow::clearBoard() {
    const QString board = selectedBoard();
    if (board.isEmpty()) {
        return;
    }
    if (QMessageBox::warning(this, tr("Clear board"), tr("Delete every entry of %1?").arg(board),
                             QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    api_.remove(QStringLiteral("/api/leaderboards?board=%1").arg(enc(board)),
                [this](const ApiClient::Reply& r) {
                    if (!r.ok()) {
                        reportError(tr("Clear board"), r);
                        return;
                    }
                    setStatus(tr("Board cleared"));
                    refresh();
                });
}

void MainWindow::updateButtons() {
    refresh_->setEnabled(connected_);
    kick_->setEnabled(connected_ && selectedIndex(sessionTable_) >= 0);
    revoke_->setEnabled(connected_ && selectedIndex(tokenTable_) >= 0);
    clearBoard_->setEnabled(connected_ && !selectedBoard().isEmpty());
    removeEntry_->setEnabled(connected_ && selectedIndex(entryTable_) >= 0);
    addEntry_->setEnabled(connected_);
    setFame_->setEnabled(connected_ && selectedIndex(playerTable_) >= 0);
    resetCounter_->setEnabled(connected_ && selectedIndex(counterTable_) >= 0);
    saveGhost_->setEnabled(connected_ && selectedIndex(ghostTable_) >= 0);
    deleteGhost_->setEnabled(connected_ && selectedIndex(ghostTable_) >= 0);
    deleteUnused_->setEnabled(connected_ && !ghosts_.isEmpty());
}

void MainWindow::setStatus(const QString& text, bool error) {
    statusBar()->setStyleSheet(error ? QStringLiteral("color: #c02020;") : QString());
    statusBar()->showMessage(text);
}

void MainWindow::reportError(const QString& action, const ApiClient::Reply& reply) {
    if (reply.status == 401) {
        connected_ = false;
        timer_->stop();
        trafficTimer_->stop();
        updateButtons();
    }
    setStatus(tr("%1 failed: %2").arg(action, reply.error), true);
}
