set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_BUILD_TYPE release)
# The direct TLS probe reproduces an MSVC ARM64 release crash independently of
# Qt. Keep this workaround confined to OpenSSL (upstream issues 26239/27030).
if(PORT STREQUAL "openssl")
  set(VCPKG_C_FLAGS_RELEASE "/Od")
  set(VCPKG_CXX_FLAGS_RELEASE "/Od")
endif()
