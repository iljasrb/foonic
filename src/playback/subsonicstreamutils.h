/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QByteArray>
#include <QUrl>
#include <QUrlQuery>

namespace Fooyin::Subsonic {
//! True for a http(s) URL pointing at the Subsonic `stream` endpoint with an id.
inline bool isSubsonicStreamUrl(const QUrl& url)
{
    if(url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https")) {
        return false;
    }
    return url.path().endsWith(QStringLiteral("/rest/stream"), Qt::CaseInsensitive)
        && !QUrlQuery{url}.queryItemValue(QStringLiteral("id")).isEmpty();
}

struct ContentRange
{
    qint64 start{-1};
    qint64 total{-1};
};

//! Parses a `Content-Range: bytes 0-123/456` header (`*` total yields -1).
inline ContentRange parseContentRange(const QByteArray& header)
{
    ContentRange range;
    const qsizetype slash = header.indexOf('/');
    if(slash < 0) {
        return range;
    }

    const QByteArray total = header.mid(slash + 1).trimmed();
    if(total != "*") {
        bool totalOk{false};
        range.total = total.toLongLong(&totalOk);
        if(!totalOk) {
            range.total = -1;
        }
    }

    const qsizetype space = header.indexOf(' ');
    const qsizetype dash  = space >= 0 ? header.indexOf('-', space + 1) : -1;
    if(dash > space) {
        bool startOk{false};
        range.start = header.mid(space + 1, dash - space - 1).trimmed().toLongLong(&startOk);
        if(!startOk) {
            range.start = -1;
        }
    }

    return range;
}

} // namespace Fooyin::Subsonic
