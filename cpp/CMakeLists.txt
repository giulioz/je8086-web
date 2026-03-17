cmake_minimum_required(VERSION 3.20)
project(jpwasm VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ESP_SOURCES
  core/esp.hpp
  core/esp_opt.hpp
  core/esp_jit.h
  core/esp_jit_backend.cpp
  core/esp_jit_backend.h
  core/esp_jit_types.h
  core/esp_jit_arm64.cpp
  core/esp_jit_arm64.h
  core/esp_jit_x64.cpp
  core/esp_jit_x64.h
  core/esp_jit_x64_pool.cpp
  core/esp_jit_x64_pool.h
  core/esp_jit_x64_types.h
)

set(CORE_SOURCES
  core/esp_jit.cpp
  core/esp_jit.h
  core/h8s.hpp
  core/h8sdevices.hpp
  core/jp8000_devices.cpp
  core/jp8000_devices.hpp
  core/jp8000_emulator.cpp
  core/jp8000_emulator.hpp
  core/lcd_font.cpp
  core/lcd_renderer.hpp
  core/esp_jit_binaryen.cpp
  core/esp_jit_binaryen.h
)

option(JPWASM_ENABLE_BINARYEN "Enable Binaryen dependency for wasm/JIT integration" ON)

if(EMSCRIPTEN)
  add_executable(jpwasm_wasm
    wasm/jp8000_wasm.cpp
    ${CORE_SOURCES}
  )

  target_compile_options(jpwasm_wasm PRIVATE
    -O3
    -flto
    -msimd128
    -DNDEBUG
    -fomit-frame-pointer
    -fno-rtti
  )

  target_link_options(jpwasm_wasm PRIVATE
    -O3
    -flto
    -msimd128
    -sWASM=1
    -sMODULARIZE=1
    -sEXPORT_NAME=JP8000WasmModule
    -sENVIRONMENT=web,worker
    -sALLOW_MEMORY_GROWTH=1
    -sFILESYSTEM=0
    -sASSERTIONS=0
    -sSAFE_HEAP=0
    -sSTACK_OVERFLOW_CHECK=0
    -sNO_EXIT_RUNTIME=1
    -sEXPORTED_FUNCTIONS=[_jp8_create,_jp8_destroy,_jp8_generate_samples,_jp8_lcd_width,_jp8_lcd_height,_jp8_render_lcd,_jp8_copy_lcd_pixels,_jp8_press_button,_jp8_release_button,_jp8_provide_midi,_jp8_build_esp_wasm_snapshot,_jp8_get_esp_wasm_snapshot_ptr,_jp8_get_esp_wasm_snapshot_size,_jp8_fill_esp_wasm_runtime_ptrs,_jp8_run_esp_wasm_reference_once,_jp8_esp_coredata_size,_jp8_reset_jit_scheduler,_jp8_step_host_and_count_dsp_samples,_jp8_get_esp_program_dirty_mask,_malloc,_free]
    -sEXPORTED_RUNTIME_METHODS=[HEAPU8,HEAP32,HEAPU32,wasmMemory]
  )

  set_target_properties(jpwasm_wasm PROPERTIES
    OUTPUT_NAME "jp8000_wasm"
    SUFFIX ".js"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/../public"
  )

  if(JPWASM_ENABLE_BINARYEN)
    # The submodule is included so we can progressively migrate ESP JIT lowering to Binaryen.
    # We don't link it into the wasm target yet; this keeps current behavior stable.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/binaryen/CMakeLists.txt")
      # Emscripten cross-compilation can't auto-detect pthreads; pre-satisfy FindThreads.
      set(CMAKE_THREAD_LIBS_INIT "-lpthread")
      set(CMAKE_HAVE_LIBC_PTHREAD ON)
      set(THREADS_PREFER_PTHREAD_FLAG OFF)
      set(BUILD_TESTS OFF CACHE BOOL "Build Binaryen tests" FORCE)
      set(BUILD_FUZZTEST OFF CACHE BOOL "Build Binaryen fuzz tests" FORCE)
      set(BUILD_TOOLS OFF CACHE BOOL "Build Binaryen tools" FORCE)
      set(BUILD_STATIC_LIB ON CACHE BOOL "Build Binaryen static library" FORCE)
      add_subdirectory(binaryen EXCLUDE_FROM_ALL)
      target_compile_definitions(jpwasm_wasm PRIVATE JPWASM_USE_BINARYEN=1)
      target_include_directories(jpwasm_wasm PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/binaryen/src")
      target_link_libraries(jpwasm_wasm PRIVATE binaryen)
    else()
      message(WARNING "Binaryen submodule not initialized. Run: git submodule update --init --recursive")
    endif()
  endif()
else()
  # Build a fat binary that runs on both Intel and Apple Silicon.
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "OS X Architectures")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-deprecated-anon-enum-enum-conversion -Wno-bitwise-instead-of-logical -O3")
  set(BUILD_SHARED_LIBS OFF)

  if(CMAKE_GENERATOR STREQUAL Xcode AND XCODE_VERSION VERSION_GREATER_EQUAL 14.0.0)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum OS X deployment version")
  else()
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.9" CACHE STRING "Minimum OS X deployment version")
  endif()

  set(ASMJIT_STATIC TRUE)
  add_subdirectory(asmjit)

  set(SDL_STATIC ON)
  add_subdirectory(SDL)
  set(SDL2_INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/SDL/include)
  set(SDL2_LIBRARY SDL2::SDL2-static)

  add_executable(jpwasm_desktop
    desktop/main_sdl.cpp
    ${CORE_SOURCES}
    ${ESP_SOURCES}
  )

  target_include_directories(jpwasm_desktop PRIVATE ${SDL2_INCLUDE_DIR})
  target_compile_definitions(jpwasm_desktop PRIVATE ESP_ENABLE_ASMJIT=1)
  target_link_libraries(jpwasm_desktop PRIVATE ${SDL2_LIBRARY} asmjit::asmjit)
endif()
