//
// Undo and redo for the editor: every change to the scene, as the scene's text before and after it
// (Scene::contentText: entities, components, the models' material and light edits, the environment, the scene's
// settings; not the camera). The editor commits after each edit (a field or slider let go, a checkbox or menu item
// clicked, a drag in the Outliner dropped, a handle let go, a key like Delete): what differs from the text at the
// last commit is one change. Undoing loads the text before it back (Scene::loadText: the entity ids stay the same),
// redoing the text after; the camera stays where it is.
//
// Scene texts are small (only what differs from the model files; a few KB even for Jungle Ruins), so whole texts
// are kept: no per-kind undo code, and adding, deleting, re-parenting, materials and the rest all undo the same way.
//

#ifndef VULKANDEMO_EDITHISTORY_H
#define VULKANDEMO_EDITHISTORY_H

#include <cstddef>
#include <string>
#include <vector>
#include "scene/Scene.h"

class EditHistory {
public:
    static constexpr size_t maxChanges = 100;            // the oldest go
    static constexpr size_t maxBytes = 64u << 20;        // and when the texts together get bigger than this

    // A new start (a scene opened, New, Stop): nothing to undo, the scene as it is now is where changes count from
    void reset(const Scene& scene) {
        undoList.clear();
        redoList.clear();
        bytes = 0;
        committed = scene.contentText();
    }

    // The scene as it is now is where the next change counts from, without a change for undo (Stop put back the
    // scene of the last commit)
    void sync(const Scene& scene) { committed = scene.contentText(); }

    // After an edit: the scene's change since the last commit becomes one change (none if it didn't change).
    // A new change drops what could be redone. True if there was one.
    bool commit(const Scene& scene) {
        std::string now = scene.contentText();
        if (now == committed) {
            return false;
        }
        for (const Change& change : redoList) {
            bytes -= change.before.size() + change.after.size();
        }
        redoList.clear();
        bytes += committed.size() + now.size();
        undoList.push_back({std::move(committed), now});
        committed = std::move(now);
        while (!undoList.empty() && (undoList.size() > maxChanges || bytes > maxBytes)) {
            bytes -= undoList.front().before.size() + undoList.front().after.size();
            undoList.erase(undoList.begin());
        }
        return true;
    }

    // The last change undone / redone (a change not committed yet is committed first). False when there is none,
    // or the text couldn't be loaded (not expected: the texts are the scene's own; the change is dropped then).
    bool undo(Scene& scene) {
        commit(scene);
        return step(scene, undoList, redoList, false);
    }
    bool redo(Scene& scene) {
        commit(scene);
        return step(scene, redoList, undoList, true);
    }

    [[nodiscard]] bool canUndo() const { return !undoList.empty(); }
    [[nodiscard]] bool canRedo() const { return !redoList.empty(); }
    [[nodiscard]] size_t undoCount() const { return undoList.size(); }

private:
    struct Change {
        std::string before;
        std::string after;
    };
    std::vector<Change> undoList;
    std::vector<Change> redoList;
    std::string committed;  // the scene's text at the last commit (or reset, undo, redo)
    size_t bytes = 0;       // of the texts in both lists

    bool step(Scene& scene, std::vector<Change>& from, std::vector<Change>& to, bool forward) {
        if (from.empty()) {
            return false;
        }
        Change change = std::move(from.back());
        from.pop_back();
        const std::string& text = forward ? change.after : change.before;
        Camera camera = scene.camera();  // (not part of a change: it stays where it is)
        try {
            scene.loadText(text, forward ? "redo" : "undo");
        } catch (const std::exception&) {
            bytes -= change.before.size() + change.after.size();
            return false;
        }
        scene.camera() = camera;
        committed = scene.contentText();
        to.push_back(std::move(change));
        return true;
    }
};

#endif //VULKANDEMO_EDITHISTORY_H
