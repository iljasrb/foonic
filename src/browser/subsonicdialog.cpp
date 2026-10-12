/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "subsonicdialog.h"

#include "subsonicbrowser.h"
#include "subsonicsettings.h"

#include <QTimer>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace Fooyin::Subsonic {
SubsonicDialog::SubsonicDialog(std::shared_ptr<NetworkAccessManager> network, PlaylistInteractor* interactor,
                               SubsonicDownloadManager* downloads, SubsonicLibraryDownloader* libraryDownloads,
                               SettingsManager* settings, QWidget* parent)
    : QDialog{parent}
    , m_browser{new SubsonicBrowser{std::move(network), interactor, downloads, libraryDownloads, settings, this}}
{
    setWindowTitle(tr("Foosonic Browser"));

    auto* layout = new QVBoxLayout{this};
    layout->setContentsMargins(9, 9, 9, 9);
    layout->setSpacing(6);
    layout->addWidget(m_browser);

    resize(900, 600);

    if(!loadSubsonicSettings(settings).isValid()) {
        QTimer::singleShot(0, this, [this] { m_browser->openSettings(); });
    }
}
} // namespace Fooyin::Subsonic
