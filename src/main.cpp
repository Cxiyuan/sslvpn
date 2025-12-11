/*
 * Copyright (C) 2014 Red Hat
 * Copyright (C) 2016 by Lubomír Carik <Lubomir.Carik@gmail.com>
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

#include "common.h"
#include "config.h"
#include "dialog/MyInputDialog.h"
#include "dialog/mainwindow.h"
#include "avpn.h"

#include "FileLogger.h"
#include "logger.h"

extern "C" {
#include <gnutls/pkcs11.h>
#include <openconnect.h>
}

#include <QApplication>
#if !defined(_WIN32) && !defined(PROJ_GNUTLS_DEBUG)
#include <QMessageBox>
#endif
#include <QCommandLineParser>
#include <QSettings>
#include <QtSingleApplication>

#ifdef __MACH__
#include <Security/Security.h>
#include <mach-o/dyld.h>
#endif

#include <csignal>
#include <cstdio>

static void log_callback(int level, const char* str)
{
    Logger::instance().addMessage(QString(str).trimmed(),
        Logger::MessageType::DEBUG,
        Logger::ComponentType::GNUTLS);
}

#if defined(Q_OS_MACOS) && defined(PROJ_ADMIN_PRIV_ELEVATION)
bool relaunch_as_root()
{
    QMessageBox msgBox;
    char appPath[2048];
    uint32_t size = sizeof(appPath);
    AuthorizationRef authRef;
    OSStatus status;

    /* Get the path of the current program */
    if (_NSGetExecutablePath(appPath, &size) != 0) {
        msgBox.setText(QObject::tr("无法获取程序路径以提升权限。"));
        return false;
    }

    status = AuthorizationCreate(NULL, kAuthorizationEmptyEnvironment,
        kAuthorizationFlagDefaults, &authRef);

    if (status != errAuthorizationSuccess) {
        msgBox.setText(QObject::tr("创建授权引用失败。"));
        return false;
    }
    status = AuthorizationExecuteWithPrivileges(authRef, appPath,
        kAuthorizationFlagDefaults, NULL, NULL);
    AuthorizationFree(authRef, kAuthorizationFlagDestroyRights);

    if (status == errAuthorizationSuccess) {
        /* We've successfully re-launched with root privs. */
        return true;
    }

    return false;
}
#endif

int pin_callback(void* userdata, int attempt, const char* token_url,
    const char* token_label, unsigned flags, char* pin,
    size_t pin_max)
{
    QString type = QObject::tr("用户");
    if (flags & GNUTLS_PIN_SO) {
        type = QObject::tr("安全管理员");
    }

    QString outtext = QObject::tr("请输入 ") + type + QObject::tr(" 的 PIN 码用于 ") + QLatin1String(token_label) + ".";
    if (flags & GNUTLS_PKCS11_PIN_FINAL_TRY) {
        outtext += QObject::tr(" 这是最后一次尝试！");
    }
    if (flags & GNUTLS_PKCS11_PIN_COUNT_LOW) {
        outtext += QObject::tr(" 仅剩少数次尝试机会，之后令牌将被锁定！");
    }

    MainWindow* w = (MainWindow*)userdata;
    MyInputDialog dialog(w, QLatin1String(token_url), outtext, QLineEdit::Password);
    dialog.show();

    QString text;
    bool ok = dialog.result(text);
    if (ok == false) {
        return -1;
    }

    snprintf(pin, pin_max, "%s", text.toLatin1().data());
    return 0;
}

int main(int argc, char* argv[])
{
    qputenv("LOG2FILE", "1");

#if !defined(Q_OS_MACOS)
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);

    qRegisterMetaType<Logger::Message>();

#ifdef PROJ_INI_SETTINGS
    QSettings::setDefaultFormat(QSettings::IniFormat);
#endif

#if defined(Q_OS_MACOS) && defined(PROJ_ADMIN_PRIV_ELEVATION)
    /* Re-launching with root privs on OS X needs Qt to allow setsuid */
    QApplication::setSetuidAllowed(true);
#endif
    QCoreApplication::setApplicationName(appDescription);
    QCoreApplication::setApplicationVersion(appVersion);
    QCoreApplication::setOrganizationName(appOrganizationName);
    QCoreApplication::setOrganizationDomain(appOrganizationDomain);

    QtSingleApplication app(argc, argv);
    if (app.isRunning()) {
        QSettings settings;
        if (settings.value(QLatin1Literal("Settings/singleInstanceMode"), true).toBool()) {
            app.sendMessage("Wake up!");
            return 0;
        }
    }
    app.setApplicationDisplayName(appDescriptionLong);
    app.setQuitOnLastWindowClosed(false);

#if defined(Q_OS_MACOS) && defined(PROJ_ADMIN_PRIV_ELEVATION)
    if (geteuid() != 0) {
        if (relaunch_as_root()) {
            /* We have re-launched with root privs. Exit this process. */
            return 0;
        }

        QMessageBox msgBox;
        msgBox.setText(QObject::tr("此程序需要 root 权限才能正常运行。"));
        msgBox.setInformativeText(QObject::tr("VPN 连接建立将会失败。"));
        msgBox.exec();
        return -1;
    }
#endif

    auto fileLog = std::make_unique<FileLogger>();
    Logger::instance().addMessage(QString("%1 (%2) logging started...").arg(app.applicationDisplayName()).arg(app.applicationVersion()));

    gnutls_global_init();
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    openconnect_init_ssl();

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QObject::tr("OpenConnect 是一个 VPN 客户端，利用 TLS 和 DTLS "
                    "建立安全会话，兼容 CISCO AnyConnect SSL VPN 协议。"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({ { "s", "server" },
        QObject::tr("自动连接到现有配置 <名称>"),
        QObject::tr("名称")

    });

    parser.process(app);

    const QString profileName{ parser.value(QLatin1String("server")) };
    MainWindow mainWindow(nullptr, profileName);
    app.setWindowIcon(QIcon(":/avpn.ico"));
    app.setActivationWindow(&mainWindow);
#ifdef PROJ_PKCS11
    gnutls_pkcs11_set_pin_function(pin_callback, &mainWindow);
#endif
    gnutls_global_set_log_function(log_callback);
#ifdef PROJ_GNUTLS_DEBUG
    gnutls_global_set_log_level(3);
#endif

    mainWindow.show();
    QObject::connect(&app, &QtSingleApplication::messageReceived,
        [&mainWindow](const QString& message) {
            Logger::instance().addMessage(message);
        });
    return app.exec();
}