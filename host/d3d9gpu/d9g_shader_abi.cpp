// Host 2's shader translator, the SDL half (R1 U2): DxvkIrShader::getCode's steps
// (src/dxvk/dxvk_shader_ir.cpp:2022-2143) with SDL_gpu's ABI in place of DXVK's binding model.
//
// What differs from DXVK, and only this:
//   - spec constants: dxbc-spirv's own LowerIoPass::lowerSpecConstantsToCbv, which is what DXVK's
//     graphics-pipeline-library variant does (:2077), for every build, linked or not;
//   - push data: DXVK's two push blocks (D3D9SharedPushData, D3D9VsPushData) become members of the
//     same uniform buffer, after the seven spec dwords, with DXVK's member layout;
//   - descriptors: SDL_gpu's sets and slots (d9g_shader.h) instead of DXVK's heap and sets;
//   - the dynamic float buffer: declared at SDL's 4 KiB window (256 vectors) where DXVK declares
//     its count plus one zero element (d3d9_shader.cpp:328-343); the device never maps a draw
//     whose dynamic float count would read the 257th;
//   - with the profile's state_words (a device without 8- and 16-bit access), the push blocks'
//     u8 and u16 members are declared as the 32-bit words that hold them, at the same bytes, and
//     each read of one is a bit extract of its word (lower_narrow below).
// Everything else (the SPIR-V options, the linkage I/O resolution, the builder) is DXVK's.
#include "d9g_shader.h"
#include "d9g_shader_internal.h"

#include <ir/ir_serialize.h>
#include <spirv/spirv_builder.h>
#include <spirv/spirv_mapping.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <unordered_map>

namespace d9g::shader {

using namespace dxbc_spv;

namespace {

// D3D9ShaderResourceMapping::CbvIndex (src/d3d9/d3d9_shader.h:53-63)
constexpr uint32_t kCbvVsClipPlanes = 0u, kCbvVsStatic = 3u, kCbvVsDynamic = 4u, kCbvPsShared = 5u, kCbvPsStatic = 6u;

class SdlResourceMapping : public spirv::ResourceMapping {
 public:
  SdlResourceMapping(Stage stage, std::vector<uint32_t> samplers) : m_stage(stage), m_samplers(std::move(samplers)) {}

  spirv::DescriptorBinding mapDescriptor(ir::ScalarType type, uint32_t regSpace, uint32_t regIndex) override {
    spirv::DescriptorBinding b = {};
    bool vs = m_stage == Stage::Vertex;
    switch (type) {
      case ir::ScalarType::eCbv:
        b.set = vs ? 1u : 3u;
        if (regSpace == kStateSpace && regIndex == 0u)
          b.binding = vs ? kVsState : kPsState;
        else if (regSpace == kBorderSpace && regIndex == 0u && !vs)
          b.binding = kPsBorderColours;
        else if (regSpace == 0u && vs && regIndex == kCbvVsStatic)
          b.binding = kVsStaticConstants;
        else if (regSpace == 0u && vs && regIndex == kCbvVsDynamic)
          b.binding = kVsDynamicConstants;
        else if (regSpace == 0u && vs && regIndex == kCbvVsClipPlanes)
          b.binding = kVsClipPlanes;
        else if (regSpace == 0u && !vs && regIndex == kCbvPsStatic)
          b.binding = kPsStaticConstants;
        else if (regSpace == 0u && !vs && regIndex == kCbvPsShared)
          b.binding = kPsTextureStages;
        else
          fail("a constant buffer outside SDL's slot table (space " + std::to_string(regSpace) + ", index " + std::to_string(regIndex) + ")");
        m_slots.push_back(b.binding);
        break;
      case ir::ScalarType::eSrv:
      case ir::ScalarType::eSampler: {
        b.set = vs ? 0u : 2u;
        // dxbc-spirv's SM3 front end declares a sampler register's sampler in space 1 and its
        // image in space 0 (sm3_resources.cpp); like DXVK (D3D9ShaderConverter::
        // determineResourceIndex, d3d9_shader.cpp:1133-1156), only the register index matters.
        auto it = std::find(m_samplers.begin(), m_samplers.end(), regIndex);
        if (it == m_samplers.end())
          fail("a sampler register outside the shader's declared set");
        b.binding = uint32_t(it - m_samplers.begin());
      } break;
      default:
        fail("a descriptor type SDL_gpu's graphics stages have no slot for");
    }
    return b;
  }

  uint32_t mapPushData(ir::ShaderStageMask) override {
    fail("push data left after the SDL ABI pass");
    return 0u;
  }

  const std::string& error() const { return m_error; }
  const std::vector<uint32_t>& slots() const { return m_slots; }

 private:
  void fail(const std::string& e) {
    if (m_error.empty())
      m_error = e;
  }
  Stage m_stage;
  std::vector<uint32_t> m_samplers;
  std::vector<uint32_t> m_slots;
  std::string m_error;
};

// getCode's SPIR-V options (dxvk_shader_ir.cpp:2086-2134) for the profile.
spirv::SpirvBuilder::Options spirv_options(const ShaderProfile& p) {
  spirv::SpirvBuilder::Options options = {};
  options.includeDebugNames = true;
  options.nvRawAccessChains = p.supports_nv_raw_access_chains;
  options.dualSourceBlending = false;  // D3D9 has no dual-source blending (linkage->fsDualSrcBlend)

  if (p.independent_denorm_mode && p.supports_rte[1] && p.supports_denorm_flush[1]) {
    if (p.supports_rte[0]) options.supportedRoundModesF16 |= ir::RoundMode::eNearestEven;
    if (p.supports_rte[1]) options.supportedRoundModesF32 |= ir::RoundMode::eNearestEven;
    if (p.supports_rte[2]) options.supportedRoundModesF64 |= ir::RoundMode::eNearestEven;
    if (p.supports_rtz[0]) options.supportedRoundModesF16 |= ir::RoundMode::eZero;
    if (p.supports_rtz[1]) options.supportedRoundModesF32 |= ir::RoundMode::eZero;
    if (p.supports_rtz[2]) options.supportedRoundModesF64 |= ir::RoundMode::eZero;
    if (p.supports_denorm_flush[0]) options.supportedDenormModesF16 |= ir::DenormMode::eFlush;
    if (p.supports_denorm_flush[1]) options.supportedDenormModesF32 |= ir::DenormMode::eFlush;
    if (p.supports_denorm_flush[2]) options.supportedDenormModesF64 |= ir::DenormMode::eFlush;
    if (p.supports_denorm_preserve[0]) options.supportedDenormModesF16 |= ir::DenormMode::ePreserve;
    if (p.supports_denorm_preserve[1]) options.supportedDenormModesF32 |= ir::DenormMode::ePreserve;
    if (p.supports_denorm_preserve[2]) options.supportedDenormModesF64 |= ir::DenormMode::ePreserve;
    if (p.supports_sz_inf_nan_preserve[1])
      options.floatControls2 = p.supports_float_controls2;
  }

  options.supportsZeroInfNanPreserveF16 = p.supports_sz_inf_nan_preserve[0];
  options.supportsZeroInfNanPreserveF32 = p.supports_sz_inf_nan_preserve[1];
  options.supportsZeroInfNanPreserveF64 = p.supports_sz_inf_nan_preserve[2];
  options.maxCbvSize = p.max_uniform_buffer_size;
  options.maxCbvCount = p.max_uniform_buffer_count;
  return options;
}

// D3D9SharedPushData and D3D9VsPushData as DXVK's lowering declares them
// (d3d9_shader.cpp:226-264, src/d3d9/d3d9_state.h:37-60).
ir::Type shared_push_type() {
  return ir::Type()
      .addStructMember(ir::ScalarType::eU8, 3u)
      .addStructMember(ir::ScalarType::eU8)
      .addStructMember(ir::ScalarType::eF32)
      .addStructMember(ir::ScalarType::eF32)
      .addStructMember(ir::ScalarType::eF32);
}

ir::Type vs_push_type() {
  return ir::Type()
      .addStructMember(ir::ScalarType::eU16)
      .addStructMember(ir::ScalarType::eU16)
      .addStructMember(ir::ScalarType::eU16)
      .addStructMember(ir::ScalarType::eU16);
}

const char* const kSharedNames[5] = {"fogColor", "alphaRef", "fogDistanceScale", "fogDistanceEnd", "fogDensity"};
const char* const kVsNames[4] = {"cFSize", "pointSize", "pointSizeMin", "pointSizeMax"};

// The same blocks as state_words declares them: the bytes of D3D9SharedPushData's u8x3 fogColor and
// u8 alphaRef are one u32 (fogColor[i] in bits 8i to 8i+7, alphaRef in bits 24-31; the state buffer is
// little-endian, d9g_draw_state.cpp writes bytes 28-31), and D3D9VsPushData's four u16 are two u32
// (member m in bits 16(m % 2) to 16(m % 2)+15 of word m / 2). The layout, the offsets and the size are
// the ones above, so the device's bytes are unchanged.
ir::Type shared_word_type() {
  return ir::Type()
      .addStructMember(ir::ScalarType::eU32)
      .addStructMember(ir::ScalarType::eF32)
      .addStructMember(ir::ScalarType::eF32)
      .addStructMember(ir::ScalarType::eF32);
}

ir::Type vs_word_type() {
  return ir::Type().addStructMember(ir::ScalarType::eU32).addStructMember(ir::ScalarType::eU32);
}

const char* const kSharedWordNames[4] = {"fogColorAlphaRef", "fogDistanceScale", "fogDistanceEnd", "fogDensity"};
const char* const kVsWordNames[2] = {"cFSizePointSize", "pointSizeMinMax"};

// Where member m of DXVK's block lands in the state buffer's type: its own member (bits 0), or the
// word that holds it and its first bit and width.
struct Place {
  uint32_t member = 0, bit = 0, bits = 0;
};

Place place_of(bool vs_block, uint32_t m, uint32_t base, bool words) {
  if (!words) return {base + m, 0u, 0u};
  if (vs_block) return {base + m / 2u, 16u * (m % 2u), 16u};
  if (m == 0u) return {base, 0u, 8u};    // fogColor: element i at 8i
  if (m == 1u) return {base, 24u, 8u};   // alphaRef
  return {base + m - 1u, 0u, 0u};
}

bool is_u32(const ir::Type& t) {
  return t.isScalarType() && t.getBaseType(0u).getBaseType() == ir::ScalarType::eU32;
}

// state_words: every use of `def`, a sub-dword value (u8, u16 or a u8 vector) read from bits [bit,
// bit + bits) of `word` (each vector element `bits` wide), made a use of a u32 bit extract of the
// word, and `def` removed. DXVK's lowering converts each such load at once (d3d9_shader.cpp: alphaRef
// and the dynamic float count by ConvertItoI to u32, the point sizes by ConvertItoF, the fog colour
// by CompositeExtract then ConvertItoF); any other use is refused by name, so a module that would still
// need sub-dword types is never built.
bool lower_narrow(ir::Builder& b, ir::SsaDef def, ir::SsaDef word, uint32_t bit, uint32_t bits, const char* name,
                  std::string& error) {
  const ir::Type type = b.getOp(def).getType();
  util::small_vector<ir::SsaDef, 16u> uses;
  b.getUses(def, uses);
  for (auto use : uses) {
    const ir::Op op = b.getOp(use);
    switch (op.getOpCode()) {
      case ir::OpCode::eConvertItoI:
        if (!type.isScalarType() || !is_u32(op.getType())) break;
        b.rewriteOp(use, ir::Op::UBitExtract(ir::ScalarType::eU32, word, b.makeConstant(bit), b.makeConstant(bits)));
        continue;
      case ir::OpCode::eConvertItoF: {
        if (!type.isScalarType()) break;
        auto x = b.addBefore(use, ir::Op::UBitExtract(ir::ScalarType::eU32, word, b.makeConstant(bit), b.makeConstant(bits)));
        b.rewriteOp(use, ir::Op(op).setOperand(0u, x));
      } continue;
      case ir::OpCode::eCompositeExtract: {
        const auto& address = b.getOpForOperand(op, 1u);
        if (!type.isVectorType() || !address || !address.isConstant() || address.getOperandCount() != 1u) break;
        const uint32_t i = uint32_t(address.getOperand(0u));
        if (i >= type.getBaseType(0u).getVectorSize()) break;
        if (!lower_narrow(b, use, word, bit + bits * i, bits, name, error)) return false;
      } continue;
      case ir::OpCode::eDebugName:
        b.remove(use);
        continue;
      default:
        break;
    }
    std::ostringstream s;
    s << op.getOpCode();
    error = std::string("unmapped: the state buffer's ") + name + " is read by " + s.str() +
            ", which the 32-bit state words (no 8- or 16-bit access on this device) cannot lower";
    return false;
  }
  b.remove(def);
  return true;
}

ir::SsaDef find_entry_point(ir::Builder& b) {
  for (auto it = b.getDeclarations().first; it != b.getDeclarations().second; ++it)
    if (it->getOpCode() == ir::OpCode::eEntryPoint)
      return it->getDef();
  return ir::SsaDef();
}

void remove_with_debug_names(ir::Builder& b, ir::SsaDef def) {
  util::small_vector<ir::SsaDef, 16u> uses;
  b.getUses(def, uses);
  for (auto use : uses) {
    auto code = b.getOp(use).getOpCode();
    if (code == ir::OpCode::eDebugName || code == ir::OpCode::eDebugMemberName)
      b.remove(use);
  }
  b.remove(def);
}

}  // namespace

bool deserialize(const std::vector<uint8_t>& ir, ir::Builder& builder) {
  ir::Deserializer deserializer(ir.data(), ir.size());
  return deserializer.deserialize(builder);
}

bool lower_sdl_abi(ir::Builder& b, Stage stage, bool state_words, std::string& error, bool& capped_dynamic,
                   const uint32_t* spec) {
  auto entry = find_entry_point(b);
  if (!entry) {
    error = "no entry point";
    return false;
  }
  // Spec constants: declare spec id kSpecDwords - 1 if absent, so that lowerSpecConstantsToCbv
  // builds the full seven-dword prefix whatever the shader reads; its own RemoveUnusedPass drops
  // the extra declaration again.
  bool hasLast = false;
  for (auto it = b.getDeclarations().first; it != b.getDeclarations().second; ++it) {
    if (it->getOpCode() == ir::OpCode::eDclSpecConstant) {
      uint32_t id = uint32_t(it->getOperand(it->getFirstLiteralOperandIndex()));
      if (id >= kSpecDwords) {
        error = "a spec constant beyond D3D9SpecData's seven dwords";
        return false;
      }
      hasLast = hasLast || id == kSpecDwords - 1u;
      if (!it->getType().isScalarType() || it->getType().getBaseType(0u).getBaseType() != ir::ScalarType::eU32) {
        error = "a spec constant that is not a u32";
        return false;
      }
    }
  }
  if (!hasLast)
    b.add(ir::Op::DclSpecConstant(ir::ScalarType::eU32, entry, kSpecDwords - 1u, 0u));
  ir::SsaDef state;
  {
    ir::LowerIoPass io(b);
    state = io.lowerSpecConstantsToCbv(kStateSpace, 0u);
  }
  if (!state || b.getOp(state).getType().getStructMemberCount() != kSpecDwords) {
    error = "lowerSpecConstantsToCbv did not build the seven-dword state prefix";
    return false;
  }
  // A specialized build: every load lowerSpecConstantsToCbv made of a spec dword (each a BufferLoad
  // of member 0-6 through a DescriptorLoad of the state buffer, the only loads of it so far) becomes
  // the draw's word, a constant, so the driver folds what the word decides (the alpha test, fog,
  // sampler projection, BOOL constants...) instead of every pixel loading it and branching. The
  // state buffer stays declared, its layout as before, for the push data below.
  if (spec) {
    util::small_vector<ir::SsaDef, 64u> descriptors;
    b.getUses(state, descriptors);
    for (auto d : descriptors) {
      if (b.getOp(d).getOpCode() != ir::OpCode::eDescriptorLoad)
        continue;
      util::small_vector<ir::SsaDef, 64u> loads;
      b.getUses(d, loads);
      for (auto l : loads) {
        const ir::Op& load = b.getOp(l);
        if (load.getOpCode() != ir::OpCode::eBufferLoad)
          continue;
        const auto& address = b.getOpForOperand(load, 1u);
        if (!address || !address.isConstant() || address.getOperandCount() != 1u ||
            uint32_t(address.getOperand(0u)) >= kSpecDwords || !load.getType().isScalarType() ||
            load.getType().getBaseType(0u).getBaseType() != ir::ScalarType::eU32) {
          error = "a spec dword load that is not one whole u32 member";
          return false;
        }
        const uint32_t word = spec[uint32_t(address.getOperand(0u))];
        b.rewriteDef(l, b.makeConstant(word));
      }
      util::small_vector<ir::SsaDef, 4u> rest;
      b.getUses(d, rest);
      if (rest.empty())
        b.remove(d);
    }
  }

  // Push data: append DXVK's push blocks to the state buffer's type, then turn every PushDataLoad
  // into a load of the member it reads.
  ir::Type stateType = b.getOp(state).getType();
  const uint32_t sharedBase = stateType.getStructMemberCount();
  ir::Type shared = state_words ? shared_word_type() : shared_push_type();
  for (uint32_t i = 0; i < shared.getStructMemberCount(); i++)
    stateType.addStructMember(shared.getBaseType(i));
  uint32_t vsBase = stateType.getStructMemberCount();
  ir::Type vsType = state_words ? vs_word_type() : vs_push_type();
  if (stage == Stage::Vertex) {
    for (uint32_t i = 0; i < vsType.getStructMemberCount(); i++)
      stateType.addStructMember(vsType.getBaseType(i));
  }
  if (stateType.byteOffset(sharedBase) != kStateSharedPushOffset ||
      (stage == Stage::Vertex && stateType.byteOffset(vsBase) != kStateVsPushOffset) ||
      stateType.byteSize() != (stage == Stage::Vertex ? kStateBytesVs : kStateBytesPs)) {
    error = "the state buffer's layout is not the fixed one";
    return false;
  }
  b.rewriteOp(state, ir::Op(b.getOp(state)).setType(stateType));
  for (uint32_t i = 0; i < shared.getStructMemberCount(); i++)
    b.add(ir::Op::DebugMemberName(state, sharedBase + i, state_words ? kSharedWordNames[i] : kSharedNames[i]));
  if (stage == Stage::Vertex)
    for (uint32_t i = 0; i < vsType.getStructMemberCount(); i++)
      b.add(ir::Op::DebugMemberName(state, vsBase + i, state_words ? kVsWordNames[i] : kVsNames[i]));

  std::vector<ir::SsaDef> pushDecls;
  for (auto it = b.getDeclarations().first; it != b.getDeclarations().second; ++it)
    if (it->getOpCode() == ir::OpCode::eDclPushData)
      pushDecls.push_back(it->getDef());
  for (auto decl : pushDecls) {
    const auto& op = b.getOp(decl);
    auto stages = ir::ShaderStageMask(op.getOperand(op.getFirstLiteralOperandIndex() + 1u));
    bool local = bool(stages);
    if (local && stage != Stage::Vertex) {
      error = "pixel shader push data that is not DXVK's shared block";
      return false;
    }
    ir::Type want = local ? vs_push_type() : shared_push_type();
    if (op.getType() != want || uint32_t(op.getOperand(op.getFirstLiteralOperandIndex())) != 0u) {
      error = "push data whose type or offset is not DXVK's D3D9 block";
      return false;
    }
    uint32_t base = local ? vsBase : sharedBase;
    util::small_vector<ir::SsaDef, 64u> uses;
    b.getUses(decl, uses);
    for (auto use : uses) {
      ir::Op useOp = b.getOp(use);
      if (useOp.getOpCode() != ir::OpCode::ePushDataLoad)
        continue;
      const auto& address = b.getOpForOperand(useOp, 1u);
      if (!address || !address.isConstant() || address.getOperandCount() != 1u) {
        error = "a push data load whose address is not one constant member";
        return false;
      }
      const uint32_t m = uint32_t(address.getOperand(0u));
      auto type = useOp.getType();
      if (m >= want.getStructMemberCount() || !type.isBasicType() || want.getBaseType(m) != type.getBaseType(0u)) {
        error = "a push data load that is not a whole member";
        return false;
      }
      const Place at = place_of(local, m, base, state_words);
      auto descriptor = b.addBefore(use, ir::Op::DescriptorLoad(ir::ScalarType::eCbv, state, b.makeConstant(0u)));
      if (!at.bits) {
        b.rewriteOp(use, ir::Op::BufferLoad(type, descriptor, b.makeConstant(at.member),
                                            ir::byteSize(type.getBaseType(0u).getBaseType())));
        continue;
      }
      auto word = b.addBefore(use, ir::Op::BufferLoad(ir::ScalarType::eU32, descriptor, b.makeConstant(at.member), 4u));
      if (!lower_narrow(b, use, word, at.bit, at.bits, local ? kVsNames[m] : kSharedNames[m], error))
        return false;
    }
    remove_with_debug_names(b, decl);
  }

  // The dynamic float buffer at SDL's window.
  capped_dynamic = false;
  for (auto it = b.getDeclarations().first; it != b.getDeclarations().second; ++it) {
    if (it->getOpCode() != ir::OpCode::eDclCbv)
      continue;
    if (uint32_t(it->getOperand(1u)) != 0u || uint32_t(it->getOperand(2u)) != kCbvVsDynamic)
      continue;
    ir::Type t = it->getType();
    if (t.getArrayDimensions() == 1u && t.getArraySize(0u) * 16u > kSdlUniformWindow) {
      t.setArraySize(0u, kSdlUniformWindow / 16u);
      b.rewriteOp(it->getDef(), ir::Op(*it).setType(t));
      capped_dynamic = true;
    }
  }
  return true;
}

Built build(const Translated& shader, const ShaderProfile& profile, const Linkage* linkage, const uint32_t* spec) {
  Built out;
  FpEnvGuard env;
  CapturingLogger logger;
  if (!shader.ok) {
    out.error = "the shader did not translate";
    return out;
  }
  // dxvk_shader_ir.cpp:2079-2082: with EnableSampleRateShading (d3d9.forceSampleRateShading) DXVK
  // interpolates every pixel-shader input per sample, linked or not. Host 2 does not mirror that,
  // so it refuses rather than build a module DXVK would not.
  if (shader.stage == Stage::Pixel && profile.enable_sample_rate_shading) {
    out.error = "unmapped: sample-rate shading (d3d9.forceSampleRateShading, dxvk_shader_ir.cpp:2079-2082) is not mirrored";
    return out;
  }
  ir::Builder b;
  if (!deserialize(shader.ir, b)) {
    out.error = "ir::Deserializer failed";
    return out;
  }
  if (linkage) {
    if (shader.stage != Stage::Pixel || !linkage->vs || linkage->vs->stage != Stage::Vertex) {
      out.error = "a linked build is a pixel shader against a vertex shader";
      return out;
    }
    if (linkage->rt_nonidentity_swizzle_mask) {
      out.error = "unmapped: a render-target swizzle that is not identity";
      return out;
    }
    std::string faultError;
    if (active_fault(faultError) != Fault::LinkageSkipped &&
        !link_pixel_io(b, *linkage->vs, shader, *linkage, out.io_rewritten, out.error))
      return out;
    out.linked = true;
  }
  bool capped = false;
  if (!lower_sdl_abi(b, shader.stage, profile.state_words, out.error, capped, spec))
    return out;
  Fault fault = active_fault(out.error);
  if (!out.error.empty())
    return out;

  std::vector<uint32_t> samplers;
  for (auto it = b.getDeclarations().first; it != b.getDeclarations().second; ++it) {
    if (it->getOpCode() == ir::OpCode::eDclSampler || it->getOpCode() == ir::OpCode::eDclSrv) {
      uint32_t reg = uint32_t(it->getOperand(2u));
      if (std::find(samplers.begin(), samplers.end(), reg) == samplers.end())
        samplers.push_back(reg);
    }
  }
  std::sort(samplers.begin(), samplers.end());
  std::vector<uint32_t> bindingOrder = samplers;
  if (fault == Fault::SamplersSwapped && bindingOrder.size() >= 2u)
    std::swap(bindingOrder[0], bindingOrder[1]);
  ShaderProfile faulted = profile;
  if (fault == Fault::DenormFtz) {
    faulted.independent_denorm_mode = true;
    faulted.supports_rte[1] = true;
    faulted.supports_denorm_flush[1] = true;
  }
  SdlResourceMapping mapping(shader.stage, bindingOrder);
  spirv::SpirvBuilder spirvBuilder(b, mapping, spirv_options(faulted));
  spirvBuilder.buildSpirvBinary();
  if (!mapping.error().empty()) {
    out.error = mapping.error();
    return out;
  }
  if (logger.errors) {
    out.error = "dxbc-spirv logged errors: " + logger.text;
    return out;
  }
  out.sampler_registers = samplers;
  out.num_samplers = uint32_t(samplers.size());
  out.dynamic_capped = capped;
  for (auto it = b.getDeclarations().first; it != b.getDeclarations().second; ++it) {
    if (it->getOpCode() != ir::OpCode::eDclCbv)
      continue;
    uint32_t space = uint32_t(it->getOperand(1u)), index = uint32_t(it->getOperand(2u));
    UniformUse u;
    u.bytes = it->getType().byteSize();
    bool vs = shader.stage == Stage::Vertex;
    if (space == kStateSpace) { u.slot = vs ? kVsState : kPsState; u.kind = "state"; }
    else if (space == kBorderSpace) { u.slot = kPsBorderColours; u.kind = "border_colours"; }
    else if (index == kCbvVsStatic || index == kCbvPsStatic) { u.slot = vs ? kVsStaticConstants : kPsStaticConstants; u.kind = "static"; }
    else if (index == kCbvVsDynamic) { u.slot = kVsDynamicConstants; u.kind = "dynamic"; }
    else if (index == kCbvVsClipPlanes) { u.slot = kVsClipPlanes; u.kind = "clip_planes"; }
    else { u.slot = kPsTextureStages; u.kind = "texture_stages"; }
    if (u.bytes > kSdlUniformWindow) {
      out.error = "unmapped: a " + u.kind + " buffer of " + std::to_string(u.bytes) + " bytes exceeds SDL's 4 KiB uniform window";
      return out;
    }
    out.num_uniform_buffers = std::max(out.num_uniform_buffers, u.slot + 1u);
    out.uniforms.push_back(std::move(u));
  }
  std::sort(out.uniforms.begin(), out.uniforms.end(), [](const UniformUse& a, const UniformUse& b2) { return a.slot < b2.slot; });
  out.spirv = spirvBuilder.getSpirvBinary();
  // The vk11 profile: the module lowered to what SDL_gpu's Vulkan 1.1 device takes, or refused by name
  // (lower_vk11). The 1.3 profile never comes here, so its modules are the builder's own bytes.
  if (profile.vk11) {
    const std::string e = lower_vk11(out.spirv, shader.stage, !profile.no_clip_distance);
    if (!e.empty()) {
      out.spirv.clear();
      out.error = e;
      return out;
    }
  }
  out.ok = true;
  return out;
}

namespace {

// SPIR-V's numbers (spirv.hpp, unified1) for what lower_vk11 reads, rewrites or names.
constexpr uint32_t kSpvMagic = 0x07230203u, kSpv13 = 0x00010300u, kSpv16 = 0x00010600u;
enum SpvOp : uint32_t {
  kOpExtension = 10, kOpExtInstImport = 11, kOpMemoryModel = 14, kOpEntryPoint = 15, kOpExecutionMode = 16,
  kOpCapability = 17, kOpTypeBool = 20, kOpTypeInt = 21, kOpTypeFloat = 22, kOpTypeVector = 23, kOpTypePointer = 32,
  kOpFunction = 54, kOpFunctionEnd = 56, kOpFunctionCall = 57, kOpVariable = 59, kOpLoad = 61, kOpStore = 62,
  kOpDecorate = 71, kOpMemberDecorate = 72, kOpSelect = 169, kOpPhi = 245, kOpLabel = 248, kOpBranch = 249,
  kOpBranchConditional = 250, kOpSwitch = 251, kOpKill = 252, kOpDemoteToHelperInvocation = 5380,
};
constexpr uint32_t kCapClipDistance = 32, kCapVulkanMemoryModel = 5345, kCapPhysicalStorageBufferAddresses = 5347,
                   kCapDemoteToHelperInvocation = 5379;
constexpr uint32_t kAddrPhysicalStorageBuffer64 = 5348, kMemVulkan = 3, kAddrLogical = 0, kMemGlsl450 = 1;
constexpr uint32_t kModelVertex = 0, kModelFragment = 4;

struct SpvName { uint32_t value; const char* name; };
template <size_t N> const char* spv_name(const SpvName (&t)[N], uint32_t v) {
  for (const SpvName& n : t)
    if (n.value == v) return n.name;
  return nullptr;
}
template <size_t N> std::string spv_named(const SpvName (&t)[N], uint32_t v) {
  const char* n = spv_name(t, v);
  return n ? std::string(n) : std::to_string(v);
}

// Kept as they are: Vulkan 1.0's own capabilities for a vertex or fragment shader (SampleRateShading:
// SDL_gpu always enables it); ClipDistance where the device was made with it.
constexpr uint32_t kCapsKept[] = {0 /* Matrix */, 1 /* Shader */, 35 /* SampleRateShading */, 43 /* Sampled1D */,
                                  50 /* ImageQuery */, 51 /* DerivativeControl */};
// Named when refused (any other by number).
const SpvName kCapNames[] = {
    {9, "Float16"}, {10, "Float64"}, {11, "Int64"}, {22, "Int16"}, {25, "ImageGatherExtended"}, {32, "ClipDistance"},
    {39, "Int8"}, {42, "MinLod"}, {4427, "DrawParameters"}, {4433, "StorageBuffer16BitAccess"},
    {4434, "UniformAndStorageBuffer16BitAccess"}, {4435, "StoragePushConstant16"}, {4436, "StorageInputOutput16"},
    {4448, "StorageBuffer8BitAccess"}, {4449, "UniformAndStorageBuffer8BitAccess"}, {4450, "StoragePushConstant8"},
    {4464, "DenormPreserve"}, {4465, "DenormFlushToZero"}, {4466, "SignedZeroInfNanPreserve"}, {4467, "RoundingModeRTE"},
    {4468, "RoundingModeRTZ"}, {5301, "ShaderNonUniform"}, {5302, "RuntimeDescriptorArray"},
    {5346, "VulkanMemoryModelDeviceScope"}, {5414, "RawAccessChainsNV"}, {6029, "FloatControls2"}};
// Refused wherever they appear.
const SpvName kOpsRefused[] = {
    {39, "OpTypeForwardPointer"}, {63, "OpCopyMemory"}, {64, "OpCopyMemorySized"}, {117, "OpConvertPtrToU"},
    {120, "OpConvertUToPtr"}, {224, "OpControlBarrier"}, {225, "OpMemoryBarrier"}, {227, "OpAtomicLoad"},
    {228, "OpAtomicStore"}, {229, "OpAtomicExchange"}, {230, "OpAtomicCompareExchange"},
    {231, "OpAtomicCompareExchangeWeak"}, {232, "OpAtomicIIncrement"}, {233, "OpAtomicIDecrement"},
    {234, "OpAtomicIAdd"}, {235, "OpAtomicISub"}, {236, "OpAtomicSMin"}, {237, "OpAtomicUMin"}, {238, "OpAtomicSMax"},
    {239, "OpAtomicUMax"}, {240, "OpAtomicAnd"}, {241, "OpAtomicOr"}, {242, "OpAtomicXor"}, {252, "OpKill"},
    {331, "OpExecutionModeId"}, {332, "OpDecorateId"}, {400, "OpCopyLogical"}, {401, "OpPtrEqual"},
    {402, "OpPtrNotEqual"}, {403, "OpPtrDiff"}, {4416, "OpTerminateInvocation"}, {5381, "OpIsHelperInvocationEXT"},
    {5632, "OpDecorateString"}, {5633, "OpMemberDecorateString"}};
// What uses the quad's derivatives: the derivatives and the implicit-LOD image instructions.
const SpvName kOpsDerivative[] = {
    {87, "OpImageSampleImplicitLod"}, {89, "OpImageSampleDrefImplicitLod"}, {91, "OpImageSampleProjImplicitLod"},
    {93, "OpImageSampleProjDrefImplicitLod"}, {105, "OpImageQueryLod"}, {207, "OpDPdx"}, {208, "OpDPdy"},
    {209, "OpFwidth"}, {210, "OpDPdxFine"}, {211, "OpDPdyFine"}, {212, "OpFwidthFine"}, {213, "OpDPdxCoarse"},
    {214, "OpDPdyCoarse"}, {215, "OpFwidthCoarse"}};
// The image instructions (87-99) and the word their image-operand mask sits at.
const SpvName kOpsImage[] = {
    {87, "OpImageSampleImplicitLod"}, {88, "OpImageSampleExplicitLod"}, {89, "OpImageSampleDrefImplicitLod"},
    {90, "OpImageSampleDrefExplicitLod"}, {91, "OpImageSampleProjImplicitLod"}, {92, "OpImageSampleProjExplicitLod"},
    {93, "OpImageSampleProjDrefImplicitLod"}, {94, "OpImageSampleProjDrefExplicitLod"}, {95, "OpImageFetch"},
    {96, "OpImageGather"}, {97, "OpImageDrefGather"}, {98, "OpImageRead"}, {99, "OpImageWrite"}};
uint32_t image_operands_word(uint32_t op) {
  switch (op) {
    case 89: case 90: case 93: case 94: case 96: case 97: return 6;   // a Dref, or a gather's component
    case 99: return 4;                                                   // OpImageWrite has no result
    default: return 5;
  }
}
const SpvName kImageOperandBits[] = {{0x100, "MakeTexelAvailable"}, {0x200, "MakeTexelVisible"},
                                     {0x400, "NonPrivateTexel"},    {0x800, "VolatileTexel"},
                                     {0x1000, "SignExtend"},        {0x2000, "ZeroExtend"},
                                     {0x4000, "Nontemporal"},       {0x10000, "Offsets"}};
const SpvName kMemoryAccessBits[] = {{0x4, "Nontemporal"}, {0x8, "MakePointerAvailable"},
                                     {0x10, "MakePointerVisible"}, {0x20, "NonPrivatePointer"}};
// Kept: Vulkan 1.0's own for a vertex or fragment shader. The float-control modes are named.
constexpr uint32_t kModesKept[] = {7 /* OriginUpperLeft */, 9 /* EarlyFragmentTests */, 12 /* DepthReplacing */,
                                   14 /* DepthGreater */, 15 /* DepthLess */, 16 /* DepthUnchanged */};
const SpvName kModeNames[] = {{4459, "DenormPreserve"}, {4460, "DenormFlushToZero"}, {4461, "SignedZeroInfNanPreserve"},
                              {4462, "RoundingModeRTE"}, {4463, "RoundingModeRTZ"}};
// Kept: UniformConstant, Input, Uniform, Output, Private, Function.
constexpr uint32_t kStorageKept[] = {0, 1, 2, 3, 6, 7};
const SpvName kStorageNames[] = {{4, "Workgroup"}, {5, "CrossWorkgroup"}, {8, "Generic"}, {9, "PushConstant"},
                                 {10, "AtomicCounter"}, {11, "Image"}, {12, "StorageBuffer"},
                                 {5349, "PhysicalStorageBuffer"}};
const SpvName kDecorationsRefused[] = {{27, "UniformId"}, {40, "FPFastMathMode"}, {4469, "NoSignedWrap"},
                                       {4470, "NoUnsignedWrap"}, {5300, "NonUniform"}, {5355, "RestrictPointer"},
                                       {5356, "AliasedPointer"}};
const SpvName kAddressingNames[] = {{0, "Logical"}, {5348, "PhysicalStorageBuffer64"}};
const SpvName kMemoryModelNames[] = {{0, "Simple"}, {1, "GLSL450"}, {2, "OpenCL"}, {3, "Vulkan"}};

template <size_t N> bool in(const uint32_t (&t)[N], uint32_t v) { return std::find(std::begin(t), std::end(t), v) != std::end(t); }

// The opcodes whose word 1 is a result type and word 2 a result id (the core grammar's), for the types
// OpSelect's rule reads.
bool has_result_type(uint32_t op) {
  return op == 1 || op == 12 || (op >= 41 && op <= 46) || (op >= 48 && op <= 52) || op == 54 || op == 55 ||
         op == 57 || (op >= 59 && op <= 61) || (op >= 65 && op <= 70) || (op >= 77 && op <= 84) ||
         (op >= 86 && op <= 98) || (op >= 100 && op <= 107) || (op >= 109 && op <= 124) || (op >= 126 && op <= 152) ||
         (op >= 154 && op <= 205) || (op >= 207 && op <= 215) || op == kOpPhi;
}

struct SpvInst {
  size_t at;         // its first word
  uint32_t op, wc;
};

}  // namespace

std::string lower_vk11(std::vector<uint32_t>& words, Stage stage, bool clip_distance) {
  const std::string unmapped = "unmapped: vk11: ";
  auto malformed = [](const std::string& what) { return "vk11 lowering: malformed SPIR-V (" + what + ")"; };
  if (words.size() < 5 || words[0] != kSpvMagic) return malformed("no SPIR-V header");
  if (words[1] != kSpv16)
    return "vk11 lowering: SPIR-V version " + std::to_string((words[1] >> 16) & 0xff) + "." +
           std::to_string((words[1] >> 8) & 0xff) + "; the lowering takes the pinned translator's 1.6";
  std::vector<SpvInst> insts;
  for (size_t at = 5; at < words.size();) {
    const uint32_t wc = words[at] >> 16;
    if (!wc) return malformed("an instruction of length 0 at word " + std::to_string(at));
    if (at + wc > words.size()) return malformed("an instruction overruns the module at word " + std::to_string(at));
    insts.push_back({at, words[at] & 0xffffu, wc});
    at += wc;
  }
  auto w = [&](const SpvInst& i, uint32_t k) { return words[i.at + k]; };
  // A literal string's words from word k: the index of the word after it, 0 when it has no terminator.
  auto string_end = [&](const SpvInst& i, uint32_t k) -> uint32_t {
    for (uint32_t j = k; j < i.wc; j++) {
      const uint32_t v = w(i, j);
      if (!(v & 0xffu) || !(v & 0xff00u) || !(v & 0xff0000u) || !(v & 0xff000000u)) return j + 1;
    }
    return 0;
  };
  auto string_at = [&](const SpvInst& i, uint32_t k) {
    std::string s;
    for (uint32_t j = k; j < i.wc; j++)
      for (int b = 0; b < 4; b++) {
        const char c = char((w(i, j) >> (8 * b)) & 0xffu);
        if (!c) return s;
        s += c;
      }
    return s;
  };
  // Operand counts the reads below rely on.
  auto short_of = [&](const SpvInst& i, uint32_t n) { return i.wc < n; };

  // The tables: variables and pointer types by storage class, scalar and vector types by their
  // component count, each result's type, the functions, and their blocks.
  std::unordered_map<uint32_t, uint32_t> variable_class, type_components, result_type;
  struct Block { uint32_t label; size_t first, last; size_t function; std::vector<uint32_t> successors; };
  struct Function { uint32_t id; size_t first, last; std::vector<size_t> blocks; std::vector<uint32_t> calls; uint32_t derivative = 0; };
  std::vector<Block> blocks;
  std::vector<Function> functions;
  std::unordered_map<uint32_t, size_t> block_of, function_of;
  std::vector<size_t> entry_points, demotes;
  size_t memory_model = SIZE_MAX;
  for (size_t n = 0; n < insts.size(); n++) {
    const SpvInst& i = insts[n];
    if (has_result_type(i.op)) {
      if (short_of(i, 3)) return malformed("a short instruction (opcode " + std::to_string(i.op) + ")");
      result_type[w(i, 2)] = w(i, 1);
    }
    switch (i.op) {
      case kOpCapability: {
        if (short_of(i, 2)) return malformed("a short OpCapability");
        const uint32_t cap = w(i, 1);
        if (in(kCapsKept, cap) || (cap == kCapClipDistance && clip_distance) || cap == kCapVulkanMemoryModel ||
            cap == kCapPhysicalStorageBufferAddresses || cap == kCapDemoteToHelperInvocation)
          break;
        return unmapped + "the capability " + spv_named(kCapNames, cap) +
               " (SDL's Vulkan 1.1 device enables no feature or extension for it)";
      }
      case kOpExtension:
        return unmapped + "OpExtension " + string_at(i, 1) + " (SDL's Vulkan 1.1 device enables no extension)";
      case kOpExtInstImport:
        if (short_of(i, 3)) return malformed("a short OpExtInstImport");
        if (string_at(i, 2) != "GLSL.std.450") return unmapped + "OpExtInstImport " + string_at(i, 2) + " (only GLSL.std.450)";
        break;
      case kOpMemoryModel:
        if (short_of(i, 3)) return malformed("a short OpMemoryModel");
        if (w(i, 1) != kAddrPhysicalStorageBuffer64 || w(i, 2) != kMemVulkan)
          return unmapped + "OpMemoryModel " + spv_named(kAddressingNames, w(i, 1)) + " " + spv_named(kMemoryModelNames, w(i, 2)) +
                 "; the lowering takes PhysicalStorageBuffer64 Vulkan";
        memory_model = n;
        break;
      case kOpEntryPoint:
        if (short_of(i, 4) || !string_end(i, 3)) return malformed("a short OpEntryPoint");
        entry_points.push_back(n);
        break;
      case kOpExecutionMode:
        if (short_of(i, 3)) return malformed("a short OpExecutionMode");
        if (!in(kModesKept, w(i, 2))) return unmapped + "the execution mode " + spv_named(kModeNames, w(i, 2));
        break;
      case kOpTypeBool: case kOpTypeInt: case kOpTypeFloat:
        if (short_of(i, 2)) return malformed("a short scalar type");
        type_components[w(i, 1)] = 1;
        break;
      case kOpTypeVector:
        if (short_of(i, 4)) return malformed("a short OpTypeVector");
        type_components[w(i, 1)] = w(i, 3);
        break;
      case kOpTypePointer:
        if (short_of(i, 4)) return malformed("a short OpTypePointer");
        if (!in(kStorageKept, w(i, 2))) return unmapped + "the storage class " + spv_named(kStorageNames, w(i, 2));
        break;
      case kOpVariable:
        if (short_of(i, 4)) return malformed("a short OpVariable");
        if (!in(kStorageKept, w(i, 3))) return unmapped + "the storage class " + spv_named(kStorageNames, w(i, 3));
        variable_class[w(i, 2)] = w(i, 3);
        break;
      case kOpLoad:
        if (i.wc >= 5 && (w(i, 4) & ~3u)) {
          const uint32_t bit = w(i, 4) & ~3u & (0u - (w(i, 4) & ~3u));
          return unmapped + "the memory operand " + spv_named(kMemoryAccessBits, bit) + " on OpLoad";
        }
        break;
      case kOpStore:
        if (i.wc >= 4 && (w(i, 3) & ~3u)) {
          const uint32_t bit = w(i, 3) & ~3u & (0u - (w(i, 3) & ~3u));
          return unmapped + "the memory operand " + spv_named(kMemoryAccessBits, bit) + " on OpStore";
        }
        break;
      case kOpDecorate:
        if (short_of(i, 3)) return malformed("a short OpDecorate");
        if (spv_name(kDecorationsRefused, w(i, 2))) return unmapped + "the decoration " + spv_named(kDecorationsRefused, w(i, 2));
        break;
      case kOpMemberDecorate:
        if (short_of(i, 4)) return malformed("a short OpMemberDecorate");
        if (spv_name(kDecorationsRefused, w(i, 3))) return unmapped + "the decoration " + spv_named(kDecorationsRefused, w(i, 3));
        break;
      case kOpSelect: {
        if (short_of(i, 6)) return malformed("a short OpSelect");
        auto result = type_components.find(w(i, 1));
        if (result == type_components.end()) return unmapped + "an OpSelect whose result is not a scalar or a vector (SPIR-V 1.4's)";
        auto cond = result_type.find(w(i, 3));
        auto cond_components = cond == result_type.end() ? type_components.end() : type_components.find(cond->second);
        if (cond_components == type_components.end() || cond_components->second != result->second)
          return unmapped + "an OpSelect whose condition and result have different component counts (SPIR-V 1.4's)";
        break;
      }
      case kOpFunction:
        if (short_of(i, 5)) return malformed("a short OpFunction");
        if (!functions.empty() && functions.back().last == SIZE_MAX) return malformed("an OpFunction inside a function");
        function_of[w(i, 2)] = functions.size();
        functions.push_back({w(i, 2), n, SIZE_MAX, {}, {}, 0});
        break;
      case kOpFunctionEnd:
        if (functions.empty() || functions.back().last != SIZE_MAX) return malformed("an OpFunctionEnd outside a function");
        if (!blocks.empty() && blocks.back().function == functions.size() - 1 && blocks.back().last == SIZE_MAX)
          return malformed("a block without a terminator");
        functions.back().last = n;
        break;
      case kOpLabel:
        if (short_of(i, 2)) return malformed("a short OpLabel");
        if (functions.empty() || functions.back().last != SIZE_MAX) return malformed("an OpLabel outside a function");
        if (!blocks.empty() && blocks.back().last == SIZE_MAX) return malformed("a block without a terminator");
        block_of[w(i, 1)] = blocks.size();
        functions.back().blocks.push_back(blocks.size());
        blocks.push_back({w(i, 1), n, SIZE_MAX, functions.size() - 1, {}});
        break;
      case kOpFunctionCall:
        if (short_of(i, 4)) return malformed("a short OpFunctionCall");
        if (functions.empty()) return malformed("an OpFunctionCall outside a function");
        functions.back().calls.push_back(w(i, 3));
        break;
      case kOpDemoteToHelperInvocation:
        demotes.push_back(n);
        break;
      default:
        if (const char* name = spv_name(kOpsRefused, i.op)) return unmapped + name;
        if (spv_name(kOpsImage, i.op) && i.wc > image_operands_word(i.op) && (w(i, image_operands_word(i.op)) & ~0xffu)) {
          const uint32_t high = w(i, image_operands_word(i.op)) & ~0xffu, bit = high & (0u - high);
          return unmapped + "the image operand " + spv_named(kImageOperandBits, bit) + " on " + spv_name(kOpsImage, i.op);
        }
        if (spv_name(kOpsDerivative, i.op) && !functions.empty() && !functions.back().derivative)
          functions.back().derivative = i.op;
        break;
    }
    // A block's terminator (OpBranch, OpBranchConditional, OpSwitch, OpReturn, OpReturnValue,
    // OpUnreachable; OpKill and OpTerminateInvocation were refused above): its successors.
    if (i.op == kOpBranch || i.op == kOpBranchConditional || i.op == kOpSwitch || (i.op >= 253 && i.op <= 255)) {
      if (blocks.empty() || blocks.back().last != SIZE_MAX) return malformed("a terminator outside a block");
      Block& b = blocks.back();
      b.last = n;
      if (i.op == kOpBranch) {
        if (short_of(i, 2)) return malformed("a short OpBranch");
        b.successors.push_back(w(i, 1));
      } else if (i.op == kOpBranchConditional) {
        if (short_of(i, 4)) return malformed("a short OpBranchConditional");
        b.successors = {w(i, 2), w(i, 3)};
      } else if (i.op == kOpSwitch) {
        // the selector is 32-bit: no 64-bit integer capability is kept
        if (short_of(i, 3) || (i.wc - 3) % 2) return malformed("an OpSwitch with an odd target list");
        b.successors.push_back(w(i, 2));
        for (uint32_t k = 4; k < i.wc; k += 2) b.successors.push_back(w(i, k));
      }
    }
  }
  if (memory_model == SIZE_MAX) return malformed("no OpMemoryModel");
  if (!functions.empty() && functions.back().last == SIZE_MAX) return malformed("a function without OpFunctionEnd");
  for (const Block& b : blocks)
    for (uint32_t s : b.successors)
      if (!block_of.count(s) || blocks[block_of[s]].function != b.function)
        return malformed("a branch to a label outside its function");

  // The entry point: exactly one, of the stage's execution model, its interface variables only.
  const char* stage_name = stage == Stage::Vertex ? "vertex" : "pixel";
  if (entry_points.size() != 1) return unmapped + std::to_string(entry_points.size()) + " entry points (the lowering takes one)";
  const SpvInst& ep = insts[entry_points[0]];
  if (w(ep, 1) != (stage == Stage::Vertex ? kModelVertex : kModelFragment))
    return unmapped + "the execution model " + std::to_string(w(ep, 1)) + " in a " + stage_name + " module";
  const uint32_t interface_from = string_end(ep, 3);
  for (uint32_t k = interface_from; k < ep.wc; k++)
    if (!variable_class.count(w(ep, k))) return unmapped + "an entry point interface id that is not a global variable";
  auto entry = function_of.find(w(ep, 2));
  if (entry == function_of.end()) return malformed("the entry point names no function");

  // Each function's derivative use, its own or through what it calls (a fixed point over the calls).
  for (bool changed = true; changed;) {
    changed = false;
    for (Function& f : functions) {
      if (f.derivative) continue;
      for (uint32_t callee : f.calls) {
        auto c = function_of.find(callee);
        if (c != function_of.end() && functions[c->second].derivative) {
          f.derivative = functions[c->second].derivative;
          changed = true;
          break;
        }
      }
    }
  }
  // Each demote: [OpLabel L, OpDemoteToHelperInvocation, OpBranch M] in the fragment entry function, no
  // OpPhi reading L, L not reachable from M, and nothing reachable from M using the quad's derivatives.
  for (size_t d : demotes) {
    if (stage != Stage::Pixel) return unmapped + "a demote in a vertex module";
    if (d == 0 || d + 1 >= insts.size() || insts[d - 1].op != kOpLabel || insts[d + 1].op != kOpBranch)
      return unmapped + "a demote whose block is not [OpLabel, OpDemoteToHelperInvocation, OpBranch]";
    const uint32_t label = w(insts[d - 1], 1);
    const Block& own = blocks[block_of[label]];
    if (own.function != entry->second) return unmapped + "a demote outside the entry point's function";
    const Function& f = functions[own.function];
    for (size_t n = f.first; n < f.last; n++)
      if (insts[n].op == kOpPhi)
        for (uint32_t k = 4; k < insts[n].wc; k += 2)
          if (w(insts[n], k) == label) return unmapped + "an OpPhi takes a value from a demote's block";
    std::vector<size_t> todo = {block_of[own.successors[0]]};
    std::vector<bool> seen(blocks.size(), false);
    while (!todo.empty()) {
      const size_t b = todo.back();
      todo.pop_back();
      if (seen[b]) continue;
      seen[b] = true;
      if (blocks[b].label == label) return unmapped + "a demote inside a loop";
      for (size_t n = blocks[b].first; n <= blocks[b].last; n++) {
        std::string use;
        if (const char* name = spv_name(kOpsDerivative, insts[n].op)) use = name;
        if (insts[n].op == kOpFunctionCall) {
          auto c = function_of.find(w(insts[n], 3));
          if (c != function_of.end() && functions[c->second].derivative)
            use = std::string("a call to a function using ") + spv_name(kOpsDerivative, functions[c->second].derivative);
        }
        if (!use.empty())
          return unmapped + "a demote (D3D9 texkill) followed by " + use +
                 ": OpKill would leave its quad's derivatives undefined, and SDL's 1.1 device has no demote";
      }
      for (uint32_t s : blocks[b].successors) todo.push_back(block_of[s]);
    }
  }

  // The rewrite, into a copy: words changes only when every check above passed.
  std::vector<uint32_t> out(words.begin(), words.begin() + 5);
  out[1] = kSpv13;
  out.reserve(words.size());
  for (size_t n = 0; n < insts.size(); n++) {
    const SpvInst& i = insts[n];
    if (i.op == kOpCapability && (w(i, 1) == kCapVulkanMemoryModel || w(i, 1) == kCapPhysicalStorageBufferAddresses ||
                                  w(i, 1) == kCapDemoteToHelperInvocation))
      continue;
    if (i.op == kOpMemoryModel) {
      out.insert(out.end(), {(3u << 16) | kOpMemoryModel, kAddrLogical, kMemGlsl450});
      continue;
    }
    if (i.op == kOpEntryPoint) {
      const size_t head = out.size();
      out.insert(out.end(), words.begin() + i.at, words.begin() + i.at + interface_from);
      for (uint32_t k = interface_from; k < i.wc; k++) {
        const uint32_t sc = variable_class[w(i, k)];
        if (sc == 1 || sc == 3) out.push_back(w(i, k));   // Input, Output
      }
      out[head] = (uint32_t(out.size() - head) << 16) | kOpEntryPoint;
      continue;
    }
    if (i.op == kOpDemoteToHelperInvocation) {
      out.push_back((1u << 16) | kOpKill);
      n++;   // its OpBranch
      continue;
    }
    out.insert(out.end(), words.begin() + i.at, words.begin() + i.at + i.wc);
  }
  words = std::move(out);
  return "";
}

}  // namespace d9g::shader
