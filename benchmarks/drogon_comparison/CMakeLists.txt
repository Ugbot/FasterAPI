cmake_minimum_required(VERSION 3.16)
project(drogon_benchmark)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release)

# Optimization flags
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native")

find_package(Drogon CONFIG REQUIRED)

add_executable(drogon_benchmark drogon_benchmark.cpp)
target_link_libraries(drogon_benchmark PRIVATE Drogon::Drogon)
