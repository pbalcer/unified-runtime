# Copyright (C) 2022 Intel Corporation
# SPDX-License-Identifier: MIT

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/UrLoaderVersion.rc.in
    ${CMAKE_CURRENT_BINARY_DIR}/UrLoaderVersion.rc
    @ONLY)

add_library(ur_loader
    SHARED
    ""
    ${CMAKE_CURRENT_BINARY_DIR}/UrLoaderVersion.rc
)

add_library(${PROJECT_NAME}::loader ALIAS ur_loader)

target_include_directories(ur_loader PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/source/common
    ${CMAKE_SOURCE_DIR}/source/loader
)

set_target_properties(ur_loader PROPERTIES
    VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
)

target_link_libraries(ur_loader
    ${PROJECT_NAME}::headers
    ${CMAKE_DL_LIBS}
)

if (UNIX)
    set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
    set(THREADS_PREFER_PTHREAD_FLAG TRUE)
    find_package(Threads REQUIRED)
    target_link_libraries(ur_loader Threads::Threads)
endif()

if(WIN32)
    target_link_libraries(ur_loader cfgmgr32.lib)
endif()


if(UNIX)
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/libur_loader.pc.in
        ${CMAKE_CURRENT_BINARY_DIR}/libur_loader.pc
        @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/libur_loader.pc" DESTINATION "${CMAKE_INSTALL_FULL_LIBDIR}/pkgconfig" COMPONENT unified-runtime)
endif()

install(TARGETS ur_loader
    EXPORT ${PROJECT_NAME}-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

target_sources(ur_loader
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/ur_object.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ur_loader.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ur_loader.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ur_ldrddi.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ur_ldrddi.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ur_libapi.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ur_libddi.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ur_lib.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ur_lib.cpp
)

if(WIN32)
    target_sources(ur_loader
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/windows/lib_init.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/windows/platform_discovery_win.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/windows/loader_init.cpp
    )
else()
    target_sources(ur_loader
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/lib_init.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/platform_discovery_lin.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/loader_init.cpp
    )
endif()
