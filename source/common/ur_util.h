/*
 *
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#ifndef UR_UTIL_H
#define UR_UTIL_H 1

#include <stdlib.h>
#include <string.h>
#include <string>

///////////////////////////////////////////////////////////////////////////////
#if defined(_WIN32)
#  include <Windows.h>
#  define MAKE_LIBRARY_NAME(NAME, VERSION)    NAME".dll"
#  define MAKE_LAYER_NAME(NAME)    NAME".dll"
#  define LOAD_DRIVER_LIBRARY(NAME) LoadLibraryExA(NAME, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)
#  define FREE_DRIVER_LIBRARY(LIB)  if(LIB) FreeLibrary(LIB)
#  define GET_FUNCTION_PTR(LIB, FUNC_NAME) GetProcAddress(LIB, FUNC_NAME)
#  define string_copy_s strncpy_s
#else
#  include <dlfcn.h>
#  define HMODULE void*
#  define MAKE_LIBRARY_NAME(NAME, VERSION)    "lib" NAME ".so." VERSION
#  define MAKE_LAYER_NAME(NAME)    "lib" NAME ".so." L0_VALIDATION_LAYER_SUPPORTED_VERSION
#  define LOAD_DRIVER_LIBRARY(NAME) dlopen(NAME, RTLD_LAZY|RTLD_LOCAL|RTLD_DEEPBIND)
#  define FREE_DRIVER_LIBRARY(LIB)  if(LIB) dlclose(LIB)
#  define GET_FUNCTION_PTR(LIB, FUNC_NAME) dlsym(LIB, FUNC_NAME)
#  define string_copy_s strncpy
#endif

inline std::string create_library_path(const char *name, const char *path){
    std::string library_path;
    if (path && (strcmp("", path) != 0)) {
        library_path.assign(path);
#ifdef _WIN32
        library_path.append("\\");
#else
        library_path.append("/");
#endif
        library_path.append(name);
    } else {
        library_path.assign(name);
    }
    return library_path;
}

#ifdef _WIN32
inline std::string readUnifiedRuntimeLoaderLibraryPath() {
    std::string LoaderRegKeyPath = "";
    HKEY regKey = {};
    DWORD regValueType = {};
    DWORD pathSize = {};
    std::string loaderMajorVersionString = std::to_string(LOADER_VERSION_MAJOR);
    std::string loaderRegistryKeyPath = "Software\\Intel\\oneAPI\\UnifiedRuntime\\";
    loaderRegistryKeyPath.append(loaderMajorVersionString);
    static constexpr char unifiedRuntimeLoaderPathKey[] = "UnifiedRuntimeLoaderPath";

    LSTATUS regOpenStatus = RegOpenKeyA(HKEY_LOCAL_MACHINE, loaderRegistryKeyPath.c_str(), &regKey);

    if (ERROR_SUCCESS != regOpenStatus) {
        return LoaderRegKeyPath;
    }

    LSTATUS regOpStatus = RegQueryValueExA(regKey, unifiedRuntimeLoaderPathKey, NULL,
                                           &regValueType, NULL, &pathSize);

    if ((ERROR_SUCCESS == regOpStatus) && (REG_SZ == regValueType)) {
        LoaderRegKeyPath.resize(pathSize);
        regOpStatus = RegQueryValueExA(regKey, unifiedRuntimeLoaderPathKey, NULL,
                                       &regValueType, (LPBYTE) & *LoaderRegKeyPath.begin(),
                                       &pathSize);
        if (ERROR_SUCCESS != regOpStatus) {
            LoaderRegKeyPath.clear();
            LoaderRegKeyPath.assign("");
        }
    }

    return LoaderRegKeyPath;
}
#endif

//////////////////////////////////////////////////////////////////////////
#if !defined(_WIN32) && (__GNUC__ >= 4)
#define __urdlllocal  __attribute__ ((visibility ("hidden")))
#else
#define __urdlllocal
#endif

///////////////////////////////////////////////////////////////////////////////
inline bool getenv_tobool( const char* name )
{
    const char* env = nullptr;

#if defined(_WIN32)
    char buffer[8];
    auto rc = GetEnvironmentVariable(name, buffer, 8);
    if (0 != rc && rc <= 8) {
        env = buffer;
    }
#else
    env = getenv(name);
#endif

    if( nullptr == env )
        return false;
    return 0 == strcmp( "1", env );
}

#endif /* UR_UTIL_H */
