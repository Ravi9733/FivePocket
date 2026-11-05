# One shared build path for the live backend and device diagnostic. A failed
# compiler or validator cannot publish a fresh includable shader header.
function(attach_native_constant_shaders target)
    string(TOLOWER "${CMAKE_HOST_SYSTEM_NAME}" shader_host)
    find_program(NATIVE_GLSLC NAMES glslc HINTS "${CMAKE_ANDROID_NDK}/shader-tools/${shader_host}-x86_64" REQUIRED)
    find_program(NATIVE_SPIRV_VAL NAMES spirv-val HINTS "${CMAKE_ANDROID_NDK}/shader-tools/${shader_host}-x86_64" REQUIRED)
    set(shader_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/native-constant-shaders")
    file(MAKE_DIRECTORY "${generated}")
    set(header "${generated}/native_constant_shaders.h")
    add_custom_command(OUTPUT "${header}"
        COMMAND "${NATIVE_GLSLC}" --target-env=vulkan1.1 "${shader_dir}/constant_point.vert" -o "${generated}/vert.spv.unvalidated"
        COMMAND "${NATIVE_SPIRV_VAL}" --target-env vulkan1.1 "${generated}/vert.spv.unvalidated"
        COMMAND "${NATIVE_GLSLC}" --target-env=vulkan1.1 "${shader_dir}/constant_point.frag" -o "${generated}/frag.spv.unvalidated"
        COMMAND "${NATIVE_SPIRV_VAL}" --target-env vulkan1.1 "${generated}/frag.spv.unvalidated"
        COMMAND "${CMAKE_COMMAND}" "-DVERTEX=${generated}/vert.spv.unvalidated" "-DFRAGMENT=${generated}/frag.spv.unvalidated"
            "-DOUTPUT=${header}" -P "${shader_dir}/embed_constant_shaders.cmake"
        DEPENDS "${shader_dir}/constant_point.vert" "${shader_dir}/constant_point.frag"
            "${shader_dir}/embed_constant_shaders.cmake" "${NATIVE_GLSLC}" "${NATIVE_SPIRV_VAL}" VERBATIM)
    target_sources(${target} PRIVATE "${header}")
    target_include_directories(${target} PRIVATE "${generated}")
endfunction()
