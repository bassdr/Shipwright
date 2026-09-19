cmake_minimum_required(VERSION 3.29.0 FATAL_ERROR)

set(CMAKE_SYSTEM_VERSION 10.0 CACHE STRING "" FORCE)

project(soh LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20 CACHE STRING "The C++ standard to use")
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 23 CACHE STRING "The C standard to use")

if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    enable_language(OBJCXX)
    set(CMAKE_OBJC_FLAGS "${CMAKE_OBJC_FLAGS} -fobjc-arc")
    set(CMAKE_OBJCXX_FLAGS "${CMAKE_OBJCXX_FLAGS} -fobjc-arc")
endif()

set (BUILD_UTILS OFF CACHE STRING "no utilities")
set (BUILD_SHARED_LIBS OFF CACHE STRING "install/link shared instead of static libs")

################################################################################
# Set target arch type if empty. Visual studio solution generator provides it.
################################################################################
if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    if(NOT CMAKE_VS_PLATFORM_NAME)
        set(CMAKE_VS_PLATFORM_NAME "x64")
    endif()
    if("${CMAKE_VS_PLATFORM_NAME}" MATCHES "^[Aa][Rr][Mm]64$")
        set(SOH_WINDOWS_ARM64 TRUE)
    else()
        set(SOH_WINDOWS_ARM64 FALSE)
    endif()
    if("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x64" OR SOH_WINDOWS_ARM64)
        set(SOH_WINDOWS_64BIT TRUE)
    else()
        set(SOH_WINDOWS_64BIT FALSE)
    endif()
    message("${CMAKE_VS_PLATFORM_NAME} architecture in use")

    if(NOT ("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x64"
        OR "${CMAKE_VS_PLATFORM_NAME}" STREQUAL "Win32"
        OR SOH_WINDOWS_ARM64))
        message(FATAL_ERROR "${CMAKE_VS_PLATFORM_NAME} arch is not supported!")
    endif()
endif()

################################################################################
# Global configuration types
################################################################################
set(CMAKE_CONFIGURATION_TYPES
    "Debug"
    "Release"
    CACHE STRING "" FORCE
)

# CMake's per-platform compiler and linker defaults are kept. Clearing them, as the Visual Studio
# generator's boilerplate did, dropped /O2 /Ob2 /DNDEBUG and the default linker flags, which then
# had to be put back one at a time (#7185, #7188).

################################################################################
# Common utils
################################################################################
include(CMake/Utils.cmake)

################################################################################
# Additional Global Settings(add specific info there)
################################################################################
include(CMake/GlobalSettingsInclude.cmake OPTIONAL)

################################################################################
# Use solution folders feature
################################################################################
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

################################################################################
# Sub-projects
################################################################################
if (NOT TARGET libultraship)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../libultraship ${CMAKE_BINARY_DIR}/libultraship SYSTEM)
endif()

set(PROJECT_NAME soh)

################################################################################
# Sources
################################################################################
configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/src/boot/build.c.in ${CMAKE_CURRENT_SOURCE_DIR}/src/boot/build.c @ONLY)
configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/properties.h.in ${CMAKE_CURRENT_SOURCE_DIR}/properties.h @ONLY)

set(Header_Files "resource.h")
source_group("headers" FILES ${Header_Files})

# include {{{
file(GLOB Header_Files__include CONFIGURE_DEPENDS "include/*.h" "include/*.inc")
list(APPEND Header_Files__include ${CMAKE_CURRENT_SOURCE_DIR}/include/libc/stdarg.h)
list(REMOVE_ITEM Header_Files__include ${CMAKE_CURRENT_SOURCE_DIR}/include/bgm.h)
list(REMOVE_ITEM Header_Files__include ${CMAKE_CURRENT_SOURCE_DIR}/include/math_n64.h)
list(REMOVE_ITEM Header_Files__include ${CMAKE_CURRENT_SOURCE_DIR}/include/stdbool_n64.h)
list(REMOVE_ITEM Header_Files__include ${CMAKE_CURRENT_SOURCE_DIR}/include/stddef_n64.h)
list(REMOVE_ITEM Header_Files__include ${CMAKE_CURRENT_SOURCE_DIR}/include/stdlib_n64.h)
list(REMOVE_ITEM Header_Files__include ${CMAKE_CURRENT_SOURCE_DIR}/include/ultra64.h)
source_group("include" FILES ${Header_Files__include})
# }}}

# soh (root)
file(GLOB_RECURSE soh__ CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "soh/*.c" "soh/*.cpp" "soh/*.h" "soh/*.hpp")

# Add specific files that don't match the pattern
list(APPEND soh__ ${CMAKE_CURRENT_SOURCE_DIR}/soh/Enhancements/speechsynthesizer/DarwinSpeechSynthesizer.mm)

# Create source groups that match the real file directory paths
foreach(source IN LISTS soh__)
    get_filename_component(source_path "${source}" PATH)
    string(REPLACE "/" "\\" source_path_adjusted "${source_path}")
    source_group("${source_path_adjusted}" FILES "${source}")
endforeach()

# Fix UTF-8 issues for MSVC. Keyed on the MSVC variable, not the compiler id, so it also covers
# clang-cl, which reports its id as Clang.
if (MSVC)
    set_source_files_properties(soh/OTRGlobals.cpp PROPERTIES COMPILE_FLAGS "/utf-8")
    set_source_files_properties(soh/Enhancements/tts/tts.cpp PROPERTIES COMPILE_FLAGS "/utf-8")
    set_source_files_properties(soh/Enhancements/custom-message/CustomMessageManager.cpp PROPERTIES COMPILE_FLAGS "/utf-8")
    set_source_files_properties(soh/Enhancements/custom-message/CustomMessageManager.h PROPERTIES COMPILE_FLAGS "/utf-8")
endif()

# handle speechsynthesizer removals. espeak reads text only where the platform has no
# reader of its own, so the other two win where they exist.
if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    list(FILTER soh__ EXCLUDE REGEX "soh/Enhancements/speechsynthesizer/(Darwin|ESpeak)")
elseif (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    list(FILTER soh__ EXCLUDE REGEX "soh/Enhancements/speechsynthesizer/(SAPI|ESpeak)")
else()
    list(FILTER soh__ EXCLUDE REGEX "soh/Enhancements/speechsynthesizer/(Darwin|SAPI)")
endif()

# soh/Extractor {{{
list(FILTER ship__ EXCLUDE REGEX "soh/Extractor/*")

if(NOT CMAKE_SYSTEM_NAME MATCHES "NintendoSwitch|CafeOS")
    file(GLOB_RECURSE soh__Extractor CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
        "soh/Extractor/*.c"
        "soh/Extractor/*.cpp"
        "soh/Extractor/*.h"
        "soh/Extractor/*.hpp"
    )
else()
    file(GLOB_RECURSE soh__Extractor CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
        "soh/Extractor/*.h"
        "soh/Extractor/*.hpp"
    )
endif()
# }}}

# src (decomp) {{{
file(GLOB_RECURSE src__ CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "src/*.c" "src/*.h")
set_source_files_properties(${src__} PROPERTIES COMPILE_OPTIONS "${WARNING_OVERRIDE}")

list(APPEND src__ ${CMAKE_CURRENT_SOURCE_DIR}/Resource.rc)
list(FILTER src__ EXCLUDE REGEX "src/dmadata/")
list(FILTER src__ EXCLUDE REGEX "src/elf_message/")
list(FILTER src__ EXCLUDE REGEX "src/libultra/io/")
list(FILTER src__ EXCLUDE REGEX "src/libultra/libc/")
list(FILTER src__ EXCLUDE REGEX "src/libultra/os/")
list(FILTER src__ EXCLUDE REGEX "src/libultra/rmon/")
list(APPEND src__ "src/libultra/libc/sprintf.c")
list(REMOVE_ITEM src__ "src/libultra/gu/cosf.c")
list(REMOVE_ITEM src__ "src/libultra/gu/lookat.c")
list(REMOVE_ITEM src__ "src/libultra/gu/lookathil.c")
list(REMOVE_ITEM src__ "src/libultra/gu/perspective.c")
list(REMOVE_ITEM src__ "src/libultra/gu/position.c")
list(REMOVE_ITEM src__ "src/libultra/gu/sinf.c")
list(REMOVE_ITEM src__ "src/libultra/gu/sinf.c")
list(REMOVE_ITEM src__ "src/libultra/gu/sqrtf.c")
list(REMOVE_ITEM src__ "src/libultra/gu/us2dex.c")

source_group("src" REGULAR_EXPRESSION "src/*")
source_group("src\\boot" REGULAR_EXPRESSION "src/boot/*")
source_group("src\\buffers" REGULAR_EXPRESSION "src/buffers/*")
source_group("src\\code" REGULAR_EXPRESSION "src/code/*")
source_group("src\\libultra" REGULAR_EXPRESSION "src/libultra/*")
source_group("src\\overlays\\actors" REGULAR_EXPRESSION "src/overlays/actors/*")
source_group("src\\overlays\\effects" REGULAR_EXPRESSION "src/overlays/effects/*")
source_group("src\\overlays\\gamestates" REGULAR_EXPRESSION  "src/overlays/gamestates/*")
source_group("src\\overlays\\misc" REGULAR_EXPRESSION  "src/overlays/misc/*")
# }}}

set(ALL_FILES
    ${Header_Files}
    ${Header_Files__include}
    ${soh__}
    ${soh__Extractor}
    ${src__}
)

################################################################################
# Target
################################################################################
add_executable(${PROJECT_NAME} ${ALL_FILES})

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
use_props(${PROJECT_NAME} "${CMAKE_CONFIGURATION_TYPES}" "${DEFAULT_CXX_PROPS}")
endif()

# FluidSynth synthesis backend.
# Always built from source and linked statically into soh, on every platform:
# a system/shared libfluidsynth drags in glib and leaves a dylib the AppImage
# (and macOS/Windows bundlers) must ship -- exactly what black-screened the
# Linux AppImage when the lib wasn't found at runtime. osal=cpp11 replaces glib
# with STL threading primitives and libinstpatch=OFF keeps it from being pulled back in,
# so the only remaining shared dep is libsndfile (SF3 Ogg decode), which the
# platform's normal bundling handles cleanly.
if(ENABLE_FLUIDSYNTH)
    include(FetchContent)
    set(BUILD_SHARED_LIBS OFF)
    set(osal cpp11 CACHE STRING "" FORCE)
    set(enable-framework OFF CACHE BOOL "" FORCE)
    set(enable-libsndfile ON CACHE BOOL "" FORCE)
    set(enable-libinstpatch OFF CACHE BOOL "" FORCE)
    # soh renders FluidSynth into its own audio pipeline (Render() -> LUS), so no
    # FluidSynth audio/MIDI driver, network shell, or effect host is ever used.
    # Disabling them all keeps the static lib to what we call and drops their
    # link-time deps (alsa, gomp, ...). Options for other platforms are harmless.
    set(enable-alsa OFF CACHE BOOL "" FORCE)
    set(enable-oss OFF CACHE BOOL "" FORCE)
    set(enable-pipewire OFF CACHE BOOL "" FORCE)
    set(enable-pulseaudio OFF CACHE BOOL "" FORCE)
    set(enable-jack OFF CACHE BOOL "" FORCE)
    set(enable-portaudio OFF CACHE BOOL "" FORCE)
    set(enable-sdl3 OFF CACHE BOOL "" FORCE)
    set(enable-aufile OFF CACHE BOOL "" FORCE)
    set(enable-coreaudio OFF CACHE BOOL "" FORCE)
    set(enable-coremidi OFF CACHE BOOL "" FORCE)
    set(enable-waveout OFF CACHE BOOL "" FORCE)
    set(enable-wasapi OFF CACHE BOOL "" FORCE)
    set(enable-dsound OFF CACHE BOOL "" FORCE)
    set(enable-winmidi OFF CACHE BOOL "" FORCE)
    set(enable-midishare OFF CACHE BOOL "" FORCE)
    set(enable-network OFF CACHE BOOL "" FORCE)
    set(enable-ipv6 OFF CACHE BOOL "" FORCE)
    set(enable-ladspa OFF CACHE BOOL "" FORCE)
    set(enable-systemd OFF CACHE BOOL "" FORCE)
    set(enable-readline OFF CACHE BOOL "" FORCE)
    set(enable-dbus OFF CACHE BOOL "" FORCE)
    # Keep OpenMP: it parallelizes SF3 (ogg) decode, and soundfonts reload on a
    # velocity-mode switch -- single-threaded decode made that switch much slower.
    set(enable-openmp ON CACHE BOOL "" FORCE)
    # DLS/GIG loading is unused (SF2/SF3 only).
    set(enable-native-dls OFF CACHE BOOL "" FORCE)
    # Match soh's static CRT: FluidSynth honors CMP0091 but defaults to the DLL
    # runtime, which mismatches soh's MultiThreaded (/MT) runtime at link time.
    if(MSVC)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    # EXCLUDE_FROM_ALL drops what we never use: FluidSynth's CLI player, which nothing links,
    # and the install rules that would otherwise package a binary this build no longer produces.
    FetchContent_Declare(fluidsynth
        GIT_REPOSITORY https://github.com/FluidSynth/fluidsynth.git
        GIT_TAG v2.5.5
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(fluidsynth)
    quiet_dependency_tree("${fluidsynth_SOURCE_DIR}")
    target_link_libraries(${PROJECT_NAME} PRIVATE libfluidsynth)
    target_compile_definitions(${PROJECT_NAME} PRIVATE ENABLE_FLUIDSYNTH=1)
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE ENABLE_FLUIDSYNTH=0)
endif()

# Voice-acting synthesis.
# The clips are generated on the player's machine from their own game text, so the
# synthesiser has to be in-process rather than an offline script. espeak-ng (already
# linked for the accessibility reader) supplies the phonemes and Opus (already linked
# for streamed samples) writes the cache, which leaves the neural model as the only
# new dependency.
#
# ONNX Runtime ships as prebuilt archives; building it from source is impractical.
# That means a shared library the bundlers have to carry, unlike the static
# FluidSynth above -- turn ENABLE_VOICE_SYNTH off for a build that must stay
# self-contained.
if(ENABLE_VOICE_SYNTH)
    include(FetchContent)
    set(ORT_VERSION 1.20.1)
    set(ORT_BASE "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}")
    if(WIN32)
        set(ORT_ARCHIVE "onnxruntime-win-x64-${ORT_VERSION}.zip")
        set(ORT_SHA256 78d447051e48bd2e1e778bba378bec4ece11191c9e538cf7b2c4a4565e8f5581)
    elseif(APPLE)
        set(ORT_ARCHIVE "onnxruntime-osx-universal2-${ORT_VERSION}.tgz")
        set(ORT_SHA256 da4349e01a7e997f5034563183c7183d069caadc1d95f499b560961787813efd)
    else()
        set(ORT_ARCHIVE "onnxruntime-linux-x64-${ORT_VERSION}.tgz")
        set(ORT_SHA256 67db4dc1561f1e3fd42e619575c82c601ef89849afc7ea85a003abbac1a1a105)
    endif()
    # URL_HASH is what makes this safe: a truncated or 404'd download fails the
    # configure instead of leaving a corrupt archive to fail mysteriously later.
    FetchContent_Declare(onnxruntime
        URL "${ORT_BASE}/${ORT_ARCHIVE}"
        URL_HASH SHA256=${ORT_SHA256}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(onnxruntime)

    # The unversioned library the archive ships is a symlink; what the loader asks for is
    # the SONAME, so the copy beside the binary has to carry that name instead.
    if(WIN32)
        set(ORT_LIB "${onnxruntime_SOURCE_DIR}/lib/onnxruntime.lib")
        set(ORT_RUNTIME "${onnxruntime_SOURCE_DIR}/lib/onnxruntime.dll")
        set(ORT_RUNTIME_NAME "onnxruntime.dll")
    elseif(APPLE)
        set(ORT_LIB "${onnxruntime_SOURCE_DIR}/lib/libonnxruntime.dylib")
        set(ORT_RUNTIME "${onnxruntime_SOURCE_DIR}/lib/libonnxruntime.${ORT_VERSION}.dylib")
        set(ORT_RUNTIME_NAME "libonnxruntime.${ORT_VERSION}.dylib")
    else()
        set(ORT_LIB "${onnxruntime_SOURCE_DIR}/lib/libonnxruntime.so")
        set(ORT_RUNTIME "${onnxruntime_SOURCE_DIR}/lib/libonnxruntime.so.${ORT_VERSION}")
        set(ORT_RUNTIME_NAME "libonnxruntime.so.1")
    endif()

    add_library(onnxruntime_shared SHARED IMPORTED)
    set_target_properties(onnxruntime_shared PROPERTIES
        IMPORTED_LOCATION "${ORT_RUNTIME}"
        IMPORTED_IMPLIB "${ORT_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${onnxruntime_SOURCE_DIR}/include")
    target_link_libraries(${PROJECT_NAME} PRIVATE onnxruntime_shared)
    target_compile_definitions(${PROJECT_NAME} PRIVATE ENABLE_VOICE_SYNTH=1)

    # The runtime is shipped beside the binary, so the loader has to look there too rather
    # than only in the build tree the linker recorded. Installing rewrites the rpath from
    # scratch, which is why the installed copy needs its own rather than inheriting one.
    if(APPLE)
        set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY BUILD_RPATH "@loader_path")
        set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY INSTALL_RPATH "@loader_path")
    elseif(NOT WIN32)
        set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY BUILD_RPATH "$ORIGIN")
        set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY INSTALL_RPATH "$ORIGIN")
    endif()

    # The game loads it at runtime, so it has to sit beside the binary in both trees.
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ORT_RUNTIME}" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${ORT_RUNTIME_NAME}")
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE ENABLE_VOICE_SYNTH=0)
endif()

set(ROOT_NAMESPACE soh)

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set_target_properties(${PROJECT_NAME} PROPERTIES
        VS_GLOBAL_KEYWORD "Win32Proj"
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set_target_properties(${PROJECT_NAME} PROPERTIES
        XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES
        OUTPUT_NAME "soh-macos"
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "soh.elf"
    )
endif()
################################################################################
# MSVC runtime library
################################################################################
if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    get_property(MSVC_RUNTIME_LIBRARY_DEFAULT TARGET ${PROJECT_NAME} PROPERTY MSVC_RUNTIME_LIBRARY)
    if(SOH_WINDOWS_64BIT)
        string(CONCAT "MSVC_RUNTIME_LIBRARY_STR"
            $<$<CONFIG:Debug>:
                MultiThreadedDebug
            >
            $<$<CONFIG:Release>:
                MultiThreaded
            >
            $<$<NOT:$<OR:$<CONFIG:Debug>,$<CONFIG:Release>>>:${MSVC_RUNTIME_LIBRARY_DEFAULT}>
        )
    elseif("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "Win32")
        string(CONCAT "MSVC_RUNTIME_LIBRARY_STR"
            $<$<CONFIG:Debug>:
                MultiThreadedDebug
            >
            $<$<CONFIG:Release>:
                MultiThreaded
            >
            $<$<NOT:$<OR:$<CONFIG:Debug>,$<CONFIG:Release>>>:${MSVC_RUNTIME_LIBRARY_DEFAULT}>
        )
    endif()
    set_target_properties(${PROJECT_NAME} PROPERTIES MSVC_RUNTIME_LIBRARY ${MSVC_RUNTIME_LIBRARY_STR})
endif()
################################################################################
# Find/download Dr Libs (For custom audio)
################################################################################
include(FetchContent)

FetchContent_Declare(
        dr_libs
        GIT_REPOSITORY https://github.com/mackron/dr_libs.git
        GIT_TAG da35f9d6c7374a95353fd1df1d394d44ab66cf01
)
FetchContent_MakeAvailable(dr_libs)

find_package(SDL3)
set(SDL3-INCLUDE ${SDL3_INCLUDE_DIRS})

# SDL3_net is too new to be packaged on most distros (Gentoo has no ebuild yet), so
# build it rather than making the remote features depend on the platform.
find_package(SDL3_net QUIET)

if(NOT SDL3_net_FOUND)
    message(STATUS "SDL3_net not found; building it from source.")
    set(SDLNET_SAMPLES OFF CACHE BOOL "" FORCE)
    set(SDLNET_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3_net
        GIT_REPOSITORY https://github.com/libsdl-org/SDL_net.git
        GIT_TAG release-3.2.0
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(SDL3_net)
    quiet_dependency_tree("${SDL3_net_SOURCE_DIR}")
else()
    set(SDL3-NET-INCLUDE ${SDL3_net_INCLUDE_DIRS})
endif()

# espeak-ng from source rather than from the system: the voice pipeline needs its
# phonemiser on every platform, not just the one distribution that packages it, and a
# system copy alongside the one we link would be a second set of its globals.
include(FetchContent)
set(BUILD_SHARED_LIBS OFF)
# Nothing here ever asks espeak for sound. It phonemises for the voice models and
# hands the reader its samples, both of which the game mixes itself, so the audio
# layer is dead weight and the worker thread is ours.
set(USE_LIBPCAUDIO OFF CACHE BOOL "" FORCE)
set(USE_ASYNC OFF CACHE BOOL "" FORCE)
set(USE_MBROLA OFF CACHE BOOL "" FORCE)
set(USE_LIBSONIC OFF CACHE BOOL "" FORCE)
set(ESPEAK_COMPAT OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(espeak-ng
    GIT_REPOSITORY https://github.com/espeak-ng/espeak-ng.git
    GIT_TAG 1.52.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(espeak-ng)
quiet_dependency_tree("${espeak-ng_SOURCE_DIR}")

# espeak publishes its compiler compatibility shims on the same public include path as its
# real headers, and a wchar.h of theirs then shadows the system one for every file the game
# compiles. They belong to espeak's own sources.
set_property(TARGET espeak-include PROPERTY INTERFACE_INCLUDE_DIRECTORIES
             "${espeak-ng_SOURCE_DIR}/src/include")
foreach(espeak_target espeak-ng espeak-ng-bin)
    target_include_directories(${espeak_target} BEFORE PRIVATE "${espeak-ng_SOURCE_DIR}/src/include/compat")
endforeach()

target_link_libraries(${PROJECT_NAME} PRIVATE espeak-ng)
add_compile_definitions(ESPEAK=1)

# espeak_Initialize is handed the directory holding espeak-ng-data, so the phoneme and
# dictionary data the build compiles has to travel with the binary. `data` is espeak's
# own target for it, and nothing orders it against ours without being told.
add_dependencies(${PROJECT_NAME} data)
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${espeak-ng_BINARY_DIR}/espeak-ng-data" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/espeak-ng-data")

################################################################################
# Compile definitions
################################################################################

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/
    ${CMAKE_CURRENT_SOURCE_DIR}/src/
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/
    .
)

target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE
    ${SDL3-INCLUDE}
    ${SDL3-NET-INCLUDE}
    ${dr_libs_SOURCE_DIR}
)

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    if(SOH_WINDOWS_64BIT)
        target_compile_definitions(${PROJECT_NAME} PRIVATE
            "$<$<CONFIG:Debug>:"
                "_DEBUG;"
                "_CRT_SECURE_NO_WARNINGS;"
                "ENABLE_DX11;"
            ">"
            "$<$<CONFIG:Release>:"
                "NDEBUG;"
            ">"
            "INCLUDE_GAME_PRINTF;"
            "F3DEX_GBI_2"
            "UNICODE;"
            "_UNICODE"
            SPDLOG_ACTIVE_LEVEL=${SPDLOG_MIN_CUTOFF}
            LOG_LEVEL_GAME_PRINTS=${SPDLOG_LEVEL_OFF}
            STORMLIB_NO_AUTO_LINK
            "_CRT_SECURE_NO_WARNINGS;"
            NOMINMAX
        )
    elseif("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "Win32")
        target_compile_definitions(${PROJECT_NAME} PRIVATE
            "$<$<CONFIG:Debug>:"
                "NOINCLUDE_GAME_PRINTF;"
                "_DEBUG;"
                "_CRT_SECURE_NO_WARNINGS;"
                "ENABLE_OPENGL;"
            ">"
            "$<$<CONFIG:Release>:"
                "NDEBUG;"
            ">"
            "INCLUDE_GAME_PRINTF;"
            "F3DEX_GBI_2"
            "WIN32;"
            "UNICODE;"
            "_UNICODE;"
            SPDLOG_ACTIVE_LEVEL=${SPDLOG_MIN_CUTOFF}
            LOG_LEVEL_GAME_PRINTS=${SPDLOG_LEVEL_OFF}
            STORMLIB_NO_AUTO_LINK
            NOMINMAX
        )
    endif()
elseif (CMAKE_SYSTEM_NAME STREQUAL "CafeOS")
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        "$<$<CONFIG:Debug>:"
            "_DEBUG;"
        ">"
        "$<$<CONFIG:Release>:"
            "NDEBUG;"
        ">"
        "F3DEX_GBI_2;"
        "SPDLOG_NO_THREAD_ID;"
        "SPDLOG_NO_TLS;"
        "STBI_NO_THREAD_LOCALS;"
        SPDLOG_ACTIVE_LEVEL=${SPDLOG_MIN_CUTOFF}
        LOG_LEVEL_GAME_PRINTS=${SPDLOG_LEVEL_OFF}
    )
elseif ("${CMAKE_CXX_COMPILER_ID}" MATCHES "GNU|Clang|AppleClang")
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        "$<$<CONFIG:Debug>:"
            "_DEBUG;"
        ">"
        "$<$<CONFIG:Release>:"
            "NDEBUG;"
        ">"
        "F3DEX_GBI_2;"
        "_CONSOLE;"
        "_CRT_SECURE_NO_WARNINGS;"
        "ENABLE_OPENGL;"
        "UNICODE;"
        "_UNICODE;"
        SPDLOG_ACTIVE_LEVEL=${SPDLOG_MIN_CUTOFF}
        LOG_LEVEL_GAME_PRINTS=${SPDLOG_LEVEL_OFF}
    )
endif()
################################################################################
# Compile and link options
################################################################################
# One warning vocabulary for every platform. clang-cl's driver reinterprets the GNU
# spellings - -Wall there is an alias for -Weverything - so it gets them through /clang:,
# which reaches the same frontend the other two platforms use.
set(SOH_WARNINGS
    -Wall -Wextra -Wno-error
    -Wformat-security
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-missing-field-initializers
    -Wno-parentheses
    -Wno-narrowing
    -Wno-missing-braces
    -Wno-unknown-pragmas # #pragma region is an editor folding marker, GCC doesn't know it
)
# Decompiled C leans on conversions that clang makes hard errors in C23, which no warning
# level and no -Wno-error will undo.
set(SOH_C_WARNINGS
    -Werror-implicit-function-declaration
    -Wno-implicit-int
    -Wno-incompatible-pointer-types
    -Wno-int-conversion
)
if(SOH_CLANG_CL)
    list(TRANSFORM SOH_WARNINGS PREPEND "/clang:")
    list(TRANSFORM SOH_C_WARNINGS PREPEND "/clang:")
endif()

if(MSVC)
    # cl's level, for cl only: clang-cl takes its warnings from SOH_WARNINGS above, where
    # /W3 would mean clang's -Wall on top of the list rather than a level.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        set(SOH_MSVC_WARNING_LEVEL /W3)
        set_property(TARGET ${PROJECT_NAME} PROPERTY COMPILE_WARNING_AS_ERROR ON)
    else()
        set(SOH_MSVC_WARNING_LEVEL "")
    endif()
    if(SOH_WINDOWS_64BIT)
        target_compile_options(${PROJECT_NAME} PRIVATE
            $<$<CONFIG:Debug>:
                /Od
            >
            $<$<CONFIG:Release>:
                /O2;
                /Oi;
                /Gy;
            >
            ${SOH_MSVC_WARNING_LEVEL};
            /bigobj;
            /sdl-;
            /permissive-;
            ${DEFAULT_CXX_DEBUG_INFORMATION_FORMAT};
            ${DEFAULT_CXX_EXCEPTION_HANDLING}
        )
        target_compile_options(${PROJECT_NAME} PRIVATE  $<$<CONFIG:Debug>:/ZI;>)
    elseif("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "Win32")
        target_compile_options(${PROJECT_NAME} PRIVATE
            $<$<CONFIG:Debug>:
                /RTCs
            >
            $<$<CONFIG:Release>:
                /O2;
                /Oi;
                /Gy
            >
            /permissive-;
            /sdl-;
            ${SOH_MSVC_WARNING_LEVEL};
            ${DEFAULT_CXX_DEBUG_INFORMATION_FORMAT};
            ${DEFAULT_CXX_EXCEPTION_HANDLING}
        )
    endif()
    if(SOH_WINDOWS_64BIT)
        target_link_options(${PROJECT_NAME} PRIVATE
            $<$<CONFIG:Debug>:
                /INCREMENTAL
            >
            $<$<CONFIG:Release>:
                /OPT:REF;
                /OPT:ICF;
                /INCREMENTAL:NO;
                /FORCE:MULTIPLE
            >
            /MANIFEST:NO;
            $<$<NOT:$<CONFIG:Release>>:/DEBUG>;
            /SUBSYSTEM:WINDOWS
        )
    elseif("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "Win32")
        target_link_options(${PROJECT_NAME} PRIVATE
            $<$<CONFIG:Debug>:
                /STACK:8777216
            >
            $<$<CONFIG:Release>:
                /OPT:REF;
                /OPT:ICF;
                /INCREMENTAL:NO;
                /FORCE:MULTIPLE
            >
            /MANIFEST:NO;
            $<$<NOT:$<CONFIG:Release>>:/DEBUG>;
            /SUBSYSTEM:WINDOWS
        )
    endif()

    if(SOH_CLANG_CL)
        target_compile_options(${PROJECT_NAME} PRIVATE
            ${SOH_WARNINGS}
            $<$<COMPILE_LANGUAGE:C>:${SOH_C_WARNINGS}>
        )
    endif()
endif()

if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
    if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        target_compile_options(${PROJECT_NAME} PRIVATE
            ${SOH_WARNINGS}
            -Wno-return-type
            $<$<COMPILE_LANGUAGE:C>:${SOH_C_WARNINGS}>
            $<$<COMPILE_LANGUAGE:CXX>:-fpermissive>
            $<$<COMPILE_LANGUAGE:CXX>:
                -Wno-c++11-narrowing
                -Wno-deprecated-enum-enum-conversion
            >
            -pthread
        )

        target_link_options(${PROJECT_NAME} PRIVATE
            -pthread
        )
    elseif (CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
        target_compile_options(${PROJECT_NAME} PRIVATE
            -Wall -Wextra -Wno-error
            -Wformat-security
            -Wno-return-type
            -Wno-unused-parameter
            -Wno-unused-function
            -Wno-unused-variable
            -Wno-missing-field-initializers
            -Wno-parentheses
            -Wno-narrowing
            -Wno-missing-braces
            -Wno-unknown-pragmas # #pragma region is an editor folding marker, GCC doesn't know it
            $<$<COMPILE_LANGUAGE:C>:
                -Werror-implicit-function-declaration
                -Wno-incompatible-pointer-types
            >
            $<$<COMPILE_LANGUAGE:CXX>:-fpermissive>
            $<$<COMPILE_LANGUAGE:CXX>:
                -Wno-c++11-narrowing
                -Wno-deprecated-enum-enum-conversion
            >
            -pthread
        )

        target_link_options(${PROJECT_NAME} PRIVATE
            -pthread
        )
    elseif (CMAKE_SYSTEM_NAME STREQUAL "CafeOS")
        target_compile_options(${PROJECT_NAME} PRIVATE
            -O2

            # disable some warnings to not clutter output
            -Wno-multichar
            -Wno-return-type
            -Wno-narrowing
            -Wno-switch-outside-range
            -Wno-unknown-pragmas # #pragma region is an editor folding marker, GCC doesn't know it
            $<$<COMPILE_LANGUAGE:C>:
                -Werror-implicit-function-declaration
                -Wno-incompatible-pointer-types
                -Wno-discarded-array-qualifiers
                -Wno-discarded-qualifiers
                -Wno-int-conversion
                -Wno-builtin-declaration-mismatch
                -Wno-switch-unreachable
                -Wno-stringop-overflow
            >
        )
    else()
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
        set(CPU_OPTION -msse2 -mfpmath=sse)
        endif()

        target_compile_options(${PROJECT_NAME} PRIVATE
            ${SOH_WARNINGS}
            $<$<COMPILE_LANGUAGE:C>:${SOH_C_WARNINGS}>
            $<$<COMPILE_LANGUAGE:CXX>:-fpermissive>
            $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-enum-enum-conversion>
            -pthread
        ${CPU_OPTION}
        )

        target_link_options(${PROJECT_NAME} PRIVATE
            -pthread
                #-fsanitize=address
            -Wl,-export-dynamic
        )
    endif()
endif()
################################################################################
# Pre build events
################################################################################
if (CMAKE_GENERATOR MATCHES "Visual Studio")
    add_custom_command(
    TARGET ${PROJECT_NAME}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${CMAKE_SOURCE_DIR}/soh/assets/yml ${CMAKE_BINARY_DIR}/soh/assets
    )
endif()
add_custom_command(
    TARGET ${PROJECT_NAME}
    POST_BUILD
    COMMENT "Copying asset ymls..."
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${CMAKE_SOURCE_DIR}/soh/assets/yml $<TARGET_FILE_DIR:soh>/assets
)
################################################################################
# Dependencies
################################################################################
add_dependencies(${PROJECT_NAME}
    libultraship
    torch
)

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    find_package(glfw3 REQUIRED)
    find_package(Ogg CONFIG REQUIRED)
    link_libraries(Ogg::ogg)

    find_package(Vorbis CONFIG REQUIRED)
    link_libraries(Vorbis::vorbisfile)
    find_package(Opus CONFIG REQUIRED)
    link_libraries(Opus::opus)
    find_package(OpusFile CONFIG REQUIRED)
    link_libraries(OpusFile::opusfile CONFIG REQUIRED)
    if(SOH_WINDOWS_64BIT)
        set(ADDITIONAL_LIBRARY_DEPENDENCIES
            "libultraship;"
            "torch;"
            "glu32;"
            "SDL3::SDL3;"
            "SDL3_net::SDL3_net-static"
            "glfw;"
            "winmm;"
            "imm32;"
            "version;"
            "setupapi"
            "Ogg::ogg"
            "Opus::opus"
            "Vorbis::vorbis"
            "Vorbis::vorbisenc"
            "Vorbis::vorbisfile"
            "OpusFile::opusfile"
        )
    elseif("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "Win32")
        set(ADDITIONAL_LIBRARY_DEPENDENCIES
            "libultraship;"
            "torch;"
            "glu32;"
            "SDL3::SDL3;"
            "glfw;"
            "winmm;"
            "imm32;"
            "version;"
            "setupapi"
        )
    endif()
else()
    find_package(SDL3)
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    find_package(Ogg REQUIRED)
    find_package(Vorbis REQUIRED)
    find_package(Opus REQUIRED)
    find_package(OpusFile REQUIRED)
    # clang doesn't pull libm in transitively the way gcc does, link it explicitly
    find_library(MATH_LIBRARY m)
    set(ADDITIONAL_LIBRARY_DEPENDENCIES
    "libultraship;"
        "torch;"
        SDL3::SDL3
        "Ogg::ogg"
        "Vorbis::vorbis"
        "Vorbis::vorbisenc"
        "Vorbis::vorbisfile"
        "Opus::opus"
        "Opusfile::Opusfile"
        "SDL3_net::SDL3_net"
        ${CMAKE_DL_LIBS}
        Threads::Threads
    )
    if(MATH_LIBRARY)
        list(APPEND ADDITIONAL_LIBRARY_DEPENDENCIES ${MATH_LIBRARY})
    endif()
endif()

find_package(libzip REQUIRED)
list(APPEND ADDITIONAL_LIBRARY_DEPENDENCIES libzip::zip)

if(USE_ASAN)
    # ZAPD and OTRExporter are gone (torch replaced them); naming them here made -DUSE_ASAN=ON
    # fail at configure time, so the option had been unusable.
    foreach(_asan_target libultraship soh)
        target_compile_options(${_asan_target} PRIVATE -fsanitize=address)
        target_link_options(${_asan_target} PRIVATE -fsanitize=address)
        # A report is only as good as its stack trace.
        if(NOT MSVC)
            target_compile_options(${_asan_target} PRIVATE -fno-omit-frame-pointer -g)
        endif()
        # LTO inlines away the frames ASan wants to name, and doubles the link it would
        # otherwise take to get a build in front of a repro.
        set_target_properties(${_asan_target} PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION_RELEASE FALSE
            INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO FALSE
        )
    endforeach()
    if (MSVC)
        target_compile_definitions(ImGui PRIVATE _DISABLE_VECTOR_ANNOTATION)
        target_compile_definitions(ImGui PRIVATE _DISABLE_STRING_ANNOTATION)
        target_compile_definitions(libultraship PRIVATE _DISABLE_VECTOR_ANNOTATION)
        target_compile_definitions(libultraship PRIVATE _DISABLE_STRING_ANNOTATION)

        target_compile_definitions(soh PRIVATE _DISABLE_VECTOR_ANNOTATION)
        target_compile_definitions(soh PRIVATE _DISABLE_STRING_ANNOTATION)
        target_compile_options(libultraship PRIVATE $<$<CONFIG:Debug>:/Zi;>)
        target_compile_options(soh PRIVATE $<$<CONFIG:Debug>:/Zi;>)
        # The default stack size on Windows is 1MB. ASAN uses a lot more stack so boost it to 16
        set_target_properties(soh PROPERTIES
                LINK_FLAGS "/STACK:16777216"
        )
    endif()
endif()


if(NOT CMAKE_SYSTEM_NAME MATCHES "Darwin|NintendoSwitch|CafeOS")
INSTALL(TARGETS soh DESTINATION . COMPONENT ship)
if(ENABLE_VOICE_SYNTH)
INSTALL(FILES "${ORT_RUNTIME}" DESTINATION . COMPONENT ship RENAME "${ORT_RUNTIME_NAME}")
endif()
INSTALL(DIRECTORY "${espeak-ng_BINARY_DIR}/espeak-ng-data" DESTINATION . COMPONENT ship)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
INSTALL(FILES $<TARGET_PDB_FILE:soh> DESTINATION debug COMPONENT ship OPTIONAL)
INSTALL(FILES ${CMAKE_BINARY_DIR}/soh/soh.o2r DESTINATION . COMPONENT ship)
# The only shared library the Vulkan backend pulls in; everything else here is static.
if (TARGET Vulkan::shaderc_shared)
INSTALL(FILES $<TARGET_FILE:Vulkan::shaderc_shared> DESTINATION . COMPONENT ship)
endif()
endif()

find_program(CURL NAMES curl DOC "Path to the curl program.  Used to download files.")
execute_process(COMMAND ${CURL} -sSfL https://raw.githubusercontent.com/mdqinc/SDL_GameControllerDB/master/gamecontrollerdb.txt -o ${CMAKE_BINARY_DIR}/gamecontrollerdb.txt OUTPUT_VARIABLE RESULT)

if("${CMAKE_SYSTEM_NAME}" STREQUAL "Darwin")
configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/macosx/Info.plist.in ${CMAKE_BINARY_DIR}/macosx/Info.plist @ONLY)
INSTALL(TARGETS soh DESTINATION ../MacOS COMPONENT ship)
INSTALL(FILES ${CMAKE_BINARY_DIR}/gamecontrollerdb.txt DESTINATION ../MacOS COMPONENT ship)
INSTALL(FILES ${CMAKE_BINARY_DIR}/soh/soh.o2r DESTINATION ../Resources COMPONENT ship)
elseif(NOT "${CMAKE_SYSTEM_NAME}" MATCHES "NintendoSwitch|CafeOS")
INSTALL(FILES ${CMAKE_BINARY_DIR}/gamecontrollerdb.txt DESTINATION . COMPONENT ship)
endif()

if(CMAKE_SYSTEM_NAME MATCHES "NintendoSwitch|CafeOS")
    if (NOT TARGET pathconf)
        add_library(pathconf OBJECT platform/pathconf.c)
    endif()
    target_link_libraries(${PROJECT_NAME} PRIVATE "${ADDITIONAL_LIBRARY_DEPENDENCIES}" $<TARGET_OBJECTS:pathconf> )
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE "${ADDITIONAL_LIBRARY_DEPENDENCIES}")
endif()

if(CMAKE_SYSTEM_NAME MATCHES "NintendoSwitch")

nx_generate_nacp(Ship.nacp
   NAME "Ship of Harkinian"
   AUTHOR "${PROJECT_TEAM}"
   VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}"
)

nx_create_nro(soh
    NACP Ship.nacp
    ICON ${CMAKE_CURRENT_SOURCE_DIR}/icon.jpg
)

INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/soh.nro DESTINATION . COMPONENT ship)

elseif(CMAKE_SYSTEM_NAME MATCHES "CafeOS")

wut_create_rpx(${PROJECT_NAME})

wut_create_wuhb(${PROJECT_NAME}
    NAME       "Ship of Harkinian"
    SHORTNAME  "SoH"
    AUTHOR     "${PROJECT_TEAM}"
    ICON       ${CMAKE_CURRENT_SOURCE_DIR}/icon.jpg
)

INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/soh.rpx ${CMAKE_CURRENT_BINARY_DIR}/soh.wuhb DESTINATION . COMPONENT ship)

endif()
