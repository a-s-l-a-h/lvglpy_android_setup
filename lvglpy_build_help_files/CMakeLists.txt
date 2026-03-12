cmake_minimum_required(VERSION 3.18)
project(lvglpy_android C CXX)

set(PYTHON_VERSION {{PY_VER}})

if(ANDROID_ABI STREQUAL "{{FIRST_ABI}}")
    set(DB_PYTHON_INCLUDE "{{FIRST_INC}}")
    set(DB_PYTHON_LIB_DIR "{{FIRST_LIB}}")
{{ELIFS}}else()
    message(FATAL_ERROR "ABI ${ANDROID_ABI} not in template.")
endif()

# LVGL Configuration
set(LV_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(LV_CONF_INCLUDE_SIMPLE 1 CACHE BOOL "" FORCE)

# 1. Add nanobind submodule (Required for Python bindings)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/lvglpy/nanobind nanobind EXCLUDE_FROM_ALL)

# 2. Collect LVGL sources manually
file(GLOB_RECURSE LVGL_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/lvglpy/lvgl/src/*.c")

# === INCLUDE DIRECTORIES ===
# PUT OUR CUSTOM DIRECTORY FIRST! This forces the compiler to use our lv_conf.h
include_directories(${CMAKE_CURRENT_SOURCE_DIR}) 

include_directories(${DB_PYTHON_INCLUDE})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/lvglpy)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/lvglpy/lvgl)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/lvglpy/src)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/lvglpy/backends)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/backends)

# 3. Compile everything together into the nanobind module
nanobind_add_module({{NAME}}
    lvglpy/generated/widgets.cpp
    lvglpy/src/lvglpy.cpp
    backends/android_input_sw.cpp
    ${LVGL_SOURCES}
)

target_compile_definitions({{NAME}} PRIVATE LV_CONF_INCLUDE_SIMPLE=1)
target_link_options({{NAME}} PRIVATE "-L${DB_PYTHON_LIB_DIR}" "-lpython${PYTHON_VERSION}")

# 4. Link Android system libraries
find_library(log-lib log)
find_library(gles2-lib GLESv2)
find_library(egl-lib EGL)
find_library(android-lib android)

target_link_libraries({{NAME}} PRIVATE ${gles2-lib} ${egl-lib} ${log-lib} ${android-lib})

set_target_properties({{NAME}} PROPERTIES
    PREFIX "" OUTPUT_NAME "{{NAME}}" SUFFIX ".so"
    BUILD_RPATH "" INSTALL_RPATH "" BUILD_WITH_INSTALL_RPATH TRUE
)