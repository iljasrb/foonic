/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "subsonicstreamcache.h"

#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>

#include <chrono>
#include <iostream>
#include <thread>

namespace {
int failures{0};

void check(bool condition, const char* message)
{
    if(!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void testMergeAndContains()
{
    Fooyin::Subsonic::RangeSet ranges;
    check(!ranges.contains(0), "empty set contains nothing");
    check(ranges.rangeEnd(0) == -1, "empty set range end is -1");

    ranges.add(10, 20);
    check(ranges.contains(10), "start of range is cached");
    check(ranges.contains(19), "last byte is cached");
    check(!ranges.contains(20), "end is exclusive");
    check(ranges.rangeEnd(10) == 20, "range end");
    check(ranges.rangeEnd(15) == 20, "range end inside");
    check(ranges.rangeEnd(20) == -1, "no range at end");

    ranges.add(20, 30); // adjacent, must merge
    check(ranges.ranges().size() == 1, "adjacent ranges merge");
    check(ranges.rangeEnd(10) == 30, "merged range end");
    check(ranges.contiguousBytes(10) == 20, "contiguous bytes from 10");
    check(ranges.contiguousBytes(25) == 5, "contiguous bytes from 25");

    ranges.add(5, 15); // overlap, must merge and extend
    check(ranges.ranges().size() == 1, "overlapping ranges merge");
    check(ranges.ranges().front().start == 5, "merge extends start");
    check(ranges.ranges().front().end == 30, "merge keeps end");

    ranges.add(50, 60);
    ranges.add(40, 55); // bridges 50..60 and 40..55, then joins 5..30? no
    check(ranges.ranges().size() == 2, "separate range remains separate");
    check(ranges.ranges().back().start == 40, "second range start");
    check(ranges.ranges().back().end == 60, "second range end");

    ranges.add(25, 45); // joins both
    check(ranges.ranges().size() == 1, "bridge merges everything");
    check(ranges.ranges().front().start == 5, "bridge start");
    check(ranges.ranges().front().end == 60, "bridge end");
}

void testGaps()
{
    Fooyin::Subsonic::RangeSet ranges;
    const auto firstGap = ranges.firstGap(100);
    check(firstGap.has_value() && firstGap->start == 0 && firstGap->end == 100, "empty set has full gap");

    ranges.add(0, 40);
    auto gap = ranges.firstGap(100);
    check(gap.has_value() && gap->start == 40 && gap->end == 100, "gap after prefix");
    check(!ranges.coversAll(100), "prefix does not cover all");

    ranges.add(80, 100);
    gap = ranges.firstGap(100);
    check(gap.has_value() && gap->start == 40 && gap->end == 80, "hole between ranges");

    ranges.add(40, 80);
    check(!ranges.firstGap(100).has_value(), "no gap once filled");
    check(ranges.coversAll(100), "covers all");

    ranges.add(0, 100);
    check(ranges.coversAll(100), "overlapping full range covers all");
    check(!ranges.firstGap(0).has_value(), "zero total has no gap");
}

void testSeekingRanges()
{
    Fooyin::Subsonic::RangeSet ranges;
    ranges.add(0, 100);
    ranges.add(200, 300);
    ranges.add(400, 500);

    // Seeking inside a cached range is a pure local read.
    check(ranges.contiguousBytes(0) == 100, "range 0..100");
    check(ranges.contiguousBytes(100) == 0, "hole after first range");
    check(ranges.contiguousBytes(250) == 50, "range 200..300");
    check(ranges.contiguousBytes(399) == 0, "hole before last range");
    check(ranges.contiguousBytes(499) == 1, "last byte");
    check(ranges.contiguousBytes(500) == 0, "EOF");
}

void testTrim()
{
    Fooyin::Subsonic::RangeSet ranges;
    ranges.add(0, 100);
    ranges.add(200, 300);
    ranges.trim(250);
    check(ranges.ranges().size() == 2, "trim keeps the ranges below the new total");
    check(ranges.ranges().back().end == 250, "trim cuts the last range");
    const auto hole = ranges.firstGap(250);
    check(hole.has_value() && hole->start == 100 && hole->end == 200, "trim keeps the remaining hole");
    ranges.add(100, 200);
    check(ranges.coversAll(250), "trim can make a smaller file complete");
    ranges.trim(50);
    check(ranges.ranges().size() == 1, "trim removes ranges beyond the new total");
    check(ranges.ranges().front().end == 50, "trim cuts the first range");
}

QByteArray bytes(int size, char value)
{
    return QByteArray(size, value);
}

void testEntryIO(const QString& baseDir)
{
    Fooyin::Subsonic::SessionCache cache{baseDir, 4LL * 1024 * 1024};
    auto entry = cache.entry(QStringLiteral("song-a"));
    check(entry != nullptr, "cache entry created");
    check(!entry->complete(), "new entry is incomplete");

    const QByteArray data = bytes(600 * 1024, 'A');
    check(entry->writeAt(0, data.constData(), data.size()) == data.size(), "entry write");
    entry->setTotal(data.size());
    check(entry->complete(), "entry complete after full write");

    QByteArray read(data.size(), Qt::Uninitialized);
    check(entry->readAt(read.data(), read.size(), 0) == read.size(), "entry read size");
    check(read == data, "entry read contents");
    check(entry->readAt(read.data(), 16, data.size() + 1) == 0, "uncached read returns zero");

    auto same = cache.entry(QStringLiteral("song-a"));
    check(same.get() == entry.get(), "cache returns the same entry for an id");
}

void testEstimatedTotal(const QString& baseDir)
{
    Fooyin::Subsonic::SessionCache cache{baseDir, 4LL * 1024 * 1024};
    auto entry = cache.entry(QStringLiteral("estimated"));
    entry->setEstimatedTotal(100);
    check(entry->total() == 100, "estimated total is recorded");
    entry->setEstimatedTotal(80);
    check(entry->total() == 100, "a smaller estimate does not shrink the known size");
    entry->setEstimatedTotal(120);
    check(entry->total() == 120, "a larger estimate grows the known size");
    entry->setTotal(90);
    check(entry->total() == 90, "an authoritative total can shrink the size");
}

void testEviction(const QString& baseDir)
{
    {
        Fooyin::Subsonic::SessionCache cache{baseDir, 1000};
        {
            auto first = cache.entry(QStringLiteral("first"));
            const QByteArray data = bytes(600, '1');
            first->writeAt(0, data.constData(), data.size());
            first->setTotal(data.size());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        {
            auto second = cache.entry(QStringLiteral("second"));
            const QByteArray data = bytes(600, '2');
            second->writeAt(0, data.constData(), data.size());
            second->setTotal(data.size());
        }
        cache.evict();
        check(cache.find(QStringLiteral("first")) == nullptr, "oldest unused entry is evicted");
        check(cache.find(QStringLiteral("second")) != nullptr, "newest entry is kept");
    }

    {
        Fooyin::Subsonic::SessionCache cache{baseDir, 1000};
        {
            auto pinned = cache.entry(QStringLiteral("pinned"));
            const QByteArray data = bytes(600, 'p');
            pinned->writeAt(0, data.constData(), data.size());
            pinned->setTotal(data.size());
        }
        cache.pin(QStringLiteral("pinned"));
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        {
            auto other = cache.entry(QStringLiteral("other"));
            const QByteArray data = bytes(600, 'o');
            other->writeAt(0, data.constData(), data.size());
            other->setTotal(data.size());
        }
        cache.evict();
        check(cache.find(QStringLiteral("pinned")) != nullptr, "pinned entry is not evicted");
        check(cache.find(QStringLiteral("other")) == nullptr, "unpinned entry is evicted first");
    }

    {
        Fooyin::Subsonic::SessionCache cache{baseDir, 1000};
        auto inUse = cache.entry(QStringLiteral("in-use"));
        const QByteArray data = bytes(600, 'u');
        inUse->writeAt(0, data.constData(), data.size());
        inUse->setTotal(data.size());
        {
            auto other = cache.entry(QStringLiteral("other"));
            const QByteArray otherData = bytes(600, 'x');
            other->writeAt(0, otherData.constData(), otherData.size());
            other->setTotal(otherData.size());
        }
        cache.evict();
        check(cache.find(QStringLiteral("in-use")) != nullptr, "entry held by a reader is not evicted");
    }
}

void testDiscard(const QString& baseDir)
{
    Fooyin::Subsonic::SessionCache cache{baseDir, 4LL * 1024 * 1024};
    auto entry = cache.entry(QStringLiteral("partial"));
    const QByteArray data = bytes(100, 'p');
    entry->writeAt(0, data.constData(), data.size());
    entry->setTotal(200);
    check(!entry->complete(), "partial entry is incomplete");

    entry.reset();
    check(cache.discardIncomplete(QStringLiteral("partial")), "incomplete entry is discarded");
    check(cache.find(QStringLiteral("partial")) == nullptr, "discarded entry is gone");

    auto complete = cache.entry(QStringLiteral("complete"));
    const QByteArray full = bytes(100, 'c');
    complete->writeAt(0, full.constData(), full.size());
    complete->setTotal(full.size());
    check(complete->complete(), "complete entry");
    complete.reset();
    check(!cache.discardIncomplete(QStringLiteral("complete")), "complete entry is not discarded as incomplete");
}

void testSessionDirectory(const QString& baseDir)
{
    QString directory;
    {
        Fooyin::Subsonic::SessionCache cache{baseDir, 4LL * 1024 * 1024};
        directory = cache.directory();
        check(QDir{directory}.exists(), "session directory exists");
        auto entry = cache.entry(QStringLiteral("dir-test"));
        const QByteArray data = bytes(32, 'd');
        entry->writeAt(0, data.constData(), data.size());
        entry->setTotal(data.size());
    }
    check(!QDir{directory}.exists(), "session directory is removed with the cache");
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app{argc, argv};

    testMergeAndContains();
    testGaps();
    testSeekingRanges();
    testTrim();

    QTemporaryDir root;
    if(!root.isValid()) {
        std::cerr << "FAIL: could not create a temporary cache directory\n";
        return 1;
    }
    testEntryIO(root.path());
    testEstimatedTotal(root.path());
    testEviction(root.path());
    testDiscard(root.path());
    testSessionDirectory(root.path());

    if(failures == 0) {
        std::cout << "subsonic_streamcache: all checks passed\n";
    }
    return failures == 0 ? 0 : 1;
}
