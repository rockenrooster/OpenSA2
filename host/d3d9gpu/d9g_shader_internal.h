// Internal to host 2's shader translator (d9g_shader*.cpp): the pieces the translator's units share
// and the device never sees. Includes dxbc-spirv's headers, so only the translator's units (built
// with DXVK's flags by shader/build_translator.sh) include it.
#pragma once

#include "d9g_shader.h"

#include <ir/ir.h>
#include <ir/ir_builder.h>
#include <ir/passes/ir_pass_lower_io.h>
#include <util/util_log.h>

#include <cstdint>
#include <string>

namespace d9g::shader {

// The default floating-point environment for the duration of a compile (R0 risk 5): MXCSR
// 0x1F80 and the x87 control word 0x037F, restored afterwards.
class FpEnvGuard {
 public:
  FpEnvGuard();
  ~FpEnvGuard();
  FpEnvGuard(const FpEnvGuard&) = delete;
  FpEnvGuard& operator=(const FpEnvGuard&) = delete;
 private:
  uint32_t m_mxcsr = 0;
  uint16_t m_x87 = 0;
};

// dxbc-spirv's per-thread logger (util/util_log.h): warnings and errors are kept, and an error
// fails the translation.
class CapturingLogger : public dxbc_spv::util::Logger {
 public:
  void message(dxbc_spv::util::LogLevel severity, const char* message) override;
  dxbc_spv::util::LogLevel getMinimumSeverity() override { return dxbc_spv::util::LogLevel::eWarn; }
  std::string text;
  uint32_t errors = 0;
};

// What DXVK's units log through dxvk::Logger::err (d9g_shader_shims.cpp), per thread.
extern thread_local std::string g_dxvk_errors;

std::string debug_name_for(const void* bytecode, size_t bytes, Stage stage, const ShaderProfile& profile);

// The register space of the state buffer (spec data and push data): DXVK's SpecDataSet
// (DxvkIrShader::SpecDataSet, dxvk_shader_ir.h), the space it lowers spec constants into.
constexpr uint32_t kStateSpace = 7u;
// The register space of the border fallback's colour buffer (Variant::border_u/border_v): one past
// the state buffer's, so it can never be one of DXVK's own constant buffers (space 0).
constexpr uint32_t kBorderSpace = 8u;

// d9g_shader_abi.cpp: the SDL ABI pass on a deserialized IR: spec constants and push data into the
// state buffer (its sub-dword members as 32-bit words with state_words, ShaderProfile::state_words),
// the dynamic constant buffer capped at SDL's window. With spec (kSpecDwords words), each read of a
// spec dword is that word, a constant (build's specialized modules). Returns false with error set.
bool lower_sdl_abi(dxbc_spv::ir::Builder& builder, Stage stage, bool state_words, std::string& error,
                   bool& capped_dynamic, const uint32_t* spec = nullptr);

// d9g_shader_link.cpp: DxvkIrShader::getCode's linkage step (dxvk_shader_ir.cpp:2033-2075) on the
// pixel shader's IR against the vertex shader's outputs. Sets io_rewritten when DXVK's
// checkStageCompatibility fails and the I/O was resolved.
bool link_pixel_io(dxbc_spv::ir::Builder& ps, const Translated& vs, const Translated& psInfo,
                   const Linkage& linkage, bool& io_rewritten, std::string& error);

bool deserialize(const std::vector<uint8_t>& ir, dxbc_spv::ir::Builder& builder);

// Translator faults for the canonical diff's controls (check C3): SA2_D3D9GPU_SHADER_FAULT names
// one, and it is honoured only in a test run (SA2_D3D9GPU_TEST_RUN=1); anywhere else translate()
// refuses it. Each is one wrong rule the comparer must catch, naming the shader and function.
enum class Fault {
  None, UminDropped, PointSizeF32, Fp16, NoContractionStripped, ConstantsSwapped, SamplersSwapped,
  DenormFtz, FloatEmulationEnabled, LinkageSkipped, NoFpGuard, LowerDotOff, BorderOff,
  // the sampler-mode specialization's controls (Variant::sampler_modes): each call reads
  // register r + 1's mode; the word ignored (the module is the base's); each mode with its depth bit
  // flipped
  ModesSlotShifted, ModesSkipped, ModesInverted,
};
// Fault::None and an empty error when no fault is asked for; error set when one is asked for
// outside a test run or is not known.
Fault active_fault(std::string& error);
void apply_ir_fault(dxbc_spv::ir::Builder& builder, Fault fault);

}  // namespace d9g::shader
