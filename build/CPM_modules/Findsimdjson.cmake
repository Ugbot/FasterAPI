include("/Users/bengamble/FasterAPI/build/cmake/CPM_0.38.7.cmake")
CPMAddPackage("NAME;simdjson;GITHUB_REPOSITORY;simdjson/simdjson;GIT_TAG;v3.10.1;OPTIONS;SIMDJSON_BUILD_STATIC ON")
set(simdjson_FOUND TRUE)