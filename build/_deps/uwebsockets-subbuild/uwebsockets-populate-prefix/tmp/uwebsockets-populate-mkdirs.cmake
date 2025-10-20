# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/bengamble/FasterAPI/build/cpm-cache/uwebsockets/2d18d741850ab50ce1ba644223c67d92f4b33d01")
  file(MAKE_DIRECTORY "/Users/bengamble/FasterAPI/build/cpm-cache/uwebsockets/2d18d741850ab50ce1ba644223c67d92f4b33d01")
endif()
file(MAKE_DIRECTORY
  "/Users/bengamble/FasterAPI/build/_deps/uwebsockets-build"
  "/Users/bengamble/FasterAPI/build/_deps/uwebsockets-subbuild/uwebsockets-populate-prefix"
  "/Users/bengamble/FasterAPI/build/_deps/uwebsockets-subbuild/uwebsockets-populate-prefix/tmp"
  "/Users/bengamble/FasterAPI/build/_deps/uwebsockets-subbuild/uwebsockets-populate-prefix/src/uwebsockets-populate-stamp"
  "/Users/bengamble/FasterAPI/build/_deps/uwebsockets-subbuild/uwebsockets-populate-prefix/src"
  "/Users/bengamble/FasterAPI/build/_deps/uwebsockets-subbuild/uwebsockets-populate-prefix/src/uwebsockets-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/bengamble/FasterAPI/build/_deps/uwebsockets-subbuild/uwebsockets-populate-prefix/src/uwebsockets-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/bengamble/FasterAPI/build/_deps/uwebsockets-subbuild/uwebsockets-populate-prefix/src/uwebsockets-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
