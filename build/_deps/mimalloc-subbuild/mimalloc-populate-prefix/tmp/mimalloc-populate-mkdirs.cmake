# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/bengamble/FasterAPI/build/cpm-cache/mimalloc/72f0d2cb40cc97ffde9214409da7eaed994d09ec")
  file(MAKE_DIRECTORY "/Users/bengamble/FasterAPI/build/cpm-cache/mimalloc/72f0d2cb40cc97ffde9214409da7eaed994d09ec")
endif()
file(MAKE_DIRECTORY
  "/Users/bengamble/FasterAPI/build/_deps/mimalloc-build"
  "/Users/bengamble/FasterAPI/build/_deps/mimalloc-subbuild/mimalloc-populate-prefix"
  "/Users/bengamble/FasterAPI/build/_deps/mimalloc-subbuild/mimalloc-populate-prefix/tmp"
  "/Users/bengamble/FasterAPI/build/_deps/mimalloc-subbuild/mimalloc-populate-prefix/src/mimalloc-populate-stamp"
  "/Users/bengamble/FasterAPI/build/_deps/mimalloc-subbuild/mimalloc-populate-prefix/src"
  "/Users/bengamble/FasterAPI/build/_deps/mimalloc-subbuild/mimalloc-populate-prefix/src/mimalloc-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/bengamble/FasterAPI/build/_deps/mimalloc-subbuild/mimalloc-populate-prefix/src/mimalloc-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/bengamble/FasterAPI/build/_deps/mimalloc-subbuild/mimalloc-populate-prefix/src/mimalloc-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
