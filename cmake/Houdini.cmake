# Houdini's USD ABI is intentionally isolated from the standalone 26.03 build.
find_package(Houdini 22.0 CONFIG REQUIRED
    PATHS "${HDUE_HOUDINI_ROOT}/toolkit/cmake" NO_DEFAULT_PATH)
file(STRINGS "${HDUE_HOUDINI_ROOT}/toolkit/include/pxr/pxr.h" _pxr_version REGEX "^#define PXR_VERSION ")
if(NOT _pxr_version MATCHES "2605$")
    message(FATAL_ERROR "Houdini 22.0 is expected to provide its USD 26.05 ABI")
endif()
# The HDK has no ABI promise between daily builds (HDK_API_VERSION is the build number): the build is stamped into
# the DLL's version resource and the package, and plugin.cpp warns when another build loads the delegate.
file(STRINGS "${HDUE_HOUDINI_ROOT}/toolkit/include/SYS/SYS_Version.h" HDUE_HOUDINI_BUILD REGEX "^#define SYS_VERSION_FULL ")
string(REGEX REPLACE "[^\"]*\"([^\"]*)\".*" "\\1" HDUE_HOUDINI_BUILD "${HDUE_HOUDINI_BUILD}")
execute_process(COMMAND git describe --tags --always --dirty WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE HDUE_GIT_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT HDUE_GIT_VERSION)
    set(HDUE_GIT_VERSION "${PROJECT_VERSION}")
endif()
message(STATUS "hdUE ${HDUE_GIT_VERSION} for Houdini ${HDUE_HOUDINI_BUILD}")
configure_file(resources/version.rc.in "${CMAKE_BINARY_DIR}/version.rc" @ONLY)
add_library(hdue_hdk INTERFACE)
target_link_libraries(hdue_hdk INTERFACE Houdini HoudiniThirdParty
    "${HDUE_HOUDINI_ROOT}/custom/houdini/dsolib/libpxr_glf.lib"
    "${HDUE_HOUDINI_ROOT}/custom/houdini/dsolib/libpxr_cameraUtil.lib"
    "${HDUE_HOUDINI_ROOT}/custom/houdini/dsolib/openvdb_sesi.lib"
    "${HDUE_HOUDINI_ROOT}/custom/houdini/dsolib/OpenImageDenoise.lib"
    "${HDUE_HOUDINI_ROOT}/custom/houdini/dsolib/OpenColorIO_sidefx.lib"
    "${HDUE_HOUDINI_ROOT}/custom/houdini/dsolib/OpenImageIO_sidefx.lib"
    "${HDUE_HOUDINI_ROOT}/custom/houdini/dsolib/OpenImageIO_Util_sidefx.lib")
target_compile_definitions(hdue_hdk INTERFACE HDUE_HOUDINI=1
    _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING)
add_library(hdUE SHARED src/houdini/diagnostics.cpp src/houdini/volume_fields.cpp src/hydra/delegate.cpp src/hydra/color_management.cpp src/hydra/materials.cpp src/hydra/prims.cpp
    src/hydra/render_buffer.cpp src/hydra/plugin.cpp src/hydra/picking.cpp src/hydra/implicit_surfaces.cpp src/hydra/gpu_frames.cpp
    "${CMAKE_BINARY_DIR}/version.rc")
set_target_properties(hdUE PROPERTIES OUTPUT_NAME hdUE_Houdini22)
target_compile_definitions(hdUE PRIVATE HDUE_HYDRA_EXPORTS)
target_include_directories(hdUE PUBLIC src/hydra)
# gpu_frames.cpp: HgiGL textures and the GL interop entry points HgiGL loaded through garch.
target_link_libraries(hdUE PRIVATE hdue_client hdue_hdk opengl32 version
    "${HDUE_HOUDINI_ROOT}/custom/houdini/dsolib/libpxr_hgi.lib" "${HDUE_HOUDINI_ROOT}/custom/houdini/dsolib/libpxr_garch.lib"
    "${HDUE_HOUDINI_ROOT}/custom/houdini/dsolib/libpxr_hio.lib")
target_compile_options(hdUE PRIVATE /W3 /utf-8 /bigobj /Zi)
# A Release PDB, archived per release (Package-Houdini.ps1 -Zip) so crash dumps from users can be symbolised.
target_link_options(hdUE PRIVATE /DEBUG /OPT:REF /OPT:ICF)
set(HDUE_PLUGIN_NAME hdUEHoudini22)
set(HDUE_PLUGIN_LIBRARY "Release/hdUE_Houdini22.dll")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/resources")
configure_file(resources/plugInfo.json.in "${CMAKE_BINARY_DIR}/resources/plugInfo.json" @ONLY)
add_executable(hdue_usd_render src/host/usd_render.cpp)
target_link_libraries(hdue_usd_render PRIVATE hdue_client hdue_hdk user32 "${HDUE_HOUDINI_ROOT}/custom/houdini/dsolib/libpxr_js.lib")
target_compile_options(hdue_usd_render PRIVATE /W3 /utf-8 /bigobj)
target_link_options(hdue_usd_render PRIVATE /MANIFEST:EMBED "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/host/hdue.manifest")
if(BUILD_TESTING)
    add_executable(hdue_buffer_test tests/render_buffer.cpp src/hydra/render_buffer.cpp)
    target_include_directories(hdue_buffer_test PRIVATE src/hydra include)
    target_compile_definitions(hdue_buffer_test PRIVATE HDUE_HYDRA_EXPORTS)
    target_link_libraries(hdue_buffer_test PRIVATE hdue_hdk)
    add_test(NAME render_buffer_snapshots COMMAND hdue_buffer_test)
    add_executable(hdue_pick_test tests/picking.cpp src/hydra/picking.cpp)
    target_include_directories(hdue_pick_test PRIVATE src/hydra)
    target_link_libraries(hdue_pick_test PRIVATE hdue_hdk)
    add_test(NAME picking_bvh COMMAND hdue_pick_test)
    add_executable(hdue_subset_test tests/geom_subsets.cpp)
    target_include_directories(hdue_subset_test PRIVATE src/hydra include)
    target_link_libraries(hdue_subset_test PRIVATE hdue_hdk)
    add_test(NAME geom_subset_partition COMMAND hdue_subset_test)
    add_executable(hdue_subdivision_test tests/subdivision.cpp)
    target_include_directories(hdue_subdivision_test PRIVATE src/hydra include)
    target_link_libraries(hdue_subdivision_test PRIVATE hdue_hdk)
    add_test(NAME subdivision_refine COMMAND hdue_subdivision_test)
    add_executable(hdue_instancing_test tests/instancing.cpp)
    target_include_directories(hdue_instancing_test PRIVATE src/hydra)
    target_link_libraries(hdue_instancing_test PRIVATE hdue_hdk)
    add_test(NAME instance_transforms COMMAND hdue_instancing_test)
    add_executable(hdue_placement_test tests/placement.cpp)
    target_include_directories(hdue_placement_test PRIVATE src/hydra)
    target_link_libraries(hdue_placement_test PRIVATE hdue_hdk)
    add_test(NAME mesh_placement COMMAND hdue_placement_test)
    add_executable(hdue_render_tags_test tests/render_tags.cpp)
    target_include_directories(hdue_render_tags_test PRIVATE src/hydra)
    target_link_libraries(hdue_render_tags_test PRIVATE hdue_hdk)
    add_test(NAME render_tag_purposes COMMAND hdue_render_tags_test)
    add_executable(hdue_displacement_test tests/displacement.cpp)
    target_include_directories(hdue_displacement_test PRIVATE src/hydra include)
    target_link_libraries(hdue_displacement_test PRIVATE hdue_hdk)
    add_test(NAME displacement_dice COMMAND hdue_displacement_test)
    add_executable(hdue_cryptomatte_test tests/cryptomatte.cpp)
    target_include_directories(hdue_cryptomatte_test PRIVATE src/hydra)
    add_test(NAME cryptomatte_ranks COMMAND hdue_cryptomatte_test)
    add_executable(hdue_abi_layout tests/abi_layout.c)
    target_include_directories(hdue_abi_layout PRIVATE include)
    add_test(NAME abi_c_layout COMMAND hdue_abi_layout)
    set_tests_properties(render_buffer_snapshots picking_bvh geom_subset_partition subdivision_refine instance_transforms mesh_placement displacement_dice render_tag_purposes PROPERTIES ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${HDUE_HOUDINI_ROOT}/bin;PATH=path_list_prepend:${HDUE_HOUDINI_ROOT}/python313")
endif()
