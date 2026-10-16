//
// Showing the game's fixed steps smoothly (game/GameLoop.h). The game moves entities 60 times a second; a display
// at another rate would show them holding still for some frames and jumping in others. So each frame the entities
// that moved in the last step are shown between where they were before it and after it, by how far the clock is
// into the next step (GameLoop::owed): a blend, at most one step behind. Only what is shown is blended: before the
// next steps the stepped transforms are put back, so the game never sees a blended value.
//
// It knows nothing of the game: it compares the entities' local transforms (and the camera) before and after the
// last step of a frame. A jump (respawn, teleport) is shown at once when the game says so (snap). Visibility isn't
// blended. The camera is blended only when a step moved it (the game drives it); the fly camera moves it between
// frames, which isn't a step's change.
//

#ifndef VULKANDEMO_STEPINTERPOLATION_H
#define VULKANDEMO_STEPINTERPOLATION_H

#include <unordered_set>
#include <vector>
#include "scene/Scene.h"

class StepInterpolation {
public:
    bool enabled = true;  // off: the stepped transforms are shown as they are

    void clear();  // at Play and Stop
    // Before a frame's steps: the blended entities back to their stepped transforms. One changed by something else
    // since the blend (the editor) keeps that change.
    void restore(Scene& scene);
    // Before each step: the entities' transforms, to find the ones the step moves
    void beforeStep(const Scene& scene);
    // The entity's change in this step is a jump: shown at once, not blended
    void snap(EntityId id) { snapped.insert(id); }
    void snapCamera() { cameraSnapped = true; }
    // After a frame's steps (stepsRun of them): the entities the last step moved, shown at alpha (0: before it, 1:
    // after it). A frame without a step blends the same ones again at its alpha.
    void blend(Scene& scene, int stepsRun, float alpha);

    [[nodiscard]] size_t blended() const { return moving.size(); }
    [[nodiscard]] bool cameraBlended() const { return cameraMoving; }

private:
    struct Saved {
        EntityId id;
        Transform local;
    };
    struct Moving {
        EntityId id;
        Transform previous;  // before the last step
        Transform current;   // after it: the game's
        Transform shown;     // what blend wrote
    };
    std::vector<Saved> before;  // every entity, in the scene's order, before the last step
    std::vector<Moving> moving;
    std::unordered_set<EntityId> snapped;
    Camera cameraBefore;  // before the last step
    bool cameraMoving = false;
    bool cameraSnapped = false;
    Camera cameraPrevious, cameraCurrent, cameraShown;
};

#endif //VULKANDEMO_STEPINTERPOLATION_H
