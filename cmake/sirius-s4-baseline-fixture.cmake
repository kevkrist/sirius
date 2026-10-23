add_executable(
  sirius_s4_experiment
  test/cpp/operator/s4_experiment.cpp test/cpp/operator/s4_experiment_sql.cpp
  src/sirius_extension_entry.cpp test/cpp/utils/sirius_extension_loader.cpp)
target_include_directories(
  sirius_s4_experiment BEFORE
  PRIVATE $<TARGET_PROPERTY:sirius_unittest,INCLUDE_DIRECTORIES>)
target_compile_definitions(
  sirius_s4_experiment
  PRIVATE $<TARGET_PROPERTY:sirius_unittest,COMPILE_DEFINITIONS>
          SIRIUS_S4_BASELINE_SQL=1 SIRIUS_S4_REVISION_ROLE="baseline")
target_link_libraries(
  sirius_s4_experiment PRIVATE sirius_core duckdb_static ZLIB::ZLIB
                               ${SIRIUS_CURL_TARGET} ${CMAKE_DL_LIBS})
sirius_link_duckdb_extensions(sirius_s4_experiment PRIVATE)
target_link_options(sirius_s4_experiment PRIVATE
                    "LINKER:--allow-multiple-definition")
set_target_properties(
  sirius_s4_experiment
  PROPERTIES ENABLE_EXPORTS ON
             CXX_SCAN_FOR_MODULES OFF
             CXX_STANDARD 20
             CXX_STANDARD_REQUIRED ON
             RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test/cpp")
if(VCPKG_BUILD)
  set_target_properties(sirius_s4_experiment PROPERTIES NO_SYSTEM_FROM_IMPORTED
                                                        ON)
  target_include_directories(sirius_s4_experiment BEFORE PRIVATE ${_VCPKG_INC})
endif()
