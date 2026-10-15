//
// DLSS Ray Reconstruction through NVIDIA Streamline: loading it, its part in creating the Vulkan instance and device,
// the render size it wants, and evaluating it in the frame's command buffer (the noisy frame and the guide images in,
// a denoised, upscaled HDR image out). See Dlss.cpp.
//

#ifndef VULKANDEMO_DLSS_H
#define VULKANDEMO_DLSS_H

#include <vulkan/vulkan.h>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "renderer/GpuContext.h"
#include "renderer/RayTracer.h"
#include "renderer/RendererTypes.h"
#include "scene/RenderList.h"

class Dlss {
public:
    // The renderer's: its presentation functions (Streamline's proxies go there), settings, the trace pass (its images
    // are DLSS's input), output and render size, the render list (camera), this frame's jitter and matrices, the
    // output image, its reset flag (cleared once DLSS has started over) and render mode
    Dlss(GpuContext& gpu, PresentationFunctions& vkp, const RenderSettings& settings, const RayTracer& trace,
         const VkExtent2D& outputExtent, const VkExtent2D& renderExtent, const RenderList* const& renderList,
         const glm::vec2& frameJitter, const glm::mat4& frameView, const glm::mat4& frameProjection,
         const glm::mat4& frameViewProjection, const glm::mat4& framePreviousViewProjection,
         const RenderImage& dlssOutputImage, bool& dlssReset, const int& activeRenderMode)
        : gpu(gpu), vkp(vkp), settings(settings), trace(trace), outputExtent(outputExtent), renderExtent(renderExtent),
          renderList(renderList), frameJitter(frameJitter), frameView(frameView), frameProjection(frameProjection),
          frameViewProjection(frameViewProjection), framePreviousViewProjection(framePreviousViewProjection),
          dlssOutputImage(dlssOutputImage), dlssReset(dlssReset), activeRenderMode(activeRenderMode) {}

    void initStreamline();
    void addStreamlineDeviceRequirements(std::vector<const char*>& extensions, VkPhysicalDeviceVulkan12Features& features12,
                                         VkPhysicalDeviceVulkan13Features& features13);
    void connectStreamline();
    void shutdownStreamline();
    bool dlssModelAvailable(int model) const;
    VkExtent2D dlssRenderExtent(int renderMode, VkExtent2D output);
    void recordDlss(VkCommandBuffer commandBuffer);
    void freeDlssResources();
    double memoryMegabytes() const;

    bool dlssDisabledByFlag = false;   // --no-dlss: Streamline is never loaded
    bool dlssCnnVersion = false;       // --dlss-cnn: Streamline's plugins from dlss/cnn instead of dlss/transformer
    std::string dlssVersion;           // of the loaded DLSS RR (NGX), for the UI
    void* streamlineModule = nullptr;  // HMODULE of sl.interposer.dll
    bool streamlineInitialized = false;
    bool dlssSupported = false;
    std::string dlssStatus = "not checked";
    int activeDlssModel = static_cast<int>(DlssModel::TransformerF);    // what DLSS's resources were made for
    bool dlssResourcesInUse = false;   // DLSS created its own resources (on the first evaluate); free them on changes
    uint32_t dlssFrameIndex = 0;
    std::vector<std::string> streamlineInstanceExtensions;  // what Streamline asks for
    std::vector<std::string> streamlineDeviceExtensions;
    std::vector<std::string> streamlineFeatures12;  // Vulkan 1.2 / 1.3 feature names it needs enabled
    std::vector<std::string> streamlineFeatures13;

private:
    GpuContext& gpu;
    PresentationFunctions& vkp;
    const RenderSettings& settings;
    const RayTracer& trace;
    const VkExtent2D& outputExtent;
    const VkExtent2D& renderExtent;
    const RenderList* const& renderList;
    const glm::vec2& frameJitter;
    const glm::mat4& frameView;
    const glm::mat4& frameProjection;
    const glm::mat4& frameViewProjection;
    const glm::mat4& framePreviousViewProjection;
    const RenderImage& dlssOutputImage;
    bool& dlssReset;
    const int& activeRenderMode;
    bool dlssActive() const { return activeRenderMode != static_cast<int>(RenderMode::Accumulation); }
};

#endif //VULKANDEMO_DLSS_H
