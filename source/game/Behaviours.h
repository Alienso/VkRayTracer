//
// The behaviour types (game/behaviours/*.cpp), listed once in Behaviours.cpp. See game/Behaviour.h.
//

#ifndef VULKANDEMO_BEHAVIOURS_H
#define VULKANDEMO_BEHAVIOURS_H

#include <memory>
#include <vector>
#include "game/Behaviour.h"

// One empty list per type, in update order: when two behaviours move the same thing (spin and door both turn the
// entity), the later one wins
std::vector<std::unique_ptr<BehaviourListBase>> makeBehaviourLists();

#endif //VULKANDEMO_BEHAVIOURS_H
