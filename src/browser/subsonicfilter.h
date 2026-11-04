/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

using namespace Qt::StringLiterals;

namespace Fooyin::Subsonic {
enum class MatchMode
{
    Contains = 0,
    AllWords,
    AnyWord,
};

struct FilterTerm
{
    QString field; // empty = match any field
    QString value;
    bool negated{false};
};

//! Splits the filter into field-scoped/negated terms. In Contains mode, plain words are joined back
//! into one phrase so they must appear together; in the word modes they stay separate.
inline QList<FilterTerm> parseFilter(const QString& text, MatchMode mode)
{
    static const QSet<QString> knownFields
        = {u"artist"_s, u"album"_s, u"title"_s, u"genre"_s, u"playlist"_s, u"year"_s};

    QStringList words;
    QString current;
    bool inQuotes{false};
    for(const QChar ch : text) {
        if(ch == u'"') {
            inQuotes = !inQuotes;
            continue;
        }
        if(ch.isSpace() && !inQuotes) {
            if(!current.isEmpty()) {
                words.push_back(current);
                current.clear();
            }
            continue;
        }
        current += ch;
    }
    if(!current.isEmpty()) {
        words.push_back(current);
    }

    QList<FilterTerm> terms;
    QString barePhrase;
    for(QString word : words) {
        FilterTerm term;
        if(word.size() > 1 && word.startsWith(u'-')) {
            term.negated = true;
            word.remove(0, 1);
        }
        const int colon = word.indexOf(u':');
        if(colon > 0) {
            const QString field = word.left(colon).toLower();
            if(knownFields.contains(field)) {
                term.field = field;
                word       = word.mid(colon + 1);
            }
        }
        if(word.isEmpty()) {
            continue;
        }
        if(mode == MatchMode::Contains && !term.negated && term.field.isEmpty()) {
            if(!barePhrase.isEmpty()) {
                barePhrase += u' ';
            }
            barePhrase += word;
            continue;
        }
        term.value = word;
        terms.push_back(term);
    }
    if(!barePhrase.isEmpty()) {
        FilterTerm phrase;
        phrase.value = barePhrase;
        terms.push_front(phrase);
    }
    return terms;
}

inline QString serverQueryFor(const QList<FilterTerm>& terms)
{
    QStringList parts;
    for(const FilterTerm& term : terms) {
        if(!term.negated && !term.value.isEmpty()) {
            parts.push_back(term.value);
        }
    }
    return parts.join(u' ');
}

inline bool termMatches(const FilterTerm& term, const QHash<QString, QString>& fields)
{
    if(term.field.isEmpty()) {
        for(const QString& value : fields) {
            if(value.contains(term.value, Qt::CaseInsensitive)) {
                return true;
            }
        }
        return false;
    }
    return fields.value(term.field).contains(term.value, Qt::CaseInsensitive);
}

inline bool matchesFilter(const QList<FilterTerm>& terms, MatchMode mode, const QHash<QString, QString>& fields)
{
    if(terms.isEmpty()) {
        return true;
    }
    bool anyPositive{false};
    bool positive{mode != MatchMode::AnyWord}; // AND starts true, OR starts false
    for(const FilterTerm& term : terms) {
        const bool matched = termMatches(term, fields);
        if(term.negated) {
            if(matched) {
                return false;
            }
            continue;
        }
        anyPositive = true;
        positive    = (mode == MatchMode::AnyWord) ? (positive || matched) : (positive && matched);
    }
    return anyPositive ? positive : true;
}

inline QHash<QString, QString> artistFields(const QJsonObject& artist)
{
    return {{u"artist"_s, artist.value(u"name"_s).toString()}};
}

inline QHash<QString, QString> albumFields(const QJsonObject& album)
{
    return {{u"album"_s, album.value(u"name"_s).toString()}, {u"artist"_s, album.value(u"artist"_s).toString()}};
}

inline QHash<QString, QString> songFields(const QJsonObject& song)
{
    QHash<QString, QString> fields;
    fields[u"title"_s]  = song.value(u"title"_s).toString();
    fields[u"artist"_s] = song.value(u"artist"_s).toString();
    fields[u"album"_s]  = song.value(u"album"_s).toString();
    fields[u"genre"_s]  = song.value(u"genre"_s).toString();
    fields[u"year"_s]   = QString::number(song.value(u"year"_s).toInt());
    return fields;
}

inline QHash<QString, QString> playlistFields(const QJsonObject& playlist, const QJsonArray& entries)
{
    QHash<QString, QString> fields;
    fields[u"playlist"_s] = playlist.value(u"name"_s).toString();

    QString titles;
    QString artists;
    QString albums;
    for(const QJsonValue& value : entries) {
        const QJsonObject entry = value.toObject();
        titles += entry.value(u"title"_s).toString() + u' ';
        artists += entry.value(u"artist"_s).toString() + u' ';
        albums += entry.value(u"album"_s).toString() + u' ';
    }
    if(!titles.isEmpty()) {
        fields[u"title"_s] = titles;
    }
    if(!artists.isEmpty()) {
        fields[u"artist"_s] = artists;
    }
    if(!albums.isEmpty()) {
        fields[u"album"_s] = albums;
    }
    return fields;
}
} // namespace Fooyin::Subsonic
