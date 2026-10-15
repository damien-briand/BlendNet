add_library(BlendNet-server STATIC RenderServer.cpp RenderServer.h)
add_library(BlendNet::server ALIAS BlendNet-server)
target_include_directories(BlendNet-server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(BlendNet-server PUBLIC BlendNet-core)
