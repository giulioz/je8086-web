#pragma once

#include "esp.hpp"
#include "esp_opt.hpp"
#include "h8s.hpp"
#include "lcd_renderer.hpp"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <vector>

class MultiAsic : public H8SDevice {
public:
    MultiAsic() : asic0_opt_(&asic0_), asic1_opt_(&asic1_), asic2_opt_(&asic2_), asic3_opt_(&asic3_) {}

    void setPostSample(std::function<void(int32_t, int32_t)> postSample) {
        postSample_ = std::move(postSample);
    }

    uint8_t read(uint32_t address) override {
        const int asic = (address >> 14) & 3;
        address &= 0x3fff;
        if (asic == 0) return asic0_.readuC(address);
        if (asic == 1) return asic1_.readuC(address);
        if (asic == 2) return asic2_.readuC(address);
        return asic3_.readuC(address);
    }

    void write(uint32_t address, uint8_t value) override {
        const int asic = (address >> 14) & 3;
        address &= 0x3fff;
        if (asic == 0) asic0_.writeuC(address, value);
        else if (asic == 1) asic1_.writeuC(address, value);
        else if (asic == 2) asic2_.writeuC(address, value);
        else asic3_.writeuC(address, value);
    }

    void runForCycles(uint64_t cycles) {
        uint64_t diff = cycles + cyclesResidual_ - lastCycles_;
        lastCycles_ = cycles;

        uint64_t samples = diff / (768 / 2);
        cyclesResidual_ = diff % (768 / 2);

        for (uint64_t i = 0; i < samples; i++) {
            // INTERPRETER:
			for (size_t j = 0; j < (768/2); j++) asic0_.step_cores();
			for (size_t j = 0; j < (768/2); j++) asic1_.step_cores();
			for (size_t j = 0; j < (768/2); j++) asic2_.step_cores();
			for (size_t j = 0; j < (768/2); j++) asic3_.step_cores();

			// JIT:
			// asic0_opt_.genProgramIfDirty();
			// asic1_opt_.genProgramIfDirty();
			// asic2_opt_.genProgramIfDirty();
			// asic3_opt_.genProgramIfDirty();

			// asic0_opt_.callOptimized(&asic0_);
			// asic1_opt_.callOptimized(&asic1_);
			// asic2_opt_.callOptimized(&asic2_);
			// asic3_opt_.callOptimized(&asic3_);

            postSample_(asic3_.readGRAM(0xe8), asic3_.readGRAM(0xec));

            for (int k = 0; k <= 0x4; k += 2) asic1_.writeGRAM(asic0_.readGRAM(0x80 + k), k);
            for (int k = 0; k <= 0xa; k += 2) asic2_.writeGRAM(asic1_.readGRAM(0x80 + k), k);
            for (int k = 0; k <= 0xe; k += 2) asic3_.writeGRAM(asic2_.readGRAM(0x80 + k), k);

            asic3_.writeGRAM(asic2_.readGRAM(0xa0), 0x20);
            asic3_.writeGRAM(asic2_.readGRAM(0xa2), 0x22);

            asic0_.sync_cores();
            asic1_.sync_cores();
            asic2_.sync_cores();
            asic3_.sync_cores();
        }
    }

    std::vector<uint8_t> buildEspWasmSnapshot(int asic, int core) {
        if (core != 0 && core != 1) {
            return {};
        }

        switch (asic) {
            case 0: return asic0_opt_.buildWasmSnapshotModule(static_cast<uint32_t>(core));
            case 1: return asic1_opt_.buildWasmSnapshotModule(static_cast<uint32_t>(core));
            case 2: return asic2_opt_.buildWasmSnapshotModule(static_cast<uint32_t>(core));
            case 3: return asic3_opt_.buildWasmSnapshotModule(static_cast<uint32_t>(core));
            default: return {};
        }
    }

    bool getEspWasmRuntimePointers(int asic, int core, uint32_t* outPtrs, size_t outCount) {
        if (outPtrs == nullptr || outCount < 12) {
            return false;
        }
        if (core != 0 && core != 1) {
            return false;
        }

        uintptr_t temp[12] = {};
        bool ok = false;
        switch (asic) {
            case 0: ok = asic0_opt_.getWasmRuntimePointers(static_cast<uint32_t>(core), temp, std::size(temp)); break;
            case 1: ok = asic1_opt_.getWasmRuntimePointers(static_cast<uint32_t>(core), temp, std::size(temp)); break;
            case 2: ok = asic2_opt_.getWasmRuntimePointers(static_cast<uint32_t>(core), temp, std::size(temp)); break;
            case 3: ok = asic3_opt_.getWasmRuntimePointers(static_cast<uint32_t>(core), temp, std::size(temp)); break;
            default: return false;
        }
        if (!ok) {
            return false;
        }

        for (size_t i = 0; i < 12; ++i) {
            outPtrs[i] = static_cast<uint32_t>(temp[i]);
        }
        return true;
    }

    bool runEspWasmReferenceOnce(int asic, int core) {
        if (core != 0 && core != 1) {
            return false;
        }
        switch (asic) {
            case 0: return asic0_opt_.runWasmReferenceCoreOnce(static_cast<uint32_t>(core));
            case 1: return asic1_opt_.runWasmReferenceCoreOnce(static_cast<uint32_t>(core));
            case 2: return asic2_opt_.runWasmReferenceCoreOnce(static_cast<uint32_t>(core));
            case 3: return asic3_opt_.runWasmReferenceCoreOnce(static_cast<uint32_t>(core));
            default: return false;
        }
    }

    void syncJitCoefsIfDirty() {
        asic0_opt_.syncCoefsIfDirty();
        asic1_opt_.syncCoefsIfDirty();
        asic2_opt_.syncCoefsIfDirty();
        asic3_opt_.syncCoefsIfDirty();
    }

    uint32_t getProgramDirtyMask() const {
        uint32_t mask = 0;
        if (asic0_.programDirty) mask |= 1u << 0;
        if (asic1_.programDirty) mask |= 1u << 1;
        if (asic2_.programDirty) mask |= 1u << 2;
        if (asic3_.programDirty) mask |= 1u << 3;
        return mask;
    }

private:
    esp::ESP<17> asic0_;
    esp::ESP<0> asic1_, asic2_;
    esp::ESP<19> asic3_;
    
    esp::ESPOptimizer<17> asic0_opt_;
    esp::ESPOptimizer<0> asic1_opt_, asic2_opt_;
    esp::ESPOptimizer<19> asic3_opt_;

    std::function<void(int32_t, int32_t)> postSample_;
    uint64_t lastCycles_ = 0;
    uint64_t cyclesResidual_ = 0;
};

enum FaderType {
    kFader_PitchBend = 0,
    kFader_ModWheel = 8,
    kFader_Expression,
    kFader_BattSense = 11,
    kFader_Osc2Range = 16,
    kFader_Osc2PwmDepth,
    kFader_TvfResonance,
    kFader_Osc2PulseWidth,
    kFader_Bass,
    kFader_TvfFreq,
    kFader_Treble,
    kFader_TvfEnvA,
    kFader_Chorus = 24,
    kFader_TvfEnvS,
    kFader_TvfEnvR,
    kFader_TvfEnvD,
    kFader_DelayTime,
    kFader_TvaEnvA,
    kFader_DelayFb,
    kFader_DelayLevel,
    kFader_TvfEnvDepth = 32,
    kFader_TvfLFO1,
    kFader_TvfKeyFollow,
    kFader_TvaLevel,
    kFader_TvaEnvD,
    kFader_TvaLFO1Depth,
    kFader_TvaEnvS,
    kFader_TvaEnvR,
    kFader_Osc1Ctrl2 = 40,
    kFader_Osc1Ctrl1 = 44,
    kFader_FineTune = 46,
    kFader_Tempo = 48,
    kFader_PortaTime,
    kFader_LFO2Depth,
    kFader_LFO2Rate,
    kFader_Ribbon1,
    kFader_Ribbon2,
    kFader_Unused1,
    kFader_Unused2,
    kFader_LFO1Rate = 56,
    kFader_LFO1Fade,
    kFader_OscBal,
    kFader_XModDepth,
    kFader_LFO1Depth,
    kFader_PitchEnvDepth,
    kFader_PitchEnvA,
    kFader_PitchEnvD
};

class Faders : public H8SDevice {
public:
    Faders() {
        for (int i = 0; i < 64; i++) values_[i] = 512;
        values_[kFader_BattSense] = 512;
    }

    uint8_t read(uint32_t address) override {
        if (address == 0xffffcb) return p6dr_;
        if (address == 0xffffe8) return adcsr_;
        if (address >= 0xffffe0 && address < 0xffffe8) {
            int off = (address - 0xffffe0) & 7;
            int which = scanning_ | ((off << 2) & 0x18);
            if ((which & 0x38) == 0) which = 0;
            if ((which & 0x38) == 8) which &= 0x3b;
            if ((which & 0x3C) == 40) which = 40;
            if ((which & 0x3e) == 44) which = 44;
            if ((which & 0x3e) == 46) which = 46;

            int val = values_[which] & 1023;
            return (off & 1) ? ((val << 6) & 0xc0) : ((val >> 2) & 0xff);
        }
        return 0;
    }

    void write(uint32_t address, uint8_t value) override {
        if (address == 0xffffcb) {
            scanning_ = ((value & 7)) | (scanning_ & 32);
            p6dr_ = value;
        }
        if (address == 0xffffe8) {
            scanning_ = (scanning_ & 7) | ((value << 3) & 32);
            adcsr_ = value;
        }
    }

    void setFader(int which, int value) {
        values_[which] = value;
    }

private:
    int8 scanning_ {0};
    int8 p6dr_ {0};
    int8 adcsr_ {0};
    int values_[64] {};
};

enum SwitchType {
    kSwitch_Sync = 64,
    kSwitch_Osc2Waveform,
    kSwitch_PanLfo,
    kSwitch_Exit,
    kSwitch_PerformSel = 72,
    kSwitch_3,
    kSwitch_6,
    kSwitch_Write,
    kSwitch_ValueUp = 80,
    kSwitch_2,
    kSwitch_5,
    kSwitch_8,
    kSwitch_TVF24,
    kSwitch_ValueDown = 88,
    kSwitch_1,
    kSwitch_4,
    kSwitch_7,
    kSwitch_TVFType,
    kSwitch_Upper = 128,
    kSwitch_KeyMode,
    kSwitch_Rec,
    kSwitch_Hold,
    kSwitch_Range,
    kSwitch_OnOff,
    kSwitch_Mode,
    kSwitch_Lower = 136,
    kSwitch_Motion2,
    kSwitch_Motion1,
    kSwitch_Osc1Waveform,
    kSwitch_LFO1Destination,
    kSwitch_Ring,
    kSwitch_LFO1Waveform,
    kSwitch_Portamento = 160,
    kSwitch_OctaveUp,
    kSwitch_RibbonAssign,
    kSwitch_Mono = 168,
    kSwitch_OctaveDown,
    kSwitch_VelocityAssign,
    kSwitch_BendRange = 176,
    kSwitch_Relative,
    kSwitch_VelocityOnOff,
    kSwitch_Hold2 = 185,
    kSwitch_DepthSelect
};

class KeyScanner : public H8SDevice {
public:
    uint8_t read(uint32_t address) override {
        (void)address;
        return 0;
    }

    void write(uint32_t address, uint8_t value) override {
        (void)address;
        (void)value;
    }
};

class Port : public H8SDevice {
public:
    Port(std::function<void(Port*)>&& onLedsChanged = [](Port*){}) : onLedsChanged_(std::move(onLedsChanged)) {
        releaseAll();
    }

    void releaseAll() {
        for (int i = 0; i < 32; i++) data_[i] = 0xff;
    }

    void press(int which, bool down = true) {
        int bit = 1 << (which & 7);
        which >>= 3;
        if (down) data_[which] &= ~bit;
        else data_[which] |= bit;
    }

    uint8_t read(uint32_t address) override {
        if (address == 0xffffd4) return portBDDR_;
        char id = (address == 0xffffd3) ? 'A' : 'B';
        int which = portAstate_ & 31;
        return (id == 'A') ? portAstate_ : data_[which];
    }

    void write(uint32_t address, uint8_t value) override {
        if (address == 0xffffd4) {
            portBDDR_ = value;
            return;
        }
        char id = (address == 0xffffd3) ? 'A' : 'B';
        if (id == 'A') {
            if (value & 32) latchA_ = true;
            if (!(value & 32) && latchA_) {
                latch_ = value & 31;
                latchA_ = false;
                bool diff = leds_[latch_] != portBDR_;
                leds_[latch_] = portBDR_;
                if (diff) onLedsChanged_(this);
            }
            portAstate_ = value;
        }
        if (id == 'B') portBDR_ = value;
    }

    static int getLedId(uint32_t index) {
        return lits[index];
    }

    bool getLed(uint32_t i) const {
        const int w = getLedId(i);
        return (leds_[w >> 3] & (1 << (w & 7)));
    }

private:
    static int lits[66];

    int8 data_[32] {};
    int8 leds_[32] {};
    int latch_ {0};
    bool latchA_ {false};
    int8 portAstate_ {0};
    int8 portBDDR_ {0};
    int8 portBDR_ {};
    std::function<void(Port*)> onLedsChanged_;
};

class LCD : public H8SDevice {
public:
    explicit LCD(LcdRenderer* lcd) : lcd_(lcd) {}

    uint8_t read(uint32_t address) override {
        (void)address;
        return 0x00;
    }

    void write(uint32_t address, uint8_t value) override {
        lcd_->write((address & 1), value);
    }

private:
    LcdRenderer* lcd_;
};
