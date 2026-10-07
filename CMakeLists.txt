cmake_minimum_required(VERSION 3.16)
project(vulkanDemo)

set(CMAKE_CXX_STANDARD 17)

# Optimized even in Debug builds: model loading does per-texel work on the CPU (mip levels, Toksvig textures, light
# lists) that takes about 20x longer unoptimized
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2")

include_directories(${PROJECT_SOURCE_DIR}/include/)
include_directories(${PROJECT_SOURCE_DIR}/include/imgui/)
include_directories(C:/VulkanSDK/1.4.363.0/Include/)

link_directories(${PROJECT_SOURCE_DIR}/lib)
link_directories(C:/VulkanSDK/1.4.363.0/Lib/)
link_directories(C:/VulkanSDK/1.4.363.0/Lib32/)

if (WIN32)
    link_directories(${PROJECT_SOURCE_DIR}/lib/GLFW/lib-mingw-w64)
endif(WIN32)


find_package(Vulkan REQUIRED)
find_program(glslc_executable NAMES glslc HINTS Vulkan::glslc)

add_executable(vulkanDemo main.cpp
        include/imgui/imgui.cpp include/imgui/imgui.h
        include/imgui/imgui_draw.cpp include/imgui/imgui_demo.cpp
        include/imgui/imgui_tables.cpp include/imgui/imgui_widgets.cpp
        include/imgui/backends/imgui_impl_glfw.h include/imgui/backends/imgui_impl_glfw.cpp
        include/imgui/backends/imgui_impl_vulkan.h include/imgui/backends/imgui_impl_vulkan.cpp

        include/stb_image.cpp include/stb_image.h
        include/stb_image_write.cpp include/stb_image_write.h
        source/Application.cpp source/Application.h
        source/Configuration.cpp source/Configuration.h
        source/VulkanInitHelper.cpp source/ImGuiInit.cpp
        source/VulkanExtensions.cpp source/VulkanExtensions.h
        source/RayTracer.cpp
        source/Camera.cpp source/Camera.h
        source/ModelLoader.cpp source/ModelLoader.h
        source/Scene.cpp source/Materials.cpp
        source/BuiltInModels.cpp source/BuiltInModels.h
        source/Lights.cpp
        source/Environment.cpp
        source/Exposure.cpp
        source/Noise.cpp
        source/RadianceCache.cpp
        source/Display.cpp
        source/Capture.cpp
        source/BlueNoise.cpp
        source/Dlss.cpp
        include/cgltf/cgltf.h)

target_link_libraries(vulkanDemo glfw3 gdi32 vulkan-1)

#add_custom_command(TARGET vulkanDemo PRE_BUILD
#        COMMAND ${CMAKE_COMMAND} -E copy_directory
#        ${PROJECT_SOURCE_DIR}/shaders $<TARGET_FILE_DIR:vulkanDemo>)

add_custom_command(TARGET vulkanDemo PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${PROJECT_SOURCE_DIR}/resource $<TARGET_FILE_DIR:vulkanDemo>)

# NVIDIA Streamline (DLSS Ray Reconstruction): loaded at runtime. sl.interposer.dll goes next to the executable, the
# plugins into dlss/transformer (current DLSS RR, transformer models) and dlss/cnn (older DLSS RR that still has the
# CNN models, picked with --dlss-cnn); NGX loads nvngx_dlssd.dll from next to the plugins.
# lib/streamline is not in git (download_streamline.ps1 fetches it); without it the app runs without DLSS.
set(STREAMLINE_DIR ${PROJECT_SOURCE_DIR}/lib/streamline)
set(STREAMLINE_PLUGIN_FILES sl.common.dll sl.dlss_d.dll NvLowLatencyVk.dll nvngx_dlssd.dll)
set(STREAMLINE_FOUND TRUE)
foreach (file sl.interposer.dll transformer/nvngx_dlssd.dll cnn/nvngx_dlssd.dll)
    if (NOT EXISTS ${STREAMLINE_DIR}/${file})
        set(STREAMLINE_FOUND FALSE)
    endif ()
endforeach ()
if (STREAMLINE_FOUND)
    add_custom_command(TARGET vulkanDemo PRE_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${STREAMLINE_DIR}/sl.interposer.dll $<TARGET_FILE_DIR:vulkanDemo>)
    foreach (folder transformer cnn)
        set(files)
        foreach (file ${STREAMLINE_PLUGIN_FILES})
            list(APPEND files ${STREAMLINE_DIR}/${folder}/${file})
        endforeach ()
        add_custom_command(TARGET vulkanDemo PRE_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:vulkanDemo>/dlss/${folder}
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${files} $<TARGET_FILE_DIR:vulkanDemo>/dlss/${folder})
    endforeach ()
else ()
    message(WARNING "Streamline DLLs missing in lib/streamline: DLSS will be unavailable. "
            "Run: powershell -ExecutionPolicy Bypass -File download_streamline.ps1")
endif ()


#==============================================================================
# COMPILE SHADERS
#
set(SHADER_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR})

file(GLOB SHADERS
        ${SHADER_SOURCE_DIR}/*.vert
        ${SHADER_SOURCE_DIR}/*.frag
        ${SHADER_SOURCE_DIR}/*.comp
        ${SHADER_SOURCE_DIR}/*.geom
        ${SHADER_SOURCE_DIR}/*.tesc
        ${SHADER_SOURCE_DIR}/*.tese
        ${SHADER_SOURCE_DIR}/*.mesh
        ${SHADER_SOURCE_DIR}/*.task
        ${SHADER_SOURCE_DIR}/*.rgen
        ${SHADER_SOURCE_DIR}/*.rchit
        ${SHADER_SOURCE_DIR}/*.rmiss)

add_custom_command(
        COMMAND
        ${CMAKE_COMMAND} -E make_directory ${SHADER_BINARY_DIR}
        OUTPUT ${SHADER_BINARY_DIR}
        COMMENT "Creating ${SHADER_BINARY_DIR}"
)

foreach(source IN LISTS SHADERS)
    get_filename_component(FILENAME ${source} NAME)
    add_custom_command(
            COMMAND
            ${glslc_executable}
            --target-env=vulkan1.3  # needed for GL_EXT_ray_query
            #      -MD -MF ${SHADER_BINARY_DIR}/${FILENAME}.d
            -o ${SHADER_BINARY_DIR}/${FILENAME}.spv
            ${source}
            OUTPUT ${SHADER_BINARY_DIR}/${FILENAME}.spv
            DEPENDS ${source} ${SHADER_BINARY_DIR}
            COMMENT "Compiling ${FILENAME}"
    )
    list(APPEND SPV_SHADERS ${SHADER_BINARY_DIR}/${FILENAME}.spv)
endforeach()

add_custom_target(compileShaders ALL DEPENDS ${SPV_SHADERS})
#==============================================================================

