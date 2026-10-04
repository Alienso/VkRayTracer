cmake_minimum_required(VERSION 3.16)
project(vulkanDemo)

set(CMAKE_CXX_STANDARD 17)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0")

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
        source/Application.cpp source/Application.h
        source/Configuration.cpp source/Configuration.h
        source/VulkanInitHelper.cpp source/ImGuiInit.cpp source/Vertex.cpp source/Vertex.h
        source/VulkanExtensions.cpp source/VulkanExtensions.h
        source/RayTracer.cpp)

target_link_libraries(vulkanDemo glfw3 gdi32 vulkan-1)

#add_custom_command(TARGET vulkanDemo PRE_BUILD
#        COMMAND ${CMAKE_COMMAND} -E copy_directory
#        ${PROJECT_SOURCE_DIR}/shaders $<TARGET_FILE_DIR:vulkanDemo>)

add_custom_command(TARGET vulkanDemo PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${PROJECT_SOURCE_DIR}/resource $<TARGET_FILE_DIR:vulkanDemo>)


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

