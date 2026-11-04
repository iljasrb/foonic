/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "subsonicstreamcache.h"

#include <QCoreApplication>

#include <iostream>

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
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app{argc, argv};

    testMergeAndContains();
    testGaps();
    testSeekingRanges();

    if(failures == 0) {
        std::cout << "subsonic_streamcache: all checks passed\n";
    }
    return failures == 0 ? 0 : 1;
}
