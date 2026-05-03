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
)")
  set(${output_script} "${bridge_script}" PARENT_SCOPE)
endfunction()
