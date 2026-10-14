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
        file(MAKE_DIRECTORY "${payload_dir}/bin")
        file(MAKE_DIRECTORY "${payload_dir}/bin/plugins")
        file(MAKE_DIRECTORY "${payload_dir}/lib")

        set(framegrabber_runtime_bin_files
            "${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}/BaslerCLProtocol.dll"
            "${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}/CLAllSerial_MD_VC142_v3_5_Basler_pylon_v1.dll"
            "${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}/CLProtocol_MD_VC142_v3_5_Basler_pylon_v1.dll"
            "${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}/clsercom.dll"
            "${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}/clsersis.dll"
            "${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}/common-logging-dispatcher.dll"
            "${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}/common-logging-log4cpp.dll"
            "${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}/common-logging-log4cxx.dll"
            "${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}/common-logging-log4cxx.xml"
            "${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}/common-logging-siso.dll"
            "${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}/fglib5.dll"
            "${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}/haprt.dll"
            "${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}/iolibrt.dll"
            "${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}/libcrypto-1_1-x64.dll"
            "${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}/libssl-1_1-x64.dll"
            "${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}/logging-context.dll"
            "${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}/ProducerCL.cti"
            "${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}/ProducerCXP.cti"
        )
        foreach(framegrabber_siso_runtime_name
                siso-core.dll
                siso_auxport.dll
                siso_genicam.dll
                siso_hal.dll
                siso_hw.dll
                siso_log.dll
                SiSoCsRt.dll)
            set(framegrabber_siso_runtime
                "${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}/${framegrabber_siso_runtime_name}")
            if(NOT EXISTS "${framegrabber_siso_runtime}")
                message(FATAL_ERROR
                    "[Framegrabber] Required capture runtime is missing: "
                    "${framegrabber_siso_runtime}")
            endif()
            list(APPEND framegrabber_runtime_bin_files "${framegrabber_siso_runtime}")
        endforeach()
        set(framegrabber_environment_runtime
            "${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}/siso-lib-environment.dll")
        if(NOT EXISTS "${framegrabber_environment_runtime}")
            message(FATAL_ERROR
                "[Framegrabber] Required environment runtime is missing: "
                "${framegrabber_environment_runtime}")
        endif()
        list(APPEND framegrabber_runtime_bin_files "${framegrabber_environment_runtime}")
        file(GLOB framegrabber_genicam_runtime_files
            "${PLAYGROUND_LOCAL_FRAMEGRABBER_RUNTIME_DIR}/*_MD_VC142_v3_5_Basler_pylon_v1.dll"
        )
        list(APPEND framegrabber_runtime_bin_files ${framegrabber_genicam_runtime_files})
        foreach(framegrabber_runtime_bin_file IN LISTS framegrabber_runtime_bin_files)
            if(NOT EXISTS "${framegrabber_runtime_bin_file}")
                message(FATAL_ERROR
                    "[Framegrabber] Required runtime file is missing: "
                    "${framegrabber_runtime_bin_file}")
            endif()
            file(COPY "${framegrabber_runtime_bin_file}" DESTINATION "${payload_dir}/bin")
        endforeach()
        file(COPY "${logging_config}" DESTINATION "${payload_dir}/bin")

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
            file(COPY "${framegrabber_plugin}" DESTINATION "${payload_dir}/bin/plugins")
        endforeach()

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
               framegrabber_board_entry MATCHES
                   "^(CXP12-IC-.*|iF-CXP12-.*|iW-CXP12-.*|iF2D100|mE5-MA-ACX-.*|mE5-MA-VCX-.*)$")
                list(APPEND framegrabber_cxp_models "${framegrabber_board_entry}")
            endif()
        endforeach()
        if(NOT framegrabber_cxp_models)
            message(FATAL_ERROR
                "[Framegrabber] No supported CXP/CoF applet directories were found under "
                "'${framegrabber_sdk_root}/dll'.")
        endif()
        list(SORT framegrabber_cxp_models)
        message(STATUS
            "[Framegrabber] Bundling CXP/CoF applets for: ${framegrabber_cxp_models}")
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
