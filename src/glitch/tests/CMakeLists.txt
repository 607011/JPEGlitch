set(JPEGLITCH_TESTS mt19937 header determinism eoi ff_options)

foreach(test_name ${JPEGLITCH_TESTS})
  add_executable(jpeglitch_test_${test_name} test_${test_name}.c)
  target_link_libraries(jpeglitch_test_${test_name} PRIVATE jpeglitch-glitch-core)
  set_target_properties(jpeglitch_test_${test_name} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
  add_test(NAME ${test_name} COMMAND jpeglitch_test_${test_name})
endforeach()
