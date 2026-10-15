/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <utils/settings/settingspage.h>

#include <memory>

namespace Fooyin {
class NetworkAccessManager;
class SettingsManager;

namespace Subsonic {
class SubsonicSettingsPage : public SettingsPage
{
    Q_OBJECT

public:
    SubsonicSettingsPage(std::shared_ptr<NetworkAccessManager> network, SettingsManager* settings,
                         QObject* parent = nullptr);
};
} // namespace Subsonic
} // namespace Fooyin
