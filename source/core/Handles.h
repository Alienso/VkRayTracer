//
// Handles: how the systems refer to each other's things (a scene object to its model, the scene to its environment
// map) instead of raw positions in a list. An index plus a generation: once a thing is released and its slot reused,
// the generation tells a stale handle from the new occupant. One type per kind of thing, so they can't be mixed up.
//

#ifndef VULKANDEMO_HANDLES_H
#define VULKANDEMO_HANDLES_H

#include <cstdint>

template <typename Tag>
struct Handle {
    static constexpr uint32_t invalidIndex = ~0u;
    uint32_t index = invalidIndex;
    uint32_t generation = 0;

    bool valid() const { return index != invalidIndex; }
    bool operator==(const Handle& other) const { return index == other.index && generation == other.generation; }
    bool operator!=(const Handle& other) const { return !(*this == other); }
};

using ModelHandle = Handle<struct ModelTag>;
using EnvironmentHandle = Handle<struct EnvironmentTag>;

#endif //VULKANDEMO_HANDLES_H
