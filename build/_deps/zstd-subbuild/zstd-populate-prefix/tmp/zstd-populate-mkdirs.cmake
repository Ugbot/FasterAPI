# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/bengamble/FasterAPI/build/cpm-cache/zstd/5375622b5fd73dea578f509561685c3376dce674")
  file(MAKE_DIRECTORY "/Users/bengamble/FasterAPI/build/cpm-cache/zstd/5375622b5fd73dea578f509561685c3376dce674")
endif()
file(MAKE_DIRECTORY
  "/Users/bengamble/FasterAPI/build/_deps/zstd-build"
  "/Users/bengamble/FasterAPI/build/_deps/zstd-subbuild/zstd-populate-prefix"
  "/Users/bengamble/FasterAPI/build/_deps/zstd-subbuild/zstd-populate-prefix/tmp"
  "/Users/bengamble/FasterAPI/build/_deps/zstd-subbuild/zstd-populate-prefix/src/zstd-populate-stamp"
  "/Users/bengamble/FasterAPI/build/_deps/zstd-subbuild/zstd-populate-prefix/src"
  "/Users/bengamble/FasterAPI/build/_deps/zstd-subbuild/zstd-populate-prefix/src/zstd-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/bengamble/FasterAPI/build/_deps/zstd-subbuild/zstd-populate-prefix/src/zstd-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/bengamble/FasterAPI/build/_deps/zstd-subbuild/zstd-populate-prefix/src/zstd-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
