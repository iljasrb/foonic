/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "subsonicbrowserwidget.h"

#include "subsonicbrowser.h"

#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace Fooyin::Subsonic {
SubsonicBrowserWidget::SubsonicBrowserWidget(std::shared_ptr<NetworkAccessManager> network,
                                             PlaylistInteractor* interactor, SubsonicDownloadManager* downloads,
                                             SettingsManager* settings, QWidget* parent)
    : FyWidget{parent}
    , m_browser{new SubsonicBrowser{std::move(network), interactor, downloads, settings, this}}
{
    auto* layout = new QVBoxLayout{this};
    layout->setContentsMargins({});
    layout->addWidget(m_browser);
}

QString SubsonicBrowserWidget::name() const
{
    return tr("Foosonic Browser");
}

QString SubsonicBrowserWidget::layoutName() const
{
    return u"SubsonicBrowser"_s;
}
} // namespace Fooyin::Subsonic
