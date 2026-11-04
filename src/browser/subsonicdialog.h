/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDialog>

#include <memory>

class QCloseEvent;

namespace Fooyin {
class NetworkAccessManager;
class PlaylistInteractor;
class SettingsManager;

namespace Subsonic {
class SubsonicBrowser;
class SubsonicLibraryDownloader;

class SubsonicDialog : public QDialog
{
    Q_OBJECT

public:
    SubsonicDialog(std::shared_ptr<NetworkAccessManager> network, PlaylistInteractor* interactor,
                   SubsonicLibraryDownloader* libraryDownloads, SettingsManager* settings,
                   QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    SettingsManager* m_settings;
    SubsonicBrowser* m_browser;
};
} // namespace Subsonic
} // namespace Fooyin
