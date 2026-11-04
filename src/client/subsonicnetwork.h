/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "subsonicsettings.h"

#include <core/network/networkaccessmanager.h>

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslError>

#include <memory>

using namespace Qt::StringLiterals;

namespace Fooyin::Subsonic {
//! Creates a request with the plugin's default inactivity timeout.
inline QNetworkRequest subsonicRequest(const QUrl& url, int timeoutMs = 15000)
{
    QNetworkRequest request{url};
    request.setTransferTimeout(timeoutMs);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    return request;
}

//! Applies the "ignore SSL certificate errors" setting to a reply.
inline void applySslIgnore(QNetworkReply* reply, const SubsonicSettings& settings)
{
    if(reply && settings.ignoreSslErrors) {
        QObject::connect(reply, &QNetworkReply::sslErrors, reply,
                         [reply](const QList<QSslError>&) { reply->ignoreSslErrors(); });
    }
}

inline QNetworkReply* subsonicGet(const std::shared_ptr<NetworkAccessManager>& network, const QUrl& url,
                                  const SubsonicSettings& settings, int timeoutMs = 15000)
{
    if(!network) {
        return nullptr;
    }
    auto* reply = network->get(subsonicRequest(url, timeoutMs));
    applySslIgnore(reply, settings);
    return reply;
}

inline QNetworkReply* subsonicPost(const std::shared_ptr<NetworkAccessManager>& network, const QUrl& url,
                                   const QByteArray& body, const SubsonicSettings& settings, int timeoutMs = 30000)
{
    if(!network) {
        return nullptr;
    }
    QNetworkRequest request = subsonicRequest(url, timeoutMs);
    request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/x-www-form-urlencoded"_s);
    auto* reply = network->post(request, body);
    applySslIgnore(reply, settings);
    return reply;
}
} // namespace Fooyin::Subsonic
