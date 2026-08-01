include_guard()

if (WIN32)
    if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "Dead Air: Refined supports only 64-bit Windows builds.")
    endif()

    set(XRAY_SDK_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/sdk/include")
    set(XRAY_SDK_LIBRARY_DIR "${CMAKE_SOURCE_DIR}/sdk/libraries/x64")
    set(XRAY_SDK_BINARY_DIR "${CMAKE_SOURCE_DIR}/sdk/binaries/x64")

    function(xray_add_imported_static_library target release_library debug_library)
        add_library(${target} STATIC IMPORTED GLOBAL)
        set_target_properties(${target} PROPERTIES
            IMPORTED_CONFIGURATIONS "Debug;Mixed;Release;ReleaseMasterGold"
            IMPORTED_LOCATION_DEBUG "${XRAY_SDK_LIBRARY_DIR}/${debug_library}"
            IMPORTED_LOCATION_MIXED "${XRAY_SDK_LIBRARY_DIR}/${release_library}"
            IMPORTED_LOCATION_RELEASE "${XRAY_SDK_LIBRARY_DIR}/${release_library}"
            IMPORTED_LOCATION_RELEASEMASTERGOLD "${XRAY_SDK_LIBRARY_DIR}/${release_library}"
            INTERFACE_INCLUDE_DIRECTORIES "${XRAY_SDK_INCLUDE_DIR}"
        )
    endfunction()

    function(xray_add_imported_shared_library target import_library runtime_library)
        add_library(${target} SHARED IMPORTED GLOBAL)
        set_target_properties(${target} PROPERTIES
            IMPORTED_IMPLIB "${XRAY_SDK_LIBRARY_DIR}/${import_library}"
            IMPORTED_LOCATION "${XRAY_SDK_BINARY_DIR}/${runtime_library}"
            INTERFACE_INCLUDE_DIRECTORIES "${XRAY_SDK_INCLUDE_DIR}"
        )
    endfunction()

    xray_add_imported_shared_library(OpenAL::OpenAL OpenAL32.lib OpenAL32.dll)
    xray_add_imported_shared_library(OpenSSL::Crypto libcrypto.lib libcrypto-4-x64.dll)
    xray_add_imported_shared_library(Discord::GameSDK discord_game_sdk.dll.lib discord_game_sdk.dll)
    xray_add_imported_shared_library(NVIDIA::Ansel AnselSDK64.lib AnselSDK64.dll)
    xray_add_imported_static_library(JPEG::JPEG jpeg-static.lib jpeg-static-debug.lib)
    xray_add_imported_static_library(LZO::LZO lzo.lib lzo.lib)
    xray_add_imported_static_library(Ogg::Ogg libogg_static.lib libogg_static.lib)
    xray_add_imported_static_library(Theora::Theora libtheora_static.lib libtheora_static.lib)
    xray_add_imported_static_library(Vorbis::Vorbis libvorbis_static.lib libvorbis_static.lib)
    xray_add_imported_static_library(Vorbis::VorbisFile libvorbisfile.lib libvorbisfile.lib)
    xray_add_imported_static_library(NVIDIA::NvAPI nvapi64.lib nvapi64.lib)

    add_library(AMD::AGS SHARED IMPORTED GLOBAL)
    set_target_properties(AMD::AGS PROPERTIES
        IMPORTED_CONFIGURATIONS "Debug;Mixed;Release;ReleaseMasterGold"
        IMPORTED_IMPLIB_DEBUG "${CMAKE_SOURCE_DIR}/Externals/AGS_SDK/ags_lib/lib/amd_ags_x64_2026_MDd.lib"
        IMPORTED_IMPLIB_MIXED "${CMAKE_SOURCE_DIR}/Externals/AGS_SDK/ags_lib/lib/amd_ags_x64_2026_MD.lib"
        IMPORTED_IMPLIB_RELEASE "${CMAKE_SOURCE_DIR}/Externals/AGS_SDK/ags_lib/lib/amd_ags_x64_2026_MD.lib"
        IMPORTED_IMPLIB_RELEASEMASTERGOLD "${CMAKE_SOURCE_DIR}/Externals/AGS_SDK/ags_lib/lib/amd_ags_x64_2026_MD.lib"
        IMPORTED_LOCATION_DEBUG "${CMAKE_SOURCE_DIR}/Externals/AGS_SDK/ags_lib/lib/amd_ags_x64.dll"
        IMPORTED_LOCATION_MIXED "${CMAKE_SOURCE_DIR}/Externals/AGS_SDK/ags_lib/lib/amd_ags_x64.dll"
        IMPORTED_LOCATION_RELEASE "${CMAKE_SOURCE_DIR}/Externals/AGS_SDK/ags_lib/lib/amd_ags_x64.dll"
        IMPORTED_LOCATION_RELEASEMASTERGOLD "${CMAKE_SOURCE_DIR}/Externals/AGS_SDK/ags_lib/lib/amd_ags_x64.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/Externals/AGS_SDK"
    )

    function(xray_stage_windows_runtime target)
        set(_runtime_files
            "$<TARGET_FILE:AMD::AGS>"
            "$<TARGET_FILE:Discord::GameSDK>"
            "$<TARGET_FILE:NVIDIA::Ansel>"
            "$<TARGET_FILE:OpenAL::OpenAL>"
            "$<TARGET_FILE:OpenSSL::Crypto>"
            "$<TARGET_FILE:SDL2>"
            "${XRAY_SDK_BINARY_DIR}/soft_oal.dll"
        )

        find_file(_d3dcompiler_runtime
            NAMES d3dcompiler_47.dll
            PATHS "$ENV{WindowsSdkDir}/Redist/D3D/x64"
            NO_DEFAULT_PATH
            REQUIRED
        )
        list(APPEND _runtime_files "${_d3dcompiler_runtime}")

        file(GLOB _vc_runtime_directories LIST_DIRECTORIES TRUE
            "$ENV{VCToolsRedistDir}/x64/Microsoft.VC*.CRT"
        )
        file(GLOB _openmp_runtime_directories LIST_DIRECTORIES TRUE
            "$ENV{VCToolsRedistDir}/x64/Microsoft.VC*.OpenMP"
        )
        file(GLOB _cxxamp_runtime_directories LIST_DIRECTORIES TRUE
            "$ENV{VCToolsRedistDir}/x64/Microsoft.VC*.CXXAMP"
        )
        if (NOT _vc_runtime_directories OR NOT _openmp_runtime_directories OR NOT _cxxamp_runtime_directories)
            message(FATAL_ERROR "The Visual C++ x64 redistributable files could not be located.")
        endif()

        list(GET _vc_runtime_directories 0 _vc_runtime_directory)
        list(GET _openmp_runtime_directories 0 _openmp_runtime_directory)
        list(GET _cxxamp_runtime_directories 0 _cxxamp_runtime_directory)
        file(GLOB _vc_runtime_files "${_vc_runtime_directory}/*.dll")
        list(APPEND _runtime_files
            ${_vc_runtime_files}
            "${_openmp_runtime_directory}/vcomp140.dll"
            "${_cxxamp_runtime_directory}/vcamp140.dll"
        )

        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${_runtime_files}
                "$<TARGET_FILE_DIR:${target}>"
            COMMAND_EXPAND_LISTS
            VERBATIM
        )
    endfunction()

    set(JPEG_FOUND TRUE)
    set(MEMORY_ALLOCATOR "mimalloc" CACHE STRING "Use specific memory allocator (mimalloc/standard)")
else()
    find_package(OpenAL REQUIRED)
    find_package(JPEG)
    find_package(Ogg REQUIRED)
    find_package(Vorbis REQUIRED)
    find_package(Theora REQUIRED)
    find_package(LZO REQUIRED)
    find_package(mimalloc NAMES mimalloc2 mimalloc2.0 mimalloc)

    if (mimalloc_FOUND)
        set(MEMORY_ALLOCATOR "mimalloc" CACHE STRING "Use specific memory allocator (mimalloc/standard)")
    else()
        set(MEMORY_ALLOCATOR "standard" CACHE STRING "Use specific memory allocator (mimalloc/standard)")
    endif()
endif()

set_property(CACHE MEMORY_ALLOCATOR PROPERTY STRINGS "mimalloc" "standard")
