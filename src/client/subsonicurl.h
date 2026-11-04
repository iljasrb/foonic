/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "subsonicsettings.h"

#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QUrl>
#include <QUrlQuery>

using namespace Qt::StringLiterals;

namespace Fooyin::Subsonic {
inline QString randomSalt()
{
    static constexpr auto Chars = "abcdefghijklmnopqrstuvwxyz0123456789"_L1;
    QString salt;
    salt.reserve(12);
    for(int i{0}; i < 12; ++i) {
        salt.append(Chars.at(QRandomGenerator::global()->bounded(Chars.size())));
    }
    return salt;
}

//! Builds a `/rest/<method>` URL with the configured authentication and client parameters.
inline QUrl buildSubsonicUrl(const SubsonicSettings& settings, const QString& method, QUrlQuery query)
{
    QUrl url{settings.server.trimmed()};
    if(url.scheme().isEmpty()) {
        url = QUrl{u"http://"_s + settings.server.trimmed()};
    }

    QString path = url.path();
    while(path.endsWith(u'/')) {
        path.chop(1);
    }
    // Accept both "https://host" and "https://host/rest" as the configured server URL.
    if(path.endsWith(u"/rest"_s, Qt::CaseInsensitive)) {
        path.chop(5);
    }
    url.setPath(path + u"/rest/"_s + method);

    if(settings.authMode == u"apiKey"_s) {
        query.addQueryItem(u"apiKey"_s, settings.apiKey);
    }
    else if(settings.authMode == u"password"_s) {
        query.addQueryItem(u"u"_s, settings.username);
        query.addQueryItem(u"p"_s, settings.password);
    }
    else {
        const QString salt = randomSalt();
        query.addQueryItem(u"u"_s, settings.username);
        query.addQueryItem(u"s"_s, salt);
        query.addQueryItem(u"t"_s, QString::fromLatin1(QCryptographicHash::hash((settings.password + salt).toUtf8(),
                                                                               QCryptographicHash::Md5)
                                                          .toHex()));
    }
    query.addQueryItem(u"v"_s, u"1.16.1"_s);
    query.addQueryItem(u"c"_s, u"fooyin-subsonic"_s);
    query.addQueryItem(u"f"_s, u"json"_s);
    if(!settings.musicFolderId.isEmpty()) {
        query.addQueryItem(u"musicFolderId"_s, settings.musicFolderId);
    }
    url.setQuery(query);

    return url;
}
} // namespace Fooyin::Subsonic
