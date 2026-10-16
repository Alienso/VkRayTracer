//
// RangeAllocator (core/RangeAllocator.h): first fit, freed ranges join their free neighbors, growing frees the end.
//

#include <catch_amalgamated.hpp>
#include <random>
#include <vector>
#include "core/RangeAllocator.h"

TEST_CASE("RangeAllocator: nothing fits before it grows, except an empty range", "[RangeAllocator]") {
    RangeAllocator ranges;
    uint32_t offset = 99;
    CHECK_FALSE(ranges.allocate(1, offset));
    CHECK(ranges.allocate(0, offset));
    CHECK(offset == 0);
}

TEST_CASE("RangeAllocator: ranges are taken from the front, one after the other", "[RangeAllocator]") {
    RangeAllocator ranges;
    ranges.grow(10);
    CHECK(ranges.capacity() == 10);
    uint32_t a, b, c;
    REQUIRE(ranges.allocate(4, a));
    REQUIRE(ranges.allocate(4, b));
    CHECK(a == 0);
    CHECK(b == 4);
    CHECK_FALSE(ranges.allocate(3, c));  // 2 left
    REQUIRE(ranges.allocate(2, c));
    CHECK(c == 8);
    CHECK_FALSE(ranges.allocate(1, c));
}

TEST_CASE("RangeAllocator: a freed range joins the free ranges on both sides", "[RangeAllocator]") {
    RangeAllocator ranges;
    ranges.grow(9);
    uint32_t a, b, c;
    REQUIRE(ranges.allocate(3, a));
    REQUIRE(ranges.allocate(3, b));
    REQUIRE(ranges.allocate(3, c));
    ranges.release(a, 3);
    ranges.release(c, 3);
    uint32_t offset;
    CHECK_FALSE(ranges.allocate(4, offset));  // two holes of 3
    ranges.release(b, 3);                     // joins both
    REQUIRE(ranges.allocate(9, offset));
    CHECK(offset == 0);
}

TEST_CASE("RangeAllocator: growing frees the new end, joined with a free end before it", "[RangeAllocator]") {
    RangeAllocator ranges;
    ranges.grow(10);
    uint32_t a;
    REQUIRE(ranges.allocate(8, a));
    ranges.grow(20);
    ranges.grow(15);  // smaller: nothing changes
    CHECK(ranges.capacity() == 20);
    uint32_t b;
    REQUIRE(ranges.allocate(12, b));
    CHECK(b == 8);
}

TEST_CASE("RangeAllocator: first fit takes the first hole that is long enough", "[RangeAllocator]") {
    RangeAllocator ranges;
    ranges.grow(10);
    uint32_t a, b;
    REQUIRE(ranges.allocate(2, a));  // 0..2
    REQUIRE(ranges.allocate(8, b));  // 2..10
    ranges.release(a, 2);            // hole 0..2
    ranges.release(b + 3, 5);        // hole 5..10
    uint32_t offset;
    REQUIRE(ranges.allocate(3, offset));
    CHECK(offset == 5);
    REQUIRE(ranges.allocate(2, offset));
    CHECK(offset == 0);
}

TEST_CASE("RangeAllocator: clear forgets everything", "[RangeAllocator]") {
    RangeAllocator ranges;
    ranges.grow(10);
    ranges.clear();
    uint32_t offset;
    CHECK(ranges.capacity() == 0);
    CHECK_FALSE(ranges.allocate(1, offset));
}

// Against a plain list of taken elements: ranges never overlap, and an allocation fails only when no run of free
// elements is long enough
TEST_CASE("RangeAllocator: random allocations and releases match a simple model", "[RangeAllocator]") {
    std::mt19937 random(12345);
    RangeAllocator ranges;
    std::vector<bool> taken;
    struct Range { uint32_t offset, count; };
    std::vector<Range> live;
    auto longestFreeRun = [&]() {
        uint32_t best = 0, run = 0;
        for (bool t : taken) {
            run = t ? 0 : run + 1;
            best = std::max(best, run);
        }
        return best;
    };
    for (int step = 0; step < 5000; step++) {
        int action = std::uniform_int_distribution<int>(0, 9)(random);
        if (action == 0) {
            uint32_t newCapacity = ranges.capacity() + std::uniform_int_distribution<uint32_t>(0, 40)(random);
            ranges.grow(newCapacity);
            taken.resize(newCapacity, false);
        } else if (action <= 5) {
            uint32_t count = std::uniform_int_distribution<uint32_t>(1, 30)(random);
            uint32_t offset;
            bool fits = longestFreeRun() >= count;
            REQUIRE(ranges.allocate(count, offset) == fits);
            if (fits) {
                REQUIRE(offset + count <= taken.size());
                for (uint32_t i = offset; i < offset + count; i++) {
                    REQUIRE_FALSE(taken[i]);
                    taken[i] = true;
                }
                live.push_back({offset, count});
            }
        } else if (!live.empty()) {
            size_t which = std::uniform_int_distribution<size_t>(0, live.size() - 1)(random);
            Range range = live[which];
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(which));
            ranges.release(range.offset, range.count);
            for (uint32_t i = range.offset; i < range.offset + range.count; i++) {
                taken[i] = false;
            }
        }
    }
    // Everything back: one free range again
    for (const Range& range : live) {
        ranges.release(range.offset, range.count);
    }
    uint32_t offset;
    REQUIRE(ranges.allocate(ranges.capacity(), offset));
    CHECK(offset == 0);
}
