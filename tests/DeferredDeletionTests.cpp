//
// DeferredDeletion (core/DeferredDeletion.h): something released while preparing frame F goes once frame
// F + framesInFlight starts, in the order deferred.
//

#include <catch_amalgamated.hpp>
#include <string>
#include <vector>
#include "core/DeferredDeletion.h"

TEST_CASE("DeferredDeletion: runs once the frames in flight that may use it are done", "[DeferredDeletion]") {
    DeferredDeletion deletion(2);
    int runs = 0;
    deletion.defer(5, [&]() { runs++; });
    deletion.collect(5);
    deletion.collect(6);  // frame 5 may still be on the GPU (frames 5 and 6 in flight)
    CHECK(runs == 0);
    deletion.collect(7);  // frame 7's slot is frame 5's: its fence was waited for
    CHECK(runs == 1);
    deletion.collect(8);
    CHECK(runs == 1);
    CHECK(deletion.size() == 0);
}

TEST_CASE("DeferredDeletion: keeps the ones not due yet, in order", "[DeferredDeletion]") {
    DeferredDeletion deletion(2);
    std::vector<std::string> order;
    deletion.defer(1, [&]() { order.push_back("a1"); });
    deletion.defer(3, [&]() { order.push_back("b3"); });
    deletion.defer(1, [&]() { order.push_back("c1"); });
    deletion.defer(2, [&]() { order.push_back("d2"); });
    deletion.collect(3);
    CHECK(order == std::vector<std::string>{"a1", "c1"});
    CHECK(deletion.size() == 2);
    deletion.collect(4);
    CHECK(order == std::vector<std::string>{"a1", "c1", "d2"});
    deletion.flush();
    CHECK(order == std::vector<std::string>{"a1", "c1", "d2", "b3"});
    CHECK(deletion.size() == 0);
}

// The renderer's loop: frame F's slot is F % framesInFlight; starting frame F waits for that slot's fence, i.e. for
// frame F - framesInFlight. A release made while preparing frame F runs at the start of frame F + framesInFlight:
// the first frame by which every frame that could use it (F and the ones before) is done, and no later.
TEST_CASE("DeferredDeletion: runs at the first frame no frame using it can be in flight", "[DeferredDeletion]") {
    for (uint64_t framesInFlight : {1u, 2u, 3u}) {
        DeferredDeletion deletion(framesInFlight);
        uint64_t current = 0;
        std::vector<std::pair<uint64_t, uint64_t>> ran;  // (released in, ran at)
        for (current = 0; current < 30; current++) {
            deletion.collect(current);  // start of frame, after its slot's fence
            if (current % 3 == 0 && current < 20) {
                uint64_t released = current;
                deletion.defer(current, [&ran, &current, released]() { ran.emplace_back(released, current); });
            }
        }
        REQUIRE(ran.size() == 7);
        for (const auto& [released, at] : ran) {
            CHECK(at == released + framesInFlight);
        }
    }
}
