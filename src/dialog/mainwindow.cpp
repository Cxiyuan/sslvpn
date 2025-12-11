/*
 * Copyright (C) 2014, 2015 Red Hat
 *
 * This file is part of avpn.
 *
 * avpn is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mainwindow.h"
#include "NewProfileDialog.h"
#include "config.h"
#include "editdialog.h"
#include "logdialog.h"
#include "avpn.h"
#include "server_storage.h"
#include "timestamp.h"
#include "ui_mainwindow.h"
#include "vpninfo.h"

#include "logger.h"

extern "C" {
#include <gnutls/gnutls.h>
}
#include <spdlog/spdlog.h>

#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QEventTransition>
#include <QFile>
#include <QFileDialog>
#include <QFileSelector>
#include <QFutureWatcher>
#include <QLineEdit>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalTransition>
#include <QStateMachine>
#include <QThread>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QNetworkProxyFactory>
#include <QtNetwork/QNetworkProxyQuery>

#include <cmath>
#include <cstdarg>
#include <cstdio>

#ifdef _WIN32
#define pipe_write(x, y, z) send(x, y, z, 0)
#else
#define pipe_write(x, y, z) write(x, y, z)
#endif

MainWindow::MainWindow(QWidget* parent, const QString profileName)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , zerotierProcess(nullptr)
    , sdwanEnabled(false)
    , sdwanAutoStart(false)
    , sdwanConnected(false)
    , sdwanState(SDWAN_IDLE)
{
    ui->setupUi(this);

    connect(ui->viewLogButton, &QPushButton::clicked,
        this, &MainWindow::createLogDialog);

    connect(ui->sdwanEnableCheckbox, &QCheckBox::toggled,
        this, &MainWindow::sdwanEnableToggled);
    connect(ui->sdwanAutoStartCheckbox, &QCheckBox::toggled,
        this, &MainWindow::sdwanAutoStartToggled);
    connect(ui->nodeLicenseImportButton, &QPushButton::clicked,
        this, &MainWindow::nodeLicenseImport);
    connect(ui->sdwanStartButton, &QPushButton::clicked,
        this, &MainWindow::sdwanStartClicked);
    
    connect(this, &MainWindow::sdwanZeroTierStarted, this, &MainWindow::onZeroTierStarted);
    connect(this, &MainWindow::sdwanZeroTierStartFailed, this, &MainWindow::onZeroTierStartFailed);
    connect(this, &MainWindow::sdwanNetworkJoined, this, &MainWindow::onNetworkJoined);
    connect(this, &MainWindow::sdwanNetworkJoinFailed, this, &MainWindow::onNetworkJoinFailed);
    connect(this, &MainWindow::sdwanNetworkLeft, this, &MainWindow::onNetworkLeft);
    connect(this, &MainWindow::sdwanNodeIdRetrieved, this, &MainWindow::onNodeIdRetrieved);

    timer = new QTimer(this);
    blink_timer = new QTimer(this);
    this->cmd_fd = INVALID_SOCKET;

    connect(ui->actionQuit, &QAction::triggered,
        [=]() {
            if (m_disconnectAction->isEnabled()) {
                connect(this, &MainWindow::readyToShutdown,
                    qApp, &QApplication::quit);
                on_disconnectClicked();
            } else {
                qApp->quit();
            }
        });

    connect(blink_timer, &QTimer::timeout,
        this, &MainWindow::blink_ui,
        Qt::QueuedConnection);
    connect(timer, &QTimer::timeout,
        this, &MainWindow::request_update_stats,
        Qt::QueuedConnection);
    connect(this, &MainWindow::vpn_status_changed_sig,
        this, &MainWindow::changeStatus,
        Qt::QueuedConnection);
    connect(ui->connectionButton, &QPushButton::clicked,
        this, &MainWindow::on_connectClicked,
        Qt::QueuedConnection);
    connect(this, &MainWindow::stats_changed_sig,
        this, &MainWindow::statsChanged,
        Qt::QueuedConnection);

    ui->iconLabel->setPixmap(OFF_ICON);
    QNetworkProxyFactory::setUseSystemConfiguration(true);

    ui->cipherCSTPLabelText->setVisible(true);
    ui->cipherCSTPLabel->setVisible(true);

    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        createTrayIcon();

        connect(m_trayIcon, &QSystemTrayIcon::activated,
            this, &MainWindow::iconActivated);

        QFileSelector selector;
        QIcon icon(selector.select(QStringLiteral(":/images/favicon.ico")));
        if (icon.isNull()) {
            Logger::instance().addMessage(QLatin1String("Failed to load tray icon from resources"));
            icon = QIcon(":/images/favicon.ico");
        }
        icon.setIsMask(false);
        m_trayIcon->setIcon(icon);
        m_trayIcon->setVisible(true);
        m_trayIcon->show();
        Logger::instance().addMessage(QLatin1String("Tray icon created and shown"));
    } else {
        Logger::instance().addMessage(QLatin1String("System doesn't support tray icon"));
        m_trayIcon = nullptr;
    }

    // TODO: initial state machine
    QStateMachine* machine = new QStateMachine(this);
    QState* s1_noProfiles = new QState();
    s1_noProfiles->assignProperty(ui->connectionButton, "enabled", false);
    s1_noProfiles->assignProperty(ui->serverList, "enabled", false);
    s1_noProfiles->assignProperty(ui->actionEditSelectedProfile, "enabled", false);
    s1_noProfiles->assignProperty(ui->actionRemoveSelectedProfile, "enabled", false);

    s1_noProfiles->assignProperty(m_trayIconMenuConnections, "title", tr("(没有可连接的服务器)"));
    s1_noProfiles->assignProperty(m_trayIconMenuConnections, "enabled", false);
    s1_noProfiles->assignProperty(m_disconnectAction, "enabled", false);
    machine->addState(s1_noProfiles);

    QState* s2_connectionReady = new QState();
    s2_connectionReady->assignProperty(ui->connectionButton, "enabled", true);
    s2_connectionReady->assignProperty(ui->serverList, "enabled", true);
    s2_connectionReady->assignProperty(ui->actionEditSelectedProfile, "enabled", true);
    s2_connectionReady->assignProperty(ui->actionRemoveSelectedProfile, "enabled", true);

    s2_connectionReady->assignProperty(m_trayIconMenuConnections, "title", tr("连接到..."));
    s2_connectionReady->assignProperty(m_trayIconMenuConnections, "enabled", true);
    machine->addState(s2_connectionReady);

    class ServerListTransition : public QSignalTransition {
    public:
        ServerListTransition(QComboBox* cb, bool hasServers)
            : QSignalTransition(cb, SIGNAL(currentIndexChanged(int)))
            , hasServers(hasServers)
        {
        }

    protected:
        bool eventTest(QEvent* e)
        {
            if (!QSignalTransition::eventTest(e)) {
                return false;
            }
            QStateMachine::SignalEvent* se = static_cast<QStateMachine::SignalEvent*>(e);
            bool isEmpty = se->arguments().at(0).toInt() == -1;
            return (hasServers ? !isEmpty : isEmpty);
        }

    private:
        bool hasServers;
    };

    ServerListTransition* t1 = new ServerListTransition(ui->serverList, true);
    t1->setTargetState(s2_connectionReady);
    s1_noProfiles->addTransition(t1);

    ServerListTransition* t2 = new ServerListTransition(ui->serverList, false);
    t2->setTargetState(s1_noProfiles);
    s2_connectionReady->addTransition(t2);

    machine->setInitialState(s1_noProfiles);
    machine->start();
    connect(machine, &QStateMachine::started, [=]() {
        // LCA: find better way to load/fill combobox...
        this->reload_settings();

        if (!profileName.isEmpty()) {
            // TODO: better place when refactor SM...
            const int profileIndex = ui->serverList->findText(profileName);
            if (profileIndex != -1) {
                ui->serverList->setCurrentIndex(profileIndex);
                emit on_connectClicked();
                return;
            } else {
                QMessageBox::warning(this,
                    tr("连接失败"),
                    tr("所选VPN配置 '<b>%1</b>' 不存在。").arg(profileName));
            }
        }

        QSettings settings;
        const int currentIndex = settings.value("Profiles/currentIndex", -1).toInt();
        if (currentIndex != -1 && currentIndex < ui->serverList->count()) {
            ui->serverList->setCurrentIndex(currentIndex);
        }
    });

    QMenu* serverProfilesMenu = new QMenu(this);
    serverProfilesMenu->addAction(ui->actionNewProfile);
    serverProfilesMenu->addAction(ui->actionNewProfileAdvanced);
    serverProfilesMenu->addAction(ui->actionEditSelectedProfile);
    serverProfilesMenu->addAction(ui->actionRemoveSelectedProfile);
    ui->serverListControl->setMenu(serverProfilesMenu);

    readSettings();
    // TODO: initial app window state machine
    m_appWindowStateMachine = new QStateMachine(this);

    QState* s111_normalWindow = new QState();
    m_appWindowStateMachine->addState(s111_normalWindow);
    s111_normalWindow->assignProperty(ui->actionRestore, "enabled", false);
    s111_normalWindow->assignProperty(ui->actionMinimize, "enabled", true);

    QState* s112_minimizedWindow = new QState();
    m_appWindowStateMachine->addState(s112_minimizedWindow);
    connect(s112_minimizedWindow, &QState::entered, [=]() {
        showMinimized();
        if (ui->actionMinimizeToTheNotificationArea->isChecked()) {
            QTimer::singleShot(10, this, SLOT(hide()));
        }
    });
    connect(s112_minimizedWindow, &QState::exited, [=]() {
        this->showNormal();
        if (ui->actionMinimizeToTheNotificationArea->isChecked()) {
            show();
            raise();
            activateWindow();
        }
    });
    s112_minimizedWindow->assignProperty(ui->actionRestore, "enabled", true);
    s112_minimizedWindow->assignProperty(ui->actionMinimize, "enabled", false);

    if (ui->actionStartMinimized->isChecked()) {
        m_appWindowStateMachine->setInitialState(s112_minimizedWindow);
    } else {
        m_appWindowStateMachine->setInitialState(s111_normalWindow);
    }

    // TODO: move outside...
    class MinimizeEventTransition : public QEventTransition {
    public:
        MinimizeEventTransition(QMainWindow* mw, Qt::WindowState state)
            : QEventTransition(mw, QEvent::WindowStateChange)
            , m_mw(mw)
            , m_state(state)
        {
        }

    protected:
        bool eventTest(QEvent* e) override
        {
            if (!QEventTransition::eventTest(e)) {
                return false;
            }
            QStateMachine::WrappedEvent* we = static_cast<QStateMachine::WrappedEvent*>(e);
            if (we->event()->type() == QEvent::WindowStateChange) {
                return (m_mw->windowState() == m_state);
            }
            return false;
        }

    private:
        QMainWindow* m_mw;
        Qt::WindowState m_state;
    };
    // TODO: move outside...
    class RestoreEventTransition : public QEventTransition {
    public:
        RestoreEventTransition(QMainWindow* mw, Qt::WindowState state)
            : QEventTransition(mw, QEvent::WindowStateChange)
            , m_mw(mw)
            , m_state(state)
        {
        }

    protected:
        bool eventTest(QEvent* e) override
        {
            if (!QEventTransition::eventTest(e)) {
                return false;
            }
            QStateMachine::WrappedEvent* we = static_cast<QStateMachine::WrappedEvent*>(e);
            if (we->event()->type() == QEvent::WindowStateChange) {
                return (m_mw->windowState() == m_state);
            }
            return false;
        }

    private:
        QMainWindow* m_mw;
        Qt::WindowState m_state;
    };

    MinimizeEventTransition* minimizeEvent = new MinimizeEventTransition(this, Qt::WindowMinimized);
    minimizeEvent->setTargetState(s112_minimizedWindow);
    s111_normalWindow->addTransition(minimizeEvent);

    RestoreEventTransition* restoreEvent = new RestoreEventTransition(this, Qt::WindowNoState);
    restoreEvent->setTargetState(s111_normalWindow);
    s112_minimizedWindow->addTransition(restoreEvent);

    m_appWindowStateMachine->start();
}

static void term_thread(MainWindow* m, SOCKET* fd)
{
    char cmd = OC_CMD_CANCEL;

    if (*fd != INVALID_SOCKET) {
        m->vpn_status_changed(STATUS_DISCONNECTING);
        int ret = pipe_write(*fd, &cmd, 1);
        if (ret < 0) {
            Logger::instance().addMessage(QObject::tr("term_thread: IPC error: ") + QString::number(net_errno));
        }
        *fd = INVALID_SOCKET;
        ms_sleep(200);
    } else {
        m->vpn_status_changed(STATUS_DISCONNECTED);
    }
}

MainWindow::~MainWindow()
{
    int counter = 10;
    if (this->timer->isActive()) {
        timer->stop();
    }

    if (zerotierProcess) {
        if (!lastJoinedNetworkId.isEmpty()) {
            Logger::instance().addMessage(tr("退出时离开 SD-WAN 网络: %1").arg(lastJoinedNetworkId));
            leaveZeroTierNetworkSync(lastJoinedNetworkId);
        }
        
        terminateZeroTierProcess();
    }

    {
        QMutexLocker locker(&cmd_fd_mutex);
        if (this->futureWatcher.isRunning() == true) {
            term_thread(this, &this->cmd_fd);
        }
    }
    while (this->futureWatcher.isRunning() == true && counter > 0) {
        ms_sleep(200);
        counter--;
    }

    writeSettings();

    if (m_appWindowStateMachine) {
        delete m_appWindowStateMachine;
        m_appWindowStateMachine = nullptr;
    }

    delete ui;
    delete timer;
    delete blink_timer;
}

void MainWindow::vpn_status_changed(int connected)
{
    emit vpn_status_changed_sig(connected);
}

void MainWindow::vpn_status_changed(int connected, QString& dns, QString& ip, QString& ip6, QString& cstp_cipher, QString& dtls_cipher)
{
    this->dns = dns;
    this->ip = ip;
    this->ip6 = ip6;
    this->dtls_cipher = dtls_cipher;
    this->cstp_cipher = cstp_cipher;

    emit vpn_status_changed_sig(connected);
}

QString MainWindow::normalize_byte_size(uint64_t bytes)
{
    const unsigned unit = 1024; // TODO: add support for SI units? (optional)
    if (bytes < unit) {
        return QString("%1 B").arg(QString::number(bytes));
    }
    const int exp = static_cast<int>(std::log(bytes) / std::log(unit));
    static const char suffixChar[] = "KMGTPE";
    return QString("%1 %2B").arg(QString::number(bytes / std::pow(unit, exp), 'f', 3)).arg(suffixChar[exp - 1]);
}

void MainWindow::statsChanged(QString tx, QString rx, QString dtls)
{
    ui->downloadLabel->setText(rx);
    ui->uploadLabel->setText(tx);
}

void MainWindow::updateStats(const struct oc_stats* stats, QString dtls)
{
    emit stats_changed_sig(
        normalize_byte_size(stats->tx_bytes),
        normalize_byte_size(stats->rx_bytes),
        dtls);
}

#define PREFIX "server:" // LCA: remot this...
void MainWindow::reload_settings()
{
    ui->serverList->clear();
    m_trayIconMenuConnections->clear();

    QSettings settings;
    for (const auto& key : settings.allKeys()) {
        if (key.startsWith(PREFIX) && key.endsWith("/server")) {
            QString str{ key };
            str.remove(0, sizeof(PREFIX) - 1); /* remove prefix */
            str.remove(str.size() - 7, 7); /* remove /server suffix */
            ui->serverList->addItem(str);

            QAction* act = m_trayIconMenuConnections->addAction(str);
            connect(act, &QAction::triggered, [act, this]() {
                int idx = ui->serverList->findText(act->text());
                if (idx != -1) {
                    ui->serverList->setCurrentIndex(idx);
                    on_connectClicked();
                }
            });
        }
    }
}

void MainWindow::blink_ui()
{
    static unsigned t = 1;

    if (t % 2 == 0) {
        ui->iconLabel->setPixmap(CONNECTING_ICON);
    } else {
        ui->iconLabel->setPixmap(CONNECTING_ICON2);
    }
    ++t;
}

void MainWindow::changeStatus(int val)
{
    if (val == STATUS_CONNECTED) {

        blink_timer->stop();

        ui->serverList->setEnabled(false);

        m_trayIconMenuConnections->setEnabled(false);
        m_disconnectAction->setEnabled(true);

        ui->iconLabel->setPixmap(ON_ICON);
        ui->connectionButton->setIcon(QIcon(":/images/process-stop.png"));
        ui->connectionButton->setText(tr("断开"));

        QFileSelector selector;
        QIcon icon(selector.select(QStringLiteral(":/images/network-connected.png")));
        icon.setIsMask(true);
        m_trayIcon->setIcon(icon);

        this->ui->ipV4Label->setText(ip);
        this->ui->cipherCSTPLabel->setText(cstp_cipher);

        timer->start(UPDATE_TIMER);

        if (this->minimize_on_connect) {
            if (m_trayIcon) {
                hide();
                m_trayIcon->showMessage(QLatin1String("Connected"), QLatin1String("You were connected to ") + ui->serverList->currentText(),
                    QSystemTrayIcon::Information,
                    10000);
            } else {
                this->setWindowState(Qt::WindowMinimized);
            }
        }
    } else if (val == STATUS_CONNECTING) {

        if (m_trayIcon) {
            QFileSelector selector;
            QIcon icon(selector.select(QStringLiteral(":/images/network-disconnected.png")));
            icon.setIsMask(true);
            m_trayIcon->setIcon(icon);
        }

        ui->serverList->setEnabled(false);

        m_trayIconMenuConnections->setEnabled(false);
        m_disconnectAction->setEnabled(true);

        ui->iconLabel->setPixmap(CONNECTING_ICON);
        ui->connectionButton->setIcon(QIcon(":/images/process-stop.png"));
        ui->connectionButton->setText(tr("取消"));
        blink_timer->start(1500);

        disconnect(ui->connectionButton, &QPushButton::clicked,
            this, &MainWindow::on_connectClicked);
        connect(ui->connectionButton, &QPushButton::clicked,
            this, &MainWindow::on_disconnectClicked,
            Qt::QueuedConnection);
    } else if (val == STATUS_DISCONNECTED) {
        blink_timer->stop();
        if (this->timer->isActive()) {
            timer->stop();
        }
        {
            QMutexLocker locker(&cmd_fd_mutex);
            cmd_fd = INVALID_SOCKET;
        }

        ui->ipV4Label->clear();
        ui->uploadLabel->clear();
        ui->downloadLabel->clear();
        ui->cipherCSTPLabel->clear();
        Logger::instance().addMessage(QObject::tr("Disconnected"));

        ui->serverList->setEnabled(true);

        m_trayIconMenuConnections->setEnabled(true);
        m_disconnectAction->setEnabled(false);

        ui->iconLabel->setPixmap(OFF_ICON);
        ui->connectionButton->setEnabled(true);
        ui->connectionButton->setIcon(QIcon(":/images/network-wired.png"));
        ui->connectionButton->setText(tr("连接"));

        if (m_trayIcon) {
            QFileSelector selector;
            QIcon icon(selector.select(QStringLiteral(":/images/network-disconnected.png")));
            icon.setIsMask(true);
            m_trayIcon->setIcon(icon);

            if (this->isHidden() == true)
                m_trayIcon->showMessage(QLatin1String("Disconnected"), QLatin1String("You were disconnected from the VPN"),
                    QSystemTrayIcon::Warning,
                    10000);
        }
        disconnect(ui->connectionButton, &QPushButton::clicked,
            this, &MainWindow::on_disconnectClicked);
        connect(ui->connectionButton, &QPushButton::clicked,
            this, &MainWindow::on_connectClicked,
            Qt::QueuedConnection);

        emit readyToShutdown();
    } else if (val == STATUS_DISCONNECTING) {
        ui->iconLabel->setPixmap(CONNECTING_ICON);
        ui->connectionButton->setIcon(QIcon(":/images/process-stop.png"));
        ui->connectionButton->setEnabled(false);
        blink_timer->start(1500);
    } else {
        qDebug() << "TODO: was is das?";
    }
}

static void main_loop(VpnInfo* vpninfo, MainWindow* m)
{
    m->vpn_status_changed(STATUS_CONNECTING);

    bool pass_was_empty;
    pass_was_empty = vpninfo->ss->get_password().isEmpty();

    QString ip, ip6, dns, cstp, dtls;

    int ret = 0;
    bool retry = false;
    int retries = 2;
    do {
        retry = false;
        ret = vpninfo->connect();
        if (ret != 0) {
            if (retries-- <= 0)
                goto fail;

            QString oldpass, oldgroup;
            bool reset_password = false;
            if (pass_was_empty != true) {
                /* authentication failed in batch mode? switch to non
                 * batch and retry */
                oldpass = vpninfo->ss->get_password();
                oldgroup = vpninfo->ss->get_groupname();
                vpninfo->ss->clear_password();
                vpninfo->ss->clear_groupname();
                retry = true;
                reset_password = true;
                Logger::instance().addMessage(QObject::tr("Authentication failed in batch mode, retrying with batch mode disabled"));
                vpninfo->reset_vpn();
                continue;
            }

            /* if we didn't manage to connect on a retry, the failure reason
             * may not have been a changed password, reset it */
            if (reset_password == true) {
                vpninfo->ss->set_password(oldpass);
                vpninfo->ss->set_groupname(oldgroup);
            }

            Logger::instance().addMessage(vpninfo->last_err);
            goto fail;
        }

    } while (retry == true);

    ret = vpninfo->dtls_connect();
    if (ret != 0) {
        Logger::instance().addMessage(vpninfo->last_err);
    }

    vpninfo->get_info(dns, ip, ip6);
    vpninfo->get_cipher_info(cstp, dtls);
    m->vpn_status_changed(STATUS_CONNECTED, dns, ip, ip6, cstp, dtls);

    vpninfo->ss->save();
    vpninfo->mainloop();

fail: // LCA: drop this 'goto' and optimize values...
    m->vpn_status_changed(STATUS_DISCONNECTED);

    delete vpninfo;
}

void MainWindow::on_disconnectClicked()
{
    if (this->timer->isActive()) {
        this->timer->stop();
    }
    Logger::instance().addMessage(QObject::tr("正在断开..."));
    term_thread(this, &this->cmd_fd);
}

void MainWindow::on_connectClicked()
{
    VpnInfo* vpninfo = nullptr;
    StoredServer* ss = new StoredServer();
    QFuture<void> future;
    QString name, url;
    QList<QNetworkProxy> proxies;
    QUrl turl;
    QNetworkProxyQuery query;

    {
        QMutexLocker locker(&cmd_fd_mutex);
        if (this->cmd_fd != INVALID_SOCKET) {
            QMessageBox::information(this,
                qApp->applicationName(),
                tr("先前的VPN实例仍在运行(套接字处于活动状态)"));
            return;
        }
    }

    if (this->futureWatcher.isRunning() == true) {
        QMessageBox::information(this,
            qApp->applicationName(),
            tr("先前的VPN实例仍在运行"));
        return;
    }

    if (ui->serverList->currentText().isEmpty()) {
        QMessageBox::information(this,
            qApp->applicationName(),
            tr("您需要指定一个网关，例如：vpn.example.com:443"));
        return;
    }

    name = ui->serverList->currentText();
    ss->load(name);
    turl.setUrl("https://" + ss->get_servername());
    query.setUrl(turl);

    /* ss is now deallocated by vpninfo */
    try {
        vpninfo = new VpnInfo(QStringLiteral("Open AnyConnect VPN Agent"), ss, this);
    } catch (std::exception& ex) {
        QMessageBox::information(this,
            qApp->applicationName(),
            tr("初始化VPN时出现问题 ") + "(" + ex.what() + ").");
        goto fail;
    }

    this->minimize_on_connect = vpninfo->get_minimize();

    vpninfo->parse_url(ss->get_servername().toLocal8Bit().data());

    {
        QMutexLocker locker(&cmd_fd_mutex);
        this->cmd_fd = vpninfo->get_cmd_fd();
        if (this->cmd_fd == INVALID_SOCKET) {
            QMessageBox::information(this,
                qApp->applicationName(),
                tr("与openconnect建立IPC时出现问题；请尝试重启应用程序。"));
            goto fail;
        }
    }

    if (ss->get_proxy()) {
        proxies = QNetworkProxyFactory::systemProxyForQuery(query);
        if (proxies.size() > 0 && proxies.at(0).type() != QNetworkProxy::NoProxy) {
            if (proxies.at(0).type() == QNetworkProxy::Socks5Proxy)
                url = "socks5://";
            else if (proxies.at(0).type() == QNetworkProxy::HttpCachingProxy
                || proxies.at(0).type() == QNetworkProxy::HttpProxy)
                url = "http://";

            if (url.isEmpty() == false) {

                QString str;
                if (proxies.at(0).user() != 0) {
                    str = proxies.at(0).user() + ":" + proxies.at(0).password() + "@";
                }
                str += proxies.at(0).hostName();
                if (proxies.at(0).port() != 0) {
                    str += ":" + QString::number(proxies.at(0).port());
                }
                Logger::instance().addMessage(tr("设置代理为：") + str);
                // FIXME: ...
                int ret = openconnect_set_http_proxy(vpninfo->vpninfo, str.toLatin1().data());
            }
        }
    }

    future = QtConcurrent::run(main_loop, vpninfo, this);

    this->futureWatcher.setFuture(future);

    return;
fail: // LCA: remote 'fail' label :/
    if (vpninfo != nullptr)
        delete vpninfo;
    return;
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (m_trayIcon && m_trayIcon->isVisible() && ui->actionMinimizeTheApplicationInsteadOfClosing->isChecked()) {
        this->showMinimized();
        event->ignore();
    } else {
        shutdownSDWAN();
        
        event->accept();

        if (m_disconnectAction->isEnabled()) {
            connect(this, &MainWindow::readyToShutdown,
                qApp, &QApplication::quit);
            on_disconnectClicked();
        } else {
            qApp->quit();
        }
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::shutdownSDWAN()
{
    if (!zerotierProcess) {
        return;
    }
    
    if (!lastJoinedNetworkId.isEmpty()) {
        Logger::instance().addMessage(tr("关闭时离开 SD-WAN 网络: %1").arg(lastJoinedNetworkId));
        leaveZeroTierNetworkSync(lastJoinedNetworkId);
        QThread::msleep(500);
    }
    
    terminateZeroTierProcess();
    
    Logger::instance().addMessage(tr("SD-WAN 服务已停止"));
}

void MainWindow::request_update_stats()
{
    char cmd = OC_CMD_STATS;
    QMutexLocker locker(&cmd_fd_mutex);
    if (this->cmd_fd != INVALID_SOCKET) {
        int ret = pipe_write(this->cmd_fd, &cmd, 1);
        if (ret < 0) {
            Logger::instance().addMessage(QObject::tr("update_stats: IPC error: ") + QString::number(net_errno));
            if (this->timer->isActive())
                this->timer->stop();
        }
    } else {
        Logger::instance().addMessage(QObject::tr("update_stats: invalid socket"));
        if (this->timer->isActive())
            this->timer->stop();
    }
}

void MainWindow::readSettings()
{
    QSettings settings;
    settings.beginGroup("MainWindow");
    resize(settings.value("size").toSize());
    if (settings.contains("pos")) {
        move(settings.value("pos").toPoint());
    }
    settings.endGroup();

    settings.beginGroup("Settings");
    ui->actionMinimizeToTheNotificationArea->setChecked(settings.value("minimizeToTheNotificationArea", true).toBool());
    ui->actionMinimizeTheApplicationInsteadOfClosing->setChecked(settings.value("minimizeTheApplicationInsteadOfClosing", true).toBool());
    ui->actionStartMinimized->setChecked(settings.value("startMinimized", false).toBool());
    ui->actionSingleInstanceMode->setChecked(settings.value("singleInstanceMode", true).toBool());
    connect(ui->actionSingleInstanceMode, &QAction::toggled, [](bool checked) {
        QSettings settings;
        settings.setValue("Settings/singleInstanceMode", checked);
    });
    settings.endGroup();

    settings.beginGroup("SDWAN");
    sdwanEnabled = settings.value("enabled", false).toBool();
    sdwanAutoStart = settings.value("autoStart", false).toBool();
    networkId = settings.value("networkId", "").toString();
    
    ui->sdwanEnableCheckbox->setChecked(sdwanEnabled);
    ui->sdwanAutoStartCheckbox->setChecked(sdwanAutoStart);
    ui->networkIdEdit->setText(networkId);
    settings.endGroup();
    
    updateNodeLicenseStatus();
    
    if (sdwanEnabled && sdwanAutoStart && !networkId.isEmpty()) {
        QTimer::singleShot(1000, this, [this]() {
            Logger::instance().addMessage(tr("SD-WAN 自启动..."));
            sdwanStartClicked();
        });
    }
}

void MainWindow::writeSettings()
{
    QSettings settings;
    settings.beginGroup("MainWindow");
    settings.setValue("size", size());
    settings.setValue("pos", pos());
    settings.endGroup();

    settings.beginGroup("Settings");
    settings.setValue("minimizeToTheNotificationArea", ui->actionMinimizeToTheNotificationArea->isChecked());
    settings.setValue("minimizeTheApplicationInsteadOfClosing", ui->actionMinimizeTheApplicationInsteadOfClosing->isChecked());
    settings.setValue("startMinimized", ui->actionStartMinimized->isChecked());
    settings.setValue("singleInstanceMode", ui->actionSingleInstanceMode->isChecked());
    settings.endGroup();

    settings.setValue("Profiles/currentIndex", ui->serverList->currentIndex());

    settings.beginGroup("SDWAN");
    settings.setValue("enabled", sdwanEnabled);
    settings.setValue("autoStart", sdwanAutoStart);
    settings.setValue("networkId", networkId);
    settings.endGroup();
}

void MainWindow::createLogDialog()
{
    auto dialog{ new LogDialog() };

    disconnect(ui->viewLogButton, &QPushButton::clicked,
        this, &MainWindow::createLogDialog);

    connect(ui->viewLogButton, &QPushButton::clicked,
        dialog, &QDialog::show);
    connect(ui->viewLogButton, &QPushButton::clicked,
        dialog, &QDialog::raise);
    connect(ui->viewLogButton, &QPushButton::clicked,
        dialog, &QDialog::activateWindow);

    connect(dialog, &QDialog::finished,
        [this]() {
            connect(ui->viewLogButton, &QPushButton::clicked,
                this, &MainWindow::createLogDialog);
        });
    connect(dialog, &QDialog::finished,
        dialog, &QDialog::deleteLater);

    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void MainWindow::createTrayIcon()
{
    m_trayIconMenu = new QMenu(this);

    m_trayIconMenuConnections = new QMenu(this);
    m_trayIconMenu->addMenu(m_trayIconMenuConnections);
    m_disconnectAction = new QAction(tr("断开"), this);
    m_trayIconMenu->addAction(m_disconnectAction);
    connect(m_disconnectAction, &QAction::triggered,
        this, &MainWindow::on_disconnectClicked);

    m_trayIconMenu->addSeparator();
    m_trayIconMenu->addAction(ui->actionLogWindow);
    m_trayIconMenu->addSeparator();
    m_trayIconMenu->addAction(ui->actionMinimize);
    m_trayIconMenu->addAction(ui->actionRestore);
    m_trayIconMenu->addSeparator();
    m_trayIconMenu->addAction(ui->actionQuit);

    m_trayIcon = new QSystemTrayIcon(this);
    m_trayIcon->setContextMenu(m_trayIconMenu);
}

void MainWindow::iconActivated(QSystemTrayIcon::ActivationReason reason)
{
    switch (reason) {
    case QSystemTrayIcon::Trigger:
    case QSystemTrayIcon::DoubleClick:
    case QSystemTrayIcon::MiddleClick:
#ifdef Q_OS_WIN
        if (isMinimized()) {
            showNormal();
        } else {
            showMinimized();
        }
#endif
        break;
    default:
        break;
    }
}

void MainWindow::on_actionNewProfile_triggered()
{
    NewProfileDialog dialog(this);
    connect(&dialog, &NewProfileDialog::connect,
        this, &MainWindow::on_connectClicked,
        Qt::QueuedConnection);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    reload_settings();
    ui->serverList->setCurrentText(dialog.getNewProfileName());
}

void MainWindow::on_actionNewProfileAdvanced_triggered()
{
    // TODO: the new profile has no name yet...
    EditDialog dialog("", this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    reload_settings();
    ui->serverList->setCurrentText(dialog.getEditedProfileName());
}

void MainWindow::on_actionEditSelectedProfile_triggered()
{
    EditDialog dialog(ui->serverList->currentText(), this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    int idx = ui->serverList->currentIndex();
    reload_settings();
    // TODO: may be signal/slot?
    if (idx < ui->serverList->maxVisibleItems() && idx >= 0) {
        ui->serverList->setCurrentIndex(idx);
    } else if (ui->serverList->maxVisibleItems() == 0) {
        ui->serverList->setCurrentIndex(0);
    }
    // LCA: else ???
}

#define PREFIX "server:" // LCA: remote this...
void MainWindow::on_actionRemoveSelectedProfile_triggered()
{
    QMessageBox mbox;
    mbox.setText(tr("您确定要删除 '%1' host?").arg(ui->serverList->currentText()));
    mbox.setStandardButtons(QMessageBox::Cancel | QMessageBox::Ok);
    mbox.setDefaultButton(QMessageBox::Cancel);
    mbox.setButtonText(QMessageBox::Ok, tr("删除"));
    if (mbox.exec() == QMessageBox::Ok) {
        QSettings settings;
        QString prefix = PREFIX;
        for (const auto& key : settings.allKeys()) {
            //qDebug() << key << ":" << QString(prefix + ui->serverList->currentText());
            if (key.startsWith(prefix + ui->serverList->currentText() + "/")) {
                settings.remove(key);
            }
        }

        reload_settings(); // LCA: remove this feature...
    }
}

void MainWindow::sdwanEnableToggled(bool checked)
{
    sdwanEnabled = checked;
    ui->nodeLicenseImportButton->setEnabled(checked);
    ui->networkIdEdit->setEnabled(checked);
    ui->sdwanAutoStartCheckbox->setEnabled(checked);
    ui->sdwanStartButton->setEnabled(checked);
    
    if (checked) {
        ui->sdwanStatusLabel->setText(tr("未连接"));
        ui->sdwanZerotierIdLabel->clear();
    } else {
        ui->sdwanStatusLabel->setText(tr("未启用"));
        ui->sdwanZerotierIdLabel->clear();
        lastJoinedNetworkId.clear();
        sdwanConnected = false;
    }
    
    updateSDWANStatus();
}

void MainWindow::sdwanAutoStartToggled(bool checked)
{
    sdwanAutoStart = checked;
}

void MainWindow::sdwanStartClicked()
{
    if (sdwanState != SDWAN_IDLE) {
        Logger::instance().addMessage(tr("SD-WAN 操作正在进行中，请稍候"));
        return;
    }
    
    networkId = ui->networkIdEdit->text().trimmed();
    
    if (networkId.isEmpty()) {
        QMessageBox::warning(this,
            qApp->applicationName(),
            tr("请输入网络ID"));
        return;
    }
    
    QRegularExpression re("^[0-9a-fA-F]{16}$");
    if (!re.match(networkId).hasMatch()) {
        QMessageBox::warning(this,
            qApp->applicationName(),
            tr("网络ID格式错误\n\n网络ID必须是16位十六进制字符"));
        return;
    }

    pendingNetworkId = networkId;
    sdwanState = SDWAN_STARTING;
    ui->sdwanStatusLabel->setText(tr("正在启动..."));
    
    startZeroTierAsync();
}

void MainWindow::onZeroTierStarted()
{
    if (sdwanState != SDWAN_STARTING) {
        return;
    }
    
    // 检查是否需要切换网络：如果之前没有加入网络，或者加入的是同一个网络，直接加入
    if (lastJoinedNetworkId.isEmpty() || lastJoinedNetworkId == pendingNetworkId) {
        sdwanState = SDWAN_JOINING_NETWORK;
        ui->sdwanStatusLabel->setText(tr("正在加入网络..."));
        joinZeroTierNetworkAsync(pendingNetworkId);
    } else {
        // 网络ID已变化，需要先离开旧网络
        Logger::instance().addMessage(tr("网络ID已变化，先离开旧网络: %1").arg(lastJoinedNetworkId));
        sdwanState = SDWAN_LEAVING_OLD_NETWORK;
        ui->sdwanStatusLabel->setText(tr("正在离开旧网络..."));
        leaveZeroTierNetworkAsync(lastJoinedNetworkId);
    }
}

void MainWindow::onZeroTierStartFailed()
{
    sdwanState = SDWAN_IDLE;
    sdwanConnected = false;
    updateSDWANStatus();
}

void MainWindow::onNetworkLeft(QString networkId)
{
    Q_UNUSED(networkId);
    
    if (sdwanState != SDWAN_LEAVING_OLD_NETWORK) {
        return;
    }
    
    // 离开旧网络后，等待2秒让网卡释放，然后加入新网络
    Logger::instance().addMessage(tr("等待网卡释放..."));
    
    QTimer::singleShot(NETWORK_ADAPTER_RELEASE_WAIT_MS, this, [this]() {
        sdwanState = SDWAN_JOINING_NETWORK;
        ui->sdwanStatusLabel->setText(tr("正在加入网络..."));
        joinZeroTierNetworkAsync(pendingNetworkId);
    });
}

void MainWindow::onNetworkJoined(QString networkId)
{
    if (sdwanState != SDWAN_JOINING_NETWORK) {
        return;
    }
    
    lastJoinedNetworkId = networkId;
    sdwanState = SDWAN_GETTING_NODE_ID;
    
    getZeroTierIdAsync();
}

void MainWindow::onNetworkJoinFailed(QString networkId)
{
    Q_UNUSED(networkId);
    
    sdwanState = SDWAN_IDLE;
    sdwanConnected = false;
    updateSDWANStatus();
}

void MainWindow::onNodeIdRetrieved(QString nodeId)
{
    if (sdwanState != SDWAN_GETTING_NODE_ID) {
        return;
    }
    
    // 显示获取到的节点ID（如果获取成功）
    if (!nodeId.isEmpty()) {
        ui->sdwanZerotierIdLabel->setText(tr("  ID: %1").arg(nodeId));
    }
    
    // 标记为已连接状态
    sdwanState = SDWAN_CONNECTED;
    sdwanConnected = true;
    updateSDWANStatus();
}

void MainWindow::updateSDWANStatus()
{
    if (sdwanEnabled) {
        ui->sdwanMainStatusLabel->setVisible(true);
        if (sdwanConnected) {
            ui->sdwanMainStatusLabel->setText(tr("SD-WAN: 就绪"));
            ui->sdwanMainStatusLabel->setStyleSheet("font-size: 9pt; color: green;");
        } else {
            ui->sdwanMainStatusLabel->setText(tr("SD-WAN: 未就绪"));
            ui->sdwanMainStatusLabel->setStyleSheet("font-size: 9pt; color: red;");
        }
    } else {
        ui->sdwanMainStatusLabel->setVisible(false);
    }
}

void MainWindow::nodeLicenseImport()
{
    QString fileName = QFileDialog::getOpenFileName(this,
        tr("选择节点许可文件"),
        QString(),
        tr("许可文件 (node.lic);;所有文件 (*.*)"));
    
    if (fileName.isEmpty()) {
        return;
    }
    
    if (!QFile::exists(fileName)) {
        QMessageBox::warning(this,
            qApp->applicationName(),
            tr("文件不存在: %1").arg(fileName));
        return;
    }
    
    QString ztPath = getZeroTierPath();
    QString planetPath = ztPath + "/planet";
    
    QDir dir;
    if (!dir.exists(ztPath)) {
        if (!dir.mkpath(ztPath)) {
            QMessageBox::critical(this,
                qApp->applicationName(),
                tr("无法创建目录: %1").arg(ztPath));
            return;
        }
    }
    
    if (QFile::exists(planetPath)) {
        QFile::remove(planetPath);
    }
    
    if (QFile::copy(fileName, planetPath)) {
        Logger::instance().addMessage(tr("节点许可文件已导入: %1").arg(planetPath));
        updateNodeLicenseStatus();
    } else {
        Logger::instance().addMessage(tr("导入节点许可文件失败: %1 -> %2").arg(fileName).arg(planetPath));
        QMessageBox::critical(this,
            qApp->applicationName(),
            tr("导入节点许可文件失败！\n\n请检查文件权限。"));
    }
}

QString MainWindow::getZeroTierPath()
{
#ifdef _WIN32
    QString programFiles = qEnvironmentVariable("ProgramFiles");
    if (programFiles.isEmpty()) {
        programFiles = "C:\\Program Files";
    }
    return programFiles + "\\aVPN\\node";
#else
    return "/usr/local/bin";
#endif
}

bool MainWindow::isZeroTierRunning()
{
    if (!zerotierProcess || zerotierProcess->state() != QProcess::Running) {
        return false;
    }
    return true;
}

void MainWindow::startZeroTierAsync()
{
    // 如果ZeroTier已经在运行，直接发送启动成功信号
    if (isZeroTierRunning()) {
        emit sdwanZeroTierStarted();
        return;
    }

    QString ztPath = getZeroTierPath();
    QString ztExecutable = ztPath + "/zerotier-one_x64.exe";
    
    Logger::instance().addMessage(tr("SD-WAN 路径: %1").arg(ztPath));
    Logger::instance().addMessage(tr("SD-WAN 可执行文件: %1").arg(ztExecutable));
    
    // 检查可执行文件是否存在
    if (!QFile::exists(ztExecutable)) {
        Logger::instance().addMessage(tr("SD-WAN 可执行文件不存在: %1").arg(ztExecutable));
        ui->sdwanStatusLabel->setText(tr("错误: 程序未找到"));
        emit sdwanZeroTierStartFailed();
        return;
    }

    // 如果有旧的进程实例，先终止
    if (zerotierProcess) {
        if (zerotierProcess->state() == QProcess::Running) {
            Logger::instance().addMessage(tr("终止旧的 SD-WAN 进程..."));
            zerotierProcess->terminate();
            if (!zerotierProcess->waitForFinished(ZEROTIER_TERMINATE_TIMEOUT_MS)) {
                zerotierProcess->kill();
                zerotierProcess->waitForFinished(1000);
            }
        }
        delete zerotierProcess;
        zerotierProcess = nullptr;
    }
    
    // 创建新的进程对象
    zerotierProcess = new QProcess(this);
    zerotierProcess->setWorkingDirectory(ztPath);
    
    // 监听进程意外退出
    connect(zerotierProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
            Q_UNUSED(exitCode);
            Q_UNUSED(exitStatus);
            Logger::instance().addMessage(tr("SD-WAN 进程意外退出"));
            sdwanConnected = false;
            sdwanState = SDWAN_IDLE;
            ui->sdwanStatusLabel->setText(tr("已断开"));
        });
    
    // 设置启动参数
#ifdef _WIN32
    zerotierProcess->setProgram(ztExecutable);
    zerotierProcess->setArguments(QStringList() << "-C" << ".");
    zerotierProcess->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
        args->startupInfo->dwFlags &= ~STARTF_USESTDHANDLES;
    });
    zerotierProcess->start();
#else
    zerotierProcess->start(ztExecutable, QStringList() << "-C" << ".");
#endif
    
    Logger::instance().addMessage(tr("启动命令: %1 -C .").arg(ztExecutable));
    
    // 异步检查启动结果（5秒超时）
    QTimer::singleShot(ZEROTIER_START_TIMEOUT_MS, this, [this, ztExecutable]() {
        if (!zerotierProcess || zerotierProcess->state() != QProcess::Running) {
            Logger::instance().addMessage(tr("SD-WAN 服务启动失败"));
            ui->sdwanStatusLabel->setText(tr("启动失败"));
            if (zerotierProcess) {
                delete zerotierProcess;
                zerotierProcess = nullptr;
            }
            emit sdwanZeroTierStartFailed();
            return;
        }
        
        // 启动成功，等待初始化（3秒）
        Logger::instance().addMessage(tr("SD-WAN 服务已启动，等待初始化..."));
        ui->sdwanStatusLabel->setText(tr("正在初始化..."));
        
        QTimer::singleShot(ZEROTIER_INIT_WAIT_MS, this, [this]() {
            Logger::instance().addMessage(tr("SD-WAN 服务初始化完成"));
            emit sdwanZeroTierStarted();
        });
    });
}

void MainWindow::leaveZeroTierNetworkSync(const QString& networkId)
{
    if (networkId.isEmpty()) {
        return;
    }

    QString ztPath = getZeroTierPath();
    QString networkBat = ztPath + "/sdwan-network.bat";
    
    QProcess leaveProcess;
    leaveProcess.setWorkingDirectory(ztPath);
    
    QStringList arguments;
#ifdef _WIN32
    arguments << "/c" << networkBat << "leave" << networkId;
    leaveProcess.start("cmd.exe", arguments);
#else
    arguments << "leave" << networkId;
    leaveProcess.start(networkBat, arguments);
#endif
    
    Logger::instance().addMessage(tr("执行离开命令: cmd.exe /c %1 leave %2").arg(networkBat).arg(networkId));
    
    if (!leaveProcess.waitForFinished(NETWORK_LEAVE_TIMEOUT_MS)) {
        Logger::instance().addMessage(tr("离开 SD-WAN 网络超时: %1").arg(networkId));
        leaveProcess.kill();
        leaveProcess.waitForFinished(1000);
    } else {
        Logger::instance().addMessage(tr("已离开 SD-WAN 网络: %1").arg(networkId));
    }
}

void MainWindow::leaveZeroTierNetworkAsync(const QString& networkId)
{
    if (networkId.isEmpty()) {
        emit sdwanNetworkLeft(networkId);
        return;
    }

    QString ztPath = getZeroTierPath();
    QString networkBat = ztPath + "/sdwan-network.bat";
    
    QProcess* leaveProcess = new QProcess(this);
    leaveProcess->setWorkingDirectory(ztPath);
    
    connect(leaveProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        this, [this, leaveProcess, networkId, ztPath](int exitCode, QProcess::ExitStatus exitStatus) {
            Q_UNUSED(exitStatus);
            
            QString output = QString::fromLocal8Bit(leaveProcess->readAllStandardOutput());
            QString errors = QString::fromLocal8Bit(leaveProcess->readAllStandardError());
            
            Logger::instance().addMessage(tr("离开命令输出: %1").arg(output.isEmpty() ? "(空)" : output));
            if (!errors.isEmpty()) {
                Logger::instance().addMessage(tr("离开错误输出: %1").arg(errors));
            }
            
            if (exitCode == 0) {
                Logger::instance().addMessage(tr("已离开 SD-WAN 网络: %1").arg(networkId));
            } else {
                Logger::instance().addMessage(tr("离开网络失败 (退出码: %1)").arg(exitCode));
            }
            
            QString networkConfigFile = ztPath + "/networks.d/" + networkId + ".conf";
            if (QFile::exists(networkConfigFile)) {
                if (QFile::remove(networkConfigFile)) {
                    Logger::instance().addMessage(tr("已删除网络配置文件: %1").arg(networkConfigFile));
                } else {
                    Logger::instance().addMessage(tr("删除网络配置文件失败: %1").arg(networkConfigFile));
                }
            }
            
            leaveProcess->deleteLater();
            emit sdwanNetworkLeft(networkId);
        });
    
    QTimer::singleShot(NETWORK_LEAVE_TIMEOUT_MS, this, [leaveProcess, networkId, this]() {
        if (leaveProcess->state() == QProcess::Running) {
            Logger::instance().addMessage(tr("离开 SD-WAN 网络超时: %1").arg(networkId));
            leaveProcess->kill();
        }
    });
    
    QStringList arguments;
#ifdef _WIN32
    arguments << "/c" << networkBat << "leave" << networkId;
    leaveProcess->start("cmd.exe", arguments);
#else
    arguments << "leave" << networkId;
    leaveProcess->start(networkBat, arguments);
#endif
    
    Logger::instance().addMessage(tr("执行离开命令: cmd.exe /c %1 leave %2").arg(networkBat).arg(networkId));
}

    return true;
}

void MainWindow::joinZeroTierNetworkAsync(const QString& networkId)
{
    if (networkId.isEmpty()) {
        Logger::instance().addMessage(tr("网络ID为空"));
        emit sdwanNetworkJoinFailed(networkId);
        return;
    }

    QString ztPath = getZeroTierPath();
    QString networkBat = ztPath + "/sdwan-network.bat";
    
    QProcess* joinProcess = new QProcess(this);
    joinProcess->setWorkingDirectory(ztPath);
    
    connect(joinProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        this, [this, joinProcess, networkId](int exitCode, QProcess::ExitStatus exitStatus) {
            Q_UNUSED(exitStatus);
            
            QString output = QString::fromLocal8Bit(joinProcess->readAllStandardOutput());
            QString errors = QString::fromLocal8Bit(joinProcess->readAllStandardError());
            
            Logger::instance().addMessage(tr("命令输出: %1").arg(output.isEmpty() ? "(空)" : output));
            if (!errors.isEmpty()) {
                Logger::instance().addMessage(tr("错误输出: %1").arg(errors));
            }
            
            joinProcess->deleteLater();
            
            if (exitCode == 0) {
                Logger::instance().addMessage(tr("已加入 SD-WAN 网络: %1").arg(networkId));
                ui->sdwanStatusLabel->setText(tr("已连接"));
                emit sdwanNetworkJoined(networkId);
            } else {
                Logger::instance().addMessage(tr("加入网络失败 (退出码: %1)").arg(exitCode));
                ui->sdwanStatusLabel->setText(tr("加入失败"));
                emit sdwanNetworkJoinFailed(networkId);
            }
        });
    
    QTimer::singleShot(NETWORK_JOIN_TIMEOUT_MS, this, [joinProcess, networkId, this]() {
        if (joinProcess->state() == QProcess::Running) {
            Logger::instance().addMessage(tr("加入 SD-WAN 网络超时"));
            ui->sdwanStatusLabel->setText(tr("加入网络超时"));
            joinProcess->kill();
        }
    });
    
    QStringList arguments;
#ifdef _WIN32
    arguments << "/c" << networkBat << "join" << networkId;
    joinProcess->start("cmd.exe", arguments);
#else
    arguments << "join" << networkId;
    joinProcess->start(networkBat, arguments);
#endif
    
    Logger::instance().addMessage(tr("执行命令: cmd.exe /c %1 join %2").arg(networkBat).arg(networkId));
}

void MainWindow::getZeroTierIdAsync()
{
    QString ztPath = getZeroTierPath();
    QString ztExecutable = ztPath + "/zerotier-one_x64.exe";
    
    if (!QFile::exists(ztExecutable)) {
        Logger::instance().addMessage(tr("获取SD-WAN节点ID失败: 可执行文件不存在"));
        emit sdwanNodeIdRetrieved(QString());
        return;
    }
    
    QProcess* process = new QProcess(this);
    process->setWorkingDirectory(ztPath);
    
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        this, [this, process](int exitCode, QProcess::ExitStatus exitStatus) {
            Q_UNUSED(exitCode);
            Q_UNUSED(exitStatus);
            
            QString output = QString::fromUtf8(process->readAllStandardOutput()).trimmed();
            QString errors = QString::fromUtf8(process->readAllStandardError()).trimmed();
            
            Logger::instance().addMessage(tr("SD-WAN节点信息输出: %1").arg(output));
            if (!errors.isEmpty()) {
                Logger::instance().addMessage(tr("SD-WAN节点信息错误: %1").arg(errors));
            }
            
            QString nodeId;
            QStringList lines = output.split('\n');
            for (const QString& line : lines) {
                if (line.contains("200 info")) {
                    QStringList parts = line.split(' ', Qt::SkipEmptyParts);
                    Logger::instance().addMessage(tr("解析结果: 共 %1 个字段").arg(parts.size()));
                    if (parts.size() >= 3) {
                        nodeId = parts[2];
                        Logger::instance().addMessage(tr("提取到SD-WAN节点ID: %1").arg(nodeId));
                        break;
                    }
                }
            }
            
            if (nodeId.isEmpty()) {
                Logger::instance().addMessage(tr("未能从输出中解析SD-WAN节点ID"));
            }
            
            process->deleteLater();
            emit sdwanNodeIdRetrieved(nodeId);
        });
    
    QTimer::singleShot(NODE_ID_QUERY_TIMEOUT_MS, this, [process, this]() {
        if (process->state() == QProcess::Running) {
            Logger::instance().addMessage(tr("获取SD-WAN节点ID超时"));
            process->kill();
        }
    });
    
    QStringList args;
    args << "-q" << "-D." << "info";
    process->start(ztExecutable, args);
}

void MainWindow::terminateZeroTierProcess()
{
    if (!zerotierProcess) {
        return;
    }
    
    Logger::instance().addMessage(tr("正在终止 SD-WAN 服务进程..."));
    
    QString ztPath = getZeroTierPath();
    QString ztExecutable = ztPath + "/zerotier-one_x64.exe";
    
    QProcess stopProcess;
    stopProcess.setWorkingDirectory(ztPath);
    QStringList arguments;
    arguments << "-q" << "terminate";
    
    stopProcess.start(ztExecutable, arguments);
    if (stopProcess.waitForFinished(ZEROTIER_TERMINATE_TIMEOUT_MS)) {
        Logger::instance().addMessage(tr("SD-WAN 服务已发送终止命令"));
    } else {
        stopProcess.kill();
    }
    
    if (zerotierProcess->state() == QProcess::Running) {
        zerotierProcess->terminate();
        if (!zerotierProcess->waitForFinished(ZEROTIER_TERMINATE_TIMEOUT_MS)) {
            Logger::instance().addMessage(tr("强制终止 SD-WAN 服务进程"));
            zerotierProcess->kill();
            zerotierProcess->waitForFinished(500);
        }
    }
    
    zerotierProcess->deleteLater();
    zerotierProcess = nullptr;
    
    Logger::instance().addMessage(tr("SD-WAN 服务进程已停止"));
}

void MainWindow::updateNodeLicenseStatus()
{
    QString planetPath = getZeroTierPath() + "/planet";
    
    if (QFile::exists(planetPath)) {
        ui->nodeLicenseStatusLabel->setText(tr("已导入"));
        ui->nodeLicenseStatusLabel->setStyleSheet("color: green;");
    } else {
        ui->nodeLicenseStatusLabel->setText(tr("未导入"));
        ui->nodeLicenseStatusLabel->setStyleSheet("color: gray;");
    }
}