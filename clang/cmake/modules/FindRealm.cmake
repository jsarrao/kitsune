# 
#  REALM_FOUND       -- realm was found.
#  REALM_INCLUDE_DIR -- directory with realm header files. 
#  REALM_LIBRARY     -- full path to the realm library. 
#  REALM_LIBRARY_DIR -- path to where the realm library is installed. 
#  REALM_WRAPPER_LIBRARY_DIR -- path to where the kitsune-rt realm wrapper library is installed. 
#  REALM_LINK_LIBS   -- set of link libraries (e.g. -lrealm) 
# 

message(STATUS "kitsune: looking for realm...")

find_path(REALM_INCLUDE_DIR  realm.h)
find_library(REALM_LIBRARY realm)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Realm DEFAULT_MSG
                                  REALM_INCLUDE_DIR 
                                  REALM_LIBRARY)

if (Realm_FOUND) 
  message(STATUS "kitsune: looking for realm... FOUND")
  get_filename_component(REALM_LIBRARY_DIR
                         ${REALM_LIBRARY}
                         DIRECTORY
                         CACHE)
  set(REALM_LINK_LIBS -lrealm CACHE STRING "List of libraries need to link with for Realm.")

  message(STATUS "kitsune: looking for kitsune-rt's realm wrapper...")
  
  #find_package(kitsunerealm REQUIRED) # kitsune-rt dependency, if externally installed prior to clang build
  set(REALM_WRAPPER_LIBRARY_DIR ${CMAKE_INSTALL_PREFIX}/lib CACHE STRING "directory where the kitsune-rt wrapper library for realm is installed")

  find_package_handle_standard_args(Realm_Wrapper DEFAULT_MSG
                                    REALM_WRAPPER_LIBRARY_DIR) 

  if (Realm_Wrapper_FOUND)
     message(STATUS "kitsune: looking for kitsune-rt's realm wrapper... FOUND")
     set(KITSUNE_ENABLE_REALM TRUE CACHE BOOL "Enable automatic include and library flags for Realm.")
     set(REALM_LINK_LIBS "-lrealm -lkitsunerealm -lhwloc -lnuma -lpthread" CACHE STRING "List of libraries need to link with for Realm." FORCE)
  else()
    message(STATUS "kitsune: looking for kitsune-rt's realm wrapper... NOT FOUND")
    set(KITSUNE_ENABLE_REALM FALSE CACHE BOOL "Enable automatic include and library flags for Realm.")
    set(REALM_LINK_LIBS "" CACHE STRING "List of libraries need to link with for Realm.")
  endif()

else()
  message(STATUS "kitsune: looking for realm... NOT FOUND")
  set(KITSUNE_ENABLE_REALM FALSE CACHE BOOL "Enable automatic include and library flags for Realm.")
  set(REALM_LINK_LIBS "" CACHE STRING "List of libraries need to link with for Realm.")
endif()

#mark_as_advanced(REALM_INCLUDE_DIR REALM_LIBRARY REALM_LIBRARY_DIR Realm_FOUND)
