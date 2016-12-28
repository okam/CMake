cmake_minimum_required(VERSION 3.3)
enable_language(CXX)

set(CMAKE_XCODE_ATTRIBUTE_SUPPORTED_PLATFORMS "macosx iphonesimulator")
set(CMAKE_MACOSX_BUNDLE true)

add_library(library STATIC foo.cpp)

add_executable(main main.cpp)
target_link_libraries(main library)

install(TARGETS library ARCHIVE DESTINATION lib)
