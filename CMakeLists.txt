# Black Enhanced Online — Recompilação estática (Xbox -> PC / WebAssembly) + Multiplayer

cmake_minimum_required(VERSION 3.25)
project(BlackEnhancedOnline LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# O núcleo de rede (libs/netcore) não depende do motor nem dos arquivos de BLACK.
# Em CI ou em máquinas sem o disco do jogo, use -DBLACK_NETCORE_ONLY=ON para compilar e testar o netcore.
option(BLACK_NETCORE_ONLY "Build only the engine-independent netcore library and unit tests" OFF)

if(EMSCRIPTEN AND NOT BLACK_NETCORE_ONLY)
    add_compile_options(-pthread -fwasm-exceptions)
    add_link_options(-pthread -fwasm-exceptions)
endif()

enable_testing()
add_subdirectory(libs/netcore)

if(BLACK_NETCORE_ONLY)
    return()
endif()

# Localização do SDK XboxXBE-Recomp (submódulo em sdk/ por padrão)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sdk/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp)
else()
    include(FetchContent)
    FetchContent_Declare(xbe_sdk GIT_REPOSITORY https://github.com/TGP482/XboxXBE-Recomp.git GIT_TAG main)
    FetchContent_MakeAvailable(xbe_sdk)
    set(SDK ${xbe_sdk_SOURCE_DIR})
endif()

if(EMSCRIPTEN)
    set(GENERATED_GAME_DIR ${CMAKE_CURRENT_SOURCE_DIR}/generated/game)
    if(NOT EXISTS ${GENERATED_GAME_DIR}/x86_func_mapping.cpp)
        message(FATAL_ERROR "generated/game/ nao encontrado. Execute build.bat no host antes do build WebAssembly.")
    endif()

    file(GLOB WEB_RUNTIME_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/web/*.cpp)
    file(GLOB GAME_OVERRIDE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/game/*.cpp)
    file(GLOB RECOMP_PARTITION_SOURCES CONFIGURE_DEPENDS ${GENERATED_GAME_DIR}/x86_recomp.*.cpp)

    set(SDK_PORTABLE_SOURCES
        ${SDK}/runtime/core/log.cpp
        ${SDK}/runtime/gpu/shaders.cpp
        ${SDK}/runtime/gpu/texture.cpp
        ${SDK}/runtime/gpu/vertex.cpp
        ${SDK}/runtime/kernel/crypto.cpp
        ${SDK}/runtime/kernel/files.cpp
        ${SDK}/runtime/kernel/objects.cpp
        ${SDK}/runtime/kernel/ordinals.cpp
        ${SDK}/runtime/kernel/rtl.cpp
        ${SDK}/runtime/kernel/xboxkrnl.cpp
        ${SDK}/runtime/xdk/bink.cpp
        ${SDK}/runtime/xdk/crt_math.cpp
        ${SDK}/runtime/xdk/d3d8.cpp
        ${SDK}/runtime/xdk/dsound.cpp
        ${SDK}/runtime/xdk/dsp_image.cpp
        ${SDK}/runtime/xdk/xapi.cpp
    )

    add_executable(BlackEnhancedWeb
        ${WEB_RUNTIME_SOURCES}
        ${GAME_OVERRIDE_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/game_info.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/pc_input.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/mp/mp_bridge.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/mp/mp_webrtc.cpp
        ${SDK_PORTABLE_SOURCES}
        ${RECOMP_PARTITION_SOURCES}
        ${GENERATED_GAME_DIR}/x86_func_mapping.cpp
    )

    target_include_directories(BlackEnhancedWeb BEFORE PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/web/override
    )
    target_include_directories(BlackEnhancedWeb PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${GENERATED_GAME_DIR}
        ${SDK}/runtime
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/netcore/include
    )

    target_compile_definitions(BlackEnhancedWeb PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        BLACK_WEB_BUILD=1
    )
    target_compile_options(BlackEnhancedWeb PRIVATE
        -Wno-parentheses-equality
        -Wno-unused-value
        -Wno-sign-compare
    )
    target_link_libraries(BlackEnhancedWeb PRIVATE netcore)

    set_target_properties(BlackEnhancedWeb PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/out/web"
        OUTPUT_NAME "black_enhanced"
        SUFFIX ".js"
    )

    target_link_options(BlackEnhancedWeb PRIVATE
        -sUSE_PTHREADS=1
        -sPTHREAD_POOL_SIZE=24
        -sALLOW_MEMORY_GROWTH=0
        -sINITIAL_MEMORY=1073741824
        -sMAXIMUM_MEMORY=1073741824
        -sSTACK_SIZE=1048576
        -sDEFAULT_PTHREAD_STACK_SIZE=4194304
        -sMODULARIZE=1
        -sEXPORT_ES6=1
        -sEXPORT_NAME=createBlackEnhancedModule
        -sFORCE_FILESYSTEM=1
        -lidbfs.js
        -sEXIT_RUNTIME=0
        "-sEXPORTED_FUNCTIONS=['_main','_malloc','_free','_BlackWebRequestBoot','_BlackWebSetRole','_BlackWebSetRoleEx','_BlackWebSetRoleMode','_BlackWebTerritorialAction','_BlackWebConfigurePreMatch','_BlackWebSetSkipCutscenes','_BlackWebGetSkipCutscenes','_BlackWebSetEnhancedConfig','_BlackWebSetRenderPercent','_BlackWebSetOverhaulConfig','_BlackWebBuyLoadout','_BlackWebEquipSlot','_BlackWebGetWeaponsJson','_BlackWebPushKeyState','_BlackWebPushMouseState','_BlackWebPushGamepadState','_BlackWebPushIncomingNet','_BlackWebPopOutgoingNet','_BlackWebGetStatusJson','_BlackWebNotifyCanvasReady','_BlackWebDrainAudio','_BlackWebGetBuiltinWgsl']"
        "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','FS','IDBFS','HEAPU8','HEAP32','HEAPU32','HEAPF32','UTF8ToString','stringToUTF8','lengthBytesUTF8']"
    )
    return()
endif()

include(${SDK}/cmake/XbeRecomp.cmake)

# Fontes do host (src/common/*.cpp e src/mp/*.cpp).
# /src/game/ é excluído aqui porque add_xbe_image coleta src/${game_dir}/*.cpp diretamente.
# /src/web/ é exclusivo do alvo WebAssembly.
file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/(game|web)/")

# Número de partições gravado pelo xbe_symbols em config/game.toml
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS config/game.toml)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/config/game.toml)
    file(STRINGS config/game.toml GAME_PARTITIONS REGEX "^partitions *=")
    string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${GAME_PARTITIONS}")
endif()
if(NOT GAME_PARTITIONS)
    set(GAME_PARTITIONS 8)
endif()

add_xbe_image(BlackEnhancedOnline default.xbe game.toml ${GAME_PARTITIONS} game)

target_include_directories(BlackEnhancedOnline PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(BlackEnhancedOnline PRIVATE netcore)
