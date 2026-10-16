//
// Destroying GPU objects only once the frames in flight that may still use them are done (GpuScene's releases).
// No Vulkan itself: it runs what it was given, by frame number.
//

#ifndef VULKANDEMO_DEFERREDDELETION_H
#define VULKANDEMO_DEFERREDDELETION_H

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

// GPU objects to destroy once no frame in flight can use them any more. Something released while preparing frame
// F may still be read by frames F-1 .. F-maxFramesInFlight+1 (already submitted); it goes once frame F's slot comes
// round again, i.e. once frame F + framesInFlight starts (its fence wait means frame F finished).
class DeferredDeletion {
public:
    explicit DeferredDeletion(uint64_t framesInFlight) : framesInFlight(framesInFlight) {}

    // destroy runs at the start of frame `frame + framesInFlight` or later
    void defer(uint64_t frame, std::function<void()> destroy) { pending.emplace_back(frame, std::move(destroy)); }

    // Call at the start of `frame`, after waiting for its slot's fence
    void collect(uint64_t frame) {
        size_t kept = 0;
        for (size_t i = 0; i < pending.size(); i++) {
            if (pending[i].first + framesInFlight <= frame) {
                pending[i].second();
            } else {
                if (kept != i) {
                    pending[kept] = std::move(pending[i]);
                }
                kept++;
            }
        }
        pending.resize(kept);
    }

    // Everything now (the device must be idle: shutdown)
    void flush() {
        for (auto& entry : pending) {
            entry.second();
        }
        pending.clear();
    }

    size_t size() const { return pending.size(); }

private:
    uint64_t framesInFlight;
    std::vector<std::pair<uint64_t, std::function<void()>>> pending;
};

#endif //VULKANDEMO_DEFERREDDELETION_H
