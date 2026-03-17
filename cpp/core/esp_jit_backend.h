#pragma once

#include <functional>
#include <memory>

#include "esp_jit.h"

namespace esp
{
	using EspRunCore = void(*)(int8_t* coefsPtr, int32_t* iramPtr, int32_t* gramPtr, CoreData* varPtr, uint32_t eramPos, uint32_t iramPos, int64_t shouldJump, int64_t unused);
	using EmitJitProgramFn = std::function<void(EspJitBase&)>;

	class EspJitCompiler
	{
	public:
		virtual ~EspJitCompiler() = default;

		virtual void release(EspRunCore fn) = 0;
		virtual void compile(EspRunCore* dest, const JitInputData& input, const EmitJitProgramFn& emitProgram) = 0;
	};

	std::unique_ptr<EspJitCompiler> createDefaultJitCompiler();

#if !defined(ESP_ENABLE_ASMJIT)
	inline std::unique_ptr<EspJitCompiler> createDefaultJitCompiler()
	{
		return nullptr;
	}
#endif
}
