#
# CMake package configuration file for wangle
#
# Defines the target "wangle::wangle"
# Add this to your target_link_libraries() call to depend on wangle.
#
# Also sets the variables WANGLE_INCLUDE_DIR and WANGLE_LIBRARIES.
# However, in most cases using the wangle::wangle target is sufficient,
# and you won't need these variables.
#


####### Expanded from @PACKAGE_INIT@ by configure_package_config_file() #######
####### Any changes to this file will be overwritten by the next CMake run ####
####### The input file was wangle-config.cmake.in                            ########

get_filename_component(PACKAGE_PREFIX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../" ABSOLUTE)

macro(set_and_check _var _file)
  set(${_var} "${_file}")
  if(NOT EXISTS "${_file}")
    message(FATAL_ERROR "File or directory ${_file} referenced by variable ${_var} does not exist !")
  endif()
endmacro()

macro(check_required_components _NAME)
  foreach(comp ${${_NAME}_FIND_COMPONENTS})
    if(NOT ${_NAME}_${comp}_FOUND)
      if(${_NAME}_FIND_REQUIRED_${comp})
        set(${_NAME}_FOUND FALSE)
      endif()
    endif()
  endforeach()
endmacro()

####################################################################################

set_and_check(WANGLE_INCLUDE_DIR "${PACKAGE_PREFIX_DIR}/include")
set_and_check(WANGLE_CMAKE_DIR "${PACKAGE_PREFIX_DIR}/lib/cmake/wangle")

if (NOT TARGET wangle::wangle)
  include("${WANGLE_CMAKE_DIR}/wangle-targets.cmake")
endif()

set(WANGLE_LIBRARIES wangle::wangle)

if (NOT wangle_FIND_QUIETLY)
  message(STATUS "Found wangle: ${PACKAGE_PREFIX_DIR}")
endif()
