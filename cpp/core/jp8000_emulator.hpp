#pragma once

#include "h8sdevices.hpp"
#include "jp8000_devices.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

// Keep this object heap-allocated in hosts: it owns a large emulation state.
class JP8000Emulator {
public:
    JP8000Emulator(std::vector<uint8_t> romData, std::vector<uint8_t> ramDumpData, std::function<void(int32_t, int32_t)> postSample);

    void step();
    void resetJitScheduler();
    int32_t stepHostAndCountDspSamples();

    void pressButton(SwitchType which);
    void releaseButton(SwitchType which);
    void releaseAllButtons();
    void provideMIDI(const uint8_t* data, size_t len);

    void setFader(FaderType which, int value);

    void renderLcd();
    const std::vector<uint32_t>& lcdPixels() const;
    std::vector<uint8_t> buildEspWasmSnapshot(int asic, int core);
    bool getEspWasmRuntimePointers(int asic, int core, uint32_t* outPtrs, size_t outCount);
    bool runEspWasmReferenceOnce(int asic, int core);
    uint32_t getEspProgramDirtyMask() const;
    static constexpr int espCoreDataSize() { return static_cast<int>(sizeof(esp::CoreData)); }

    static constexpr int lcdWidth() { return LcdRenderer::kWidth; }
    static constexpr int lcdHeight() { return LcdRenderer::kHeight; }

private:
    H8SEmulator emu_;
    MultiAsic asics_;
    LcdRenderer lcdRenderer_;
    LCD lcd_;
    Port ports_;
    Faders faders_;
    HWRegs hwregs_;
    Serial midi_;
    KeyScanner keyScanner_;
    Timers timers_;
    CatchAllDevice catchAll_;
    uint64_t jitLastCycles_ = 0;
    uint64_t jitCyclesResidual_ = 0;
};
