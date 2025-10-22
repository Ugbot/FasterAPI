include("/Users/bengamble/FasterAPI/benchmarks/cmake/CPM_0.38.7.cmake")
CPMAddPackage("NAME;zstd;GITHUB_REPOSITORY;facebook/zstd;GIT_TAG;v1.5.6;OPTIONS;ZSTD_BUILD_PROGRAMS OFF;ZSTD_BUILD_TESTS OFF")
set(zstd_FOUND TRUE)