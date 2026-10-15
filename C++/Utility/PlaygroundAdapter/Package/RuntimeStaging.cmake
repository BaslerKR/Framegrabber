# Framegrabber-owned Playground plugin runtime payload. The host copies
# PLAYGROUND_PLUGIN_RUNTIME_PAYLOAD_DIR into plugins/framegrabber/current/runtime.

function(framegrabber_prepare_playground_plugin_runtime target_name)
    if(NOT TARGET ${target_name})
        message(FATAL_ERROR
            "[Framegrabber] ${target_name} must exist before its plugin runtime is prepared.")
    endif()

    set(search_paths)
    set(host_bundle_files)
    set(payload_dir "")
    set(logging_config
        "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Package/BaslerFgSdkLogging.properties")
    if(NOT EXISTS "${logging_config}")
        message(FATAL_ERROR
            "[Framegrabber] SDK logging configuration is missing at '${logging_config}'.")
    endif()

    if(WIN32)
        set(PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR
            "${BASLER_FG_SDK_DIR}/bin" CACHE PATH
            "Directory containing the Basler Frame Grabber SDK runtime binaries."
        )
        if(NOT IS_DIRECTORY "${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}")
            message(FATAL_ERROR
                "[Framegrabber] Required SDK runtime directory not found at "
                "'${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}'. "
                "Set PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR or BASLER_FG_SDK_DIR.")
        endif()
        list(APPEND search_paths "${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}")

        set(payload_dir "${CMAKE_CURRENT_BINARY_DIR}/plugin-runtime")
        file(REMOVE_RECURSE "${payload_dir}")
        file(MAKE_DIRECTORY "${payload_dir}/lib")

        foreach(framegrabber_required_runtime_name
                fglib5.dll
                siso-core.dll
                siso_auxport.dll
                siso_genicam.dll
                siso_hal.dll
                siso_hw.dll
                siso_log.dll
                SiSoCsRt.dll
                siso-lib-environment.dll
                common-logging-dispatcher.dll
                ProducerCXP.cti)
            set(framegrabber_required_runtime
                "${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}/${framegrabber_required_runtime_name}")
            if(NOT EXISTS "${framegrabber_required_runtime}")
                message(FATAL_ERROR
                    "[Framegrabber] Required capture runtime is missing: "
                    "${framegrabber_required_runtime}")
            endif()
        endforeach()
        if(NOT EXISTS "${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}/common-logging-log4cpp.dll" AND
           NOT EXISTS "${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}/common-logging-log4cxx.dll")
            message(FATAL_ERROR
                "[Framegrabber] Required logging backend is missing under "
                "'${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}'. "
                "Expected common-logging-log4cpp.dll or common-logging-log4cxx.dll.")
        endif()
        foreach(framegrabber_plugin_name
                siso-plugin-environment-physical-boards.dll
                siso-plugin-environment-physical-dll.dll
                siso-plugin-environment-physical-hap.dll)
            set(framegrabber_plugin
                "${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}/plugins/${framegrabber_plugin_name}")
            if(NOT EXISTS "${framegrabber_plugin}")
                message(FATAL_ERROR
                    "[Framegrabber] Required board plugin is missing: ${framegrabber_plugin}")
            endif()
        endforeach()

        # Names loaded after process start stay in this directory, so the
        # installed bin is copied whole instead of enumerated.
        file(COPY "${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}/"
            DESTINATION "${payload_dir}/bin")
        file(COPY "${logging_config}" DESTINATION "${payload_dir}/bin")

        get_filename_component(framegrabber_sdk_root
            "${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}/.." ABSOLUTE)
        list(APPEND search_paths
            "${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}"
            "${framegrabber_sdk_root}/plugins"
            "${framegrabber_sdk_root}/lib"
            "${framegrabber_sdk_root}/dll")

        file(GLOB framegrabber_sdk_board_entries
            LIST_DIRECTORIES true
            RELATIVE "${framegrabber_sdk_root}/dll"
            "${framegrabber_sdk_root}/dll/*"
        )
        set(framegrabber_cxp_models)
        foreach(framegrabber_board_entry IN LISTS framegrabber_sdk_board_entries)
            set(framegrabber_board_dir
                "${framegrabber_sdk_root}/dll/${framegrabber_board_entry}")
            if(IS_DIRECTORY "${framegrabber_board_dir}" AND
               framegrabber_board_entry MATCHES "^(iF-CXP.*|iW-CXP.*)$")
                list(APPEND framegrabber_cxp_models "${framegrabber_board_entry}")
            endif()
        endforeach()
        if(NOT framegrabber_cxp_models)
            message(FATAL_ERROR
                "[Framegrabber] No iF/iW CXP applet directories were found under "
                "'${framegrabber_sdk_root}/dll'.")
        endif()
        list(SORT framegrabber_cxp_models)
        message(STATUS
            "[Framegrabber] Bundling iF/iW CXP applets for: ${framegrabber_cxp_models}")
        file(MAKE_DIRECTORY "${payload_dir}/dll")
        foreach(framegrabber_model IN LISTS framegrabber_cxp_models)
            file(COPY "${framegrabber_sdk_root}/dll/${framegrabber_model}"
                DESTINATION "${payload_dir}/dll")
        endforeach()
        if(IS_DIRECTORY "${framegrabber_sdk_root}/genicam")
            file(COPY "${framegrabber_sdk_root}/genicam"
                DESTINATION "${payload_dir}")
        endif()
        foreach(framegrabber_hardware_runtime_name siso_hw_me5.dll siso_hw_me6.dll)
            set(framegrabber_hardware_runtime
                "${framegrabber_sdk_root}/lib/${framegrabber_hardware_runtime_name}")
            if(NOT EXISTS "${framegrabber_hardware_runtime}")
                message(FATAL_ERROR
                    "[Framegrabber] Required hardware runtime is missing at "
                    "'${framegrabber_hardware_runtime}'.")
            endif()
            file(COPY "${framegrabber_hardware_runtime}" DESTINATION "${payload_dir}/lib")
        endforeach()
    elseif(UNIX AND NOT APPLE)
        set(PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR
            "/opt/Basler/FramegrabberSDK/lib" CACHE PATH
            "Directory containing the Basler Frame Grabber SDK runtime.")
        if(IS_DIRECTORY "${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}")
            list(APPEND search_paths "${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}")
        else()
            message(WARNING
                "[Framegrabber] SDK runtime directory was not found. "
                "Set PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR for a standalone bundle.")
        endif()
    endif()

    set_target_properties(${target_name} PROPERTIES
        PLAYGROUND_PLUGIN_RUNTIME_DEPENDENCY_DEST "runtime/bin"
        PLAYGROUND_PLUGIN_RUNTIME_SEARCH_PATHS "${search_paths}"
        PLAYGROUND_PLUGIN_HOST_BUNDLE_FILES "${host_bundle_files}"
    )
    if(payload_dir)
        set_target_properties(${target_name} PROPERTIES
            PLAYGROUND_PLUGIN_RUNTIME_PAYLOAD_DIR "${payload_dir}"
        )
    endif()
endfunction()
