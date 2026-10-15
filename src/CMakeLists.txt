qt_add_executable(BlendNet main.cpp)
# Un module QML statique doit être lié avec son plugin.
target_link_libraries(BlendNet PRIVATE BlendNet-ui BlendNet-uiplugin Qt6::Gui Qt6::Quick)
install(TARGETS BlendNet RUNTIME DESTINATION bin)
