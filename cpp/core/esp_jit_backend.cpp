#include "esp_jit_backend.h"

#if defined(ESP_ENABLE_ASMJIT)

#include <sstream>
#include <stdexcept>

#include "asmjit/asmjit.h"
#include "asmjit/a64.h"
#include "esp_jit_arm64.h"
#include "esp_jit_types.h"
#include "esp_jit_x64.h"

namespace esp
{
	class AsmJitCompiler final : public EspJitCompiler
	{
	public:
		void release(EspRunCore fn) override
		{
			if (fn)
				m_rt.release(fn);
		}

		void compile(EspRunCore* dest, const JitInputData& input, const EmitJitProgramFn& emitProgram) override
		{
			asmjit::CodeHolder code;
			code.init(m_rt.environment());

			m_logger.addFlags(asmjit::FormatFlags::kHexImms | asmjit::FormatFlags::kMachineCode);
			// code.setLogger(&m_logger);

			esp::Builder builder(&code);
			esp::EspJit jit(builder, input);
			emitProgram(jit);

			builder.finalize();

			const auto err = m_rt.add(dest, &code);
			if (err)
			{
				const auto* const errString = asmjit::DebugUtils::errorAsString(err);
				std::stringstream ss;
				ss << "JIT failed: " << err << " - " << errString;
				throw std::runtime_error(ss.str());
			}
		}

	private:
		asmjit::JitRuntime m_rt;
		asmjit::FileLogger m_logger;
	};

	std::unique_ptr<EspJitCompiler> createDefaultJitCompiler()
	{
		return std::make_unique<AsmJitCompiler>();
	}
}

#endif
