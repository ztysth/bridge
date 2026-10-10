# vcpkg supplies the upstream config. Ubuntu's pinned SDK supplies headers/library.
find_package(utf8proc 2.9 CONFIG QUIET)
if(NOT TARGET utf8proc::utf8proc)
  find_path(BRIDGE_UTF8PROC_INCLUDE_DIR utf8proc.h REQUIRED)
  find_library(BRIDGE_UTF8PROC_LIBRARY NAMES utf8proc REQUIRED)
  add_library(utf8proc::utf8proc UNKNOWN IMPORTED)
  set_target_properties(utf8proc::utf8proc PROPERTIES
    IMPORTED_LOCATION "${BRIDGE_UTF8PROC_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${BRIDGE_UTF8PROC_INCLUDE_DIR}")
endif()
