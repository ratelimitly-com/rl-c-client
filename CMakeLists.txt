cmake_minimum_required(VERSION 3.20)

set(RCLIENT_VERSION "0.0.0" CACHE STRING
    "rl-c-client version used for untagged builds")
if(NOT RCLIENT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR
        "RCLIENT_VERSION must be a numeric MAJOR.MINOR.PATCH version")
endif()

project(rl-c-client VERSION "${RCLIENT_VERSION}" LANGUAGES C)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(RCLIENT_BUILD_STATIC "Build the static rl-c-client library" ON)
option(RCLIENT_BUILD_SHARED "Build the shared rl-c-client library" ON)
option(RCLIENT_BUILD_TESTS "Build rl-c-client tests" OFF)
option(RCLIENT_BUILD_EXAMPLES "Build rl-c-client examples" OFF)
option(RCLIENT_ENABLE_INSTALL "Enable rl-c-client install rules"
       "${PROJECT_IS_TOP_LEVEL}")
option(RCLIENT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(RCLIENT_PACKAGE_FORMAT "" CACHE STRING
    "Native package profile (debian13 or ubuntu24.04)")

if(NOT RCLIENT_BUILD_STATIC AND NOT RCLIENT_BUILD_SHARED)
    message(FATAL_ERROR
        "At least one of RCLIENT_BUILD_STATIC or RCLIENT_BUILD_SHARED must be ON")
endif()

function(rclient_read_source_manifest manifest output_variable)
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/manifest/${manifest}.sources"
         manifest_lines)
    set(manifest_sources)
    foreach(manifest_line IN LISTS manifest_lines)
        string(STRIP "${manifest_line}" manifest_line)
        if(manifest_line STREQUAL "" OR manifest_line MATCHES "^#")
            continue()
        endif()
        if(IS_ABSOLUTE "${manifest_line}" OR manifest_line MATCHES "\\.\\.")
            message(FATAL_ERROR
                "Source manifest entry must be repository-relative: "
                "${manifest_line}")
        endif()
        list(APPEND manifest_sources
             "${CMAKE_CURRENT_SOURCE_DIR}/${manifest_line}")
    endforeach()
    if(NOT manifest_sources)
        message(FATAL_ERROR "Source manifest ${manifest}.sources is empty")
    endif()
    set("${output_variable}" "${manifest_sources}" PARENT_SCOPE)
endfunction()

rclient_read_source_manifest(core RCLIENT_CORE_SOURCES)
rclient_read_source_manifest(workflow RCLIENT_WORKFLOW_SOURCES)
rclient_read_source_manifest(runtime RCLIENT_RUNTIME_SOURCES)
set(RCLIENT_EMBED_SOURCES
    ${RCLIENT_CORE_SOURCES}
    ${RCLIENT_WORKFLOW_SOURCES}
    ${RCLIENT_RUNTIME_SOURCES})

find_package(OpenSSL REQUIRED COMPONENTS Crypto)
if(NOT WIN32)
    find_package(Threads REQUIRED)
endif()

function(rclient_configure_library target linkage)
    target_sources("${target}" PRIVATE ${RCLIENT_EMBED_SOURCES})
    target_compile_features("${target}" PUBLIC c_std_11)
    target_include_directories("${target}"
        PUBLIC
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
            "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src")
    set_target_properties("${target}" PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO
        POSITION_INDEPENDENT_CODE YES)

    if(MSVC)
        target_compile_options("${target}" PRIVATE /W4)
        if(RCLIENT_WARNINGS_AS_ERRORS)
            target_compile_options("${target}" PRIVATE /WX)
        endif()
    else()
        target_compile_options("${target}" PRIVATE -Wall -Wextra)
        if(RCLIENT_WARNINGS_AS_ERRORS)
            target_compile_options("${target}" PRIVATE -Werror)
        endif()
    endif()

    if(linkage STREQUAL "STATIC")
        set(dependency_visibility PUBLIC)
    else()
        set(dependency_visibility PRIVATE)
    endif()
    target_link_libraries("${target}"
        "${dependency_visibility}" OpenSSL::Crypto)
    if(WIN32)
        target_link_libraries("${target}"
            "${dependency_visibility}" ws2_32 dnsapi)
    else()
        target_link_libraries("${target}"
            "${dependency_visibility}"
            Threads::Threads
            resolv)
    endif()
endfunction()

if(RCLIENT_BUILD_SHARED)
    add_library(rclient_shared SHARED)
    rclient_configure_library(rclient_shared SHARED)
    target_compile_definitions(rclient_shared
        PRIVATE RCLIENT_BUILDING_DLL
        PUBLIC RCLIENT_SHARED)
    set_target_properties(rclient_shared PROPERTIES
        OUTPUT_NAME rclient
        EXPORT_NAME shared
        VERSION "${PROJECT_VERSION}"
        SOVERSION "${PROJECT_VERSION_MAJOR}"
        C_VISIBILITY_PRESET hidden)
    add_library(rclient::shared ALIAS rclient_shared)
endif()

if(RCLIENT_BUILD_STATIC)
    add_library(rclient_static STATIC)
    rclient_configure_library(rclient_static STATIC)
    target_compile_definitions(rclient_static PUBLIC RCLIENT_STATIC)
    set_target_properties(rclient_static PROPERTIES
        OUTPUT_NAME rclient
        EXPORT_NAME static)
    if(WIN32)
        set_target_properties(rclient_static PROPERTIES
            OUTPUT_NAME rclient-static)
    endif()
    add_library(rclient::static ALIAS rclient_static)
endif()

add_library(rclient INTERFACE)
set_target_properties(rclient PROPERTIES EXPORT_NAME rclient)
if(RCLIENT_BUILD_SHARED)
    target_link_libraries(rclient INTERFACE rclient_shared)
else()
    target_link_libraries(rclient INTERFACE rclient_static)
endif()
add_library(rclient::rclient ALIAS rclient)

if(RCLIENT_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_executable(rclient-test-public-api tests/test_public_api.c)
    target_compile_features(rclient-test-public-api PRIVATE c_std_11)
    target_link_libraries(rclient-test-public-api PRIVATE rclient)
    add_test(NAME rclient-public-api COMMAND rclient-test-public-api)
endif()

if(RCLIENT_ENABLE_INSTALL)
    set(rclient_export_targets rclient)
    if(RCLIENT_BUILD_SHARED)
        list(APPEND rclient_export_targets rclient_shared)
    endif()
    if(RCLIENT_BUILD_STATIC)
        list(APPEND rclient_export_targets rclient_static)
    endif()

    install(TARGETS ${rclient_export_targets}
        EXPORT rclient-targets
        RUNTIME
            DESTINATION "${CMAKE_INSTALL_BINDIR}"
            COMPONENT Runtime
        LIBRARY
            DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            COMPONENT Runtime
            NAMELINK_COMPONENT Development
        ARCHIVE
            DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            COMPONENT Development
        INCLUDES
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
    install(DIRECTORY include/
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT Development
        FILES_MATCHING PATTERN "*.h")

    if(WIN32)
        set(RCLIENT_PC_PRIVATE_LIBS "-lws2_32 -ldnsapi")
    else()
        set(RCLIENT_PC_PRIVATE_LIBS "-lresolv -pthread")
    endif()
    configure_file(
        cmake/rclient.pc.in
        "${CMAKE_CURRENT_BINARY_DIR}/rclient.pc"
        @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/rclient.pc"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
        COMPONENT Development)

    configure_package_config_file(
        cmake/rclient-config.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/rclient-config.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/rclient")
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/rclient-config-version.cmake"
        VERSION "${PROJECT_VERSION}"
        COMPATIBILITY SameMajorVersion)
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/rclient-config.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/rclient-config-version.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/rclient"
        COMPONENT Development)
    install(EXPORT rclient-targets
        FILE rclient-targets.cmake
        NAMESPACE rclient::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/rclient"
        COMPONENT Development)
    install(FILES LICENSE
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/rl-c-client"
        COMPONENT Runtime)
endif()

if(RCLIENT_PACKAGE_FORMAT)
    include(cmake/rclient-packaging.cmake)
endif()
