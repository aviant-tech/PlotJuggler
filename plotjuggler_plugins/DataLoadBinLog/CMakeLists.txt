add_library(DataLoadBinLog SHARED dataload_bin_log.h dataload_bin_log.cpp bin_log_decoder.h
                                  bin_log_decoder.cpp)

target_link_libraries(DataLoadBinLog PRIVATE Qt5::Widgets Qt5::Xml plotjuggler_base)

target_compile_definitions(DataLoadBinLog PRIVATE QT_PLUGIN)

install(TARGETS DataLoadBinLog DESTINATION ${PJ_PLUGIN_INSTALL_DIRECTORY})

# Manual check, not a registered test: dumps a .log as CSV files that
# tests/compare_with_vendor.py diffs against the vendor parser's output.
if(BUILD_TESTING)
  add_executable(bin_log_dump_csv tests/dump_csv.cpp bin_log_decoder.cpp)
  target_include_directories(bin_log_dump_csv PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()
