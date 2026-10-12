/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gui/fywidget.h>

#include <memory>

namespace Fooyin {
class NetworkAccessManager;
class PlaylistInteractor;
class SettingsManager;

namespace Subsonic {
class SubsonicBrowser;
class SubsonicDownloadManager;

/*!
 * Layout widget wrapping the Subsonic browser so it can live in a fooyin layout like the library tree.
 */
class SubsonicBrowserWidget : public FyWidget
{
    Q_OBJECT

public:
    SubsonicBrowserWidget(std::shared_ptr<NetworkAccessManager> network, PlaylistInteractor* interactor,
                          SubsonicDownloadManager* downloads, SettingsManager* settings, QWidget* parent = nullptr);

    [[nodiscard]] QString name() const override;
    [[nodiscard]] QString layoutName() const override;

private:
    SubsonicBrowser* m_browser;
};
} // namespace Subsonic
} // namespace Fooyin
