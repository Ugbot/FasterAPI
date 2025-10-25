include("/Users/bengamble/FasterAPI/cmake/CPM_0.38.7.cmake")
CPMAddPackage("NAME;libuv;GITHUB_REPOSITORY;libuv/libuv;GIT_TAG;v1.49.2;OPTIONS;LIBUV_BUILD_TESTS OFF")
set(libuv_FOUND TRUE)