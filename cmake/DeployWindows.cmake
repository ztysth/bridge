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
file(GLOB bridge_sdk_dlls \"${openssl_root}/bin/*.dll\")
file(GLOB bridge_staged_dlls \"\${QT_DEPLOY_PREFIX}/bin/*.dll\")
file(GLOB_RECURSE bridge_staged_plugins \"\${QT_DEPLOY_PREFIX}/Qt6/*.dll\")
file(GET_RUNTIME_DEPENDENCIES
  EXECUTABLES \"\${QT_DEPLOY_PREFIX}/bin/$<TARGET_FILE_NAME:${target}>\"
  MODULES \${bridge_staged_plugins}
  DIRECTORIES \"${openssl_root}/bin\"
  PRE_EXCLUDE_REGEXES \"^api-ms-\" \"^ext-ms-\"
  POST_INCLUDE_FILES \${bridge_sdk_dlls} \${bridge_staged_dlls}
  POST_EXCLUDE_REGEXES \".*\"
  RESOLVED_DEPENDENCIES_VAR bridge_runtime_dlls
  UNRESOLVED_DEPENDENCIES_VAR bridge_unresolved_dlls
)
if(bridge_unresolved_dlls)
  message(FATAL_ERROR \"Unresolved Windows runtime dependencies: \${bridge_unresolved_dlls}\")
endif()
foreach(bridge_dll IN LISTS bridge_runtime_dlls)
  get_filename_component(bridge_dll_directory \"\${bridge_dll}\" DIRECTORY)
  if(NOT bridge_dll_directory STREQUAL \"\${QT_DEPLOY_PREFIX}/bin\")
    file(INSTALL DESTINATION \"\${QT_DEPLOY_PREFIX}/bin\"
      TYPE SHARED_LIBRARY FILES \"\${bridge_dll}\")
  endif()
endforeach()
")
  set(${output_script} "${bridge_script}" PARENT_SCOPE)
endfunction()
