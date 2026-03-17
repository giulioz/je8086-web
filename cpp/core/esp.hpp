#pragma once

#include <stdint.h>
#include <stdio.h>
#include <algorithm>
#include <assert.h>
#include <array>

namespace esp {

template <int32_t N> static constexpr int32_t se(int32_t x) { x <<= (32 - N); return x >> (32 - N); }

template<int lg2eram_size>
class ESP;

template<int lg2eram_size>
class ESPCore;

template<int lg2eram_size>
class ERAM;


class DspAccumulator {
public:
	inline void reset() {for (int i = 0; i < delay; i++) hist[i] = 0; head = acc = 0;}
	inline int32_t operator=(int32_t v) { return acc = se<30>(v); }
	inline int32_t operator+=(int32_t v) { return acc = se<30>(acc + v); }
	
	inline int32_t rawFull() const { return acc; }
	inline int32_t getPipelineSat24() const { return std::clamp(hist[head], -0x800000, 0x7fffff); }
	inline int32_t getPipelineRaw24() const { return se<24>(hist[head]); }
	inline int32_t getPipelineRawFull() const { return hist[head]; }
	
	inline void storePipeline() { hist[head++] = acc; if (head == delay) head = 0; }

	static constexpr int delay {3}; // delay
	int32_t acc {0}, hist[delay] = {}, head {0};
};

// NOTE: we assume it's being used the 16 bit compressed mode
template<int lg2eram_size>
class ERAM {
public:
	void reset() {
		if (eram_size) memset(eram, 0, eram_size * sizeof(int32_t));
		eramPos = eramEffectiveAddr = eramImmOffsetAccNext = eramVarOffset = eramWriteLatch = eramPCCommit = eramPCStartNext = eramModeCurrent = eramModeNext = 0;
		eramActiveCurrent = eramActiveNext = false;
	}
	
	void tickSample() { eramPos = (eramPos - 1) & ERAM_MASK_FULL; }
	
	void tickCycle(const uint8_t eramCtrl, const uint16_t pc) {
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
			// TODO: this is not accurate, the ram is written with some floating point magic, look at the jv880 chip
			if (eramModeCurrent == 0x10) eram[eramEffectiveAddr & ERAM_MASK] = crunch(eramWriteLatchNext);	// write
			else eramReadLatch = se<24>(eram[eramEffectiveAddr & ERAM_MASK]); // read
			eramActiveCurrent = false; // done
		}

		// Next stage
		if (eramActiveNext && stage1 == 5) { // FIXME: stage1 should be 4, but there are some problems with latching
			if (eramActiveCurrent) printf("ERAM transaction already active at pc %03x\n", pc);
			eramActiveCurrent = true;
			eramModeCurrent = eramModeNext;
			eramPCCommit = eramPCStartNext + ERAM_COMMIT_STAGE;
			eramActiveNext = false;
			eramWriteLatchNext = eramWriteLatch;

			// Addr computation
			eramEffectiveAddr = eramPos + eramImmOffsetAccNext;
			if (eramModeNext == 0x18)
			{
				eramEffectiveAddr = eramPos + (eramVarOffset >> 12) + ((eramImmOffsetAccNext >> 1) & 1);
				eramHighOffset = eramImmOffsetAccNext & 0x100;
			}
			// HACK for now
			if (eramHighOffset && eramPos <= 0x4000) eramEffectiveAddr += 0x40000;
			if (eramHighOffset && eramPos  > 0x4000) eramEffectiveAddr += 0xc0000;
		}
	}

	int32_t eramReadLatch = 0, eramWriteLatch = 0, eramVarOffset = 0;

	static constexpr int64_t ERAM_COMMIT_STAGE = 10, ERAM_MASK_FULL = (1 << 19) - 1;
	enum {eram_size = 1 << lg2eram_size, ERAM_MASK = eram_size - 1};
	int32_t eram[eram_size];
	uint32_t eramPos = 0, eramEffectiveAddr = 0, eramImmOffsetAccNext = 0, eramHighOffset = 0;
	int32_t eramWriteLatchNext = 0;
	uint16_t eramPCCommit = 0, eramPCStartNext = 0;
	uint8_t eramModeCurrent = 0, eramModeNext = 0;
	bool eramActiveCurrent = false, eramActiveNext = false;

	inline static int crunch(int x) {
		const int b = ((x >> 1) & 0x400000) * 3;
		if (((x << 1) & 0xc00000) != b) return x & 0xFFFFFC00;
		if (((x << 3) & 0xc00000) != b) return x & 0xFFFFFF00;
		if (((x << 5) & 0xc00000) != b) return x & 0xFFFFFFC0;
		return x & 0xFFFFFFF0;
	}
};

template<int lg2eram_size>
struct SharedState {
	int32_t gram[256] {};
	uint8_t readback_regs[4] = {0xff, 0xff, 0xff, 0x00};
	int32_t mulcoeffs[8] = {};
	uint8_t dintpins = {};
	ERAM<lg2eram_size> eram;

	void reset() {
		memset(gram, 0, sizeof(gram));
		memset(readback_regs, 0x00, sizeof(readback_regs));
		memset(mulcoeffs, 0, sizeof(mulcoeffs));
		eram.reset();
	}
};

template<int lg2eram_size>
class ESPCore
{
public:
	void reset() { memset(iram, 0, sizeof(iram)); pc = iramPos = 0; accA.reset(); accB.reset(); }

	void setup(const uint32_t *_pram, SharedState<lg2eram_size> *_shared) { pram = _pram; shared = _shared; }

	void writeGRAM(int32_t val, uint32_t offset) {shared->gram[(offset + iramPos) & IRAM_MASK] = val;}
	int32_t readGRAM(uint32_t offset) const {return shared->gram[(offset + iramPos) & IRAM_MASK];}

	void writeIRAM(int32_t val, uint32_t offset) { iram[(offset + iramPos) & IRAM_MASK] = val; }
	int32_t readIRAM(uint32_t offset) const {return iram[(offset + iramPos) & IRAM_MASK];}

	void sync() {
		pc = 0;
		iramPos = (iramPos - 1) & IRAM_MASK;
	}
	
	void steperam() { if (lg2eram_size) shared->eram.tickCycle((pram[pc] >> 23) & 0x1f, pc); }

	void step() {
		if (pc >= PRAM_SIZE) return;
		
		if (pc == pcjumpat) {pc = pcjumpto; pcjumpat = -1;}

		// Decode instr
		const uint32_t instr = pram[pc++]; // PC advances here.
		if (!instr) {
			skipfield >>= 1;
			accA.storePipeline();
			accB.storePipeline();
			return;
		}
		const uint8_t op = (instr >> 16) & 0x7c;
		const uint8_t mem = (instr >> 10) & 0xff;
		const uint8_t shiftbits = (instr >> 8) & 3;
		uint8_t shift = (0x3567 >> (shiftbits << 2)) & 0xf; // this is shift amount. pick the value 3/5/6/7 using bits 8,9.
		const uint8_t coef = instr & 0xff;

		const uint32_t mempos = ((uint32_t)mem + iramPos) & IRAM_MASK;
		int32_t mulInputA_24 = 0;
		switch (mem)
		{
			case 1: mulInputA_24 = 0x10; break; // 4
			case 2: mulInputA_24 = 0x400; break; // 10
			case 3: mulInputA_24 = 0x10000; break; // 16
			case 4: mulInputA_24 = 0x400000; break; // 22
			default: mulInputA_24 = iram[mempos]; break;
		}
		int32_t mulInputB_24 = coef;
		bool acc = false, clr = false;
		switch (op)
		{
			// MAC
			case 0x00: break;
			case 0x04: clr = true; break;
			case 0x08: iram[mempos] = mulInputA_24 = accA.getPipelineSat24(); clr = true; break;
			case 0x0c: iram[mempos] = mulInputA_24 = accB.getPipelineSat24(); clr = true; break;
			case 0x10: acc = true; break;
			case 0x14: acc = true; clr = true; break;
			case 0x18: acc = true; iram[mempos] = mulInputA_24 = accA.getPipelineSat24(); clr = true; break;
			case 0x1c: acc = true; iram[mempos] = mulInputA_24 = accB.getPipelineSat24(); clr = true; break;
			
			// SPECIAL/MUL/GRAM
			case 0x20:
				acc = (shiftbits & 2);
				shift = (shiftbits & 1) ? 6 : 7;
				mulInputA_24 = shared->gram[mempos];
				break;
			case 0x24:
				acc = (shiftbits & 2);
				clr = true;
				shift = (shiftbits & 1) ? 6 : 7;
				mulInputA_24 = shared->gram[mempos];
				break;
			case 0x28: printf("Unexpected Opcode 0x28. This should be unused\n"); break;
			case 0x2c: printf("Unexpected Opcode 0x2c. This should be unused\n"); break;
			case 0x30:
			{
				acc = (coef & 2);
				clr = !(coef & 1);
				bool weird = (coef & 0x1c) == 0x1c;
				if (coef & 4) {
					mulInputA_24 = (acc ? accB : accA).getPipelineSat24();
					if (weird) mulInputA_24 = (mulInputA_24 >= 0) ? 0x7fffff : 0xFF800000;
					iram[mempos] = mulInputA_24;
				}
				mulInputB_24 = shared->mulcoeffs[coef >> 5];
				if ((coef >> 5) == 6) mulInputB_24 = (shared->eram.eramVarOffset << 11) & 0x7fffff;
				if ((coef >> 5) == 7) mulInputB_24 = shared->mulcoeffs[5];
				if ((coef & 8) && !weird) mulInputB_24 *= -1;
				if ((coef & 16) && mulInputB_24 >= 0 && !weird) mulInputB_24 = (~mulInputB_24 & 0x7fffff);
				else if ((coef & 16) && mulInputB_24 < 0 && !weird) mulInputB_24 = ~(mulInputB_24 & 0x7fffff);
				last_mulInputB_24 = mulInputB_24;
				mulInputB_24 >>= 16;
			}
				break;
			case 0x34:
				if (mem < 0xa0 || (mem & 0xf0) == 0xb0) printf("Unexpected value for mem (%02x) with opcode 0x34\n", mem);
				if (mem >= 0xa0 && mem < 0xb0) shared->mulcoeffs[(mem >> 1) & 7] = ((mem & 1) ? accB : accA).getPipelineSat24();
				if (mem >= 0xc0)
				{
					acc = (mem & 0x20);
					clr = (mem & 0x10);
					DspAccumulator &ac = acc ? accB : accA;
					mulInputA_24 = ac.getPipelineRawFull();
					switch (mem & 0xf)
					{
						// jumps
						case 0x0:
							if (!mulInputA_24) jumpto(coef | ((int)shiftbits << 8));
							break;
						case 0x1:
							if (mulInputA_24 < 0) jumpto(coef | ((int)shiftbits << 8));
							break;
						case 0x2:
							if (mulInputA_24 >= 0) jumpto(coef | ((int)shiftbits << 8));
							break;
						case 0x3:
							jumpto(coef | ((int)shiftbits << 8));
							break;
						
						case 0x4:
							// set INT pins
							shared->dintpins = ((mulInputA_24 < 0) ? 1 : 0);
							break;
						case 0x6:
							// double precision
							mulInputA_24 = last_mulInputA_24 >> 7;
							if (lastMul30) mulInputB_24 = (last_mulInputB_24 >> 9) & 0x7f;
							break;
						case 0x7: shared->eram.eramVarOffset = mulInputA_24; break;
						case 0xa: *((int32_t*)&shared->readback_regs) = ac.getPipelineSat24(); break;
						case 0xb: shared->eram.eramWriteLatch = ac.getPipelineSat24(); break;
						case 0xc:
						case 0xd:
						case 0xe:
						case 0xf:
							mulInputA_24 = shared->eram.eramReadLatch;
							writeIRAM(mulInputA_24, mem | 0xf0);
							break;
						default:
							printf("Unknown value for mem (%02x) with opcode 0x34\n", mem);
							break;
					}
				}
				break;
			case 0x38: shared->gram[mempos] = mulInputA_24 = accA.getPipelineSat24(); break;
			case 0x3c: shared->gram[mempos] = mulInputA_24 = accB.getPipelineSat24(); break;
			
			// UNSAT/CLAMP
			case 0x40: iram[mempos] = mulInputA_24 = accA.getPipelineRaw24(); break;
			case 0x44: iram[mempos] = mulInputA_24 = accA.getPipelineRaw24(); clr = true; break;
			case 0x48: iram[mempos] = mulInputA_24 = std::max(0, accA.getPipelineSat24()); break;
			case 0x4c: iram[mempos] = mulInputA_24 = std::max(0, accA.getPipelineSat24()); clr = true; break;
			
			case 0x50:
				// Super unsure here! The hardware seems to be clearing the acc, but it works better without
				clr = true;
				setcondition = 2;
				break;
			case 0x54: printf("Mysterious opcode 54 at pc = %04x\n", pc - 1); break; // TODO: what is this?
			case 0x58: iram[mempos] = mulInputA_24 = accA.getPipelineSat24(); break;
			case 0x5c: acc = true; iram[mempos] = mulInputA_24 = accB.getPipelineSat24(); break;
			
			// TODO: mask 0x7fffff might be different, but the important thing is to remove the sign
			case 0x60: mulInputA_24 = (~mulInputA_24 & 0x7fffff); break;
			case 0x64: clr = true; mulInputA_24 = (~mulInputA_24 & 0x7fffff); break;
			case 0x68:
				iram[mempos] = mulInputA_24 = accA.getPipelineSat24();
				if (mulInputA_24 >= 0) mulInputA_24 = ~mulInputA_24;
				mulInputA_24 &= 0x7fffff;
				break;
			case 0x6c:
				iram[mempos] = mulInputA_24 = accA.getPipelineSat24();
				if (mulInputA_24 >= 0) mulInputA_24 = ~mulInputA_24;
				mulInputA_24 &= 0x7fffff;
				clr = true;
				break;
			case 0x70: mulInputA_24 = (~mulInputA_24 & 0x7fffff); break;
			case 0x74: clr = true; mulInputA_24 = (~mulInputA_24 & 0x7fffff); break;
			case 0x78:
				iram[mempos] = mulInputA_24 = accA.getPipelineSat24();
				if (mulInputA_24 < 0) mulInputA_24 = ~mulInputA_24;
				mulInputA_24 &= 0x7fffff;
				break;
			case 0x7c:
				iram[mempos] = mulInputA_24 = accA.getPipelineSat24();
				if (mulInputA_24 < 0) mulInputA_24 = ~mulInputA_24;
				mulInputA_24 &= 0x7fffff;
				clr = true;
				break;
			
			default: printf("mysterious\n"); break; // TODO: few more opcodes here
		}
		
		if (skipfield & 1) mulInputA_24 = 0;

		// Multiplier
		DspAccumulator &storeAcc = (acc) ? accB : accA;
		if (clr) storeAcc = 0;
		int64_t mulResult = (int64_t)se<24>(mulInputA_24) * (int64_t) se<8>(mulInputB_24);
		mulResult >>= shift;
		storeAcc += (int32_t)mulResult;

		skipfield >>= 1;
		if (setcondition)
		{
			setcondition--;
			if (!setcondition) skipfield |= ((storeAcc.rawFull() < 0) ? 0x3c0 : 0x30) >> 1;
		}

		last_mulInputA_24 = mulInputA_24;
		lastMul30 = (op == 0x30);
		accA.storePipeline();
		accB.storePipeline();
	}

	static constexpr int64_t PRAM_SIZE = 768, IRAM_SIZE = 0x100, IRAM_MASK = IRAM_SIZE - 1;
	void jumpto(uint16_t newpc) { if (pcjumpat != -1) printf("Oh no! Jump overlap!\n"); pcjumpto = newpc; pcjumpat = pc + 2;}

	int32_t iram[IRAM_SIZE] {}, last_mulInputA_24 {0}, last_mulInputB_24 {0}, skipfield {0};
	bool lastMul30 = false;
	const uint32_t *pram {nullptr};
	uint32_t pc = 0, iramPos = 0;
	int pcjumpat {-1}, pcjumpto {-1}; // TODO: are jumps shared by the two cores? -> no they seem to be independent
	uint8_t setcondition {0};
	DspAccumulator accA, accB;
	SharedState<lg2eram_size> *shared;
};


template<int lg2eram_size>
class ESP
{
public:
	ESP() {
		core0.setup((uint32_t*)&intmem[0x0000], &shared);
		core1.setup((uint32_t*)&intmem[0x1000], &shared);
		reset();
	}

	void reset() {
		memset(intmem, 0, sizeof(intmem));
		core0.reset();
		core1.reset();
		shared.reset();
	}

	// Interface with hardware / other chips etc.
	void writeGRAM(int32_t val, uint8_t offset) {core0.writeGRAM(val, offset);}
	int32_t readGRAM(uint8_t offset) const {return core0.readGRAM(offset);}
	int32_t readIRAM0(uint8_t offset) const {return core0.readIRAM(offset);}
	int32_t readIRAM1(uint8_t offset) const {return core1.readIRAM(offset);}
	void writeIRAM0(int32_t val, uint8_t offset) {core0.writeIRAM(val, offset);}
	void writeIRAM1(int32_t val, uint8_t offset) {core1.writeIRAM(val, offset);}
	uint8_t readPRAM(size_t offset) const {return intmem[offset];}

	// For running apart from h8 emu
	void writePMem(uint16_t address, uint8_t val) {intmem[address] = val;}
	void writePMem32(uint16_t address, uint32_t val, bool _recompile = true) {((uint32_t*)&intmem[0])[address] = val;} // addresses are /4 here
	uint32_t readHostReg() const {return *(uint32_t *)&shared.readback_regs[0];}
	uint8_t readDint() const {return shared.dintpins;}
	void step_cores() { core1.steperam(); core1.step(); core0.step();}
	void sync_cores() {core0.sync(); core1.sync(); if (lg2eram_size) shared.eram.tickSample();}

	uint8_t readuC(uint32_t address) { return shared.readback_regs[address & 3]; }

	void writeuC(uint32_t address, uint8_t value) {
		address&=0x3fff;
		program_writing_word[address & 3] = value;

		if (address == 0x2003) {
			if_mode = value;
		}
		else if (if_mode == 0x54 && (address & 3) == 3) {
			const int addr = address >> 2;

			auto oldValue = *reinterpret_cast<uint32_t*>(&intmem[(addr << 2)]);

			intmem[(addr<<2) + 0] = program_writing_word[0];
			intmem[(addr<<2) + 1] = program_writing_word[1];
			intmem[(addr<<2) + 2] = program_writing_word[2];
			intmem[(addr<<2) + 3] = program_writing_word[3];

			auto newValue = *reinterpret_cast<uint32_t*>(&intmem[(addr << 2)]);

			if (newValue != oldValue)
				programDirty = true;
		}
		else if (if_mode == 0x55 && (address & 3) == 3) {
			const int addr = address >> 2;
			uint32_t *pmem = (uint32_t*)intmem;
			
			pmem[addr] &= 0xffffff00;
			pmem[addr] |= program_writing_word[0] & 0xff;
			pmem[addr] &= 0xfffffcff;
			pmem[addr] |= (program_writing_word[1] & 3) << 8;
			pmem[addr + 1] &= 0xffffffc0;
			pmem[addr + 1] |= (program_writing_word[1] >> 2) & 0x3f;
			pmem[addr + 1] &= 0xfffffc3f;
			pmem[addr + 1] |= (program_writing_word[2] & 0xf) << 6;

			coefsDirty = true;
		}
		else if (if_mode == 0x56 && (address & 3) == 3) {
			const int addr = address >> 2;
			uint32_t *pmem = (uint32_t*)intmem;

			const std::array<uint32_t ,5> oldValues{
				pmem[addr],
				pmem[addr + 1],
				pmem[addr + 2],
				pmem[addr + 3],
				pmem[addr + 4]
			};
			
			pmem[addr] &= 0xf07fffff;
			pmem[addr] |= ((program_writing_word[0] >> 3) & 0x1f) << 23;
			pmem[addr + 1] &= 0xf07fffff;
			pmem[addr + 1] |= (program_writing_word[1] & 0x1f) << 23;
			pmem[addr + 2] &= 0xfc7fffff;
			pmem[addr + 2] |= ((program_writing_word[1] >> 5) & 7) << 23;
			pmem[addr + 2] &= 0xf3ffffff;
			pmem[addr + 2] |= (program_writing_word[2] & 3) << 26;
			pmem[addr + 3] &= 0xf07fffff;
			pmem[addr + 3] |= ((program_writing_word[2] >> 2) & 0x1f) << 23;
			pmem[addr + 4] &= 0xff7fffff;
			pmem[addr + 4] |= ((program_writing_word[2] >> 7) & 1) << 23;
			pmem[addr + 4] &= 0xf0ffffff;
			pmem[addr + 4] |= (program_writing_word[3] & 0xf) << 24;

			const std::array<uint32_t, 5> newValues{
				pmem[addr],
				pmem[addr + 1],
				pmem[addr + 2],
				pmem[addr + 3],
				pmem[addr + 4]
			};

			if (newValues != oldValues)
				programDirty = true;
		}
		else if (if_mode == 0x57 && (address & 3) == 3) {
			addr_sel = address & ~3;
			shared.readback_regs[0] = intmem[addr_sel+0];
			shared.readback_regs[1] = intmem[addr_sel+1];
			shared.readback_regs[2] = intmem[addr_sel+2];
			shared.readback_regs[3] = intmem[addr_sel+3];
			// printf("Asic sel addr 0x%04x\n",addr_sel);
		}
		else {
			// printf("Asic unknown %x write 0x%06x, 0x%02x\n",if_mode,address, value&255);
		}
	}

	bool programDirty = true;
	bool coefsDirty = true;


	static constexpr int32_t clockrate = 768 * 2 * 44100; // This is clockrate. It MAY NOT be fs in that last term. DO NOT substitute fs in there.
	const int32_t fs {44100}; // samplerate.
	const int32_t stepsPerFS {clockrate / (fs * 2)}; // probably 768 or 384. Definitely not more than 768.
	
	ESPCore<lg2eram_size> core0, core1;
	char ss[64];
	uint8_t if_mode = 0;
	uint32_t addr_sel = 0;
	uint8_t program_writing_word[4] {};
	uint8_t intmem[0x4000] = {0};
	SharedState<lg2eram_size> shared;
};

} // namespace esp
