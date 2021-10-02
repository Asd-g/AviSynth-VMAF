cmake_minimum_required(VERSION 3.16)

project(libvmaf LANGUAGES CXX)

find_package (Git)

if (GIT_FOUND)
    execute_process (COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0
        OUTPUT_VARIABLE ver
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
else ()
    message (STATUS "GIT not found")
endif ()

if (MINGW)
    configure_file (
    "${PROJECT_SOURCE_DIR}/vmaf.rc.in"
    "${PROJECT_SOURCE_DIR}/vmaf.rc"
    )
endif ()

set (sources src/VMAF.cpp)

if (MINGW)
    set (sources ${sources} vmaf.rc)
endif ()

add_library(vmaf SHARED ${sources})

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "" FORCE)
endif()

message(STATUS "Build type - ${CMAKE_BUILD_TYPE}")

if (CMAKE_BUILD_TYPE MATCHES Release)
    string(APPEND CMAKE_CXX_FLAGS "-ffunction-sections -s")
endif ()

target_include_directories(vmaf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/vmaf/vmaf_install/include
)

target_link_libraries(vmaf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/AviSynth.lib
    ${CMAKE_CURRENT_SOURCE_DIR}/vmaf/vmaf_install/lib/libvmaf.a
)

if (MINGW)
    target_link_libraries(vmaf PRIVATE
        -static-libstdc++
        -static-libgcc
        -static -lwinpthread
    )
endif ()

if (MINGW)
    set_target_properties(vmaf PROPERTIES PREFIX "")
    set_target_properties(vmaf PROPERTIES OUTPUT_NAME "VMAF")
else ()
    set_target_properties(vmaf PROPERTIES OUTPUT_NAME "vmaf.${ver}")
endif ()

target_compile_features(vmaf PRIVATE cxx_std_17)

if (UNIX)
    include(GNUInstallDirs)
    
    INSTALL(TARGETS vmaf LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/avisynth")
            
    # uninstall target
    if(NOT TARGET uninstall)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake_uninstall.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
        IMMEDIATE @ONLY)
    
    add_custom_target(uninstall
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)
    endif()
endif ()
