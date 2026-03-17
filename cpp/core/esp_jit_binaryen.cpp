#include "esp_jit_binaryen.h"

#if defined(JPWASM_USE_BINARYEN)
#include "../binaryen/src/binaryen-c.h"
#endif

#include <array>
#include <cstddef>
#include <cstdlib>

namespace esp {

EspJitBinaryen::EspJitBinaryen() = default;

void EspJitBinaryen::jitEnter() {
    eramEvents_.clear();
    recordedOps_.clear();
    nextSequence_ = 0;
}

void EspJitBinaryen::jitExit() {
}

void EspJitBinaryen::eramRead(uint32_t eramMask) {
    EramEvent ev;
    ev.type = EramEventType::Read;
    ev.sequence = nextSequence_++;
    ev.eramMask = eramMask;
    eramEvents_.push_back(ev);
}

void EspJitBinaryen::eramWrite(uint32_t eramMask) {
    EramEvent ev;
    ev.type = EramEventType::Write;
    ev.sequence = nextSequence_++;
    ev.eramMask = eramMask;
    eramEvents_.push_back(ev);
}

void EspJitBinaryen::eramComputeAddr(uint32_t immOffset, bool highOffset, bool shouldUseVarOffset) {
    EramEvent ev;
    ev.type = EramEventType::ComputeAddr;
    ev.sequence = nextSequence_++;
    ev.immOffset = immOffset;
    ev.highOffset = highOffset;
    ev.useVarOffset = shouldUseVarOffset;
    eramEvents_.push_back(ev);
}

void EspJitBinaryen::emitOp(uint32_t pc, const ESPOptInstr& instr, bool lastMul30) {
    RecordedOp op;
    op.sequence = nextSequence_++;
    op.pc = pc;
    op.instr = instr;
    op.lastMul30 = lastMul30;
    recordedOps_.push_back(op);
}

std::vector<uint8_t> EspJitBinaryen::finalizeModuleBytes() {
#if !defined(JPWASM_USE_BINARYEN)
    return {};
#else
    BinaryenModuleRef module = BinaryenModuleCreate();
    BinaryenSetOptimizeLevel(0);
    BinaryenSetShrinkLevel(0);

    auto i32c = [&](int32_t value) -> BinaryenExpressionRef {
        return BinaryenConst(module, BinaryenLiteralInt32(value));
    };

    auto i64c = [&](int64_t value) -> BinaryenExpressionRef {
        return BinaryenConst(module, BinaryenLiteralInt64(value));
    };

    auto get32 = [&](BinaryenIndex idx) -> BinaryenExpressionRef {
        return BinaryenLocalGet(module, idx, BinaryenTypeInt32());
    };

    auto get64 = [&](BinaryenIndex idx) -> BinaryenExpressionRef {
        return BinaryenLocalGet(module, idx, BinaryenTypeInt64());
    };

    auto add32 = [&](BinaryenExpressionRef a, BinaryenExpressionRef b) -> BinaryenExpressionRef {
        return BinaryenBinary(module, BinaryenAddInt32(), a, b);
    };

    auto add64 = [&](BinaryenExpressionRef a, BinaryenExpressionRef b) -> BinaryenExpressionRef {
        return BinaryenBinary(module, BinaryenAddInt64(), a, b);
    };

    auto sub32 = [&](BinaryenExpressionRef a, BinaryenExpressionRef b) -> BinaryenExpressionRef {
        return BinaryenBinary(module, BinaryenSubInt32(), a, b);
    };

    auto mul32 = [&](BinaryenExpressionRef a, BinaryenExpressionRef b) -> BinaryenExpressionRef {
        return BinaryenBinary(module, BinaryenMulInt32(), a, b);
    };

    auto and32 = [&](BinaryenExpressionRef a, BinaryenExpressionRef b) -> BinaryenExpressionRef {
        return BinaryenBinary(module, BinaryenAndInt32(), a, b);
    };

    auto xor32 = [&](BinaryenExpressionRef a, BinaryenExpressionRef b) -> BinaryenExpressionRef {
        return BinaryenBinary(module, BinaryenXorInt32(), a, b);
    };

    auto shl32 = [&](BinaryenExpressionRef a, BinaryenExpressionRef b) -> BinaryenExpressionRef {
        return BinaryenBinary(module, BinaryenShlInt32(), a, b);
    };

    auto shrS32 = [&](BinaryenExpressionRef a, BinaryenExpressionRef b) -> BinaryenExpressionRef {
        return BinaryenBinary(module, BinaryenShrSInt32(), a, b);
    };

    auto ltS32 = [&](BinaryenExpressionRef a, BinaryenExpressionRef b) -> BinaryenExpressionRef {
        return BinaryenBinary(module, BinaryenLtSInt32(), a, b);
    };

    auto gtS32 = [&](BinaryenExpressionRef a, BinaryenExpressionRef b) -> BinaryenExpressionRef {
        return BinaryenBinary(module, BinaryenGtSInt32(), a, b);
    };

    auto geS32 = [&](BinaryenExpressionRef a, BinaryenExpressionRef b) -> BinaryenExpressionRef {
        return BinaryenBinary(module, BinaryenGeSInt32(), a, b);
    };

    auto ltS64 = [&](BinaryenExpressionRef a, BinaryenExpressionRef b) -> BinaryenExpressionRef {
        return BinaryenBinary(module, BinaryenLtSInt64(), a, b);
    };

    auto gtS64 = [&](BinaryenExpressionRef a, BinaryenExpressionRef b) -> BinaryenExpressionRef {
        return BinaryenBinary(module, BinaryenGtSInt64(), a, b);
    };

    auto eq32 = [&](BinaryenExpressionRef a, BinaryenExpressionRef b) -> BinaryenExpressionRef {
        return BinaryenBinary(module, BinaryenEqInt32(), a, b);
    };

    auto localSet = [&](std::vector<BinaryenExpressionRef>& exprs, BinaryenIndex idx, BinaryenExpressionRef value) {
        exprs.push_back(BinaryenLocalSet(module, idx, value));
    };

    auto se24 = [&](BinaryenExpressionRef in) -> BinaryenExpressionRef {
        return shrS32(shl32(in, i32c(8)), i32c(8));
    };

    auto satAcc64ToReadAcc32 = [&](BinaryenExpressionRef acc64, bool unsat) -> BinaryenExpressionRef {
        if (unsat) {
            return BinaryenUnary(module, BinaryenWrapInt64(), acc64);
        }

        BinaryenExpressionRef lo = BinaryenSelect(module,
                                                  ltS64(acc64, i64c(-0x800000)),
                                                  i64c(-0x800000),
                                                  acc64);
        BinaryenExpressionRef hi = BinaryenSelect(module,
                                                  gtS64(lo, i64c(0x7fffff)),
                                                  i64c(0x7fffff),
                                                  lo);
        return BinaryenUnary(module, BinaryenWrapInt64(), hi);
    };

    auto load32At = [&](BinaryenExpressionRef addr) -> BinaryenExpressionRef {
        return BinaryenLoad(module, 4, true, 0, 4, BinaryenTypeInt32(), addr, "mem");
    };

    auto load64At = [&](BinaryenExpressionRef addr) -> BinaryenExpressionRef {
        return BinaryenLoad(module, 8, false, 0, 8, BinaryenTypeInt64(), addr, "mem");
    };

    auto load8SAt = [&](BinaryenExpressionRef addr) -> BinaryenExpressionRef {
        return BinaryenLoad(module, 1, true, 0, 1, BinaryenTypeInt32(), addr, "mem");
    };
    auto load8UAt = [&](BinaryenExpressionRef addr) -> BinaryenExpressionRef {
        return BinaryenLoad(module, 1, false, 0, 1, BinaryenTypeInt32(), addr, "mem");
    };

    auto store32At = [&](std::vector<BinaryenExpressionRef>& exprs,
                         BinaryenExpressionRef addr,
                         BinaryenExpressionRef value) {
        exprs.push_back(BinaryenStore(module, 4, 0, 4, addr, value, BinaryenTypeInt32(), "mem"));
    };

    auto store64At = [&](std::vector<BinaryenExpressionRef>& exprs,
                         BinaryenExpressionRef addr,
                         BinaryenExpressionRef value) {
        exprs.push_back(BinaryenStore(module, 8, 0, 8, addr, value, BinaryenTypeInt64(), "mem"));
    };

    enum Param : BinaryenIndex {
        PCoreData = 0,
        PIram,
        PGram,
        PEramPos,
        PIramPos,
        PEramEffectiveAddr,
        PEramWriteLatchNext,
        PEramReadLatch,
        PEramWriteLatch,
        PEramVarOffset,
        PLastMulA,
        PLastMulB,
    };

    constexpr BinaryenIndex kNumParams = PLastMulB + 1;

    enum Local : BinaryenIndex {
        LMemPos = kNumParams,
        LReadAcc,
        LMulInA,
        LMulInB,
        LLastMulA,
        LLastMulB,
        LEramEffectiveAddr,
        LEramWriteLatchNext,
        LEramReadLatch,
        LEramWriteLatch,
        LEramVarOffset,
        LCondition,
        LIramPos,
        LEramPos,
        LShouldJump,
        LSignature,
        LAcc0,
        LAcc1,
        LAcc2,
        LAcc3,
        LAcc4,
        LAcc5,
        LMulCoef0,
        LMulCoef1,
        LMulCoef2,
        LMulCoef3,
        LMulCoef4,
        LMulCoef5,
        LMulCoef6,
        LMulCoef7,
    };

    constexpr BinaryenIndex kNumLocals = LMulCoef7 - kNumParams + 1;
    std::array<BinaryenType, kNumLocals> vars{};
    vars.fill(BinaryenTypeInt32());

    auto setLocalType = [&](BinaryenIndex local, BinaryenType ty) {
        vars[local - kNumParams] = ty;
    };

    setLocalType(LAcc0, BinaryenTypeInt64());
    setLocalType(LAcc1, BinaryenTypeInt64());
    setLocalType(LAcc2, BinaryenTypeInt64());
    setLocalType(LAcc3, BinaryenTypeInt64());
    setLocalType(LAcc4, BinaryenTypeInt64());
    setLocalType(LAcc5, BinaryenTypeInt64());

    BinaryenAddMemoryImport(module, "mem", "env", "memory", 0);

    std::vector<BinaryenExpressionRef> exprs;
    exprs.reserve(recordedOps_.size() * 12 + eramEvents_.size() * 5 + 64);

    localSet(exprs, LIramPos, load32At(get32(PIramPos)));
    localSet(exprs, LEramPos, load32At(get32(PEramPos)));
    localSet(exprs, LEramEffectiveAddr, load32At(get32(PEramEffectiveAddr)));
    localSet(exprs, LEramWriteLatchNext, load32At(get32(PEramWriteLatchNext)));
    localSet(exprs, LEramReadLatch, load32At(get32(PEramReadLatch)));
    localSet(exprs, LEramWriteLatch, load32At(get32(PEramWriteLatch)));
    localSet(exprs, LEramVarOffset, load32At(get32(PEramVarOffset)));
    localSet(exprs, LLastMulA, load32At(get32(PLastMulA)));
    localSet(exprs, LLastMulB, load32At(get32(PLastMulB)));

    auto accAddr = [&](int reg) -> BinaryenExpressionRef {
        return add32(get32(PCoreData), i32c(static_cast<int32_t>(offsetof(CoreData, accs) + reg * sizeof(int64_t))));
    };
    auto mulcoefAddr = [&](int idx) -> BinaryenExpressionRef {
        return add32(get32(PCoreData), i32c(static_cast<int32_t>(offsetof(CoreData, mulcoeffs) + idx * sizeof(int32_t))));
    };
    auto coefsAddr = [&](uint32_t pc) -> BinaryenExpressionRef {
        return add32(get32(PCoreData), i32c(static_cast<int32_t>(offsetof(CoreData, coefs) + pc)));
    };
    auto shiftAddr = [&](uint32_t pc) -> BinaryenExpressionRef {
        return add32(get32(PCoreData), i32c(static_cast<int32_t>(offsetof(CoreData, shiftAmounts) + pc)));
    };
    auto eramBasePtr = [&]() -> BinaryenExpressionRef {
        return load32At(add32(get32(PCoreData), i32c(static_cast<int32_t>(offsetof(CoreData, eramPtr)))));
    };
    auto eramIndexAddr = [&](BinaryenExpressionRef index32) -> BinaryenExpressionRef {
        return add32(eramBasePtr(), shl32(index32, i32c(2)));
    };

    localSet(exprs, LAcc0, load64At(accAddr(0)));
    localSet(exprs, LAcc1, load64At(accAddr(1)));
    localSet(exprs, LAcc2, load64At(accAddr(2)));
    localSet(exprs, LAcc3, load64At(accAddr(3)));
    localSet(exprs, LAcc4, load64At(accAddr(4)));
    localSet(exprs, LAcc5, load64At(accAddr(5)));

    localSet(exprs, LMulCoef0, load32At(mulcoefAddr(0)));
    localSet(exprs, LMulCoef1, load32At(mulcoefAddr(1)));
    localSet(exprs, LMulCoef2, load32At(mulcoefAddr(2)));
    localSet(exprs, LMulCoef3, load32At(mulcoefAddr(3)));
    localSet(exprs, LMulCoef4, load32At(mulcoefAddr(4)));
    localSet(exprs, LMulCoef5, load32At(mulcoefAddr(5)));
    localSet(exprs, LMulCoef6, load32At(mulcoefAddr(6)));
    localSet(exprs, LMulCoef7, load32At(mulcoefAddr(7)));

    localSet(exprs, LSignature, i32c(static_cast<int32_t>(2166136261u)));

    auto mix = [&](std::vector<BinaryenExpressionRef>& out, uint32_t value) {
        BinaryenExpressionRef x = xor32(get32(LSignature), i32c(static_cast<int32_t>(value)));
        BinaryenExpressionRef m = mul32(x, i32c(static_cast<int32_t>(16777619u)));
        localSet(out, LSignature, m);
    };

    auto iramIndexAddr = [&](BinaryenExpressionRef mempos) -> BinaryenExpressionRef {
        return add32(get32(PIram), shl32(and32(mempos, i32c(0xff)), i32c(2)));
    };

    auto gramIndexAddr = [&](BinaryenExpressionRef mempos) -> BinaryenExpressionRef {
        return add32(get32(PGram), shl32(and32(mempos, i32c(0xff)), i32c(2)));
    };

    auto emitEramEvent = [&](const EramEvent& ev) {
        switch (ev.type) {
        case EramEventType::Read:
            localSet(exprs,
                     LEramEffectiveAddr,
                     and32(get32(LEramEffectiveAddr), i32c(static_cast<int32_t>(ev.eramMask))));
            localSet(exprs, LEramReadLatch, load32At(eramIndexAddr(get32(LEramEffectiveAddr))));
            mix(exprs, ev.eramMask ^ 0x20001u);
            break;
        case EramEventType::Write:
            localSet(exprs,
                     LEramEffectiveAddr,
                     and32(get32(LEramEffectiveAddr), i32c(static_cast<int32_t>(ev.eramMask))));
            store32At(exprs, eramIndexAddr(get32(LEramEffectiveAddr)), get32(LEramWriteLatchNext));
            mix(exprs, ev.eramMask ^ 0x20002u);
            break;
        case EramEventType::ComputeAddr: {
            localSet(exprs, LEramWriteLatchNext, get32(LEramWriteLatch));
            BinaryenExpressionRef addr = add32(get32(LEramPos), i32c(static_cast<int32_t>(ev.immOffset)));
            if (ev.useVarOffset) {
                addr = add32(addr, shrS32(get32(LEramVarOffset), i32c(12)));
            }
            if (ev.highOffset) {
                BinaryenExpressionRef off = BinaryenSelect(module,
                                                           BinaryenBinary(module, BinaryenLeSInt32(), get32(LEramPos), i32c(0x4000)),
                                                           i32c(0x40000),
                                                           i32c(0xc0000));
                addr = add32(addr, off);
            }
            localSet(exprs, LEramEffectiveAddr, addr);
            mix(exprs, ev.immOffset ^ 0x20003u);
        } break;
        }
    };

    auto accLocal = [](int reg) -> BinaryenIndex {
        return static_cast<BinaryenIndex>(LAcc0 + reg);
    };

    auto mulCoefLocal = [](int idx) -> BinaryenIndex {
        return static_cast<BinaryenIndex>(LMulCoef0 + idx);
    };

    size_t eramEventIndex = 0;
    for (const RecordedOp& op : recordedOps_) {
        while (eramEventIndex < eramEvents_.size() && eramEvents_[eramEventIndex].sequence < op.sequence) {
            emitEramEvent(eramEvents_[eramEventIndex]);
            ++eramEventIndex;
        }

        const ESPOptInstr& instr = op.instr;

        if (instr.opType == kNop) {
            continue;
        }

        if (instr.m_access.save && instr.m_access.readReg >= 0 && instr.m_access.readReg < 6) {
            const bool unsat = instr.opType == kStoreIRAMUnsat || instr.opType == kWriteEramVarOffset;
            localSet(exprs, LReadAcc, satAcc64ToReadAcc32(get64(accLocal(instr.m_access.readReg)), unsat));
        }

        if (instr.opType != kDMAC) {
            localSet(exprs,
                     LMemPos,
                     and32(add32(i32c(static_cast<int32_t>(instr.mem)), get32(LIramPos)), i32c(0xff)));
            if (instr.useImm) {
                localSet(exprs, LMulInA, i32c(instr.imm));
            } else {
                localSet(exprs, LMulInA, load32At(iramIndexAddr(get32(LMemPos))));
            }
        }

        if (instr.opType != kMulCoef && !(instr.opType == kDMAC && op.lastMul30)) {
            localSet(exprs, LMulInB, load8SAt(coefsAddr(op.pc)));
        }

        switch (instr.opType) {
        case kNop:
        case kMAC:
            break;

        case kDMAC:
            localSet(exprs, LMulInA, shrS32(get32(LLastMulA), i32c(7)));
            if (op.lastMul30) {
                localSet(exprs, LMulInB, and32(shrS32(get32(LLastMulB), i32c(9)), i32c(0x7f)));
            }
            break;

        case kInterp:
            localSet(exprs,
                     LMulInA,
                     and32(xor32(get32(LMulInA), i32c(-1)), i32c(0x7fffff)));
            break;

        case kStoreIRAM:
        case kStoreIRAMUnsat:
            localSet(exprs, LMulInA, get32(LReadAcc));
            store32At(exprs, iramIndexAddr(get32(LMemPos)), get32(LMulInA));
            break;

        case kReadGRAM:
            localSet(exprs, LMulInA, load32At(gramIndexAddr(get32(LMemPos))));
            break;

        case kStoreGRAM:
            localSet(exprs, LMulInA, get32(LReadAcc));
            store32At(exprs, gramIndexAddr(get32(LMemPos)), get32(LMulInA));
            break;

        case kStoreIRAMRect:
            localSet(exprs,
                     LMulInA,
                     BinaryenSelect(module,
                                    geS32(get32(LReadAcc), i32c(0)),
                                    get32(LReadAcc),
                                    i32c(0)));
            store32At(exprs, iramIndexAddr(get32(LMemPos)), get32(LMulInA));
            break;

        case kWriteEramVarOffset:
            localSet(exprs, LEramVarOffset, get32(LReadAcc));
            break;

        case kWriteHost:
            store32At(exprs,
                      load32At(add32(get32(PCoreData), i32c(static_cast<int32_t>(offsetof(CoreData, hostRegPtr))))),
                      get32(LReadAcc));
            break;

        case kWriteEramWriteLatch:
            localSet(exprs, LEramWriteLatch, get32(LReadAcc));
            break;

        case kReadEramReadLatch:
            localSet(exprs, LMulInA, get32(LEramReadLatch));
            localSet(exprs,
                     LMemPos,
                     and32(add32(get32(LIramPos), i32c(static_cast<int32_t>(instr.mem | 0xf0))), i32c(0xff)));
            store32At(exprs, iramIndexAddr(get32(LMemPos)), get32(LMulInA));
            break;

        case kWriteMulCoef: {
            const int index = static_cast<int>((instr.mem >> 1) & 7);
            localSet(exprs, mulCoefLocal(index), get32(LReadAcc));
        } break;

        case kMulCoef: {
            const bool weird = (instr.coef & 0x1c) == 0x1c;

            if (instr.coef & 4) {
                localSet(exprs, LMulInA, get32(LReadAcc));
                if (weird) {
                    localSet(exprs,
                             LMulInA,
                             BinaryenSelect(module,
                                            geS32(get32(LMulInA), i32c(0)),
                                            i32c(0x7fffff),
                                            i32c(static_cast<int32_t>(0xff800000u))));
                }
                store32At(exprs, iramIndexAddr(get32(LMemPos)), get32(LMulInA));
            }

            if ((instr.coef >> 5) == 6) {
                localSet(exprs,
                         LMulInB,
                         and32(shl32(get32(LEramVarOffset), i32c(11)), i32c(0x7fffff)));
            } else if ((instr.coef >> 5) == 7) {
                localSet(exprs, LMulInB, get32(mulCoefLocal(5)));
            } else {
                localSet(exprs, LMulInB, get32(mulCoefLocal((instr.coef >> 5) & 7)));
            }

            if ((instr.coef & 8) && !weird) {
                localSet(exprs, LMulInB, sub32(i32c(0), get32(LMulInB)));
            }

            if ((instr.coef & 16) && !weird) {
                BinaryenExpressionRef posPath = and32(xor32(get32(LMulInB), i32c(-1)), i32c(0x7fffff));
                BinaryenExpressionRef negPath = xor32(and32(get32(LMulInB), i32c(0x7fffff)), i32c(-1));
                localSet(exprs,
                         LMulInB,
                         BinaryenSelect(module,
                                        geS32(get32(LMulInB), i32c(0)),
                                        posPath,
                                        negPath));
            }

            localSet(exprs, LLastMulB, get32(LMulInB));
            localSet(exprs, LMulInB, shrS32(get32(LMulInB), i32c(16)));
        } break;

        case kInterpStorePos:
            localSet(exprs, LMulInA, get32(LReadAcc));
            store32At(exprs, iramIndexAddr(get32(LMemPos)), get32(LMulInA));
            localSet(exprs,
                     LMulInA,
                     and32(BinaryenSelect(module,
                                          geS32(get32(LMulInA), i32c(0)),
                                          xor32(get32(LMulInA), i32c(-1)),
                                          get32(LMulInA)),
                           i32c(0x7fffff)));
            break;

        case kInterpStoreNeg:
            localSet(exprs, LMulInA, get32(LReadAcc));
            store32At(exprs, iramIndexAddr(get32(LMemPos)), get32(LMulInA));
            localSet(exprs,
                     LMulInA,
                     and32(BinaryenSelect(module,
                                          ltS32(get32(LMulInA), i32c(0)),
                                          xor32(get32(LMulInA), i32c(-1)),
                                          get32(LMulInA)),
                           i32c(0x7fffff)));
            break;

        case kJmpAlways:
            localSet(exprs, LShouldJump, i32c(1));
            break;

        case kJmpZero:
            localSet(exprs,
                     LShouldJump,
                     BinaryenSelect(module,
                                    eq32(get32(LReadAcc), i32c(0)),
                                    i32c(1),
                                    i32c(0)));
            break;

        case kJmpLessZero:
            localSet(exprs,
                     LShouldJump,
                     BinaryenSelect(module,
                                    ltS32(get32(LReadAcc), i32c(0)),
                                    i32c(1),
                                    i32c(0)));
            break;

        case kJmpGreaterZero:
            localSet(exprs,
                     LShouldJump,
                     BinaryenSelect(module,
                                    gtS32(get32(LReadAcc), i32c(0)),
                                    i32c(1),
                                    i32c(0)));
            break;

        case kSetCondition:
            localSet(exprs,
                     LCondition,
                     BinaryenSelect(module,
                                    geS32(get32(LReadAcc), i32c(0)),
                                    i32c(1),
                                    i32c(0)));
            break;
        }

        if (!instr.m_access.nomac && instr.m_access.srcReg != -1 && instr.m_access.destReg != -1) {
            localSet(exprs, LLastMulA, get32(LMulInA));

            BinaryenExpressionRef mulASe24 = se24(get32(LMulInA));
            BinaryenExpressionRef product64 = BinaryenBinary(module,
                                                             BinaryenMulInt64(),
                                                             BinaryenUnary(module, BinaryenExtendSInt32(), mulASe24),
                                                             BinaryenUnary(module, BinaryenExtendSInt32(), get32(LMulInB)));
            BinaryenExpressionRef shiftAmount64 = BinaryenUnary(module, BinaryenExtendUInt32(), load8UAt(shiftAddr(op.pc)));
            BinaryenExpressionRef shifted64 = BinaryenBinary(module,
                                                             BinaryenShrSInt64(),
                                                             product64,
                                                             shiftAmount64);
            BinaryenExpressionRef result64 = shifted64;

            if (!instr.m_access.clr && instr.m_access.srcReg >= 0 && instr.m_access.srcReg < 6) {
                result64 = add64(result64, get64(accLocal(instr.m_access.srcReg)));
            }

            if (instr.m_access.destReg >= 0 && instr.m_access.destReg < 6) {
                localSet(exprs, accLocal(instr.m_access.destReg), result64);
            }
        } else {
            localSet(exprs, LLastMulA, i32c(0));
        }
    }

    while (eramEventIndex < eramEvents_.size()) {
        emitEramEvent(eramEvents_[eramEventIndex]);
        ++eramEventIndex;
    }

    store64At(exprs, accAddr(0), get64(LAcc0));
    store64At(exprs, accAddr(1), get64(LAcc1));
    store64At(exprs, accAddr(2), get64(LAcc2));
    store64At(exprs, accAddr(3), get64(LAcc3));
    store64At(exprs, accAddr(4), get64(LAcc4));
    store64At(exprs, accAddr(5), get64(LAcc5));

    store32At(exprs, mulcoefAddr(0), get32(LMulCoef0));
    store32At(exprs, mulcoefAddr(1), get32(LMulCoef1));
    store32At(exprs, mulcoefAddr(2), get32(LMulCoef2));
    store32At(exprs, mulcoefAddr(3), get32(LMulCoef3));
    store32At(exprs, mulcoefAddr(4), get32(LMulCoef4));
    store32At(exprs, mulcoefAddr(5), get32(LMulCoef5));
    store32At(exprs, mulcoefAddr(6), get32(LMulCoef6));
    store32At(exprs, mulcoefAddr(7), get32(LMulCoef7));

    store32At(exprs, get32(PEramEffectiveAddr), get32(LEramEffectiveAddr));
    store32At(exprs, get32(PEramWriteLatchNext), get32(LEramWriteLatchNext));
    store32At(exprs, get32(PEramReadLatch), get32(LEramReadLatch));
    store32At(exprs, get32(PEramWriteLatch), get32(LEramWriteLatch));
    store32At(exprs, get32(PEramVarOffset), get32(LEramVarOffset));
    store32At(exprs, get32(PLastMulA), get32(LLastMulA));
    store32At(exprs, get32(PLastMulB), get32(LLastMulB));

    BinaryenExpressionRef body = BinaryenBlock(module,
                                               nullptr,
                                               exprs.data(),
                                               static_cast<BinaryenIndex>(exprs.size()),
                                               BinaryenTypeAuto());

    std::array<BinaryenType, kNumParams> paramsArr;
    paramsArr.fill(BinaryenTypeInt32());
    BinaryenType params = BinaryenTypeCreate(paramsArr.data(), kNumParams);

    BinaryenAddFunction(module,
                        "run",
                        params,
                        BinaryenTypeNone(),
                        vars.data(),
                        static_cast<BinaryenIndex>(vars.size()),
                        body);
    BinaryenAddFunctionExport(module, "run", "run");

    if (!BinaryenModuleValidate(module)) {
        BinaryenModuleDispose(module);
        return {};
    }

    BinaryenModuleAllocateAndWriteResult result = BinaryenModuleAllocateAndWrite(module, nullptr);
    BinaryenModuleDispose(module);

    if (result.binary == nullptr || result.binaryBytes == 0) {
        if (result.binary) {
            std::free(result.binary);
        }
        if (result.sourceMap) {
            std::free(result.sourceMap);
        }
        return {};
    }

    std::vector<uint8_t> outBytes(static_cast<uint8_t*>(result.binary),
                                  static_cast<uint8_t*>(result.binary) + result.binaryBytes);

    std::free(result.binary);
    if (result.sourceMap) {
        std::free(result.sourceMap);
    }
    return outBytes;
#endif
}

} // namespace esp
