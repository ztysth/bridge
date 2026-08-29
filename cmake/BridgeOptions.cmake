function(bridge_options target)
  if(MSVC)
    if(BRIDGE_SANITIZER)
      if(NOT BRIDGE_SANITIZER STREQUAL "address" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8
          OR CMAKE_SYSTEM_PROCESSOR MATCHES "[Aa][Rr][Mm]64|aarch64")
        message(FATAL_ERROR "Native MSVC sanitizers require x64 AddressSanitizer; ARM64/UBSan/TSan use supported native toolchains separately.")
      endif()
      target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
      target_link_options(${target} PUBLIC /INCREMENTAL:NO)
      # The pinned SDK's static Catch2 library has unannotated STL containers.
      # Match its ABI across all consumers; ASan still checks allocation bounds,
      # lifetimes and stacks, but not unused vector/string capacity.
      target_compile_definitions(${target} PUBLIC
        _DISABLE_VECTOR_ANNOTATION _DISABLE_STRING_ANNOTATION)
    endif()
    target_compile_options(${target} PRIVATE /W4 /permissive-)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
      # MSVC ignores /pathmap without deterministic compilation enabled.
      target_compile_options(${target} PRIVATE
        "$<$<CONFIG:Release>:/experimental:deterministic>"
        "$<$<CONFIG:Release>:/pathmap:${PROJECT_SOURCE_DIR}=.>"
        "$<$<CONFIG:Release>:/pathmap:${PROJECT_BINARY_DIR}=./build>")
    else()
      target_compile_options(${target} PRIVATE
        "$<$<CONFIG:Release>:/clang:-ffile-prefix-map=${PROJECT_SOURCE_DIR}=.>"
        "$<$<CONFIG:Release>:/clang:-ffile-prefix-map=${PROJECT_BINARY_DIR}=./build>")
    endif()
    if(BRIDGE_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    target_compile_options(${target} PRIVATE
      "$<$<CONFIG:Release>:-ffile-prefix-map=${PROJECT_SOURCE_DIR}=.>"
      "$<$<CONFIG:Release>:-ffile-prefix-map=${PROJECT_BINARY_DIR}=./build>")
    if(BRIDGE_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
    if(BRIDGE_SANITIZER)
      if(NOT BRIDGE_SANITIZER MATCHES "^(address|undefined|thread)$")
        message(FATAL_ERROR "Unsupported sanitizer")
      endif()
      target_compile_options(${target} PRIVATE -fsanitize=${BRIDGE_SANITIZER} -fno-omit-frame-pointer -fno-sanitize-recover=all)
      target_link_options(${target} PUBLIC -fsanitize=${BRIDGE_SANITIZER})
    endif()
  endif()
  if(BRIDGE_CLANG_TIDY)
    find_program(BRIDGE_TIDY NAMES clang-tidy-19 clang-tidy-20 clang-tidy REQUIRED)
    set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${BRIDGE_TIDY};--warnings-as-errors=*")
  endif()
endfunction()
