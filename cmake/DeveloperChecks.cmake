# Public developer commands do not depend on the locally excluded scripts/ tree.
find_program(BRIDGE_FORMAT NAMES clang-format-18 clang-format)
if(BRIDGE_FORMAT)
  file(GLOB_RECURSE bridge_format_sources CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/src/*.cpp" "${PROJECT_SOURCE_DIR}/src/*.hpp"
    "${PROJECT_SOURCE_DIR}/include/*.hpp" "${PROJECT_SOURCE_DIR}/tests/*.cpp"
    "${PROJECT_SOURCE_DIR}/tools/*.cpp" "${PROJECT_SOURCE_DIR}/benchmarks/*.cpp")
  add_custom_target(format-check
    COMMAND ${BRIDGE_FORMAT} --dry-run --Werror ${bridge_format_sources}
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} VERBATIM)
  add_custom_target(format
    COMMAND ${BRIDGE_FORMAT} -i ${bridge_format_sources}
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} VERBATIM)
endif()
if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_program(BRIDGE_VALGRIND NAMES valgrind)
  if(BRIDGE_VALGRIND)
    add_custom_target(memcheck)
    foreach(binary bridge_unit_tests bridge_io_tests bridge_integration_tests
        bridge_transfer_tests bridge_worker_tests bridge_model_tests bridge_openssl_probe)
      if(TARGET ${binary})
        add_dependencies(memcheck ${binary})
        add_custom_command(TARGET memcheck POST_BUILD
          COMMAND ${BRIDGE_VALGRIND} --error-exitcode=99 --leak-check=full
            --errors-for-leak-kinds=definite,indirect $<TARGET_FILE:${binary}>
          VERBATIM)
      endif()
    endforeach()
    if(TARGET bridge_openssl_probe)
      add_custom_command(TARGET memcheck POST_BUILD
        COMMAND ${BRIDGE_VALGRIND} --error-exitcode=99 --leak-check=full
          --errors-for-leak-kinds=definite,indirect
          $<TARGET_FILE:bridge_openssl_probe> --reject-legacy VERBATIM)
    endif()
    foreach(tool checkpoint session)
      if(TARGET bridge_${tool}_tool)
        add_dependencies(memcheck bridge_${tool}_tool)
        if(tool STREQUAL "checkpoint")
          set(process_test process_checkpoints.py)
        else()
          set(process_test process_pairing.py)
        endif()
        add_custom_command(TARGET memcheck POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E env BRIDGE_VALGRIND=1
            ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/integration/${process_test}
            $<TARGET_FILE:bridge_${tool}_tool> VERBATIM)
      endif()
    endforeach()
  endif()
endif()
