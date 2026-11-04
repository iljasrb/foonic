/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "subsonicdialog.h"

#include "subsonicbrowser.h"
#include "subsonicsettings.h"

#include <QCloseEvent>
#include <QTimer>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace Fooyin::Subsonic {
SubsonicDialog::SubsonicDialog(std::shared_ptr<NetworkAccessManager> network, PlaylistInteractor* interactor,
                               SubsonicLibraryDownloader* libraryDownloads, SettingsManager* settings,
                               QWidget* parent)
    : QDialog{parent}
    , m_settings{settings}
    , m_browser{new SubsonicBrowser{std::move(network), interactor, libraryDownloads, settings, this}}
{
    setWindowTitle(tr("Foonic Browser"));

    auto* layout = new QVBoxLayout{this};
    layout->setContentsMargins(9, 9, 9, 9);
    layout->setSpacing(6);
    layout->addWidget(m_browser);

    const QByteArray geometry = settings->value(u"Subsonic/DialogGeometry"_s).toByteArray();
    if(geometry.isEmpty() || !restoreGeometry(geometry)) {
        resize(900, 600);
    }

    if(!loadSubsonicSettings(settings).isValid()) {
        QTimer::singleShot(0, this, [this] { m_browser->openSettings(); });
    }
}

void SubsonicDialog::closeEvent(QCloseEvent* event)
{
    if(m_settings) {
        m_settings->set(u"Subsonic/DialogGeometry"_s, saveGeometry());
    }
    QDialog::closeEvent(event);
}
} // namespace Fooyin::Subsonic
