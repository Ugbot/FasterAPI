# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/bengamble/FasterAPI/build/cpm-cache/libuv/0fdf53e314c00b21e3a18653959403e1fdb56ee9")
  file(MAKE_DIRECTORY "/Users/bengamble/FasterAPI/build/cpm-cache/libuv/0fdf53e314c00b21e3a18653959403e1fdb56ee9")
endif()
file(MAKE_DIRECTORY
  "/Users/bengamble/FasterAPI/build/_deps/libuv-build"
  "/Users/bengamble/FasterAPI/build/_deps/libuv-subbuild/libuv-populate-prefix"
  "/Users/bengamble/FasterAPI/build/_deps/libuv-subbuild/libuv-populate-prefix/tmp"
  "/Users/bengamble/FasterAPI/build/_deps/libuv-subbuild/libuv-populate-prefix/src/libuv-populate-stamp"
  "/Users/bengamble/FasterAPI/build/_deps/libuv-subbuild/libuv-populate-prefix/src"
  "/Users/bengamble/FasterAPI/build/_deps/libuv-subbuild/libuv-populate-prefix/src/libuv-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/bengamble/FasterAPI/build/_deps/libuv-subbuild/libuv-populate-prefix/src/libuv-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/bengamble/FasterAPI/build/_deps/libuv-subbuild/libuv-populate-prefix/src/libuv-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
