add_executable(BlendNet-cli cli_main.cpp)
# Règle : cli ne dépend que de core + client.
target_link_libraries(BlendNet-cli PRIVATE BlendNet-core BlendNet-client)
