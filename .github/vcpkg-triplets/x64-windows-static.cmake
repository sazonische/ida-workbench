# CI override of vcpkg's built-in x64-windows-static triplet. Identical to it except
# for VCPKG_BUILD_TYPE: the debug half of qtbase is never packaged, and skipping it
# halves a build that is otherwise measured in hours. The name is kept so CMakeLists
# still recognises the static triplet (and switches to the static CRT) unchanged.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_BUILD_TYPE release)
