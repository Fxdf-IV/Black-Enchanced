# Black Enhanced Online — Recompilação estática (Xbox -> PC / WebAssembly) + Multiplayer

cmake_minimum_required(VERSION 3.25)
project(BlackEnhancedOnline LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# O núcleo de rede (libs/netcore) não depende do motor nem dos arquivos de BLACK.
# Em CI ou em máquinas sem o disco do jogo, use -DBLACK_NETCORE_ONLY=ON para compilar e testar o netcore.
option(BLACK_NETCORE_ONLY "Build only the engine-independent netcore library and unit tests" OFF)

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
include(${SDK}/cmake/XbeRecomp.cmake)

# Fontes do host (src/common/*.cpp e src/mp/*.cpp).
# /src/game/ é excluído aqui porque add_xbe_image coleta src/${game_dir}/*.cpp diretamente.
file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/game/")

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
