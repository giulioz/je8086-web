#include "jp8000_emulator.hpp"

#include <algorithm>
#include <stdexcept>

JP8000Emulator::JP8000Emulator(std::vector<uint8_t> romData, std::vector<uint8_t> ramDumpData, std::function<void(int32_t, int32_t)> postSample)
    : lcd_(&lcdRenderer_),
      ports_([](Port*) {}),
      midi_(0, [](uint8_t) {}) {
    if (romData.empty()) {
        throw std::invalid_argument("ROM data must not be empty");
    }

    emu_.loadmem(romData.data(), static_cast<int>(romData.size()), 0);
    if (!ramDumpData.empty()) {
        emu_.loadmem(ramDumpData.data(), static_cast<int>(ramDumpData.size()), 0x200000);
    }

    emu_.memmap(&catchAll_, static_cast<int>(romData.size()), 0x200000 - static_cast<int>(romData.size()));
    emu_.memmap(&catchAll_, 0x240000, 0xFFFD10 - 0x240000);
    emu_.memmap(&catchAll_, 0xFFFF10, 0xFFFF1C - 0xFFFF10);
    emu_.memmap(&catchAll_, 0x410000, 0x600000 - 0x410000);
    emu_.memmap(&asics_, 0x400000, 0x10000);
    emu_.memmap(&keyScanner_, 0x600000, 4);
    emu_.memmap(&lcd_, 0x600004, 2);
    emu_.memmap(&hwregs_, 0xffff1c, 0xe4);
    emu_.memmap(&faders_, 0xffffe0, 9);
    emu_.memmap(&faders_, 0xffffcb, 1);
    emu_.memmap(&timers_, 0xffff60, 64);
    emu_.memmap(&ports_, 0xffffd3, 2);
    emu_.memmap(&ports_, 0xffffd6, 1);
    emu_.memmap(&midi_, 0xffffb0, 6);

    asics_.setPostSample(postSample);
    emu_.boot();
}

void JP8000Emulator::pressButton(SwitchType which) {
    ports_.press(which, true);
}

void JP8000Emulator::releaseButton(SwitchType which) {
    ports_.press(which, false);
}

void JP8000Emulator::releaseAllButtons() {
    ports_.releaseAll();
}

void JP8000Emulator::provideMIDI(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) {
        return;
    }
    midi_.provideMIDI(data, len);
}

void JP8000Emulator::setFader(FaderType which, int value) {
    faders_.setFader(which, std::clamp(value, 0, 1023));
}

void JP8000Emulator::renderLcd() {
    lcdRenderer_.render();
}

const std::vector<uint32_t>& JP8000Emulator::lcdPixels() const {
    return lcdRenderer_.pixels();
}

std::vector<uint8_t> JP8000Emulator::buildEspWasmSnapshot(int asic, int core) {
    return asics_.buildEspWasmSnapshot(asic, core);
}

bool JP8000Emulator::getEspWasmRuntimePointers(int asic, int core, uint32_t* outPtrs, size_t outCount) {
    return asics_.getEspWasmRuntimePointers(asic, core, outPtrs, outCount);
}

bool JP8000Emulator::runEspWasmReferenceOnce(int asic, int core) {
    return asics_.runEspWasmReferenceOnce(asic, core);
}

uint32_t JP8000Emulator::getEspProgramDirtyMask() const {
    return asics_.getProgramDirtyMask();
}

void JP8000Emulator::resetJitScheduler() {
    jitLastCycles_ = emu_.getCycles() * 1323 / 625;
    jitCyclesResidual_ = 0;
}

int32_t JP8000Emulator::stepHostAndCountDspSamples() {
    emu_.step();
    timers_.tick();
    midi_.tick();
    asics_.syncJitCoefsIfDirty();

    const uint64_t cycles = emu_.getCycles() * 1323 / 625;
    const uint64_t diff = cycles + jitCyclesResidual_ - jitLastCycles_;
    jitLastCycles_ = cycles;

    constexpr uint64_t kCyclesPerDspSample = 768 / 2;
    jitCyclesResidual_ = diff % kCyclesPerDspSample;
    return static_cast<int32_t>(diff / kCyclesPerDspSample);
}

void JP8000Emulator::step() {
    emu_.step();
    timers_.tick();
    midi_.tick();
    asics_.runForCycles(emu_.getCycles() * 1323 / 625);
}
