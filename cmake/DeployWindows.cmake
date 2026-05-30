function(bridge_windows_deploy_script target openssl_root output_script)
  # Write separate quoted arguments: older Qt's QML convenience wrapper inserts
  # a semicolon list, which does not preserve quotes around SDK paths correctly.
  qt_generate_deploy_script(TARGET ${target} OUTPUT_SCRIPT bridge_script CONTENT "
qt_deploy_qml_imports(TARGET ${target} PLUGINS_FOUND bridge_plugins)
qt_deploy_runtime_dependencies(
  EXECUTABLE \"$<TARGET_FILE:${target}>\"
  ADDITIONAL_MODULES \${bridge_plugins}
  GENERATE_QT_CONF
  DEPLOY_TOOL_OPTIONS
    --openssl-root \"${openssl_root}\"
    --skip-plugin-types qmltooling
)
# windeployqt collects Qt, but does not deploy its non-Qt SDK dependencies.
file(GLOB_RECURSE bridge_staged_plugins \"\${QT_DEPLOY_PREFIX}/Qt6/*.dll\")
file(GET_RUNTIME_DEPENDENCIES
  EXECUTABLES \"\${QT_DEPLOY_PREFIX}/bin/$<TARGET_FILE_NAME:${target}>\"
  MODULES \${bridge_staged_plugins}
  DIRECTORIES \"${openssl_root}/bin\"
  PRE_EXCLUDE_REGEXES \"^api-ms-\" \"^ext-ms-\"
  RESOLVED_DEPENDENCIES_VAR bridge_runtime_dlls
  UNRESOLVED_DEPENDENCIES_VAR bridge_unresolved_dlls
)
if(bridge_unresolved_dlls)
  message(FATAL_ERROR \"Unresolved Windows runtime dependencies: \${bridge_unresolved_dlls}\")
endif()
if(NOT bridge_runtime_dlls)
  message(FATAL_ERROR \"The Windows runtime dependency scan returned no DLLs\")
endif()
list(LENGTH bridge_runtime_dlls bridge_runtime_count)
message(STATUS \"Checking \${bridge_runtime_count} Windows runtime dependencies\")
file(REAL_PATH \"${openssl_root}/bin\" bridge_sdk_bin)
file(REAL_PATH \"\${QT_DEPLOY_PREFIX}\" bridge_stage_root)
file(TO_CMAKE_PATH \"\$ENV{SystemRoot}\" bridge_system_root)
if(NOT bridge_system_root)
  message(FATAL_ERROR \"The Windows system root is unavailable\")
endif()
foreach(bridge_root bridge_sdk_bin bridge_stage_root bridge_system_root)
  string(TOLOWER \"\${\${bridge_root}}\" \${bridge_root})
endforeach()
foreach(bridge_dll IN LISTS bridge_runtime_dlls)
  file(REAL_PATH \"\${bridge_dll}\" bridge_dll_path)
  string(TOLOWER \"\${bridge_dll_path}\" bridge_dll_path)
  cmake_path(IS_PREFIX bridge_sdk_bin \"\${bridge_dll_path}\" NORMALIZE bridge_sdk_dependency)
  cmake_path(IS_PREFIX bridge_stage_root \"\${bridge_dll_path}\" NORMALIZE bridge_staged_dependency)
  cmake_path(IS_PREFIX bridge_system_root \"\${bridge_dll_path}\" NORMALIZE bridge_system_dependency)
  if(bridge_sdk_dependency OR bridge_staged_dependency)
    get_filename_component(bridge_dll_directory \"\${bridge_dll_path}\" DIRECTORY)
    if(NOT bridge_dll_directory STREQUAL \"\${bridge_stage_root}/bin\")
      file(INSTALL DESTINATION \"\${QT_DEPLOY_PREFIX}/bin\"
        TYPE SHARED_LIBRARY FILES \"\${bridge_dll}\")
    endif()
  elseif(NOT bridge_system_dependency)
    message(FATAL_ERROR \"Runtime dependency is outside the SDK, stage and Windows directories: \${bridge_dll}\")
  endif()
endforeach()
")
  set(${output_script} "${bridge_script}" PARENT_SCOPE)
endfunction()
