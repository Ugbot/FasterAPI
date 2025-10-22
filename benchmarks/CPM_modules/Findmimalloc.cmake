include("/Users/bengamble/FasterAPI/benchmarks/cmake/CPM_0.38.7.cmake")
CPMAddPackage("NAME;mimalloc;GITHUB_REPOSITORY;microsoft/mimalloc;GIT_TAG;v2.1.7;OPTIONS;MI_BUILD_TESTS OFF;MI_BUILD_SHARED ON;MI_BUILD_STATIC ON")
set(mimalloc_FOUND TRUE)