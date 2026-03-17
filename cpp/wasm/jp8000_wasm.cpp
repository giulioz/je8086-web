#include "../core/jp8000_emulator.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include <emscripten/emscripten.h>

namespace {

struct SampleFrame {
    int32_t left = 0;
    int32_t right = 0;
};

struct WasmEmulator {
    explicit WasmEmulator(std::vector<uint8_t> rom, std::vector<uint8_t> ram)
        : emulator(std::make_unique<JP8000Emulator>(
              std::move(rom),
              std::move(ram),
              [this](int32_t l, int32_t r) {
                  sampleQueue.push_back({l, r});
              })) {}

    std::unique_ptr<JP8000Emulator> emulator;
    std::deque<SampleFrame> sampleQueue;
    std::vector<uint8_t> espWasmSnapshot;
};

} // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE
WasmEmulator* jp8_create(const uint8_t* rom, size_t romSize, const uint8_t* ram, size_t ramSize) {
    if (rom == nullptr || romSize == 0) {
        return nullptr;
    }

    std::vector<uint8_t> romVec(rom, rom + romSize);
    std::vector<uint8_t> ramVec;
    if (ram != nullptr && ramSize > 0) {
        ramVec.assign(ram, ram + ramSize);
    }

    return new WasmEmulator(std::move(romVec), std::move(ramVec));
}

EMSCRIPTEN_KEEPALIVE
void jp8_destroy(WasmEmulator* handle) {
    delete handle;
}

EMSCRIPTEN_KEEPALIVE
int32_t jp8_generate_samples(WasmEmulator* handle, int32_t* outInterleaved, int32_t frames) {
    if (handle == nullptr || outInterleaved == nullptr || frames <= 0) {
        return 0;
    }

    int32_t produced = 0;
    while (produced < frames) {
        while (handle->sampleQueue.empty()) {
            handle->emulator->step();
        }

        const SampleFrame frame = handle->sampleQueue.front();
        handle->sampleQueue.pop_front();
        outInterleaved[produced * 2 + 0] = frame.left;
        outInterleaved[produced * 2 + 1] = frame.right;
        produced += 1;
    }

    return produced;
}

EMSCRIPTEN_KEEPALIVE
int32_t jp8_lcd_width() {
    return JP8000Emulator::lcdWidth();
}

EMSCRIPTEN_KEEPALIVE
int32_t jp8_lcd_height() {
    return JP8000Emulator::lcdHeight();
}

EMSCRIPTEN_KEEPALIVE
void jp8_render_lcd(WasmEmulator* handle) {
    if (handle == nullptr) {
        return;
    }
    handle->emulator->renderLcd();
}

EMSCRIPTEN_KEEPALIVE
int32_t jp8_copy_lcd_pixels(WasmEmulator* handle, uint32_t* outPixels, int32_t maxPixels) {
    if (handle == nullptr || outPixels == nullptr || maxPixels <= 0) {
        return 0;
    }

    const auto& pixels = handle->emulator->lcdPixels();
    const int32_t count = std::min<int32_t>(maxPixels, static_cast<int32_t>(pixels.size()));
    for (int32_t i = 0; i < count; ++i) {
        outPixels[i] = pixels[static_cast<size_t>(i)];
    }
    return count;
}

EMSCRIPTEN_KEEPALIVE
void jp8_press_button(WasmEmulator* handle, int32_t which) {
    if (handle == nullptr) {
        return;
    }
    handle->emulator->pressButton(static_cast<SwitchType>(which));
}

EMSCRIPTEN_KEEPALIVE
void jp8_release_button(WasmEmulator* handle, int32_t which) {
    if (handle == nullptr) {
        return;
    }
    handle->emulator->releaseButton(static_cast<SwitchType>(which));
}

EMSCRIPTEN_KEEPALIVE
void jp8_provide_midi(WasmEmulator* handle, const uint8_t* data, int32_t len) {
    if (handle == nullptr || data == nullptr || len <= 0) {
        return;
    }
    handle->emulator->provideMIDI(data, static_cast<size_t>(len));
}

EMSCRIPTEN_KEEPALIVE
int32_t jp8_build_esp_wasm_snapshot(WasmEmulator* handle, int32_t asic, int32_t core) {
    if (handle == nullptr) {
        return 0;
    }

    handle->espWasmSnapshot = handle->emulator->buildEspWasmSnapshot(asic, core);
    return static_cast<int32_t>(handle->espWasmSnapshot.size());
}

EMSCRIPTEN_KEEPALIVE
const uint8_t* jp8_get_esp_wasm_snapshot_ptr(WasmEmulator* handle) {
    if (handle == nullptr || handle->espWasmSnapshot.empty()) {
        return nullptr;
    }
    return handle->espWasmSnapshot.data();
}

EMSCRIPTEN_KEEPALIVE
int32_t jp8_get_esp_wasm_snapshot_size(WasmEmulator* handle) {
    if (handle == nullptr) {
        return 0;
    }
    return static_cast<int32_t>(handle->espWasmSnapshot.size());
}

EMSCRIPTEN_KEEPALIVE
int32_t jp8_fill_esp_wasm_runtime_ptrs(WasmEmulator* handle,
                                       int32_t asic,
                                       int32_t core,
                                       uint32_t* outPtrs,
                                       int32_t maxPtrs) {
    if (handle == nullptr || outPtrs == nullptr || maxPtrs < 12) {
        return 0;
    }
    const bool ok = handle->emulator->getEspWasmRuntimePointers(asic, core, outPtrs, static_cast<size_t>(maxPtrs));
    return ok ? 12 : 0;
}

EMSCRIPTEN_KEEPALIVE
int32_t jp8_run_esp_wasm_reference_once(WasmEmulator* handle, int32_t asic, int32_t core) {
    if (handle == nullptr) {
        return 0;
    }
    return handle->emulator->runEspWasmReferenceOnce(asic, core) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
int32_t jp8_esp_coredata_size() {
    return JP8000Emulator::espCoreDataSize();
}

EMSCRIPTEN_KEEPALIVE
void jp8_reset_jit_scheduler(WasmEmulator* handle) {
    if (handle == nullptr) {
        return;
    }
    handle->emulator->resetJitScheduler();
}

EMSCRIPTEN_KEEPALIVE
int32_t jp8_step_host_and_count_dsp_samples(WasmEmulator* handle) {
    if (handle == nullptr) {
        return 0;
    }
    return handle->emulator->stepHostAndCountDspSamples();
}

EMSCRIPTEN_KEEPALIVE
uint32_t jp8_get_esp_program_dirty_mask(WasmEmulator* handle) {
    if (handle == nullptr) {
        return 0;
    }
    return handle->emulator->getEspProgramDirtyMask();
}

} // extern "C"
