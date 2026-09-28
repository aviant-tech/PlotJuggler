add_library(DataLoadBinLog SHARED dataload_bin_log.h dataload_bin_log.cpp)

target_link_libraries(DataLoadBinLog PRIVATE Qt5::Widgets Qt5::Xml plotjuggler_base)

target_compile_definitions(DataLoadBinLog PRIVATE QT_PLUGIN)

install(TARGETS DataLoadBinLog DESTINATION ${PJ_PLUGIN_INSTALL_DIRECTORY})
