#  Copyright (c) 2014, Facebook, Inc.
#  All rights reserved.
#
#  This source code is licensed under the BSD-style license found in the
#  LICENSE file in the root directory of this source tree. An additional grant
#  of patent rights can be found in the PATENTS file in the same directory.

cmake_minimum_required(VERSION 3.0.2)

project("wangle" VERSION 1.0.0 LANGUAGES CXX C)

add_compile_options(-std=gnu++14)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake/)

# When installing Folly & Wangle in a non-default prefix, this will let
# projects linking against libwangle.so to find libfolly.so automatically.
SET(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(INCLUDE_INSTALL_DIR include CACHE STRING
    "The subdirectory where header files should be installed")
set(LIB_INSTALL_DIR lib CACHE STRING
    "The subdirectory where libraries should be installed")
set(CMAKE_INSTALL_DIR lib/cmake/wangle CACHE STRING
    "The subdirectory where CMake package config files should be installed")

# Try finding folly via its installed CMake configuration file first.
# This should work if folly was built with CMake.
find_package(folly CONFIG)
if (NOT folly_FOUND)
  # Look for folly using our FindFolly.cmake module.  This may work if the
  # folly was built with its older autoconf build files rather than with CMake.
  find_package(Folly MODULE REQUIRED)
endif()

find_package(Boost REQUIRED COMPONENTS system thread filesystem regex)
find_package(OpenSSL REQUIRED)
find_package(Glog REQUIRED)
find_package(Gflags REQUIRED)
find_package(Libevent REQUIRED)
find_package(DoubleConversion REQUIRED)
find_package(Threads REQUIRED)
if (UNIX AND NOT APPLE)
  find_package(Librt)
endif()

include(CheckAtomic)

set(WANGLE_HEADER_DIRS
  acceptor
  bootstrap
  channel
  client
  codec
  deprecated
  service
  ssl
  util
  chijinxin
)

foreach(dir ${WANGLE_HEADER_DIRS})
  file(GLOB_RECURSE headers ${dir}/*.h)
  set(WANGLE_HEADERS
    ${WANGLE_HEADERS}
    ${headers})
endforeach()

set(WANGLE_SOURCES
  acceptor/Acceptor.cpp
  acceptor/AcceptorHandshakeManager.cpp
  acceptor/ConnectionManager.cpp
  acceptor/EvbHandshakeHelper.cpp
  acceptor/LoadShedConfiguration.cpp
  acceptor/ManagedConnection.cpp
  acceptor/SecureTransportType.cpp
  acceptor/SocketOptions.cpp
  acceptor/SSLAcceptorHandshakeHelper.cpp
  acceptor/TLSPlaintextPeekingCallback.cpp
  acceptor/TransportInfo.cpp
  bootstrap/ServerBootstrap.cpp
  channel/FileRegion.cpp
  channel/Pipeline.cpp
  client/ssl/SSLSessionCacheData.cpp
  client/ssl/SSLSessionCacheUtils.cpp
  client/ssl/SSLSessionCallbacks.cpp
  codec/LengthFieldBasedFrameDecoder.cpp
  codec/LengthFieldPrepender.cpp
  codec/LineBasedFrameDecoder.cpp
  deprecated/rx/Dummy.cpp
  ssl/PasswordInFile.cpp
  ssl/ServerSSLContext.cpp
  ssl/SSLContextManager.cpp
  ssl/SSLSessionCacheManager.cpp
  ssl/SSLUtil.cpp
  ssl/TLSTicketKeyManager.cpp
  ssl/TLSCredProcessor.cpp
  util/FilePoller.cpp
)

add_library(wangle
  ${WANGLE_HEADERS}
  ${WANGLE_SOURCES}
)

include_directories(${PROJECT_SOURCE_DIR})


include(CheckLibraryExists)
check_library_exists(ssl SSL_SESSION_dup "${OPENSSL_SSL_LIBRARY}" WANGLE_HAVE_SSL_SESSION_DUP)
if(WANGLE_HAVE_SSL_SESSION_DUP)
  target_compile_definitions(wangle PRIVATE WANGLE_HAVE_SSL_SESSION_DUP)
endif()

if (BUILD_SHARED_LIBS)
  set_target_properties(wangle
    PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 1)
endif()

target_include_directories(
  wangle
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/..>
    $<INSTALL_INTERFACE:${INCLUDE_INSTALL_DIR}>
    ${FOLLY_INCLUDE_DIR}
    ${Boost_INCLUDE_DIR}
    ${OPENSSL_INCLUDE_DIR}
    ${GLOG_INCLUDE_DIRS}
    ${GFLAGS_INCLUDE_DIRS}
    ${LIBEVENT_INCLUDE_DIRS}
    ${DOUBLE_CONVERSION_INCLUDE_DIRS}
)
target_link_libraries(wangle PUBLIC
  ${FOLLY_LIBRARIES}
  ${Boost_LIBRARIES}
        boost_context
  ${OPENSSL_LIBRARIES}
  ${GLOG_LIBRARIES}
  ${GFLAGS_LIBRARIES}
  ${LIBEVENT_LIBRARIES}
  ${DOUBLE_CONVERSION_LIBRARIES}
  ${CMAKE_DL_LIBS}
  ${LIBRT_LIBRARIES})

install(
  TARGETS wangle
  EXPORT wangle-exports
  DESTINATION ${LIB_INSTALL_DIR}
)
# We unfortunately cannot install wangle's headers with the install()
# statement above.  install(TARGETS) appears to only support installing
# PUBLIC_HEADER in a flat include directory, and not a deeper tree.
foreach(dir ${WANGLE_HEADER_DIRS})
  install(DIRECTORY ${dir} DESTINATION "${INCLUDE_INSTALL_DIR}/wangle"
          FILES_MATCHING PATTERN "*.h")
endforeach()

# Install CMake package configuration files for wangle
include(CMakePackageConfigHelpers)
configure_package_config_file(
  cmake/wangle-config.cmake.in
  wangle-config.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_DIR}
  PATH_VARS
    INCLUDE_INSTALL_DIR
    CMAKE_INSTALL_DIR
)
install(
  FILES ${CMAKE_CURRENT_BINARY_DIR}/wangle-config.cmake
  DESTINATION ${CMAKE_INSTALL_DIR}
)
install(EXPORT wangle-exports
        FILE wangle-targets.cmake
        NAMESPACE wangle::
        DESTINATION ${CMAKE_INSTALL_DIR})

#add_executable(consumer-agent-wangle chijinxin/main.cc chijinxin/server.cc chijinxin/http.cc)
add_executable(ConsumerAgent chijinxin/ConsumerAgent.cpp)
#target_link_libraries(ConsumerAgent wangle etcd-cpp-api cpprest ssl crypto protobuf grpc++)
target_link_libraries(ConsumerAgent wangle)