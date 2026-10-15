include_directories(../)

qt5_add_resources(RESOURCES_SRC resources.qrc)

add_library(Publisher3DReplay SHARED publisher_3d_replay.cpp ${RESOURCES_SRC})

target_link_libraries(Publisher3DReplay PRIVATE Qt5::Widgets Qt5::Network Qt5::WebSockets
                                                plotjuggler_base)

target_compile_definitions(Publisher3DReplay PRIVATE QT_PLUGIN)

install(TARGETS Publisher3DReplay DESTINATION ${PJ_PLUGIN_INSTALL_DIRECTORY})
