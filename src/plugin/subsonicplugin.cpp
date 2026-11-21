/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "subsonicplugin.h"

#include "subsonicbrowserwidget.h"
#include "subsonicbufferbar.h"
#include "subsonicdialog.h"
#include "subsoniclibrarydownloader.h"
#include "subsonicplaybackcontroller.h"
#include "subsonicremoteplayback.h"
#include "subsonicsettings.h"
#include "subsonicsettingspage.h"
#include "subsonicstreamcache.h"
#include "subsonicstreamutils.h"

#include <core/network/networkaccessmanager.h>

#include <gui/guiconstants.h>
#include <gui/widgetprovider.h>

#include <utils/actions/actioncontainer.h>
#include <utils/actions/actionmanager.h>
#include <utils/actions/command.h>
#include <utils/utils.h>

#include <QAction>
#include <QLoggingCategory>
#include <QMainWindow>

using namespace Qt::StringLiterals;

namespace Fooyin::Subsonic {
void SubsonicPlugin::initialise(const CorePluginContext& context)
{
    m_network  = context.networkAccess;
    m_settings = context.settingsManager;

    registerSubsonicSettings(m_settings);

    const SubsonicSettings settings = loadSubsonicSettings(m_settings);
    m_cache = std::make_shared<SessionCache>(streamCacheBaseDir(),
                                             static_cast<qint64>(settings.sessionCacheMb) * 1024 * 1024);
    m_settings->subscribe(QStringLiteral("Subsonic/SessionCacheMb"), this, [this](const QVariant& value) {
        if(m_cache) {
            const int limitMb = std::clamp(value.toInt(), 128, 16384);
            m_cache->setLimitBytes(static_cast<qint64>(limitMb) * 1024 * 1024);
        }
    });

    // Subsonic streams are plain http(s), which fooyin hard-codes as non-seekable. Install a seekable
    // HTTP-Range device and a decoder that keeps using fooyin's FFmpeg backend through it.
    m_bufferMonitor = registerSubsonicRemotePlayback(context.audioLoader, m_network, m_settings, m_cache);

    new SubsonicPlaybackController{m_network, m_settings, context.playerController, m_bufferMonitor, m_cache,
                                   this};

    m_libraryDownloads = new SubsonicLibraryDownloader{m_network, m_settings, context.audioLoader,
                                                       context.library, this};

    qCInfo(SUBSONIC) << "settings: format" << settings.format << "maxBitRate" << settings.maxBitRate
                     << "sessionCacheMb" << settings.sessionCacheMb << "remotePrebufferMs"
                     << settings.remotePrebufferMs;
}

void SubsonicPlugin::initialise(const GuiPluginContext& context)
{
    m_interactor    = context.playlistInteractor;
    m_actionManager = context.actionManager;
    m_widgetProvider = context.widgetProvider;

    new SubsonicSettingsPage{m_network, m_settings, this};

    if(m_bufferMonitor) {
        new SubsonicBufferBar{m_settings, m_bufferMonitor.get(), this};
    }

    m_widgetProvider->registerWidget(
        u"SubsonicBrowser"_s,
        [this]() {
            return new SubsonicBrowserWidget{m_network, m_interactor, m_libraryDownloads, m_settings};
        },
        tr("Foonic Browser"));
    m_widgetProvider->setSubMenus(u"SubsonicBrowser"_s, {tr("Internet")});

    auto* showSubsonic = new QAction{tr("Foonic &Browser"), this};
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

    m_dialog = new SubsonicDialog{m_network, m_interactor, m_libraryDownloads, m_settings,
                                  Utils::getMainWindow()};
    m_dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_dialog->show();
}
} // namespace Fooyin::Subsonic
