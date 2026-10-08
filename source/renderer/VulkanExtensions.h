//
// Function pointers for device extensions that vulkan-1.lib does not export.
// Call them as vkext::vkCreateAccelerationStructureKHR(...) after vkext::load(device).
//

#ifndef VULKANDEMO_VULKANEXTENSIONS_H
#define VULKANDEMO_VULKANEXTENSIONS_H

#include <vulkan/vulkan.h>

namespace vkext {
    // VK_KHR_acceleration_structure
    extern PFN_vkCreateAccelerationStructureKHR vkCreateAccelerationStructureKHR;
    extern PFN_vkDestroyAccelerationStructureKHR vkDestroyAccelerationStructureKHR;
    extern PFN_vkGetAccelerationStructureBuildSizesKHR vkGetAccelerationStructureBuildSizesKHR;
    extern PFN_vkCmdBuildAccelerationStructuresKHR vkCmdBuildAccelerationStructuresKHR;
    extern PFN_vkGetAccelerationStructureDeviceAddressKHR vkGetAccelerationStructureDeviceAddressKHR;
    extern PFN_vkCmdWriteAccelerationStructuresPropertiesKHR vkCmdWriteAccelerationStructuresPropertiesKHR;
    extern PFN_vkCmdCopyAccelerationStructureKHR vkCmdCopyAccelerationStructureKHR;

    // VK_KHR_pipeline_executable_properties (optional, debug info only: nullptr if the device doesn't have it)
    extern PFN_vkGetPipelineExecutablePropertiesKHR vkGetPipelineExecutablePropertiesKHR;
    extern PFN_vkGetPipelineExecutableStatisticsKHR vkGetPipelineExecutableStatisticsKHR;

    // Loads all of the above; throws if a required one is missing.
    void load(VkDevice device);
}

#endif //VULKANDEMO_VULKANEXTENSIONS_H
