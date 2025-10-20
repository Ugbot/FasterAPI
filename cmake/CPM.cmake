# CPM.cmake - CMake Package Manager
# https://github.com/cpm-cmake/CPM.cmake
# Minimal bootstrap version

if(NOT CPM_SOURCE_CACHE)
  set(CPM_SOURCE_CACHE "${CMAKE_BINARY_DIR}/cpm-cache" CACHE STRING "CPM cache directory")
endif()

set(CPM_DOWNLOAD_VERSION 0.38.7)

if(NOT (EXISTS "${CMAKE_BINARY_DIR}/cmake/CPM_${CPM_DOWNLOAD_VERSION}.cmake"))
  message(STATUS "Downloading CPM.cmake v${CPM_DOWNLOAD_VERSION}")
  file(DOWNLOAD
    "https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_DOWNLOAD_VERSION}/CPM.cmake"
    "${CMAKE_BINARY_DIR}/cmake/CPM_${CPM_DOWNLOAD_VERSION}.cmake"
    TLS_VERIFY ON
  )
endif()

include("${CMAKE_BINARY_DIR}/cmake/CPM_${CPM_DOWNLOAD_VERSION}.cmake")

