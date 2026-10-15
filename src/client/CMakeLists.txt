add_library(BlendNet-client STATIC SourceConnection.cpp SourceConnection.h)
add_library(BlendNet::client ALIAS BlendNet-client)
target_include_directories(BlendNet-client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(BlendNet-client PUBLIC BlendNet-core)
