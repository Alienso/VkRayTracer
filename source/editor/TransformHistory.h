//
// Undo and redo for transform changes in the editor: a gizmo drag, a drag or a typed value in the Details panel's
// transform, "Reset transform". Each change is an entity with its local transform before and after; undoing puts
// the before back, redoing the after. Only transforms: adding, removing, re-parenting and the rest aren't undone
// (the history is cleared when the scene is replaced, and at Play and Stop).
//

#ifndef VULKANDEMO_TRANSFORMHISTORY_H
#define VULKANDEMO_TRANSFORMHISTORY_H

#include <cstddef>
#include <vector>
#include "scene/Scene.h"

class TransformHistory {
public:
    static constexpr size_t maxChanges = 200;  // the oldest go

    // An entity's transform went from before to after. Nothing when they are the same; a new change drops what
    // could be redone.
    void record(EntityId entity, const Transform& before, const Transform& after) {
        if (same(before, after)) {
            return;
        }
        redoList.clear();
        undoList.push_back({entity, before, after});
        if (undoList.size() > maxChanges) {
            undoList.erase(undoList.begin());
        }
    }

    // The last change undone (its entity back to before); false when there is nothing to undo. Changes of entities
    // that are gone are skipped (dropped). Returns the entity in changed.
    bool undo(Scene& scene, EntityId& changed) { return step(scene, undoList, redoList, false, changed); }
    bool redo(Scene& scene, EntityId& changed) { return step(scene, redoList, undoList, true, changed); }

    [[nodiscard]] bool canUndo() const { return !undoList.empty(); }
    [[nodiscard]] bool canRedo() const { return !redoList.empty(); }
    void clear() {
        undoList.clear();
        redoList.clear();
    }

    static bool same(const Transform& a, const Transform& b) {
        return a.position == b.position && a.rotation == b.rotation && a.scale == b.scale;
    }

private:
    struct Change {
        EntityId entity;
        Transform before;
        Transform after;
    };
    std::vector<Change> undoList;
    std::vector<Change> redoList;

    static bool step(Scene& scene, std::vector<Change>& from, std::vector<Change>& to, bool forward, EntityId& changed) {
        while (!from.empty()) {
            Change change = from.back();
            from.pop_back();
            Entity* entity = scene.find(change.entity);
            if (entity == nullptr) {
                continue;  // (removed since: its changes go too)
            }
            entity->local = forward ? change.after : change.before;
            to.push_back(change);
            changed = change.entity;
            return true;
        }
        return false;
    }
};

#endif //VULKANDEMO_TRANSFORMHISTORY_H
