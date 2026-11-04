/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Keep assertions active in Release builds; CI configures the tests with -DNDEBUG.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include "subsonicurl.h"

#include <QUrlQuery>

#include <cassert>
#include <iostream>

using namespace Fooyin::Subsonic;

int main()
{
    {
        SubsonicSettings settings;
        settings.server   = QStringLiteral("https://music.example");
        settings.username = QStringLiteral("user");
        settings.password = QStringLiteral("secret");

        const QUrl url = buildSubsonicUrl(settings, QStringLiteral("ping"), {});
        assert(url.path() == QStringLiteral("/rest/ping"));
        const QUrlQuery query{url};
        assert(query.queryItemValue(QStringLiteral("u")) == QStringLiteral("user"));
        assert(!query.queryItemValue(QStringLiteral("s")).isEmpty());
        assert(!query.queryItemValue(QStringLiteral("t")).isEmpty());
        assert(query.queryItemValue(QStringLiteral("v")) == QStringLiteral("1.16.1"));
        assert(query.queryItemValue(QStringLiteral("c")) == QStringLiteral("fooyin-subsonic"));
        assert(query.queryItemValue(QStringLiteral("f")) == QStringLiteral("json"));
    }

    {
        SubsonicSettings settings;
        settings.server   = QStringLiteral("music.example/rest/");
        settings.authMode = QStringLiteral("apiKey");
        settings.apiKey   = QStringLiteral("key");

        const QUrl url = buildSubsonicUrl(settings, QStringLiteral("getArtists"), {});
        assert(url.scheme() == QStringLiteral("http"));
        assert(url.path() == QStringLiteral("/rest/getArtists"));
        const QUrlQuery query{url};
        assert(query.queryItemValue(QStringLiteral("apiKey")) == QStringLiteral("key"));
        assert(query.queryItemValue(QStringLiteral("u")).isEmpty());
    }

    {
        SubsonicSettings settings;
        settings.server       = QStringLiteral("https://music.example/subsonic");
        settings.username     = QStringLiteral("user");
        settings.password     = QStringLiteral("secret");
        settings.authMode     = QStringLiteral("password");
        settings.musicFolderId = QStringLiteral("7");

        QUrlQuery params;
        params.addQueryItem(QStringLiteral("id"), QStringLiteral("42"));
        const QUrl url = buildSubsonicUrl(settings, QStringLiteral("stream"), params);
        assert(url.path() == QStringLiteral("/subsonic/rest/stream"));
        const QUrlQuery query{url};
        assert(query.queryItemValue(QStringLiteral("p")) == QStringLiteral("secret"));
        assert(query.queryItemValue(QStringLiteral("musicFolderId")) == QStringLiteral("7"));
        assert(query.queryItemValue(QStringLiteral("id")) == QStringLiteral("42"));
    }

    std::cout << "subsonicurl: ok\n";
    return 0;
}
