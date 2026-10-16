//
// Blending the game's steps for showing. See StepInterpolation.h.
//

#include <unordered_map>
#include "game/StepInterpolation.h"

static bool same(const Transform& a, const Transform& b) {
    return a.position == b.position && a.rotation == b.rotation && a.scale == b.scale;
}

static Transform mix(const Transform& a, const Transform& b, float t) {
    Transform result;
    result.position = glm::mix(a.position, b.position, t);
    result.rotation = glm::slerp(a.rotation, b.rotation, t);  // (the shorter way round)
    result.scale = glm::mix(a.scale, b.scale, t);
    return result;
}

void StepInterpolation::clear() {
    before.clear();
    moving.clear();
    snapped.clear();
}

void StepInterpolation::restore(Scene& scene) {
    for (size_t i = 0; i < moving.size();) {
        Entity* entity = scene.find(moving[i].id);
        if (entity != nullptr && same(entity->local, moving[i].shown)) {
            entity->local = moving[i].current;
            i++;
        } else {  // removed, or moved by something else: left as it is, no longer blended
            moving.erase(moving.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
}

void StepInterpolation::beforeStep(const Scene& scene) {
    if (!enabled) {
        return;
    }
    const std::vector<Entity>& entities = scene.entities();
    before.resize(entities.size());
    for (size_t i = 0; i < entities.size(); i++) {
        before[i] = {entities[i].id, entities[i].local};
    }
}

void StepInterpolation::blend(Scene& scene, int stepsRun, float alpha) {
    if (!enabled) {
        moving.clear();
        snapped.clear();
        return;
    }
    if (stepsRun > 0) {
        moving.clear();
        const std::vector<Entity>& entities = scene.entities();
        // The same order as before the step unless entities were added or removed in it: then by id
        std::unordered_map<EntityId, size_t> index;
        bool aligned = before.size() == entities.size();
        for (size_t i = 0; aligned && i < entities.size(); i++) {
            aligned = before[i].id == entities[i].id;
        }
        if (!aligned) {
            for (size_t i = 0; i < before.size(); i++) {
                index[before[i].id] = i;
            }
        }
        for (size_t i = 0; i < entities.size(); i++) {
            const Entity& entity = entities[i];
            const Saved* saved = nullptr;
            if (aligned) {
                saved = &before[i];
            } else if (auto it = index.find(entity.id); it != index.end()) {
                saved = &before[it->second];
            }
            // (Added in the step: shown where it is)
            if (saved != nullptr && !same(saved->local, entity.local) && !snapped.count(entity.id)) {
                moving.push_back({entity.id, saved->local, entity.local, entity.local});
            }
        }
        snapped.clear();
    }
    alpha = glm::clamp(alpha, 0.0f, 1.0f);
    for (Moving& m : moving) {
        if (Entity* entity = scene.find(m.id)) {
            m.shown = mix(m.previous, m.current, alpha);
            entity->local = m.shown;
        }
    }
}
