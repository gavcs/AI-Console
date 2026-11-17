cmake_minimum_required(VERSION 3.16)
project(ai_console LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

file(GLOB SOURCES CONFIGURE_DEPENDS src/*.cpp)
file(GLOB HEADERS CONFIGURE_DEPENDS include/*.h)

add_executable(ai_console ${SOURCES} ${HEADERS})
target_include_directories(ai_console PRIVATE include)
target_link_libraries(ai_console PRIVATE Qt6::Widgets Qt6::Network)
