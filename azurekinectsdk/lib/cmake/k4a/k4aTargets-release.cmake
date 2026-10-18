#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "k4a::k4a" for configuration "Release"
set_property(TARGET k4a::k4a APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(k4a::k4a PROPERTIES
  IMPORTED_IMPLIB_RELEASE "${_IMPORT_PREFIX}/lib/k4a.lib"
  IMPORTED_LINK_DEPENDENT_LIBRARIES_RELEASE "OrbbecSDK::OrbbecSDK"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/bin/k4a.dll"
  )

list(APPEND _IMPORT_CHECK_TARGETS k4a::k4a )
list(APPEND _IMPORT_CHECK_FILES_FOR_k4a::k4a "${_IMPORT_PREFIX}/lib/k4a.lib" "${_IMPORT_PREFIX}/bin/k4a.dll" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
