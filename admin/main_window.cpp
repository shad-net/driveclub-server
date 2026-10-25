// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "main_window.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
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
#include <QTimer>
#include <QToolBar>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>

namespace {

constexpr int MaxTrafficRows = 2000;

// ms since epoch -> local time
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
        return QStringLiteral("%L1").arg(value * 1000);
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
    setWindowTitle(tr("Driveclub Admin"));
    resize(1100, 700);

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
    sessionTable_ =
        makeTable({tr("Name"), tr("Verified"), tr("shadNet account"), tr("Account ID"),
                   tr("Client"), tr("PS Plus"), tr("Signed in"), tr("Last seen"), tr("Requests")});
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

    connect(sessionTable_, &QTableWidget::itemSelectionChanged, this, &MainWindow::updateButtons);
    connect(tokenTable_, &QTableWidget::itemSelectionChanged, this, &MainWindow::updateButtons);
    connect(kick_, &QPushButton::clicked, this, &MainWindow::kickSession);
    connect(revoke_, &QPushButton::clicked, this, &MainWindow::revokeToken);
    return page;
}

QWidget* MainWindow::buildLeaderboardsTab() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    auto* splitter = new QSplitter(Qt::Horizontal);

    boardTable_ = makeTable({tr("Board"), tr("Entries"), tr("Best"), tr("Updated")});
    splitter->addWidget(boardTable_);

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
    errorsOnly_ = new QCheckBox(tr("Errors only (status 400 and up)"));
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
            const int status = trafficTable_->item(row, 4)->text().toInt();
            trafficTable_->setRowHidden(row, on && status < 400);
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
    server_->setText(tr("<b>driveclub-server %1</b>, up %2, game port %3")
                         .arg(s.value("version").toString(),
                              duration(s.value("uptime").toInteger()),
                              s.value("listen").toString()));
    const auto add = [this](const QString& label, const QString& value) {
        auto* v = new QLabel(value);
        v->setTextInteractionFlags(Qt::TextSelectableByMouse);
        overviewForm_->addRow(label, v);
    };
    add(tr("Signed-in players (EGS):"), QString::number(s.value("sessions").toInteger()));
    add(tr("OTG tokens:"), QString::number(s.value("otg_tokens").toInteger()));
    add(tr("Leaderboards:"), QString::number(s.value("leaderboards").toInteger()));
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
    loadBoardEntries();
    updateButtons();
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
        // Scores are 64-bit; keep the exact text but sort numerically.
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
        auto* statusItem = cell(QString::number(status));
        if (status >= 400) {
            statusItem->setForeground(QColor(0xc0, 0x20, 0x20));
        }
        trafficTable_->setItem(row, 4, statusItem);
        trafficTable_->setItem(row, 5, numberCell(r.value("ms").toDouble()));
        trafficTable_->setItem(row, 6, numberCell(r.value("bytes_in").toDouble()));
        trafficTable_->setItem(row, 7, numberCell(r.value("bytes_out").toDouble()));
        trafficTable_->setItem(row, 8, cell(r.value("path").toString()));
        trafficTable_->setRowHidden(row, errorsOnly_->isChecked() && status < 400);
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

// Adds one entry, or a run of test entries, to any board (also boards the game has not written
// yet, such as fame/player), so the game's leaderboard screens can be checked without racing.
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
    kind->addItem(tr("Points (fame, drift, score)"));
    kind->addItem(tr("Time (m:ss.mmm)"));
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
