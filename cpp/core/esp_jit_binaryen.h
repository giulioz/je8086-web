#pragma once

#include "esp_jit.h"

#include <cstdint>
#include <vector>

namespace esp {

class EspJitBinaryen : public EspJitBase {
public:
    EspJitBinaryen();

    void jitEnter() override;
    void jitExit() override;

    void eramRead(uint32_t eramMask) override;
    void eramWrite(uint32_t eramMask) override;
    void eramComputeAddr(uint32_t immOffset, bool highOffset, bool shouldUseVarOffset) override;

    void emitOp(uint32_t pc, const ESPOptInstr& instr, bool lastMul30) override;

    std::vector<uint8_t> finalizeModuleBytes();

private:
    enum class EramEventType : uint8_t {
        Read,
        Write,
        ComputeAddr
    };

    struct EramEvent {
        EramEventType type = EramEventType::Read;
        uint32_t sequence = 0;
        uint32_t eramMask = 0;
        uint32_t immOffset = 0;
        bool highOffset = false;
        bool useVarOffset = false;
    };

    struct RecordedOp {
        uint32_t sequence = 0;
        uint32_t pc = 0;
        ESPOptInstr instr {};
        bool lastMul30 = false;
    };

    std::vector<EramEvent> eramEvents_;
    std::vector<RecordedOp> recordedOps_;
    uint32_t nextSequence_ = 0;
};

} // namespace esp
