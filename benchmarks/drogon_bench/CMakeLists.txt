cmake_minimum_required(VERSION 3.16)
project(drogon_bench CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release)

# Use brew-installed Drogon
find_package(Drogon CONFIG REQUIRED)

add_executable(drogon_bench main.cpp)

target_link_libraries(drogon_bench PRIVATE Drogon::Drogon)
