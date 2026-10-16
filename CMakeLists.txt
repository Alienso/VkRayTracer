cmake_minimum_required(VERSION 3.16)
project(vulkanDemo)

set(CMAKE_CXX_STANDARD 17)

# Optimized even in Debug builds: model loading does per-texel work on the CPU (mip levels, Toksvig textures, light
# lists) that takes about 20x longer unoptimized
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -Wno-unused-parameter -O2")
# No debug info by default, in any configuration: with it the exe was 97 MB and every link took ~3 s instead of 0.6.
# For a debugger: cmake -DVULKANDEMO_DEBUG_INFO=ON
option(VULKANDEMO_DEBUG_INFO "Compile with debug info (-g)" OFF)
set(CMAKE_C_FLAGS_DEBUG "")
set(CMAKE_CXX_FLAGS_DEBUG "")
if (VULKANDEMO_DEBUG_INFO)
    add_compile_options(-g)
endif ()
# Checked STL containers (out-of-range indices and invalid iterators stop the program) in Debug builds only: they slow
# down the CPU side a lot (Jungle Ruins' millions of instances). Release builds also leave out the validation layer
# (Configuration::enableValidationLayers) and assert()s.
add_compile_definitions($<$<CONFIG:Debug>:_GLIBCXX_DEBUG> $<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>)

include_directories(${PROJECT_SOURCE_DIR}/include/)
include_directories(${PROJECT_SOURCE_DIR}/source/)  # project headers by folder: "renderer/Renderer.h"
include_directories(${PROJECT_SOURCE_DIR}/include/imgui/)

link_directories(${PROJECT_SOURCE_DIR}/lib)

if (WIN32)
    link_directories(${PROJECT_SOURCE_DIR}/lib/GLFW/lib-mingw-w64)
endif(WIN32)

# The Vulkan SDK, found through the VULKAN_SDK environment variable its installer sets
find_package(Vulkan REQUIRED)
include_directories(${Vulkan_INCLUDE_DIRS})
# The shader compiler from the same SDK (FindVulkan reports it since CMake 3.19; else look in the SDK's Bin folder)
if (Vulkan_GLSLC_EXECUTABLE)
    set(glslc_executable ${Vulkan_GLSLC_EXECUTABLE})
else()
    find_program(glslc_executable NAMES glslc HINTS "$ENV{VULKAN_SDK}/Bin")
endif()
if (NOT glslc_executable)
    message(FATAL_ERROR "glslc not found: install the Vulkan SDK (it sets VULKAN_SDK)")
endif()

# What doesn't need Vulkan or a window (the scene, the asset store and model loading, render settings as JSON): a
# library the program and the tests both link
add_library(vulkanDemoCore STATIC
        include/stb_image.cpp include/stb_image.h
        source/scene/Scene.cpp source/scene/Scene.h source/scene/SceneFile.cpp source/scene/SceneEnvironment.h source/scene/Camera.cpp
        source/scene/Camera.h source/scene/RenderList.h source/scene/Portal.h
        source/assets/AssetStore.cpp source/assets/AssetStore.h source/assets/EditableFields.h source/assets/ModelLoader.cpp
        source/assets/ModelLoader.h source/assets/BuiltInModels.cpp source/assets/BuiltInModels.h
        source/renderer/RenderSettingsFile.cpp source/renderer/RenderSettingsFile.h source/renderer/RenderSettings.h
        source/app/UserSettings.cpp source/app/UserSettings.h
        source/core/Configuration.cpp source/core/Configuration.h source/core/Handles.h
        source/core/RangeAllocator.h source/core/DeferredDeletion.h
        include/cgltf/cgltf.h)
target_compile_definitions(vulkanDemoCore PRIVATE VULKANDEMO_RESOURCE_DIR="${PROJECT_SOURCE_DIR}/resource")

add_executable(vulkanDemo main.cpp
        include/imgui/imgui.cpp include/imgui/imgui.h
        include/imgui/imgui_draw.cpp include/imgui/imgui_demo.cpp
        include/imgui/imgui_tables.cpp include/imgui/imgui_widgets.cpp
        include/imgui/backends/imgui_impl_glfw.h include/imgui/backends/imgui_impl_glfw.cpp
        include/imgui/backends/imgui_impl_vulkan.h include/imgui/backends/imgui_impl_vulkan.cpp

        include/stb_image_write.cpp include/stb_image_write.h
        include/vma/vk_mem_alloc.cpp include/vma/vk_mem_alloc.h
        source/app/Application.cpp source/app/Application.h source/app/Window.cpp source/app/Window.h
        source/app/CameraController.cpp source/app/CameraController.h source/app/Bench.cpp source/app/Bench.h source/app/CpuFrameTimes.h
        source/editor/Editor.cpp source/editor/Editor.h source/editor/EditorScene.cpp source/editor/EditorFile.cpp source/editor/EditorLayout.cpp source/editor/EditorRenderer.cpp
        source/renderer/Renderer.cpp source/renderer/Renderer.h source/renderer/GpuAssets.h
        source/renderer/RayTracer.cpp source/renderer/SceneGpu.cpp source/renderer/Materials.cpp
        source/renderer/Lights.cpp source/renderer/Portals.cpp source/renderer/Environment.cpp source/renderer/Display.cpp
        source/renderer/Dlss.cpp source/renderer/Exposure.cpp source/renderer/Capture.cpp source/renderer/Noise.cpp
        source/renderer/RadianceCache.cpp source/renderer/BlueNoise.cpp source/renderer/Volumes.cpp source/renderer/ImGuiInit.cpp
        source/renderer/VulkanExtensions.cpp source/renderer/VulkanExtensions.h source/renderer/VulkanInitHelper.cpp
        source/renderer/GpuContext.cpp source/renderer/GpuContext.h
        source/renderer/GpuScene.h source/renderer/AccelerationStructures.cpp source/renderer/RendererTypes.h
        source/renderer/Capture.h
        source/renderer/Exposure.h
        source/renderer/Noise.h
        source/renderer/Display.h source/renderer/RenderImages.cpp
        source/renderer/RadianceCache.h
        source/renderer/RayTracer.h source/renderer/Frame.cpp
        source/renderer/Dlss.h
        )

target_link_libraries(vulkanDemo vulkanDemoCore glfw3 gdi32 psapi Vulkan::Vulkan)  # psapi: the process's memory (benches)

# Models and HDR environments are read from the project's resource folder, shared by all builds (copied next to each
# executable they took 5.8 GB per build folder, and ~15 s of every build). --resources <dir> overrides it. (The
# default is Configuration.cpp's, in vulkanDemoCore above.)

# Unit tests (tests/, Catch2 from include/catch2, Boost license): the pure pieces, no GPU. Run with ctest, or the
# tests executable itself (--help for Catch2's options: a test by name, tags, ...). Linked with static libstdc++ so it
# runs whatever is first on PATH (Git's libstdc++ crashes the program).
add_executable(tests
        include/catch2/catch_amalgamated.cpp include/catch2/catch_amalgamated.hpp
        tests/TestHelpers.h tests/RangeAllocatorTests.cpp tests/DeferredDeletionTests.cpp tests/AssetStoreTests.cpp
        tests/SceneTreeTests.cpp tests/RenderListTests.cpp tests/SceneFileTests.cpp tests/RenderSettingsFileTests.cpp
        tests/TransformTests.cpp tests/ModelLoaderTests.cpp tests/UserSettingsTests.cpp)
target_include_directories(tests PRIVATE ${PROJECT_SOURCE_DIR}/include/catch2)
set_source_files_properties(include/catch2/catch_amalgamated.cpp PROPERTIES COMPILE_OPTIONS "-w")
target_link_libraries(tests vulkanDemoCore -static-libgcc -static-libstdc++)
enable_testing()
add_test(NAME unit COMMAND tests)

# Scene files (scenes/): the editor opens and saves them in the scenes folder next to the executable. Copied there
# when CMake runs, and only those that aren't there yet: a build never overwrites a scene saved there. To keep a
# saved scene in git, copy it back into scenes/.
file(GLOB sceneFiles ${PROJECT_SOURCE_DIR}/scenes/*.json)
foreach (sceneFile IN LISTS sceneFiles)
    get_filename_component(sceneName ${sceneFile} NAME)
    if (NOT EXISTS ${CMAKE_BINARY_DIR}/scenes/${sceneName})
        file(COPY ${sceneFile} DESTINATION ${CMAKE_BINARY_DIR}/scenes)
    endif ()
endforeach ()

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

# The files a shader #includes (raytrace.comp's parts in shaders/raytrace/) are found by glslc, which lists them in a
# depfile: a change in one rebuilds the shader. (CMP0116: the depfile's paths are taken as glslc writes them.) Makefile
# builds read depfiles one build late, so raytrace.comp also depends on its parts directly.
if (POLICY CMP0116)
    cmake_policy(SET CMP0116 NEW)
endif ()
file(GLOB RAYTRACE_PARTS ${SHADER_SOURCE_DIR}/raytrace/*.glsl)
foreach(source IN LISTS SHADERS)
    get_filename_component(FILENAME ${source} NAME)
    set(includedParts)
    if (FILENAME STREQUAL "raytrace.comp")
        set(includedParts ${RAYTRACE_PARTS})
    endif ()
    add_custom_command(
            COMMAND
            ${glslc_executable}
            --target-env=vulkan1.3  # needed for GL_EXT_ray_query
            -MD -MF ${SHADER_BINARY_DIR}/${FILENAME}.d
            -o ${SHADER_BINARY_DIR}/${FILENAME}.spv
            ${source}
            OUTPUT ${SHADER_BINARY_DIR}/${FILENAME}.spv
            DEPENDS ${source} ${includedParts}  # (not the folder: writing a .spv into it changes its time, and Ninja then rebuilt all)
            DEPFILE ${SHADER_BINARY_DIR}/${FILENAME}.d
            COMMENT "Compiling ${FILENAME}"
    )
    list(APPEND SPV_SHADERS ${SHADER_BINARY_DIR}/${FILENAME}.spv)
endforeach()

add_custom_target(compileShaders ALL DEPENDS ${SPV_SHADERS})
#==============================================================================

