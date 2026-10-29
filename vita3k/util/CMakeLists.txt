add_library(
	util
	STATIC
	src/android_driver.cpp
	src/arm.cpp
	src/float_to_half.cpp
	src/fs_utils.cpp
	src/hash.cpp
	src/instrset_detect.cpp
	src/logging.cpp
	src/net_utils.cpp
	src/string_utils.cpp
	src/switch_storage.cpp
	src/switch_thread.cpp
	src/switch_tracelab.cpp
	src/switch_memory_validation.cpp
	src/switch_memory_capabilities.cpp
	src/switch_lsfg_profile.cpp
	src/switch_vibration.cpp
	src/tracy.cpp
	src/vita_theme_utils.cpp
)

# vc_runtime_checker.cpp is directly added from the main CMakeList (for some reason adding it here doesn't work)

target_include_directories(util PUBLIC include)
target_link_libraries(util PUBLIC ${Boost_LIBRARIES} fmt spdlog http mem)
target_link_libraries(util PRIVATE libcurl crypto SDL3::SDL3)
if(ANDROID)
	target_link_libraries(util PUBLIC emuenv android xxHash::xxhash)
	target_link_libraries(util PRIVATE adrenotools)
endif()
target_compile_definitions(util PRIVATE $<$<CONFIG:Debug,RelWithDebInfo>:TRACY_ENABLE>)

if(SWITCH)
    if(SWITCH_TRACELAB)
        find_program(SWITCH_HOOK_PYTHON NAMES python3 REQUIRED)
        execute_process(COMMAND "${SWITCH_HOOK_PYTHON}" "${CMAKE_SOURCE_DIR}/scripts/apply-switch-write-watch-hooks.py" --check-applied
            RESULT_VARIABLE SWITCH_HOOK_RESULT)
        if(NOT SWITCH_HOOK_RESULT EQUAL 0)
            message(FATAL_ERROR "Apply pinned experiment hooks: python3 scripts/apply-switch-write-watch-hooks.py")
        endif()
        target_compile_definitions(util PRIVATE VITA3K_SWITCH_WRITE_SHADOW_DEFAULT=1 VITA3K_SWITCH_CLEAN_CAPTURE_DEFAULT=1)
    endif()
    # Changing the build SHA must not invalidate every cached translation unit.
    # Embed provenance only in the two report-producing sources.
    if(SWITCH_TRACELAB AND VITA3K_MEMORY_VALIDATION_REVISION)
        set_property(SOURCE src/switch_memory_validation.cpp APPEND PROPERTY
            COMPILE_DEFINITIONS "VITA3K_MEMORY_VALIDATION_REVISION=\"${VITA3K_MEMORY_VALIDATION_REVISION}\"")
    endif()
    if(SWITCH_TRACELAB AND VITA3K_LIBNX_BUILD_VERSION)
        set_property(SOURCE src/switch_memory_capabilities.cpp APPEND PROPERTY
            COMPILE_DEFINITIONS "VITA3K_LIBNX_BUILD_VERSION=\"${VITA3K_LIBNX_BUILD_VERSION}\"")
    endif()
    # Defaults ON for Switch: render may borrow idle app cores while guest
    # priorities and core-3 audio/display/Vulkan helpers always preempt it.
    # OFF restores the original core-3-only render affinity.
    option(SWITCH_RENDER_CORE_SPREAD "Allow render thread to use spare cores 0-2" ON)
    if(SWITCH_RENDER_CORE_SPREAD)
        target_compile_definitions(util PRIVATE VITA3K_SWITCH_RENDER_CORE_SPREAD=1)
    endif()
endif()
