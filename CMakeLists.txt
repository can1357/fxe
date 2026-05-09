cmake_minimum_required(VERSION 3.24)
project(fxe VERSION 0.1.0 LANGUAGES C CXX)

# Touch CMAKE_TOOLCHAIN_FILE so passing it via preset/-D doesn't trigger
# CMake's "manually-specified variable not used" warning when vcpkg has
# already cached it.
if(CMAKE_TOOLCHAIN_FILE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(cmake/ProjectOptions.cmake)
include(cmake/deps.cmake)
include(cmake/FxeVendorPackages.cmake)

include(cmake/targets/FxeOs.cmake)
include(cmake/targets/FxeNet.cmake)
include(cmake/targets/FxeRuntime.cmake)
include(cmake/targets/FxeAudio.cmake)
include(cmake/targets/FxeCore.cmake)
include(cmake/targets/FxeWindow.cmake)
include(cmake/targets/FxeDebug.cmake)
include(cmake/targets/FxeJs.cmake)
include(cmake/targets/FxeExamples.cmake)
include(cmake/targets/FxeWebauthn.cmake)
include(cmake/targets/FxeMarkdown.cmake)
include(cmake/targets/FxePack.cmake)
include(cmake/targets/FxeTreesitter.cmake)
include(cmake/targets/FxeTests.cmake)
