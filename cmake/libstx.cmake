include(${CMAKE_CURRENT_LIST_DIR}/CFlags.cmake)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../src)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../3rdparty/pcre)
include_directories(${STX_INCLUDE})
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${STX_LDFLAGS}")
set(CMAKE_CXX_LINK_EXECUTABLE "${CMAKE_CXX_LINK_EXECUTABLE} ${STX_LDFLAGS}")
