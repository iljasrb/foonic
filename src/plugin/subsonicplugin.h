/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <core/plugins/coreplugin.h>
#include <core/plugins/plugin.h>
#include <gui/plugins/guiplugin.h>

#include <QPointer>

namespace Fooyin {
class ActionManager;
class NetworkAccessManager;
class PlaylistInteractor;
class SettingsManager;
class WidgetProvider;

namespace Subsonic {
class SubsonicDialog;
class SubsonicDownloadManager;

class SubsonicPlugin : public QObject,
                       public Plugin,
                       public CorePlugin,
                       public GuiPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.fooyin.fooyin.plugin/1.0" FILE "foosonic.json")
    Q_INTERFACES(Fooyin::Plugin Fooyin::CorePlugin Fooyin::GuiPlugin)

public:
    void initialise(const CorePluginContext& context) override;
    void initialise(const GuiPluginContext& context) override;

private:
    void showBrowser();

    std::shared_ptr<NetworkAccessManager> m_network;
    SettingsManager* m_settings{nullptr};
    PlaylistInteractor* m_interactor{nullptr};
    ActionManager* m_actionManager{nullptr};
    WidgetProvider* m_widgetProvider{nullptr};
    SubsonicDownloadManager* m_downloads{nullptr};
    QPointer<SubsonicDialog> m_dialog;
};
} // namespace Subsonic
} // namespace Fooyin
