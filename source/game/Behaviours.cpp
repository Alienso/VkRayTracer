//
// The behaviour types. A new one: its file in game/behaviours/ (and CMakeLists.txt), and a line in each list below.
//

#include "game/Behaviours.h"

std::unique_ptr<BehaviourListBase> makeSpinList();
std::unique_ptr<BehaviourListBase> makeBobList();
std::unique_ptr<BehaviourListBase> makeDoorList();
std::unique_ptr<BehaviourListBase> makePickupList();
std::unique_ptr<BehaviourListBase> makeTriggerList();
std::unique_ptr<BehaviourListBase> makeSpawnerList();
std::unique_ptr<BehaviourListBase> makeColliderList();
std::unique_ptr<BehaviourListBase> makePlayerList();
std::unique_ptr<BehaviourListBase> makeCameraPathList();

std::vector<std::unique_ptr<BehaviourListBase>> makeBehaviourLists() {
    std::vector<std::unique_ptr<BehaviourListBase>> lists;
    // (Colliders after what moves things, so they follow this step's doors; the player after them)
    for (auto make : {makeSpinList, makeBobList, makeDoorList, makePickupList, makeTriggerList, makeSpawnerList,
                      makeColliderList, makePlayerList, makeCameraPathList}) {
        lists.push_back(make());
    }
    return lists;
}
