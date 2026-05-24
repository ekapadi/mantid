# Skip these tests entirely on Windows
if(WIN32)
  message(STATUS "Skipping LiveData Python ADARA tests on Windows (no fork support)")
  return()
endif()

# The packet_playback module is in `Framework/PythonInterface/mantid/LiveData/ADARA/utils/packet_playback`. We need to
# add that directory to PYTHONPATH for the tests to find it.
set(LIVEDATA_SOURCE_DIR ${CMAKE_SOURCE_DIR}/Framework/PythonInterface/mantid/LiveData)

set(_test_runner ${CMAKE_SOURCE_DIR}/Framework/PythonInterface/test/testhelpers/testrunner.py)
set(_module_dir ${CMAKE_BINARY_DIR}/bin)
set(_python_path
    "${CMAKE_CURRENT_SOURCE_DIR}:${LIVEDATA_SOURCE_DIR}/ADARA/utils/packet_playback:$ENV{PYTHONPATH}:${_module_dir}"
)

add_test(NAME LiveDataTest_AdaraUtilsTest
         COMMAND ${CMAKE_COMMAND} -E chdir "${CMAKE_BINARY_DIR}/bin/Testing" ${Python_EXECUTABLE} ${_test_runner}
                 ${CMAKE_CURRENT_SOURCE_DIR}/test_adara_utils_suite.py
)

set(_test_env
    "PYTHONPATH=${_python_path}"
    "adara_player_conf=${LIVEDATA_SOURCE_DIR}/ADARA/utils/packet_playback/adara_player_conf.yml" "ADARA_PLAYER_LOGS=."
)
if(NOT WITH_ASAN)
  set(_ld_preload "${TBBMALLOC_RUNTIME_LIB}")
  if(LD_PRELOAD)
    set(_ld_preload "${_ld_preload}:$ENV{LD_PRELOAD}")
  endif()
  list(APPEND _test_env "LD_PRELOAD=${_ld_preload}")
endif()

set_tests_properties(
  LiveDataTest_AdaraUtilsTest PROPERTIES ENVIRONMENT "${_test_env}" TIMEOUT ${TESTING_TIMEOUT} LABELS UnitTest
)
