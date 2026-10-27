# RNF-01/08/09: only the Runtime component belongs in end-user packages.
include(GNUInstallDirs)
set(_blendnet_programs blendnet-cli blendnet-server-cli)
if(TARGET blendnet)
    list(APPEND _blendnet_programs blendnet)
endif()
set(_blendnet_library_dir "${CMAKE_INSTALL_LIBDIR}/blendnet")
set(_blendnet_data_dir "share/blendnet")
set(_blendnet_bin_dir "${CMAKE_INSTALL_BINDIR}")

if(APPLE AND TARGET blendnet)
    set_target_properties(blendnet PROPERTIES MACOSX_BUNDLE TRUE OUTPUT_NAME BlendNet
        MACOSX_BUNDLE_INFO_PLIST "${PROJECT_SOURCE_DIR}/packaging/macos/Info.plist.in")
    set(_blendnet_bin_dir "BlendNet.app/Contents/MacOS")
    set(_blendnet_data_dir "BlendNet.app/Contents/Resources")
endif()

if(WIN32)
    # Imported DLLs (vcpkg/OpenSSL/curl/GLFW...) must accompany the executables.
    # CMake excludes build-tree targets from automatic runtime dependency
    # installation: include any shared FetchContent targets explicitly as well.
    set(_blendnet_windows_targets ${_blendnet_programs})
    foreach(_name reproc libzip::zip glfw)
        if(NOT TARGET ${_name})
            continue()
        endif()
        get_target_property(_aliased ${_name} ALIASED_TARGET)
        if(_aliased)
            set(_target "${_aliased}")
        else()
            set(_target "${_name}")
        endif()
        get_target_property(_imported ${_target} IMPORTED)
        get_target_property(_type ${_target} TYPE)
        if(NOT _imported AND _type STREQUAL "SHARED_LIBRARY")
            list(APPEND _blendnet_windows_targets ${_target})
        endif()
    endforeach()
    set(BLENDNET_RUNTIME_SEARCH_DIRS "" CACHE STRING "Extra DLL directories for packaging")
    foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
        list(APPEND _blendnet_dll_dirs "${_prefix}/bin")
    endforeach()
    if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
        list(APPEND _blendnet_dll_dirs "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
    endif()
    install(TARGETS ${_blendnet_windows_targets}
        RUNTIME_DEPENDENCIES
            DIRECTORIES ${_blendnet_dll_dirs} ${BLENDNET_RUNTIME_SEARCH_DIRS}
            # Optional Windows component: exclude before resolution, since it
            # can be absent on the packaging host. CMake matches DLL names in lowercase.
            PRE_EXCLUDE_REGEXES "api-ms-.*" "ext-ms-.*" [[^hvsifiletrust\.dll$]]
            POST_EXCLUDE_REGEXES ".*/[Ww][Ii][Nn][Dd][Oo][Ww][Ss]/.*"
        RUNTIME DESTINATION "${_blendnet_bin_dir}" COMPONENT Runtime
        LIBRARY DESTINATION "${_blendnet_library_dir}" COMPONENT Runtime)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    include(InstallRequiredSystemLibraries)
    install(PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}
        DESTINATION "${_blendnet_bin_dir}" COMPONENT Runtime)
else()
    install(TARGETS ${_blendnet_programs}
        RUNTIME DESTINATION "${_blendnet_bin_dir}" COMPONENT Runtime
        BUNDLE DESTINATION . COMPONENT Runtime)
    if(APPLE)
        set(_blendnet_rpath "@executable_path/../Frameworks")
    else()
        file(RELATIVE_PATH _blendnet_relative_lib
            "${CMAKE_INSTALL_PREFIX}/${_blendnet_bin_dir}"
            "${CMAKE_INSTALL_PREFIX}/${_blendnet_library_dir}")
        set(_blendnet_rpath "$ORIGIN/${_blendnet_relative_lib}")
    endif()
    foreach(_target IN LISTS _blendnet_programs)
        set_target_properties(${_target} PROPERTIES INSTALL_RPATH "${_blendnet_rpath}"
            INSTALL_RPATH_USE_LINK_PATH FALSE)
    endforeach()
    # Linux native packages use system OpenSSL/curl/GLFW, plus libraries built
    # by FetchContent when these are not supplied by the system package manager.
    foreach(_name reproc libzip::zip)
        get_target_property(_aliased ${_name} ALIASED_TARGET)
        if(_aliased)
            set(_target "${_aliased}")
        else()
            set(_target "${_name}")
        endif()
        get_target_property(_imported ${_target} IMPORTED)
        get_target_property(_type ${_target} TYPE)
        if(NOT _imported AND _type STREQUAL "SHARED_LIBRARY" AND NOT APPLE)
            install(TARGETS ${_target} LIBRARY DESTINATION "${_blendnet_library_dir}"
                COMPONENT Runtime NAMELINK_SKIP)
        endif()
    endforeach()
endif()

install(FILES "${PROJECT_SOURCE_DIR}/packaging/fonts/DejaVuSans.ttf"
    DESTINATION "${_blendnet_data_dir}/fonts" COMPONENT Runtime)
install(FILES "${PROJECT_SOURCE_DIR}/README.md"
    DESTINATION "${_blendnet_data_dir}" COMPONENT Runtime)
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE"
    "${PROJECT_SOURCE_DIR}/packaging/THIRD-PARTY.md"
    DESTINATION "${_blendnet_data_dir}/licenses" COMPONENT Runtime)
install(FILES "${PROJECT_SOURCE_DIR}/packaging/fonts/LICENSE-DejaVu.txt"
    DESTINATION "${_blendnet_data_dir}/licenses" COMPONENT Runtime)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/docs/"
    DESTINATION "${_blendnet_data_dir}/docs" COMPONENT Runtime FILES_MATCHING PATTERN "*.md"
    PATTERN "*.json")

# Preserve the notices of third-party libraries statically linked into our binaries.
foreach(_dependency imgui json libzip reproc glfw)
    foreach(_license LICENSE LICENSE.txt LICENSE.MIT COPYING)
        if(EXISTS "${${_dependency}_SOURCE_DIR}/${_license}")
            install(FILES "${${_dependency}_SOURCE_DIR}/${_license}"
                DESTINATION "${_blendnet_data_dir}/licenses/${_dependency}" COMPONENT Runtime)
        endif()
    endforeach()
endforeach()
foreach(_dependency libasio-dev nlohmann-json3-dev zlib1g-dev)
    if(EXISTS "/usr/share/doc/${_dependency}/copyright")
        install(FILES "/usr/share/doc/${_dependency}/copyright"
            DESTINATION "${_blendnet_data_dir}/licenses/${_dependency}" COMPONENT Runtime)
    endif()
endforeach()
if(EXISTS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share")
    install(DIRECTORY "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/"
        DESTINATION "${_blendnet_data_dir}/licenses/dependencies" COMPONENT Runtime
        FILES_MATCHING PATTERN "copyright")
endif()

if(UNIX AND NOT APPLE AND TARGET blendnet)
    install(FILES "${PROJECT_SOURCE_DIR}/packaging/linux/blendnet.desktop"
        DESTINATION share/applications COMPONENT Runtime)
    install(FILES "${PROJECT_SOURCE_DIR}/packaging/linux/blendnet.svg"
        DESTINATION share/icons/hicolor/scalable/apps COMPONENT Runtime)
endif()
if(APPLE AND TARGET blendnet)
    include("${PROJECT_SOURCE_DIR}/cmake/BrewNotices.cmake")
    # Header-only libraries are not visited by Mach-O dependency scanning.
    foreach(_header "${ASIO_INCLUDE_DIR}/asio.hpp" "${nlohmann_json_DIR}")
        blendnet_brew_notices("${_header}" _name _notices)
        if(_notices)
            install(FILES ${_notices} DESTINATION "${_blendnet_data_dir}/licenses/${_name}"
                COMPONENT Runtime)
        endif()
    endforeach()
    configure_file("${PROJECT_SOURCE_DIR}/cmake/FixMacBundle.cmake.in"
        "${PROJECT_BINARY_DIR}/FixMacBundle.cmake" @ONLY)
    install(SCRIPT "${PROJECT_BINARY_DIR}/FixMacBundle.cmake" COMPONENT Runtime)
endif()

set(CPACK_PACKAGE_NAME BlendNet)
set(CPACK_PACKAGE_VENDOR "BlendNet contributors")
set(CPACK_PACKAGE_CONTACT "BlendNet contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Distributed Blender rendering over TLS")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "BlendNet-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/damien-briand/BlendNet")
set(CPACK_PACKAGE_INSTALL_DIRECTORY BlendNet)
set(CPACK_PACKAGE_CHECKSUM SHA256)
set(CPACK_PACKAGE_DIRECTORY "${PROJECT_BINARY_DIR}/packages")
set(CPACK_INSTALL_CMAKE_PROJECTS "${PROJECT_BINARY_DIR};BlendNet;Runtime;/")
set(CPACK_COMPONENTS_ALL Runtime)
set(CPACK_VERBATIM_VARIABLES YES)
# Stripping after install(SCRIPT) would invalidate the macOS bundle signature.
# Preserve Windows DLL signatures as well; Release builds do not package PDBs.
if(UNIX AND NOT APPLE)
    set(CPACK_STRIP_FILES YES)
else()
    set(CPACK_STRIP_FILES NO)
endif()
set(CPACK_SOURCE_GENERATOR TGZ)
set(CPACK_SOURCE_IGNORE_FILES
    "/\\.git/;/build[^/]*/;/\\.cache/;/\\.vscode/;/\\.blendnet/;/\\.continue/;/\\.codex/;/\\.agents/;/\\.aws/;__pycache__;\\.pyc$")

if(WIN32)
    set(CPACK_GENERATOR "NSIS;ZIP")
    set(CPACK_PACKAGE_EXECUTABLES "blendnet;BlendNet")
    set(CPACK_NSIS_EXECUTABLES_DIRECTORY "${_blendnet_bin_dir}")
    set(CPACK_NSIS_INSTALL_ROOT "$LOCALAPPDATA")
    set(CPACK_NSIS_DEFINES "RequestExecutionLevel user")
    set(CPACK_NSIS_MODIFY_PATH OFF)
elseif(APPLE)
    set(CPACK_GENERATOR "DragNDrop;TGZ")
    set(CPACK_DMG_VOLUME_NAME BlendNet)
else()
    set(CPACK_GENERATOR "TGZ;DEB")
    set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
    set(CPACK_DEBIAN_PACKAGE_NAME blendnet)
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_SECTION graphics)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "ca-certificates, tar, xz-utils, libcurl4 (>= 7.85) | libcurl4t64 (>= 7.85)")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS "${_blendnet_library_dir}")
    set(CPACK_RPM_PACKAGE_NAME blendnet)
    set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
    set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-only")
    set(CPACK_RPM_PACKAGE_REQUIRES "ca-certificates, tar, xz, libcurl >= 7.85")
    set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION /usr/share /usr/share/applications
        /usr/share/icons /usr/share/icons/hicolor /usr/share/icons/hicolor/scalable
        /usr/share/icons/hicolor/scalable/apps)
    set(CPACK_EXTERNAL_ENABLE_STAGING TRUE)
    set(CPACK_EXTERNAL_PACKAGE_SCRIPT "${PROJECT_SOURCE_DIR}/cmake/PackageAppImage.cmake")
    set(CPACK_LINUXDEPLOY_EXECUTABLE "" CACHE FILEPATH "linuxdeploy executable for CPack External/AppImage")
endif()
include(CPack)
