#include "esp_opt_common.h"
#include "esp_jit_backend.h"
#include "esp_jit.h"
#include "esp_jit_binaryen.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace esp {

template<int lg2eram_size>
class ESPOptimizer
{
public:
  ESPOptimizer(ESP<lg2eram_size>* esp) : m_esp(esp)
  {
    data_core0.hostRegPtr = (int32_t*)esp->shared.readback_regs;
    data_core0.eramPtr = &esp->shared.eram.eram[0];
    data_core1.hostRegPtr = (int32_t*)esp->shared.readback_regs;
    data_core1.eramPtr = &esp->shared.eram.eram[0];

		m_compiler = createDefaultJitCompiler();
  }

  void genProgram(ESP<lg2eram_size>* esp)
  {
    if (m_compiler)
		{
			if (runCore0) m_compiler->release(runCore0);
			if (runCore1) m_compiler->release(runCore1);
		}

		runCore0 = nullptr;
		runCore1 = nullptr;

    eramEmitter.init(esp);
    coreEmitter0.init(esp, &esp->core0);
    coreEmitter1.init(esp, &esp->core1);

    genCore(esp, 0, &coreEmitter0, &runCore0);
    
    genCore(esp, 1, &coreEmitter1, &runCore1, true);
  }
  
  void genProgramIfDirty()
  {
    syncCoefsIfDirty();

	  if (m_esp->programDirty)
	  {
		  genProgram(m_esp);
		  m_esp->programDirty = false;
	  }
  }

  void syncCoefsIfDirty()
  {
    if (!m_esp->coefsDirty && !m_esp->programDirty) {
      return;
    }

    refreshCoreCoefAndShiftCache(data_core0, m_esp->core0.pram);
    refreshCoreCoefAndShiftCache(data_core1, m_esp->core1.pram);
    m_esp->coefsDirty = false;
  }

  inline void callOptimized(ESP<lg2eram_size>* esp)
  {
    if (runCore0) runCore0(data_core0.coefs, esp->core0.iram, esp->shared.gram, &data_core0, esp->shared.eram.eramPos, esp->core0.iramPos, 0, 0);
    if (runCore1) runCore1(data_core1.coefs, esp->core1.iram, esp->shared.gram, &data_core1, esp->shared.eram.eramPos, esp->core1.iramPos, 0, 0);
  }

  std::vector<uint8_t> buildWasmSnapshotModule(uint32_t core)
  {
#if !defined(JPWASM_USE_BINARYEN)
    (void)core;
    return {};
#else
    genProgramIfDirty();

    CoreEmitter* emitter = getCoreEmitter(core);
    if (!emitter) return {};

    EspJitBinaryen jit;
    emitCoreProgram(jit, emitter, core == 1);
    return jit.finalizeModuleBytes();
#endif
  }

  bool runWasmReferenceCoreOnce(uint32_t core)
  {
    if (core > 1) {
      return false;
    }

    genProgramIfDirty();

    CoreEmitter* emitter = getCoreEmitter(core);
    if (!emitter) return false;

    CoreData& coreData = core ? data_core1 : data_core0;
    ESPCore<lg2eram_size>& espCore = core ? m_esp->core1 : m_esp->core0;
    int32_t* iram = espCore.iram;
    int32_t* gram = m_esp->shared.gram;

    bool lastMul30 = false;
    for (size_t pc = 0; pc < PRAM_SIZE; ++pc)
    {
      const ESPOptInstr &instr = emitter->pram_opt[pc];
      if (instr.opType == kNop) {
        continue;
      }

      int32_t readAcc = 0;
      if (instr.m_access.save && instr.m_access.readReg >= 0 && instr.m_access.readReg < 6)
      {
        const int64_t raw = coreData.accs[instr.m_access.readReg];
        const bool unsat = instr.opType == kStoreIRAMUnsat || instr.opType == kWriteEramVarOffset;
        if (unsat) {
          readAcc = static_cast<int32_t>(raw);
        } else {
          const int64_t sat = std::clamp<int64_t>(raw, -0x800000, 0x7fffff);
          readAcc = static_cast<int32_t>(sat);
        }
      }

      uint32_t mempos = 0;
      int32_t mulInputA_24 = 0;
      if (instr.opType != kDMAC)
      {
        mempos = (static_cast<uint32_t>(instr.mem) + espCore.iramPos) & 0xff;
        mulInputA_24 = instr.useImm ? instr.imm : iram[mempos];
      }

      int32_t mulInputB_24 = 0;
      if (instr.opType != kMulCoef && !(instr.opType == kDMAC && lastMul30))
      {
        mulInputB_24 = coreData.coefs[pc];
      }

      switch (instr.opType)
      {
      case kNop:
      case kMAC:
        break;
      case kDMAC:
        mulInputA_24 = espCore.last_mulInputA_24 >> 7;
        if (lastMul30) {
          mulInputB_24 = (espCore.last_mulInputB_24 >> 9) & 0x7f;
        }
        break;
      case kInterp:
        mulInputA_24 = (~mulInputA_24 & 0x7fffff);
        break;
      case kStoreIRAM:
      case kStoreIRAMUnsat:
        mulInputA_24 = readAcc;
        iram[mempos] = mulInputA_24;
        break;
      case kReadGRAM:
        mulInputA_24 = gram[mempos];
        break;
      case kStoreGRAM:
        mulInputA_24 = readAcc;
        gram[mempos] = mulInputA_24;
        break;
      case kStoreIRAMRect:
        mulInputA_24 = std::max(0, readAcc);
        iram[mempos] = mulInputA_24;
        break;
      case kWriteEramVarOffset:
        m_esp->shared.eram.eramVarOffset = readAcc;
        break;
      case kWriteHost:
        *coreData.hostRegPtr = readAcc;
        break;
      case kWriteEramWriteLatch:
        m_esp->shared.eram.eramWriteLatch = readAcc;
        break;
      case kReadEramReadLatch:
        mulInputA_24 = m_esp->shared.eram.eramReadLatch;
        iram[(espCore.iramPos + (instr.mem | 0xf0)) & 0xff] = mulInputA_24;
        break;
      case kWriteMulCoef:
        coreData.mulcoeffs[(instr.mem >> 1) & 7] = readAcc;
        break;
      case kMulCoef:
      {
        const bool weird = (instr.coef & 0x1c) == 0x1c;
        if (instr.coef & 4)
        {
          mulInputA_24 = readAcc;
          if (weird) {
            mulInputA_24 = (mulInputA_24 >= 0) ? 0x7fffff : static_cast<int32_t>(0xff800000u);
          }
          iram[mempos] = mulInputA_24;
        }

        if ((instr.coef >> 5) == 6) mulInputB_24 = (m_esp->shared.eram.eramVarOffset << 11) & 0x7fffff;
        else if ((instr.coef >> 5) == 7) mulInputB_24 = coreData.mulcoeffs[5];
        else mulInputB_24 = coreData.mulcoeffs[(instr.coef >> 5) & 7];

        if ((instr.coef & 8) && !weird) mulInputB_24 *= -1;
        if ((instr.coef & 16) && mulInputB_24 >= 0 && !weird) mulInputB_24 = (~mulInputB_24 & 0x7fffff);
        else if ((instr.coef & 16) && mulInputB_24 < 0 && !weird) mulInputB_24 = ~(mulInputB_24 & 0x7fffff);
        espCore.last_mulInputB_24 = mulInputB_24;
        mulInputB_24 >>= 16;
      }
        break;
      case kInterpStorePos:
        mulInputA_24 = readAcc;
        iram[mempos] = mulInputA_24;
        if (mulInputA_24 >= 0) mulInputA_24 = ~mulInputA_24;
        mulInputA_24 &= 0x7fffff;
        break;
      case kInterpStoreNeg:
        mulInputA_24 = readAcc;
        iram[mempos] = mulInputA_24;
        if (mulInputA_24 < 0) mulInputA_24 = ~mulInputA_24;
        mulInputA_24 &= 0x7fffff;
        break;
      case kSetCondition:
      case kJmpAlways:
      case kJmpZero:
      case kJmpLessZero:
      case kJmpGreaterZero:
        break;
      }

      if (!instr.m_access.nomac && instr.m_access.srcReg != -1 && instr.m_access.destReg != -1)
      {
        espCore.last_mulInputA_24 = mulInputA_24;
        int64_t result = static_cast<int64_t>(se<24>(mulInputA_24)) * static_cast<int64_t>(mulInputB_24);
        result >>= static_cast<uint8_t>(coreData.shiftAmounts[pc]);
        if (!instr.m_access.clr) {
          result += coreData.accs[instr.m_access.srcReg];
        }
        coreData.accs[instr.m_access.destReg] = result;
      }
      else
      {
        espCore.last_mulInputA_24 = 0;
      }

      lastMul30 = (instr.op == 0x30);
    }

    return true;
  }

  bool getWasmRuntimePointers(uint32_t core, uintptr_t* outPtrs, size_t outCount)
  {
    if (outPtrs == nullptr || outCount < 12) {
      return false;
    }

    if (core > 1) {
      return false;
    }

    genProgramIfDirty();

    CoreData& coreData = core ? data_core1 : data_core0;
    ESPCore<lg2eram_size>& espCore = core ? m_esp->core1 : m_esp->core0;

    outPtrs[0] = reinterpret_cast<uintptr_t>(&coreData);
    outPtrs[1] = reinterpret_cast<uintptr_t>(espCore.iram);
    outPtrs[2] = reinterpret_cast<uintptr_t>(m_esp->shared.gram);
    outPtrs[3] = reinterpret_cast<uintptr_t>(&m_esp->shared.eram.eramPos);
    outPtrs[4] = reinterpret_cast<uintptr_t>(&espCore.iramPos);
    outPtrs[5] = reinterpret_cast<uintptr_t>(&m_esp->shared.eram.eramEffectiveAddr);
    outPtrs[6] = reinterpret_cast<uintptr_t>(&m_esp->shared.eram.eramWriteLatchNext);
    outPtrs[7] = reinterpret_cast<uintptr_t>(&m_esp->shared.eram.eramReadLatch);
    outPtrs[8] = reinterpret_cast<uintptr_t>(&m_esp->shared.eram.eramWriteLatch);
    outPtrs[9] = reinterpret_cast<uintptr_t>(&m_esp->shared.eram.eramVarOffset);
    outPtrs[10] = reinterpret_cast<uintptr_t>(&espCore.last_mulInputA_24);
    outPtrs[11] = reinterpret_cast<uintptr_t>(&espCore.last_mulInputB_24);
    return true;
  }
  
private:
  static void refreshCoreCoefAndShiftCache(CoreData& out, const uint32_t* pram)
  {
    for (size_t i = 0; i < PRAM_SIZE; i++) {
      const uint32_t instr = pram[i];
      const uint32_t op = (instr >> 16) & 0x7c;
      const int8_t coef = se<8>(instr & 0xff);
      const uint32_t shift = (instr >> 8) & 3;
      uint32_t shiftAmount = (0x3567 >> (shift << 2)) & 0xf;
      if (op == 0x20 || op == 0x24) shiftAmount = (shift & 1) ? 6 : 7;
      out.coefs[i] = coef;
      out.shiftAmounts[i] = static_cast<int8_t>(shiftAmount);
    }
  }

  class CoreEmitter;

  ESP<lg2eram_size>* m_esp;
  std::unique_ptr<EspJitCompiler> m_compiler;

  using RunCore = EspRunCore;
  RunCore runCore0 = nullptr, runCore1 = nullptr;

  // State used by jitted code
  CoreData data_core0{0};
  CoreData data_core1{0};

  CoreEmitter* getCoreEmitter(uint32_t core)
  {
    if (core > 1) return nullptr;
    ESPCore<lg2eram_size>* selectedCore = (core == 0) ? &m_esp->core0 : &m_esp->core1;
    CoreEmitter* emitter = (core == 0) ? &coreEmitter0 : &coreEmitter1;
    emitter->init(m_esp, selectedCore);
    return emitter;
  }

  void emitCoreProgram(EspJitBase& jit, CoreEmitter* emitter, bool withEram)
  {
    jit.jitEnter();
    for (size_t pc = 0; pc < PRAM_SIZE; pc++)
    {
      if (withEram)
        eramEmitter.emit(static_cast<int>(pc), jit);
      emitter->emit(static_cast<int>(pc), jit);
    }
    emitter->emitEnd();
    jit.jitExit();
  }

  void genCore(ESP<lg2eram_size>* esp, uint32_t core, CoreEmitter* emitter, RunCore *dest, bool withEram = false)
  {
    if (!m_compiler)
    {
      *dest = nullptr;
      return;
    }

		CoreData& coreData = core ? data_core1 : data_core0;
		ESPCore<lg2eram_size>& espCore = core ? esp->core1 : esp->core0;

    esp::JitInputData jitData;
		jitData.coreData = &coreData;
		jitData.iram = espCore.iram;
		jitData.gram = esp->shared.gram;

  	jitData.eramPos = &esp->shared.eram.eramPos;
		jitData.iramPos = &espCore.iramPos;

		jitData.eramEffectiveAddr = &esp->shared.eram.eramEffectiveAddr;
		jitData.eramWriteLatchNext = &esp->shared.eram.eramWriteLatchNext;

  	jitData.eramReadLatch = &esp->shared.eram.eramReadLatch;
		jitData.eramWriteLatch = &esp->shared.eram.eramWriteLatch;
		jitData.eramVarOffset = &esp->shared.eram.eramVarOffset;
		jitData.last_mulInputA_24 = &espCore.last_mulInputA_24;
		jitData.last_mulInputB_24 = &espCore.last_mulInputB_24;

		m_compiler->compile(dest, jitData, [&](EspJitBase& jit)
		{
			emitCoreProgram(jit, emitter, withEram);
		});
  }

  class ERAMEmitter
  {
  public:
    void init(ESP<lg2eram_size>* _esp)
    {
      esp = _esp;

      eramPCCommit = 0, eramPCStartNext = 0;
      eramModeCurrent = 0, eramModeNext = 0;
      eramImmOffsetAccNext = 0;
      eramActiveCurrent = false, eramActiveNext = false;
      highOffset = false;
    }

    void emit(int pc, esp::EspJitBase& _jit)
    {
      if (lg2eram_size == 0) return;

      uint32_t *decode = (uint32_t*)(&esp->intmem[0x1000]);
      uint32_t eramCtrl = (decode[pc] >> 23) & 0x1f;
      int stage1 = pc - eramPCStartNext;

      // Transaction start
      if (!eramActiveNext && ((eramCtrl & 0x18) != 0)) {
        eramActiveNext = true;
        eramModeNext = eramCtrl;
        eramPCStartNext = pc;
        eramImmOffsetAccNext = 0;
        stage1 = 0;
        if (eramModeNext & 0x7) printf("wtf %03x at pc=%04x\n", eramCtrl, pc);
      }

      // Accumulate immediates
      else if (eramActiveNext && stage1 <= 4 && stage1 > 0) {
        eramImmOffsetAccNext += eramCtrl << ((stage1 - 1) * 5);
      }

      // Is it time to commit?
      if (eramActiveCurrent && (pc == eramPCCommit)) {
        if (eramModeCurrent == 0x10) emitWrite(_jit);
        else emitRead(_jit);
        eramActiveCurrent = false; // done
      }

      // Next stage
      if (eramActiveNext && stage1 == 5) { // FIXME: stage1 should be 4, but there are some problems with latching
        if (eramActiveCurrent) printf("ERAM transaction already active at pc %03x\n", pc);
        eramActiveCurrent = true;
        eramModeCurrent = eramModeNext;
        eramPCCommit = eramPCStartNext + ERAM_COMMIT_STAGE;
        eramActiveNext = false;

        // Addr computation
        uint32_t immOffset = eramImmOffsetAccNext;
        bool shouldUseVarOffset = false;
        if (eramModeNext == 0x18)
        {
          immOffset = (eramImmOffsetAccNext >> 1) & 1;
          highOffset = eramImmOffsetAccNext & 0x100;
          shouldUseVarOffset = true;
        }

        emitComputeAddr(_jit, immOffset, highOffset, shouldUseVarOffset);
      }
    }

  private:
    // WARNING: shares regs as a core

    void emitWrite(esp::EspJitBase& _jit)
    {
			_jit.eramWrite(ERAM_MASK);
    }

    void emitRead(esp::EspJitBase& _jit)
    {
			_jit.eramRead(ERAM_MASK);
    }

    void emitComputeAddr(esp::EspJitBase& _jit, uint32_t immOffset, bool highOffset, bool shouldUseVarOffset)
    {
			_jit.eramComputeAddr(immOffset, highOffset, shouldUseVarOffset);
    }
  
    uint16_t eramPCCommit = 0, eramPCStartNext = 0;
    uint8_t eramModeCurrent = 0, eramModeNext = 0;
    uint32_t eramImmOffsetAccNext = 0;
    bool eramActiveCurrent = false, eramActiveNext = false;
    bool highOffset = false;

    ESP<lg2eram_size>* esp;
    static constexpr int64_t ERAM_COMMIT_STAGE = 10, ERAM_MASK_FULL = (1 << 19) - 1;
		enum {eram_size = 1 << lg2eram_size, ERAM_MASK = eram_size - 1};
  };
  ERAMEmitter eramEmitter;

  class CoreEmitter
  {
  public:
    void init(ESP<lg2eram_size>* _esp, ESPCore<lg2eram_size>* _core)
    {
      esp = _esp;
      core = _core;

      pre_optimize();
    }

    void emit(int pc, esp::EspJitBase& _jit)
    {
		const ESPOptInstr &instr = pram_opt[pc];

    	if (instr.opType == kNop) return;

			_jit.emitOp(pc, instr, lastMul30);

			lastMul30 = (instr.op == 0x30);
    }

    void emitEnd()
    {
      // Store back accumulators
      // m_asm.mov(x8, uint64_t(&acc[0]));
      // m_asm.str(x0, ptr(x8));
      // m_asm.str(x1, ptr(x8, 1 << 3));
      // m_asm.str(x2, ptr(x8, 2 << 3));
      // m_asm.str(x3, ptr(x8, 3 << 3));
      // m_asm.str(x4, ptr(x8, 4 << 3));
      // m_asm.str(x5, ptr(x8, 5 << 3));
    }
  
    bool lastMul30 = false;

		void pre_optimize()
		{
			for (int i = 0; i < PRAM_SIZE; i++)
				pram_opt[i] = ESPOptInstr(core->pram[i]);

			// Decided limitations: We will not support op 0x34, mem & 0xcc == 0xc0 (these are the jump operations)
			for (int pc = 0; pc < PRAM_SIZE; pc++) assert((pram_opt[pc].op != 0xd || (pram_opt[pc].mem & 0xcc) != 0xc0) && "Jumps!"); // jumps. bail.

			const MemAccess wA = {kNone, false}, wB = {kNone, true}, swA = {kSavesA, false}, swB = {kSavesB, true}, sBwA = {kSavesB, false}, sAwB = {kSavesA, true};

			for (int pc = 0; pc < PRAM_SIZE; pc++)
			{
				ESPOptInstr &o = pram_opt[pc];
				MemAccess a = wA;
				switch (o.op)
				{
				case 0x00: case 0x04: case 0x28: case 0x2c: case 0x50: case 0x54: case 0x60: case 0x64: case 0x70: case 0x74: a = wA; break;
				case 0x08: case 0x38: case 0x40: case 0x44: case 0x48: case 0x4c: case 0x58: case 0x68: case 0x6c: case 0x78: case 0x7c: a = swA; break;
				case 0x0c: case 0x3c: a = sBwA; break;
				case 0x10: case 0x14: a = wB; break;
				case 0x18: a = sAwB; break;
				case 0x1c: case 0x5c: a = swB; break;
				case 0x20: case 0x24: a = (o.shift & 2) ? wB : wA; break;

				case 0x30: if (o.coef & 4) a = (o.coef & 2) ? swB : swA; else a = (o.coef & 2) ? wB : wA; break;
				case 0x34:
					if (o.mem >= 0xa0 && o.mem < 0xb0) a = (o.mem & 1) ? sBwA : swA;
					if (o.mem >= 0xc0) {
						switch (o.mem & 0xf)
						{
							case 0x7: case 0xa: case 0xb: case 0xc: case 0xd: case 0xe: case 0xf: a = (o.mem & 0x20) ? swB : swA; break;
							default: a = (o.mem & 0x20) ? wB : wA; break;
						}
					}
					break;
				default: break;
				}
				bool clr = false;
				switch (o.op)
				{
					case 0x04: case 0x08: case 0x0c: case 0x14: case 0x18: case 0x1c: case 0x24: case 0x44:
					case 0x4c: case 0x50: case 0x64: case 0x6c: case 0x74: case 0x7c: clr = true; break;
					case 0x30: clr = !(o.coef & 1); break;
					case 0x34: if (o.mem >= 0xc0) clr = (o.mem & 0x10); break;
					default: break;
				}
				a.clr = clr;
				if (!o.mem && !o.shift && !o.op && !o.coef) a.nop = true;
				if (o.op == 0x50) a.accGetsUsed = true;
				o.m_access = a;
			}
			
			for (int pc = 0; pc < PRAM_SIZE; pc++)
			{
				if (pram_opt[pc].m_access.nop || !pram_opt[pc].m_access.save) continue;
				// this op saves a value. who generated it?
				bool savesB = (pram_opt[pc].m_access.save == kSavesB);
				int i = pc - 3;
				while (i >= 0) // find the instruction that generated this value.
				{
					if (pram_opt[i].m_access.nop || pram_opt[i].m_access.writesAccB != savesB) {i--; continue;} // this writes to the wrong accumulator. skip.
					pram_opt[i].m_access.accGetsUsed = true; // mark this write as being used.
					pram_opt[pc].m_access.writePC = i;
					break;
				}
			}

			for (int pc = 0; pc < PRAM_SIZE; pc++) // does our mac achieve anything, in theory?
			{
				if (pram_opt[pc].m_access.accGetsUsed) continue; // yes.
				if (pram_opt[pc].m_access.nop) {pram_opt[pc].m_access.nomac = true; continue;} // no.
				int i = pc;
				while (++i < PRAM_SIZE)
				{
					if (pram_opt[i].m_access.nop) continue; // dont care
					if (pram_opt[i].m_access.writesAccB != pram_opt[pc].m_access.writesAccB) continue; // doesn't apply to our acc. skip
					if (pram_opt[i].m_access.clr) {pram_opt[pc].m_access.nomac = true; break;} // we get overwritten
					if (pram_opt[i].m_access.accGetsUsed) break; // the result gets saved.
				}
			}
			
			int rega = 0, regb = 0;
			bool usedrega = false, usedregb = false;
			for (int pc = 0; pc < PRAM_SIZE; pc++) // hand out register numbers
			{
				MemAccess &a = pram_opt[pc].m_access;
				if (pram_opt[pc].m_access.nop) continue;
				if (a.save) a.readReg = pram_opt[a.writePC].m_access.destReg;

				a.srcReg = (a.writesAccB) ? regb : rega;
				
				if (a.writesAccB && usedregb) {regb = (regb + 1) % 3; usedregb = false;}
				if (!a.writesAccB && usedrega) {rega = (rega + 1) % 3; usedrega = false;}

				a.destReg = (a.writesAccB) ? regb : rega;
				if (a.accGetsUsed) (a.writesAccB ? usedregb : usedrega) = true;
			}

			for (int pc = 0; pc < PRAM_SIZE; pc++) // flatten accA and accB
			{
				MemAccess &a = pram_opt[pc].m_access;
				if (a.writesAccB) { a.srcReg += 3; a.destReg += 3; }
				if (a.save == kSavesB) a.readReg += 3;
			}

			int32_t skipfieldPos = 0;
			int32_t skipfieldNeg = 0;
			for (int pc = 0; pc < PRAM_SIZE; pc++)
			{
				// decode op50 skip
				pram_opt[pc].skippablePos = skipfieldPos & 1;
				pram_opt[pc].skippableNeg = skipfieldNeg & 1;
				
				skipfieldPos >>= 1;
				skipfieldNeg >>= 1;
				
				if (pram_opt[pc].op == 0x50)
				{
					skipfieldPos |= 0x30;
					skipfieldNeg |= 0x3c0;
				}
				
				// decode jumps
				bool isJump = pram_opt[pc].op == kJmpAlways || pram_opt[pc].op == kJmpLessZero
									 || pram_opt[pc].op == kJmpGreaterZero || pram_opt[pc].op == kJmpZero;
				int jmpDest = pram_opt[pc].jmpDest;
				if (isJump && pc + 2 < PRAM_SIZE && jmpDest < PRAM_SIZE && jmpDest >= 0)
				{
					pram_opt[pc + 2].triggerJump = true;
					pram_opt[jmpDest].jmpTarget = true;
				}
			}
		}

    ESP<lg2eram_size>* esp;
    ESPCore<lg2eram_size>* core;

    ESPOptInstr pram_opt[PRAM_SIZE] {};
  };
  CoreEmitter coreEmitter0, coreEmitter1;
};

} // namespace esp
