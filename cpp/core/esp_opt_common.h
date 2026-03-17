#pragma once

#include <cstdint>
#include <stdio.h>
#include "esp.hpp"

namespace esp {

constexpr int PRAM_SIZE = 768;

struct CoreData {
	int32_t *hostRegPtr;
	int32_t *eramPtr;
	int64_t accs[6];
	int32_t mulcoeffs[8];
	int8_t coefs[PRAM_SIZE];
	int8_t shiftAmounts[PRAM_SIZE];
};

enum { kNone = 0, kSavesA = 1, kSavesB = 2 };

struct MemAccess
{
	uint8_t save{kNone};
	bool writesAccB{false}, clr{false}, nop{false}; // stage 1, extract these.
	bool accGetsUsed{false}, nomac{false};
	uint16_t writePC{0}; // stage 2, compute this
	int readReg{-1}, srcReg{-1}, destReg{-1}; // stage 3, compute this
};

enum ESPInstrOptType
{
	kNop, kMAC, kStoreIRAM, kStoreGRAM, kMulCoef, kWriteMulCoef, kDMAC,
	kWriteEramVarOffset, kWriteEramWriteLatch, kReadEramReadLatch, kWriteHost,
	kReadGRAM, kStoreIRAMUnsat, kStoreIRAMRect, kSetCondition, kInterp,
	kInterpStorePos, kInterpStoreNeg, kJmpZero, kJmpLessZero, kJmpGreaterZero, kJmpAlways
};

struct ESPOptInstr
{
	// raw data
	uint32_t coef = 0, shift = 0, mem = 0, op = 0;
	int8_t coefSigned = 0;

	// processed data
	uint8_t shiftAmount = 0;
	bool useImm = false;
	int32_t imm = 0;
	int16_t jmpDest = 0;
	ESPInstrOptType opType = kMAC;

	// from analysis
	MemAccess m_access;
	bool skippablePos = false, skippableNeg = false;
	bool triggerJump = false, jmpTarget = false;

	ESPOptInstr()
	= default;

	ESPOptInstr(uint32_t instr)
	{
		mem = (instr >> 10) & 0xff;
		op = (instr >> 16) & 0x7c;
		coef = instr & 0xff;
		coefSigned = se<8>(coef);
		shift = (instr >> 8) & 3;
		shiftAmount = (0x3567 >> (shift << 2)) & 0xf; // this is shift amount. pick the value 3/5/6/7 using bits 8,9.
		if (op == 0x20 || op == 0x24) shiftAmount = (shift & 1) ? 6 : 7;

		switch (mem)
		{
		case 1: useImm = true;
			imm = 0x10;
			break; // 4
		case 2: useImm = true;
			imm = 0x400;
			break; // 10
		case 3: useImm = true;
			imm = 0x10000;
			break; // 16
		case 4: useImm = true;
			imm = 0x400000;
			break; // 22
		}

		if (op == 0 && mem == 0 && shift == 0 && coef == 0)
		{
			opType = kNop;
		}
		else
		{
			switch (op)
			{
			case 0x08:
			case 0x0c:
			case 0x18:
			case 0x1c:
			case 0x58:
			case 0x5c: opType = kStoreIRAM;
				break;
			case 0x20:
			case 0x24: opType = kReadGRAM;
				break;
			case 0x30: opType = kMulCoef;
				break;

			case 0x34:
				if (mem < 0xa0 || (mem & 0xf0) == 0xb0) printf("Unexpected value for mem (%02x) with opcode 0x34\n",
																											mem);
				if (mem >= 0xa0 && mem < 0xb0) opType = kWriteMulCoef;
				if (mem >= 0xc0)
				{
					switch (mem & 0xf)
					{
					case 0x0:
						opType = kJmpZero;
						jmpDest = coef | ((int)shift << 8);
						break;
					case 0x1:
						opType = kJmpLessZero;
						jmpDest = coef | ((int)shift << 8);
						break;
					case 0x2:
						opType = kJmpGreaterZero;
						jmpDest = coef | ((int)shift << 8);
						break;
					case 0x3:
						opType = kJmpAlways;
						jmpDest = coef | ((int)shift << 8);
						break;
					
					// TODO
					case 0x4:
						printf("int pins!\n");
						break;

					case 0x6: opType = kDMAC;
						break;
					case 0x7: opType = kWriteEramVarOffset;
						break;
					case 0xa: opType = kWriteHost;
						break;
					case 0xb: opType = kWriteEramWriteLatch;
						break;
					case 0xc:
					case 0xd:
					case 0xe:
					case 0xf:
						opType = kReadEramReadLatch;
						break;
					default:
						printf("Unknown value for mem (%02x) with opcode 0x34\n", mem);
						break;
					}
				}
				break;

			case 0x38:
			case 0x3c: opType = kStoreGRAM;
				break;
			case 0x40:
			case 0x44: opType = kStoreIRAMUnsat;
				break;
			case 0x48:
			case 0x4c: opType = kStoreIRAMRect;
				break;
			case 0x50: opType = kSetCondition;
				printf("CONDITIONS!\n");
				break;
			case 0x60:
			case 0x64:
			case 0x70:
			case 0x74: opType = kInterp;
				break;
			case 0x68:
			case 0x6c: opType = kInterpStorePos;
				break;
			case 0x78:
			case 0x7c: opType = kInterpStoreNeg;
				break;

			case 0x54:
			case 0x28:
			case 0x2c: printf("Mysterious opcode %02x\n", op);
				break; // TODO: what is this?
			}
		}
	}
};

} // namespace esp
