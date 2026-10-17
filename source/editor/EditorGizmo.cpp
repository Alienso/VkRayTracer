//
// The editor's handles on the selected entity in the image (ImGuizmo, include/imguizmo, MIT license): move, rotate
// and scale by dragging, and the keys that go with them (the same as Unity's and Unreal's):
//   Q no handles, W move, E rotate, R scale, X world / local axes (scale is always along its own), hold Ctrl while
//   dragging to snap (0.25 units, 15 degrees, 0.1 of the scale), F frame the selection, Ctrl+D duplicate, Esc select
//   nothing, Delete delete (over the image or in the Outliner), Ctrl+Z / Ctrl+Y undo / redo (any change, EditHistory).
// The keys are off while typing, while flying (right mouse button: W/A/S/D move the camera then) and while playing
// (the game's).
//

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui/imgui.h>
#include <imguizmo/ImGuizmo.h>
#include "editor/Editor.h"

void Editor::buildGizmo() {
    Entity* entity = scene.find(selected);
    ImGuiIO& io = ImGui::GetIO();
    if (gizmoTool == GizmoTool::None || entity == nullptr || io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f) {
        finishGizmoDrag();
        return;
    }
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());  // over the image, under the panels
    ImGuizmo::SetRect(0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y);  // the image fills the window

    const Camera& camera = scene.camera();
    glm::mat4 view = camera.viewMatrix();
    // (OpenGL's clip space, as ImGuizmo expects; the same view as the image's)
    glm::mat4 projection = glm::perspective(glm::radians(camera.fov), io.DisplaySize.x / io.DisplaySize.y, 0.05f, 10000.0f);
    glm::mat4 parentWorld = entity->parent != 0 ? scene.worldTransform(entity->parent) : glm::mat4(1.0f);
    glm::mat4 world = parentWorld * entity->local.matrix();

    ImGuizmo::OPERATION operation = gizmoTool == GizmoTool::Move     ? ImGuizmo::TRANSLATE
                                    : gizmoTool == GizmoTool::Rotate ? ImGuizmo::ROTATE
                                                                     : ImGuizmo::SCALE;
    ImGuizmo::MODE mode = gizmoLocal || gizmoTool == GizmoTool::Scale ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    float snapValue = gizmoTool == GizmoTool::Move ? 0.25f : gizmoTool == GizmoTool::Rotate ? 15.0f : 0.1f;
    float snap[3] = {snapValue, snapValue, snapValue};

    Transform before = entity->local;
    if (ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(projection), operation, mode, glm::value_ptr(world),
                             nullptr, io.KeyCtrl ? snap : nullptr)) {
        // Back into the parent's space; only what the handle changes is taken (the rest stays exactly as it was)
        glm::mat4 local = glm::inverse(parentWorld) * world;
        Transform& t = entity->local;
        if (gizmoTool == GizmoTool::Move) {
            t.position = glm::vec3(local[3]);
        } else if (gizmoTool == GizmoTool::Rotate) {
            glm::mat3 axes(local);
            for (int i = 0; i < 3; i++) {
                float length = glm::length(axes[i]);
                axes[i] = length > 1e-12f ? axes[i] / length : glm::vec3(0.0f);
            }
            t.rotation = glm::normalize(glm::quat_cast(axes));
        } else {
            glm::vec3 scale(glm::length(glm::vec3(local[0])), glm::length(glm::vec3(local[1])),
                            glm::length(glm::vec3(local[2])));
            if (entity->uniformScale) {
                // The axis dragged sets all three (kept in proportion)
                int axis = 0;
                float most = -1.0f;
                for (int i = 0; i < 3; i++) {
                    float change = before.scale[i] != 0.0f ? std::abs(scale[i] / before.scale[i] - 1.0f) : 0.0f;
                    if (change > most) {
                        most = change;
                        axis = i;
                    }
                }
                float ratio = before.scale[axis] != 0.0f ? scale[axis] / before.scale[axis] : 1.0f;
                scale = before.scale * ratio;
            }
            t.scale = glm::max(scale, glm::vec3(1e-4f));  // (not to 0: the entity couldn't be scaled back by hand)
        }
    }
    // A drag is one change for undo: committed when it ends
    if (ImGuizmo::IsUsing()) {
        gizmoDragging = true;
    } else {
        finishGizmoDrag();
    }
}

void Editor::finishGizmoDrag() {
    if (!gizmoDragging) {
        return;
    }
    gizmoDragging = false;
    commitEdit();
}

// The camera moves back along its view until the selection fills about the middle of the image
void Editor::frameSelection() {
    const Entity* entity = scene.find(selected);
    if (entity == nullptr) {
        return;
    }
    glm::vec3 low(1e30f), high(-1e30f);
    for (const WorldBox& box : scene.selectionBoxes(selected)) {
        for (int corner = 0; corner < 8; corner++) {
            glm::vec3 p((corner & 1) ? box.max.x : box.min.x, (corner & 2) ? box.max.y : box.min.y,
                        (corner & 4) ? box.max.z : box.min.z);
            glm::vec3 w = glm::vec3(box.transform * glm::vec4(p, 1.0f));
            low = glm::min(low, w);
            high = glm::max(high, w);
        }
    }
    glm::vec3 center;
    float radius;
    if (low.x > high.x) {  // nothing shown (a group of groups): its position
        center = glm::vec3(scene.worldTransform(selected)[3]);
        radius = 1.0f;
    } else {
        center = (low + high) * 0.5f;
        radius = std::max(glm::length(high - low) * 0.5f, 0.05f);
    }
    Camera& camera = scene.camera();
    float halfFov = glm::radians(camera.fov) * 0.5f;
    float distance = radius / std::sin(std::max(halfFov, 0.05f)) * 1.1f;
    camera.position = center - camera.forward() * distance;
}

// The boxes' edges as lines over the image (under the panels and the handles), cut where they pass behind the camera
void Editor::drawColliderBoxes() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f) {
        return;
    }
    const Camera& camera = scene.camera();
    glm::mat4 viewProjection = glm::perspective(glm::radians(camera.fov), io.DisplaySize.x / io.DisplaySize.y, 0.05f,
                                                10000.0f) * camera.viewMatrix();  // (as the handles': OpenGL's clip space)
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    ImU32 color = IM_COL32(80, 255, 120, 220);
    const float nearW = 0.05f;
    auto line = [&](glm::vec4 a, glm::vec4 b) {
        if (a.w < nearW && b.w < nearW) {
            return;
        }
        if (a.w < nearW) {
            a = glm::mix(a, b, (nearW - a.w) / (b.w - a.w));
        } else if (b.w < nearW) {
            b = glm::mix(b, a, (nearW - b.w) / (a.w - b.w));
        }
        auto screen = [&](const glm::vec4& c) {
            return ImVec2((c.x / c.w * 0.5f + 0.5f) * io.DisplaySize.x, (0.5f - c.y / c.w * 0.5f) * io.DisplaySize.y);
        };
        drawList->AddLine(screen(a), screen(b), color, 1.5f);
    };
    for (const WorldBox& box : colliderBoxes) {
        glm::vec4 corners[8];
        for (int corner = 0; corner < 8; corner++) {
            glm::vec3 p((corner & 1) ? box.max.x : box.min.x, (corner & 2) ? box.max.y : box.min.y,
                        (corner & 4) ? box.max.z : box.min.z);
            corners[corner] = viewProjection * box.transform * glm::vec4(p, 1.0f);
        }
        for (int corner = 0; corner < 8; corner++) {
            for (int axis = 1; axis < 8; axis <<= 1) {
                if ((corner & axis) == 0) {
                    line(corners[corner], corners[corner | axis]);  // the edge along that axis
                }
            }
        }
    }
}

void Editor::handleEditorKeys() {
    ImGuiIO& io = ImGui::GetIO();
    deleteKeyUsed = false;
    if (io.WantTextInput || ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        return;
    }
    if (gameRunning()) {
        return;  // the letters are the game's (and nothing is undone while it runs: Stop puts the scene back)
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Z, false) && io.KeyCtrl && !io.KeyShift) {
        undoEdit();
    } else if ((ImGui::IsKeyPressed(ImGuiKey_Y, false) && io.KeyCtrl) ||
               (ImGui::IsKeyPressed(ImGuiKey_Z, false) && io.KeyCtrl && io.KeyShift)) {
        redoEdit();
    }
    if (!io.KeyCtrl && !io.KeyAlt) {
        if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) {
            gizmoTool = GizmoTool::None;
        } else if (ImGui::IsKeyPressed(ImGuiKey_W, false)) {
            gizmoTool = GizmoTool::Move;
        } else if (ImGui::IsKeyPressed(ImGuiKey_E, false)) {
            gizmoTool = GizmoTool::Rotate;
        } else if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
            gizmoTool = GizmoTool::Scale;
        } else if (ImGui::IsKeyPressed(ImGuiKey_X, false)) {
            gizmoLocal = !gizmoLocal;
        } else if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
            frameSelection();
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            selected = 0;
        }
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false) && scene.find(selected) != nullptr) {
        selected = scene.duplicate(selected);
        commitEdit();
    }
    // Delete with the mouse over the image (not over a panel); in the Outliner it is the tree's (buildEntityTree)
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !io.WantCaptureMouse && scene.find(selected) != nullptr) {
        deleteKeyUsed = true;  // (not the Outliner's too, this frame)
        EntityId parent = scene.find(selected)->parent;
        scene.remove(selected);
        selected = parent;
        commitEdit();
    }
}

void Editor::commitEdit() {
    if (!gameRunning()) {
        activeHistory().commit(scene);
    }
}

// (The selection stays when its entity is still there: ids are kept by undo; one an undo removed is deselected)
void Editor::undoEdit() {
    finishGizmoDrag();
    if (!gameRunning() && activeHistory().undo(scene) && scene.find(selected) == nullptr) {
        selected = 0;
    }
}

void Editor::redoEdit() {
    finishGizmoDrag();
    if (!gameRunning() && activeHistory().redo(scene) && scene.find(selected) == nullptr) {
        selected = 0;
    }
}
