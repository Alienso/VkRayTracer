//
// Noise measurement: a converged reference of the current view (accumulated), then the error of the displayed image
// against it, frame by frame, in a render mode. From the Debug panel, or run from code (benches). See Noise.cpp.
//

#ifndef VULKANDEMO_NOISE_H
#define VULKANDEMO_NOISE_H

#include <vulkan/vulkan.h>
#include <functional>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "renderer/GpuContext.h"
#include "renderer/RendererTypes.h"

// It steers the renderer while it runs: switches the render mode (settings), restarts the accumulation, keeps the
// radiance cache out of the reference (cacheQueriesSuspended) and DLSS's history out of the measurement (dlssReset).
// Resources are created on first use.
class NoiseTool {
public:
    // The renderer's state it reads and sets, its frame time per pass (lastPassTimes, ms), the HDR image shown, whether
    // a DLSS mode is active, and its wait-idle (through Streamline with DLSS)
    NoiseTool(GpuContext& gpu, RenderSettings& settings, const uint32_t& currentFrame, const VkExtent2D& outputExtent,
              const uint32_t& accumulatedFrames, const uint32_t& accumulationRestarts, const int& activeRenderMode,
              bool& resetAccumulation, bool& cacheQueriesSuspended, bool& dlssReset, const float (&lastPassTimes)[6],
              std::function<VkImageView()> displayedHdrView, std::function<bool()> dlssActive,
              std::function<void()> waitIdle)
        : gpu(gpu), settings(settings), currentFrame(currentFrame), outputExtent(outputExtent),
          accumulatedFrames(accumulatedFrames), accumulationRestarts(accumulationRestarts),
          activeRenderMode(activeRenderMode), resetAccumulation(resetAccumulation),
          cacheQueriesSuspended(cacheQueriesSuspended), dlssReset(dlssReset), lastPassTimes(lastPassTimes),
          displayedHdrView(std::move(displayedHdrView)), dlssActive(std::move(dlssActive)), waitIdle(std::move(waitIdle)) {}

    // Run from code: accumulates a reference of the current view for referenceFrames, then measures measureFrames in
    // renderMode. Busy until the results are in noiseResults.
    void startNoiseRun(int renderMode, int referenceFrames, int measureFrames, glm::ivec4 region);
    [[nodiscard]] bool noiseRunBusy() const {
        return noiseAutoRun || noiseAutoContinue || noiseState != NoiseState::Idle;
    }
    void ensureNoiseResources();
    void updateNoiseDescriptorSets();
    void startNoiseReference();
    void startNoiseMeasurement(int renderMode);
    void readNoiseResults();
    void finishNoiseMeasurement();
    void recordNoise(VkCommandBuffer commandBuffer);
    void destroyNoiseResources();
    void buildNoiseUi();

    bool noiseResourcesCreated = false;
    RenderImage noiseReferenceImage;          // R32F (luminance) at output size
    VkExtent2D noiseReferenceExtent{};
    bool noiseReferenceValid = false;
    float noiseReferenceLuminance = 0.0f;     // average over the reference image
    uint32_t noiseGroupCapacity = 0;          // workgroup sums the buffers below have room for
    std::vector<VkBuffer> noiseSumBuffers;    // one per frame in flight, host visible: the GPU writes, the CPU reads
    std::vector<VmaAllocation> noiseSumMemories;
    std::vector<void*> noiseSumMapped;
    struct NoisePending { uint32_t pass = 0; uint32_t frame = 0; uint32_t groups = 0; bool active = false; };
    std::vector<NoisePending> noisePending;   // what each frame slot's buffer holds once its fence has passed
    VkDescriptorSetLayout noiseDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool noiseDescriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> noiseDescriptorSets;  // one per frame in flight (each with its own sum buffer)
    VkPipelineLayout noisePipelineLayout = VK_NULL_HANDLE;
    VkPipeline noisePipeline = VK_NULL_HANDLE;
    NoiseState noiseState = NoiseState::Idle;
    int noiseReferenceFrames = 4096;          // UI: frames accumulated for the reference
    int noiseMeasureFrames = 256;             // UI: frames measured
    int noiseMeasureMode = 0;                 // RenderMode the measurement runs in
    glm::ivec4 noiseRegion = glm::ivec4(0);   // x0, y0, x1, y1 in output pixels; empty = the whole image
    uint32_t noiseFrame = 0;                  // frames measured so far
    uint32_t noiseRestartsAtStart = 0;
    std::vector<NoiseResult> noiseResults;
    std::string noiseStatus;
    // Run from code (startNoiseRun): reference, then measure in noiseAutoMode
    bool noiseAutoRun = false;
    int noiseAutoMode = 0;
    bool noiseAutoContinue = false;  // the run's reference is being built

private:
    GpuContext& gpu;
    RenderSettings& settings;
    const uint32_t& currentFrame;
    const VkExtent2D& outputExtent;
    const uint32_t& accumulatedFrames;
    const uint32_t& accumulationRestarts;
    const int& activeRenderMode;
    bool& resetAccumulation;
    bool& cacheQueriesSuspended;
    bool& dlssReset;
    const float (&lastPassTimes)[6];
    std::function<VkImageView()> displayedHdrView;
    std::function<bool()> dlssActive;
    std::function<void()> waitIdle;
};

#endif //VULKANDEMO_NOISE_H
