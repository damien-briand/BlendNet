# P9: CPack stages our Runtime component; linuxdeploy resolves ELF dependencies.
if(NOT EXISTS "${CPACK_LINUXDEPLOY_EXECUTABLE}")
    message(FATAL_ERROR "Set CPACK_LINUXDEPLOY_EXECUTABLE to linuxdeploy to generate an AppImage")
endif()
if(NOT EXISTS "${CPACK_TEMPORARY_DIRECTORY}/usr/bin/blendnet")
    message(FATAL_ERROR "AppImage packaging requires BLENDNET_BUILD_GUI=ON")
endif()
set(_output "${CPACK_TOPLEVEL_DIRECTORY}/${CPACK_PACKAGE_FILE_NAME}.AppImage")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env
    "APPIMAGE_EXTRACT_AND_RUN=1" "OUTPUT=${_output}" "VERSION=${CPACK_PACKAGE_VERSION}"
    "${CPACK_LINUXDEPLOY_EXECUTABLE}" --appdir "${CPACK_TEMPORARY_DIRECTORY}"
    --desktop-file "${CPACK_TEMPORARY_DIRECTORY}/usr/share/applications/blendnet.desktop"
    --icon-file "${CPACK_TEMPORARY_DIRECTORY}/usr/share/icons/hicolor/scalable/apps/blendnet.svg"
    --executable "${CPACK_TEMPORARY_DIRECTORY}/usr/bin/blendnet"
    --executable "${CPACK_TEMPORARY_DIRECTORY}/usr/bin/blendnet-cli"
    --executable "${CPACK_TEMPORARY_DIRECTORY}/usr/bin/blendnet-server-cli"
    --output appimage
    WORKING_DIRECTORY "${CPACK_TOPLEVEL_DIRECTORY}" RESULT_VARIABLE _result)
if(NOT _result EQUAL 0 OR NOT EXISTS "${_output}")
    message(FATAL_ERROR "linuxdeploy failed to build the AppImage (${_result})")
endif()
set(CPACK_EXTERNAL_BUILT_PACKAGES "${_output}")
