/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "subsonicplugin.h"

#include "subsonicbrowserwidget.h"
#include "subsonicdialog.h"
#include "subsonicdownloadmanager.h"
#include "subsonicsettings.h"
#include "subsonicsettingspage.h"

#include <core/network/networkaccessmanager.h>

#include <gui/guiconstants.h>
#include <gui/widgetprovider.h>

#include <utils/actions/actioncontainer.h>
#include <utils/actions/actionmanager.h>
#include <utils/actions/command.h>
#include <utils/utils.h>

#include <QAction>
#include <QMainWindow>

using namespace Qt::StringLiterals;

namespace Fooyin::Subsonic {
void SubsonicPlugin::initialise(const CorePluginContext& context)
{
    m_network  = context.networkAccess;
    m_settings = context.settingsManager;

    registerSubsonicSettings(m_settings);

    m_downloads = new SubsonicDownloadManager{m_network, m_settings, context.playerController,
                                              context.playlistHandler, this};

    // With the stock 2 MB read-ahead, fooyin throttles a remote file to playback speed, so the
    // HTTP connection stays open for the whole track. Servers/proxies in front of Subsonic
    // (e.g. Cloudflare/Caddy) often cut such long-lived streams, after which playback stops.
    // Buffering enough to pull the track down up-front finishes the transfer before that happens.
    // Live radio is unaffected (its server sends at real time, so the buffer stays small).
    // ponytail: only touch the stock default so an explicit user choice in Advanced settings wins.
    if(m_settings->value(QStringLiteral("Engine/RemoteReadAheadKb")).toInt() == 2048) {
        m_settings->set(QStringLiteral("Engine/RemoteReadAheadKb"), 131072); // 128 MB
        qCInfo(SUBSONIC) << "Raised remote read-ahead from 2048 kB to 131072 kB so Subsonic tracks download "
                            "up-front and finish before any proxy timeout";
    }
    else {
        qCInfo(SUBSONIC) << "Remote read-ahead is"
                         << m_settings->value(QStringLiteral("Engine/RemoteReadAheadKb")).toInt() << "kB";
    }
}

void SubsonicPlugin::initialise(const GuiPluginContext& context)
{
    m_interactor    = context.playlistInteractor;
    m_actionManager = context.actionManager;
    m_widgetProvider = context.widgetProvider;

    new SubsonicSettingsPage{m_network, m_settings, m_downloads, this};

    m_widgetProvider->registerWidget(
        u"SubsonicBrowser"_s,
        [this]() { return new SubsonicBrowserWidget{m_network, m_interactor, m_downloads, m_settings}; },
        tr("Foosonic Browser"));
    m_widgetProvider->setSubMenus(u"SubsonicBrowser"_s, {tr("Internet")});

    auto* showSubsonic = new QAction{tr("Foosonic &Browser"), this};
    showSubsonic->setStatusTip(tr("Browse music on a Subsonic server"));

    auto* command = m_actionManager->registerAction(showSubsonic, "Subsonic.ShowBrowser");
    command->setCategories({tr("View")});
    m_actionManager->actionContainer(Constants::Menus::View)->addAction(command);

    connect(showSubsonic, &QAction::triggered, this, &SubsonicPlugin::showBrowser);
}

void SubsonicPlugin::showBrowser()
{
    if(m_dialog) {
        m_dialog->show();
        m_dialog->raise();
        m_dialog->activateWindow();
        return;
    }

    m_dialog = new SubsonicDialog{m_network, m_interactor, m_downloads, m_settings, Utils::getMainWindow()};
    m_dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_dialog->show();
}
} // namespace Fooyin::Subsonic
