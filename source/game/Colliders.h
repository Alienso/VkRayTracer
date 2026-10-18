//
// The collider component's box (game/behaviours/Collider.cpp), for the game and for the editor's outlines: the
// program hands the editor the boxes (it doesn't know the game).
//

#ifndef VULKANDEMO_COLLIDERS_H
#define VULKANDEMO_COLLIDERS_H

#include <vector>
#include "game/GameProperties.h"
#include "scene/Scene.h"

// The box of a collider with these properties on the entity, in the entity's own space: around its model when size
// is [0, 0, 0] (a unit cube without a loaded model), else that size around offset
void colliderBox(const Scene& scene, EntityId entity, GameProperties& properties, glm::vec3& center,
                 glm::vec3& halfSize);

// Whether the entity's collider takes portals (its "portalable", true when not given; false without a collider)
bool colliderPortalable(const Scene& scene, EntityId entity);

// The colliders' boxes placed in the world (WorldBox: the entity's world transform, the box in its own space): of
// the entity given, or of all shown entities (0). Each one part way through a portal is followed by its pieces out of
// the other ends (ghost: their transform takes the box there; up to maxDepth crossings), as the physics has them.
std::vector<WorldBox> colliderBoxes(const Scene& scene, EntityId only, int maxDepth = 8);

#endif //VULKANDEMO_COLLIDERS_H
