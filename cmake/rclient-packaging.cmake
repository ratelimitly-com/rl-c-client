if(NOT PROJECT_IS_TOP_LEVEL)
    message(FATAL_ERROR
        "RCLIENT_PACKAGE_FORMAT may only be used for a top-level build")
endif()
if(NOT RCLIENT_ENABLE_INSTALL)
    message(FATAL_ERROR
        "RCLIENT_PACKAGE_FORMAT requires RCLIENT_ENABLE_INSTALL=ON")
endif()

set(CPACK_PACKAGE_NAME "rl-c-client")
set(CPACK_PACKAGE_VENDOR "Ratelimitly")
set(CPACK_PACKAGE_CONTACT "Ratelimitly <opensource@ratelimitly.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Portable C client for the Ratelimitly protocol")
set(CPACK_PACKAGE_HOMEPAGE_URL
    "https://github.com/ratelimitly-com/rl-c-client")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CHECKSUM SHA256)
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/packages")
set(CPACK_STRIP_FILES YES)
set(CPACK_COMPONENTS_ALL Runtime Development)
set(CPACK_COMPONENTS_GROUPING IGNORE)
set(CPACK_COMPONENT_RUNTIME_DISPLAY_NAME "rl-c-client runtime")
set(CPACK_COMPONENT_DEVELOPMENT_DISPLAY_NAME
    "rl-c-client development files")

if(RCLIENT_PACKAGE_FORMAT STREQUAL "debian13"
   OR RCLIENT_PACKAGE_FORMAT STREQUAL "ubuntu24.04")
    set(CPACK_GENERATOR DEB)
    set(CPACK_DEB_COMPONENT_INSTALL ON)
    set(CPACK_DEBIAN_ENABLE_COMPONENT_DEPENDS ON)
    set(CPACK_DEBIAN_PACKAGE_RELEASE 1)
    set(CPACK_DEBIAN_RUNTIME_PACKAGE_NAME
        "librclient${PROJECT_VERSION_MAJOR}")
    set(CPACK_DEBIAN_RUNTIME_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_RUNTIME_PACKAGE_SECTION libs)
    set(CPACK_DEBIAN_RUNTIME_PACKAGE_DEPENDS
        "libc6 (>= 2.34), libssl3t64 (>= 3.0)")
    set(CPACK_DEBIAN_RUNTIME_DESCRIPTION
        "Shared runtime library for the Ratelimitly C client")
    set(CPACK_DEBIAN_DEVELOPMENT_PACKAGE_NAME "librclient-dev")
    set(CPACK_DEBIAN_DEVELOPMENT_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_DEVELOPMENT_PACKAGE_SECTION libdevel)
    set(CPACK_DEBIAN_DEVELOPMENT_PACKAGE_DEPENDS
        "librclient${PROJECT_VERSION_MAJOR} (= ${PROJECT_VERSION}-1), libssl-dev, pkg-config")
    set(CPACK_DEBIAN_DEVELOPMENT_DESCRIPTION
        "Headers, static library, and build metadata for rl-c-client")
elseif(RCLIENT_PACKAGE_FORMAT STREQUAL "fedora44")
    set(CPACK_GENERATOR RPM)
    set(CPACK_RPM_COMPONENT_INSTALL ON)
    set(CPACK_RPM_PACKAGE_RELEASE 1)
    set(CPACK_RPM_PACKAGE_LICENSE MIT)
    set(CPACK_RPM_RUNTIME_PACKAGE_NAME "rclient-libs")
    set(CPACK_RPM_RUNTIME_FILE_NAME RPM-DEFAULT)
    set(CPACK_RPM_RUNTIME_PACKAGE_GROUP "System Environment/Libraries")
    set(CPACK_RPM_RUNTIME_PACKAGE_REQUIRES
        "glibc >= 2.34, openssl-libs >= 3.0")
    set(CPACK_RPM_RUNTIME_PACKAGE_SUMMARY
        "Shared runtime library for the Ratelimitly C client")
    set(CPACK_RPM_DEVELOPMENT_PACKAGE_NAME "rclient-devel")
    set(CPACK_RPM_DEVELOPMENT_FILE_NAME RPM-DEFAULT)
    set(CPACK_RPM_DEVELOPMENT_PACKAGE_GROUP "Development/Libraries")
    set(CPACK_RPM_DEVELOPMENT_PACKAGE_REQUIRES
        "rclient-libs = ${PROJECT_VERSION}-1, openssl-devel, pkgconf-pkg-config")
    set(CPACK_RPM_DEVELOPMENT_PACKAGE_SUMMARY
        "Development files for the Ratelimitly C client")
else()
    message(FATAL_ERROR
        "Unsupported RCLIENT_PACKAGE_FORMAT: ${RCLIENT_PACKAGE_FORMAT}")
endif()

include(CPack)
