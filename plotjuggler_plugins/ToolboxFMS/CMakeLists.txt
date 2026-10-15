include_directories(../)

add_library(ToolboxFMS SHARED toolbox_fms.h toolbox_fms.cpp fms_browser_widget.h
                              fms_browser_widget.cpp)

target_link_libraries(ToolboxFMS PRIVATE Qt5::Widgets Qt5::Network Qt5::Xml
                                         plotjuggler_base)

target_compile_definitions(ToolboxFMS PRIVATE QT_PLUGIN)

# the winch log comes zipped
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
  target_sources(ToolboxFMS PRIVATE winch_log.h winch_log.cpp)
  target_link_libraries(ToolboxFMS PRIVATE ZLIB::ZLIB)
  target_compile_definitions(ToolboxFMS PRIVATE FMS_WINCH_LOG)
else()
  message(STATUS "[ToolboxFMS] zlib not found: winch logs will not be loaded")
endif()

install(TARGETS ToolboxFMS DESTINATION ${PJ_PLUGIN_INSTALL_DIRECTORY})
