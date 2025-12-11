# Maya Engine Library
set(MAYA_SOURCES
    maya/core/engine.cpp
    maya/core/application.cpp
    maya/math/vector.cpp
    maya/math/matrix.cpp
    maya/math/quaternion.cpp
    maya/platform/window.cpp
    maya/platform/input.cpp
    maya/renderer/renderer.cpp
    maya/physics/physics.cpp
    maya/ai/ai.cpp
)

set(MAYA_HEADERS
    ${CMAKE_SOURCE_DIR}/include/maya/core/engine.hpp
    ${CMAKE_SOURCE_DIR}/include/maya/core/application.hpp
    ${CMAKE_SOURCE_DIR}/include/maya/math/vector.hpp
    ${CMAKE_SOURCE_DIR}/include/maya/math/matrix.hpp
    ${CMAKE_SOURCE_DIR}/include/maya/math/quaternion.hpp
    ${CMAKE_SOURCE_DIR}/include/maya/math/types.hpp
    ${CMAKE_SOURCE_DIR}/include/maya/platform/window.hpp
    ${CMAKE_SOURCE_DIR}/include/maya/platform/input.hpp
    ${CMAKE_SOURCE_DIR}/include/maya/renderer/renderer.hpp
    ${CMAKE_SOURCE_DIR}/include/maya/physics/physics.hpp
    ${CMAKE_SOURCE_DIR}/include/maya/ai/ai.hpp
)

# Create static library
add_library(MayaEngine STATIC ${MAYA_SOURCES} ${MAYA_HEADERS})

target_include_directories(MayaEngine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# Link dependencies
target_link_libraries(MayaEngine PUBLIC glfw)
# target_link_libraries(MayaEngine PUBLIC Vulkan::Vulkan)  # Phase 3

# Main executable
add_executable(maya main.cpp)
target_link_libraries(maya PRIVATE MayaEngine)
