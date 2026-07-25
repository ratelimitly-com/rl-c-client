get_filename_component(
    RCLIENT_EMBED_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

function(rclient_embed_read_manifest manifest output_variable)
    file(STRINGS
        "${RCLIENT_EMBED_ROOT}/manifest/${manifest}.sources"
        manifest_lines)
    set(manifest_sources)
    foreach(manifest_line IN LISTS manifest_lines)
        string(STRIP "${manifest_line}" manifest_line)
        if(manifest_line STREQUAL "" OR manifest_line MATCHES "^#")
            continue()
        endif()
        if(IS_ABSOLUTE "${manifest_line}" OR manifest_line MATCHES "\\.\\.")
            message(FATAL_ERROR
                "Source manifest entry must be bundle-relative: "
                "${manifest_line}")
        endif()
        list(APPEND manifest_sources
            "${RCLIENT_EMBED_ROOT}/${manifest_line}")
    endforeach()
    if(NOT manifest_sources)
        message(FATAL_ERROR "Source manifest ${manifest}.sources is empty")
    endif()
    set("${output_variable}" "${manifest_sources}" PARENT_SCOPE)
endfunction()

rclient_embed_read_manifest(core RCLIENT_EMBED_CORE_SOURCES)
rclient_embed_read_manifest(workflow RCLIENT_EMBED_WORKFLOW_SOURCES)
rclient_embed_read_manifest(runtime RCLIENT_EMBED_RUNTIME_SOURCES)

set(RCLIENT_EMBED_SOURCES
    ${RCLIENT_EMBED_CORE_SOURCES}
    ${RCLIENT_EMBED_WORKFLOW_SOURCES}
    ${RCLIENT_EMBED_RUNTIME_SOURCES})
set(RCLIENT_EMBED_INCLUDE_DIRS
    "${RCLIENT_EMBED_ROOT}/include"
    "${RCLIENT_EMBED_ROOT}/src")

find_package(OpenSSL REQUIRED COMPONENTS Crypto)
if(WIN32)
    set(RCLIENT_EMBED_LIBRARIES OpenSSL::Crypto ws2_32 dnsapi)
else()
    find_package(Threads REQUIRED)
    set(RCLIENT_EMBED_LIBRARIES OpenSSL::Crypto Threads::Threads resolv)
endif()
