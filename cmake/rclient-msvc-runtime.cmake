function(rclient_configure_msvc_runtime target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR
            "rclient_configure_msvc_runtime target does not exist: ${target}")
    endif()
    if(MSVC AND RCLIENT_USE_STATIC_MSVC_RUNTIME)
        set_property(TARGET "${target}" PROPERTY
            MSVC_RUNTIME_LIBRARY
            "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endfunction()
