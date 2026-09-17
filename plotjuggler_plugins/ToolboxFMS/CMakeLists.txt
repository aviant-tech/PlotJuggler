include_directories(../)

add_library(ToolboxFMS SHARED toolbox_fms.h toolbox_fms.cpp fms_browser_widget.h
                              fms_browser_widget.cpp)

target_link_libraries(ToolboxFMS PRIVATE Qt5::Widgets Qt5::Network Qt5::Xml
                                         plotjuggler_base)

target_compile_definitions(ToolboxFMS PRIVATE QT_PLUGIN)

install(TARGETS ToolboxFMS DESTINATION ${PJ_PLUGIN_INSTALL_DIRECTORY})
