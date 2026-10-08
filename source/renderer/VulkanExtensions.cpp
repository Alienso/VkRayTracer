//
// Function pointers for device extensions that vulkan-1.lib does not export.
//

#include <stdexcept>
#include <string>
#include "renderer/VulkanExtensions.h"

namespace vkext {
    PFN_vkCreateAccelerationStructureKHR vkCreateAccelerationStructureKHR = nullptr;
    PFN_vkDestroyAccelerationStructureKHR vkDestroyAccelerationStructureKHR = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR vkGetAccelerationStructureBuildSizesKHR = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR vkCmdBuildAccelerationStructuresKHR = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR vkGetAccelerationStructureDeviceAddressKHR = nullptr;
    PFN_vkCmdWriteAccelerationStructuresPropertiesKHR vkCmdWriteAccelerationStructuresPropertiesKHR = nullptr;
    PFN_vkCmdCopyAccelerationStructureKHR vkCmdCopyAccelerationStructureKHR = nullptr;
    PFN_vkGetPipelineExecutablePropertiesKHR vkGetPipelineExecutablePropertiesKHR = nullptr;
    PFN_vkGetPipelineExecutableStatisticsKHR vkGetPipelineExecutableStatisticsKHR = nullptr;

    template<typename T>
    static void loadFunction(VkDevice device, T& function, const char* name) {
        function = reinterpret_cast<T>(vkGetDeviceProcAddr(device, name));
        if (function == nullptr) {
            throw std::runtime_error(std::string("failed to load device function ") + name);
        }
    }

    void load(VkDevice device) {
        loadFunction(device, vkCreateAccelerationStructureKHR, "vkCreateAccelerationStructureKHR");
        loadFunction(device, vkDestroyAccelerationStructureKHR, "vkDestroyAccelerationStructureKHR");
        loadFunction(device, vkGetAccelerationStructureBuildSizesKHR, "vkGetAccelerationStructureBuildSizesKHR");
        loadFunction(device, vkCmdBuildAccelerationStructuresKHR, "vkCmdBuildAccelerationStructuresKHR");
        loadFunction(device, vkGetAccelerationStructureDeviceAddressKHR, "vkGetAccelerationStructureDeviceAddressKHR");
        loadFunction(device, vkCmdWriteAccelerationStructuresPropertiesKHR, "vkCmdWriteAccelerationStructuresPropertiesKHR");
        loadFunction(device, vkCmdCopyAccelerationStructureKHR, "vkCmdCopyAccelerationStructureKHR");
        // Optional: null when the extension wasn't enabled
        vkGetPipelineExecutablePropertiesKHR = reinterpret_cast<PFN_vkGetPipelineExecutablePropertiesKHR>(
                vkGetDeviceProcAddr(device, "vkGetPipelineExecutablePropertiesKHR"));
        vkGetPipelineExecutableStatisticsKHR = reinterpret_cast<PFN_vkGetPipelineExecutableStatisticsKHR>(
                vkGetDeviceProcAddr(device, "vkGetPipelineExecutableStatisticsKHR"));
    }
}
