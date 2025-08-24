cmake_minimum_required(VERSION 3.16)
project(centrifugo-cpp VERSION 0.2.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FORCE_COLORED_OUTPUT
       "Always produce ANSI-colored output (GNU/Clang only)." FALSE)

if(${FORCE_COLORED_OUTPUT})
  if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    add_compile_options(-fdiagnostics-color=always)
  elseif("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
    add_compile_options(-fcolor-diagnostics)
  endif()
endif()

# Find required packages
find_package(Boost REQUIRED CONFIG COMPONENTS system url)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json REQUIRED)

# Create the library target
file(GLOB_RECURSE SOURCES "src/*.cpp" "include/*.h")

add_library(centrifugo-cpp ${SOURCES})

# Set include directories for the library
target_include_directories(
  centrifugo-cpp
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Link libraries
target_link_libraries(centrifugo-cpp Boost::system Boost::url OpenSSL::SSL
                      OpenSSL::Crypto nlohmann_json::nlohmann_json)

# Compiler options
target_compile_options(centrifugo-cpp PRIVATE -Wall -Wextra)

# ==== EXAMPLES ====

if(BUILD_EXAMPLES)
  file(GLOB EXAMPLE_SOURCES "examples/*.cpp")
  foreach(EXAMPLE_SOURCE ${EXAMPLE_SOURCES})
    get_filename_component(EXAMPLE_NAME ${EXAMPLE_SOURCE} NAME_WE)
    add_executable(${EXAMPLE_NAME} ${EXAMPLE_SOURCE})
    target_link_libraries(${EXAMPLE_NAME} centrifugo-cpp)
  endforeach()
endif()

# ==== INSTALLATION ====

install(
  TARGETS centrifugo-cpp
  EXPORT centrifugo-cpp-targets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin
  INCLUDES
  DESTINATION include)

install(DIRECTORY include/ DESTINATION include)

install(
  EXPORT centrifugo-cpp-targets
  FILE centrifugo-cpp-config.cmake
  DESTINATION lib/cmake/centrifugo-cpp)
