# Exact ONScripter-RU SDL_gpu fork, compiled natively against VitaSDK SDL2/vitaGL.
# Included by the main Vita CMakeLists after project().
# SDL2's built-in GXM renderer calls the VFP native viewport export as
# softfp. Redirect only SDL2's archive references to vitaGL's proven bridge.
# Do not wrap sceGxmSetViewport globally: vitaGL already bridges that call.
set(ONS_NATIVE_SDL2 "${CMAKE_CURRENT_BINARY_DIR}/libons_sdl2_vita.a")
add_custom_command(OUTPUT "${ONS_NATIVE_SDL2}"
  COMMAND "${CMAKE_OBJCOPY}"
    --redefine-sym sceGxmSetViewport=sceGxmSetViewport_sfp
    "${VITASDK}/arm-vita-eabi/lib/libSDL2.a" "${ONS_NATIVE_SDL2}"
  DEPENDS "${VITASDK}/arm-vita-eabi/lib/libSDL2.a"
  COMMENT "Adapting SDL2 native GXM viewport ABI without modifying the SDK"
  VERBATIM)
add_custom_target(ons_vita_sdl2_archive DEPENDS "${ONS_NATIVE_SDL2}")
add_library(ons_vita_sdl2 STATIC IMPORTED GLOBAL)
set_target_properties(ons_vita_sdl2 PROPERTIES IMPORTED_LOCATION "${ONS_NATIVE_SDL2}")
add_dependencies(ons_vita_sdl2 ons_vita_sdl2_archive)

# Built from source with ONScripter's two offline decoder locks removed.
add_library(ons_vita_mixer STATIC IMPORTED GLOBAL)
set_target_properties(ons_vita_mixer PROPERTIES
  IMPORTED_LOCATION "${ONS_NATIVE_DEPS_ROOT}/lib/libSDL2_mixer.a")

# Override only the proven faulty GLOBAL GLSL declaration pass. Keeping GLOBAL
# mode preserves semantic assignments when shaders are compiled/relinked apart.
set(ONS_NATIVE_VITAGL "${CMAKE_CURRENT_BINARY_DIR}/libons_vitagl.a")
add_custom_command(OUTPUT "${ONS_NATIVE_VITAGL}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/gpu_translator_archive.py"
    --ar "${CMAKE_AR}" --objcopy "${CMAKE_OBJCOPY}"
    "${ONS_NATIVE_DEPS_ROOT}/lib/libvitaGL.a" "${ONS_NATIVE_VITAGL}"
  DEPENDS "${ONS_NATIVE_DEPS_ROOT}/lib/libvitaGL.a"
    "${CMAKE_CURRENT_LIST_DIR}/gpu_translator_archive.py"
  COMMENT "Selecting corrected native vitaGL global shader translator"
  VERBATIM)
add_custom_target(ons_vita_gl_archive DEPENDS "${ONS_NATIVE_VITAGL}")
add_library(ons_vita_gl STATIC IMPORTED GLOBAL)
set_target_properties(ons_vita_gl PROPERTIES IMPORTED_LOCATION "${ONS_NATIVE_VITAGL}")
add_dependencies(ons_vita_gl ons_vita_gl_archive)
add_library(ons_vita_glsl_fix OBJECT
  ${CMAKE_CURRENT_LIST_DIR}/../../vendor/vitagl-translator/global.c
  ${CMAKE_CURRENT_LIST_DIR}/gpu_render_targets.c)
target_compile_definitions(ons_vita_glsl_fix PRIVATE VITA)
target_compile_options(ons_vita_glsl_fix PRIVATE
  -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp)

set(ONS_SDL_GPU_DIR "${CMAKE_CURRENT_LIST_DIR}/../../vendor/sdl-gpu")
add_library(ons_vita_gpu STATIC
  ${CMAKE_CURRENT_LIST_DIR}/shader_cache.c
  ${ONS_SDL_GPU_DIR}/src/SDL_gpu.c
  ${ONS_SDL_GPU_DIR}/src/SDL_gpu_matrix.c
  ${ONS_SDL_GPU_DIR}/src/SDL_gpu_renderer.c
  ${ONS_SDL_GPU_DIR}/src/SDL_gpu_shapes.c
  ${ONS_SDL_GPU_DIR}/src/renderer_GLES_2.c
  ${ONS_SDL_GPU_DIR}/src/externals/stb_image/stb_image.c
  ${ONS_SDL_GPU_DIR}/src/externals/stb_image_write/stb_image_write.c)
target_include_directories(ons_vita_gpu PUBLIC
  ${ONS_SDL_GPU_DIR}/include
  ${CMAKE_CURRENT_LIST_DIR}/include
  ${VITASDK}/arm-vita-eabi/include/SDL2)
target_include_directories(ons_vita_gpu PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}
  ${ONS_SDL_GPU_DIR}/src/externals/stb_image
  ${ONS_SDL_GPU_DIR}/src/externals/stb_image_write)
target_compile_definitions(ons_vita_gpu PUBLIC VITA
  SDL_GPU_DISABLE_OPENGL SDL_GPU_DISABLE_GLES_1 SDL_GPU_DISABLE_GLES_3)
target_compile_definitions(ons_vita_gpu PRIVATE SDL_GPU_USE_BUFFER_RESET)
if(ONS_VITA_FILE_LOGGING)
  # Diagnostic builds: live image/framebuffer census for the engine's log.
  target_compile_definitions(ons_vita_gpu PRIVATE ONS_VITA_GPU_CENSUS)
  target_compile_definitions(ons_vita_glsl_fix PRIVATE ONS_VITA_GPU_CENSUS)
endif()
# Every program is exported in the same translator binding order. Fingerprint
# the whole corpus and driver, rather than mix stale binaries with new sources.
set(ONS_SHADER_INPUTS ${ONS_RESOURCES}
  "${ONS_SDL_GPU_DIR}/include/SDL_gpu_GLES_2.h"
  "${CMAKE_CURRENT_LIST_DIR}/../../vendor/vitagl-translator/global.c"
  "${ONS_NATIVE_DEPS_ROOT}/lib/libvitaGL.a")
set(ONS_SHADER_FINGERPRINT "ons-gxp-matrix-table-v1")
foreach(ONS_SHADER_INPUT IN LISTS ONS_SHADER_INPUTS)
  file(SHA256 "${ONS_SHADER_INPUT}" ONS_SHADER_INPUT_HASH)
  string(APPEND ONS_SHADER_FINGERPRINT "${ONS_SHADER_INPUT_HASH}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ONS_SHADER_INPUT}")
endforeach()
string(SHA256 ONS_SHADER_ABI "${ONS_SHADER_FINGERPRINT}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/shader-abi.txt" "${ONS_SHADER_ABI}\n")
target_compile_definitions(ons_vita_gpu PRIVATE ONS_SHADER_ABI="${ONS_SHADER_ABI}")
if(ONS_VITA_EXPORT_SHADERS)
  target_compile_definitions(ons_vita_gpu PRIVATE ONS_VITA_EXPORT_SHADERS)
  target_link_options(ons_vita_gpu INTERFACE -Wl,--wrap=sceGxmShaderPatcherRegisterProgram)
else()
  file(GLOB ONS_SHADER_BINARIES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/shaders/${ONS_SHADER_ABI}/*.bin")
  list(FILTER ONS_SHADER_BINARIES EXCLUDE REGEX "/\\._")
  if(NOT ONS_SHADER_BINARIES)
    message(FATAL_ERROR "No prepared shaders for ${ONS_SHADER_ABI}. Use the explicit developer export build, retrieve its shaders, then configure a release build.")
  endif()
  foreach(ONS_SHADER_BINARY IN LISTS ONS_SHADER_BINARIES)
    get_filename_component(ONS_SHADER_NAME "${ONS_SHADER_BINARY}" NAME)
    list(APPEND ONS_SHADER_PACKAGE_FILES "${ONS_SHADER_BINARY}" "shaders/${ONS_SHADER_ABI}/${ONS_SHADER_NAME}")
  endforeach()
endif()
target_compile_options(ons_vita_gpu PRIVATE -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp
  -Werror=implicit-function-declaration)
target_sources(ons_vita_gpu INTERFACE $<TARGET_OBJECTS:ons_vita_glsl_fix>)
target_link_options(ons_vita_gpu INTERFACE
  -Wl,--wrap=glGenFramebuffers -Wl,--wrap=glBindFramebuffer
  -Wl,--wrap=glDeleteFramebuffers
  -Wl,--wrap=glViewport
  -Wl,--wrap=vector4f_convert_to_local_space)
target_link_libraries(ons_vita_gpu PUBLIC ons_vita_sdl2 ons_vita_gl vitashark SceShaccCgExt
  SceShaccCg_stub SceGxm_stub SceDisplay_stub SceSysmodule_stub
  SceCommonDialog_stub SceAppMgr_stub SceTouch_stub SceHid_stub SceCtrl_stub
  SceAudio_stub SceAudioIn_stub SceMotion_stub ScePower_stub SceIme_stub
  SceIofilemgr_stub SceKernelThreadMgr_stub SceSysmem_stub
  SceKernelDmacMgr_stub taihen_stub mathneon m)
