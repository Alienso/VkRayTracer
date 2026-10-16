//
// Space in a buffer that grows: ranges are taken and given back (the renderer's shared vertex and index buffers,
// GpuAssets.h). No Vulkan: offsets and counts only.
//

#ifndef VULKANDEMO_RANGEALLOCATOR_H
#define VULKANDEMO_RANGEALLOCATOR_H

#include <cstdint>
#include <iterator>
#include <map>

// The free parts of a buffer of `capacity` elements: a range is taken from the first free part it fits in (first
// fit), and a freed range joins its free neighbors, so space left by unloaded meshes is reused.
class RangeAllocator {
public:
    [[nodiscard]] uint32_t capacity() const { return total; }

    // The buffer is now newCapacity long: the added end is free
    void grow(uint32_t newCapacity) {
        if (newCapacity > total) {
            release(total, newCapacity - total);
            total = newCapacity;
        }
    }

    // false: no free part is long enough (grow first). An empty range always fits (offset 0).
    bool allocate(uint32_t count, uint32_t& offset) {
        offset = 0;
        if (count == 0) {
            return true;
        }
        for (auto it = freeRanges.begin(); it != freeRanges.end(); ++it) {
            if (it->second >= count) {
                offset = it->first;
                uint32_t left = it->second - count;
                freeRanges.erase(it);
                if (left > 0) {
                    freeRanges[offset + count] = left;
                }
                return true;
            }
        }
        return false;
    }

    void release(uint32_t offset, uint32_t count) {
        if (count == 0) {
            return;
        }
        auto next = freeRanges.lower_bound(offset);
        if (next != freeRanges.end() && next->first == offset + count) {  // joins the free part after it
            count += next->second;
            next = freeRanges.erase(next);
        }
        if (next != freeRanges.begin()) {
            auto previous = std::prev(next);
            if (previous->first + previous->second == offset) {  // and the one before
                previous->second += count;
                return;
            }
        }
        freeRanges[offset] = count;
    }

    void clear() {
        freeRanges.clear();
        total = 0;
    }

private:
    std::map<uint32_t, uint32_t> freeRanges;  // offset -> length
    uint32_t total = 0;
};

#endif //VULKANDEMO_RANGEALLOCATOR_H
