/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDialog>

#include <memory>

namespace Fooyin {
class NetworkAccessManager;
class PlaylistInteractor;
class SettingsManager;

namespace Subsonic {
class SubsonicBrowser;
class SubsonicDownloadManager;

class SubsonicDialog : public QDialog
{
    Q_OBJECT

public:
    SubsonicDialog(std::shared_ptr<NetworkAccessManager> network, PlaylistInteractor* interactor,
                   SubsonicDownloadManager* downloads, SettingsManager* settings, QWidget* parent = nullptr);

private:
    SubsonicBrowser* m_browser;
};
} // namespace Subsonic
} // namespace Fooyin
