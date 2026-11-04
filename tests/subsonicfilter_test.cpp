/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Keep assertions active in Release builds; CI configures the tests with -DNDEBUG.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include "subsonicfilter.h"

#include <cassert>
#include <iostream>

using namespace Fooyin::Subsonic;

int main()
{
    {
        const auto terms = parseFilter(QStringLiteral("hello world"), MatchMode::Contains);
        assert(terms.size() == 1);
        assert(terms.front().value == QStringLiteral("hello world"));
        assert(!terms.front().negated);
    }

    {
        const auto terms = parseFilter(QStringLiteral("hello world"), MatchMode::AllWords);
        assert(terms.size() == 2);
        assert(terms[0].value == QStringLiteral("hello"));
        assert(terms[1].value == QStringLiteral("world"));
    }

    {
        const auto terms = parseFilter(QStringLiteral("artist:beatles -genre:live"), MatchMode::AllWords);
        assert(terms.size() == 2);
        assert(terms[0].field == QStringLiteral("artist"));
        assert(terms[0].value == QStringLiteral("beatles"));
        assert(terms[1].field == QStringLiteral("genre"));
        assert(terms[1].negated);
    }

    {
        const auto terms = parseFilter(QStringLiteral("\"dark side\""), MatchMode::AllWords);
        assert(terms.size() == 1);
        assert(terms.front().value == QStringLiteral("dark side"));
    }

    {
        const auto terms = parseFilter(QStringLiteral("unknown:value"), MatchMode::AllWords);
        assert(terms.size() == 1);
        assert(terms.front().field.isEmpty());
        assert(terms.front().value == QStringLiteral("unknown:value"));
    }

    {
        const auto terms = parseFilter(QStringLiteral("beatles"), MatchMode::AllWords);
        const QHash<QString, QString> fields{{QStringLiteral("artist"), QStringLiteral("The Beatles")}};
        assert(matchesFilter(terms, MatchMode::AllWords, fields));
    }

    {
        const auto terms = parseFilter(QStringLiteral("artist:beatles genre:rock"), MatchMode::AllWords);
        const QHash<QString, QString> fields{{QStringLiteral("artist"), QStringLiteral("The Beatles")},
                                             {QStringLiteral("genre"), QStringLiteral("Rock")}};
        assert(matchesFilter(terms, MatchMode::AllWords, fields));
    }

    {
        const auto terms = parseFilter(QStringLiteral("-genre:live"), MatchMode::AllWords);
        const QHash<QString, QString> fields{{QStringLiteral("genre"), QStringLiteral("Live")}};
        assert(!matchesFilter(terms, MatchMode::AllWords, fields));
    }

    {
        const auto terms = parseFilter(QStringLiteral("rock jazz"), MatchMode::AnyWord);
        const QHash<QString, QString> fields{{QStringLiteral("genre"), QStringLiteral("Jazz")}};
        assert(matchesFilter(terms, MatchMode::AnyWord, fields));
    }

    {
        const auto terms = parseFilter(QStringLiteral("rock jazz"), MatchMode::AllWords);
        const QHash<QString, QString> fields{{QStringLiteral("genre"), QStringLiteral("Jazz")}};
        assert(!matchesFilter(terms, MatchMode::AllWords, fields));
    }

    assert(serverQueryFor(parseFilter(QStringLiteral("artist:beatles -live"), MatchMode::AllWords))
           == QStringLiteral("beatles"));

    std::cout << "subsonicfilter: ok\n";
    return 0;
}
