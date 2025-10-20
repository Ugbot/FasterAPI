# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/bengamble/FasterAPI/build/cpm-cache/nghttp2/5ceb668e7b22e4a113073fa27bc3e293b452cb50")
  file(MAKE_DIRECTORY "/Users/bengamble/FasterAPI/build/cpm-cache/nghttp2/5ceb668e7b22e4a113073fa27bc3e293b452cb50")
endif()
file(MAKE_DIRECTORY
  "/Users/bengamble/FasterAPI/build/_deps/nghttp2-build"
  "/Users/bengamble/FasterAPI/build/_deps/nghttp2-subbuild/nghttp2-populate-prefix"
  "/Users/bengamble/FasterAPI/build/_deps/nghttp2-subbuild/nghttp2-populate-prefix/tmp"
  "/Users/bengamble/FasterAPI/build/_deps/nghttp2-subbuild/nghttp2-populate-prefix/src/nghttp2-populate-stamp"
  "/Users/bengamble/FasterAPI/build/_deps/nghttp2-subbuild/nghttp2-populate-prefix/src"
  "/Users/bengamble/FasterAPI/build/_deps/nghttp2-subbuild/nghttp2-populate-prefix/src/nghttp2-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/bengamble/FasterAPI/build/_deps/nghttp2-subbuild/nghttp2-populate-prefix/src/nghttp2-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/bengamble/FasterAPI/build/_deps/nghttp2-subbuild/nghttp2-populate-prefix/src/nghttp2-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
