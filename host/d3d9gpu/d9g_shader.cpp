// Host 2's shader translator, the shared-compiler half (R1 U2): DXVK 3.1.1's analysis, dxbc-spirv's
// converter, DXVK's legacy-input lowering pass and DXVK's legalisation, in DXVK's order, with
// DXVK's options. Compiled with the flags DXVK compiled src/d3d9/d3d9_shader.cpp with
// (shader/build_translator.sh), against the pinned checkout's headers. Line citations are to
// private/s5-tools/src/dxvk-3.1.1.
#include "d9g_shader.h"
#include "d9g_shader_internal.h"

#include "sha256.h"

#include "d3d9_shader.h"
#include "d3d9_shader_analysis.h"

#include <ir/ir.h>
#include <ir/ir_builder.h>
#include <ir/ir_disasm.h>
#include <ir/ir_legalize.h>
#include <ir/ir_serialize.h>
#include <ir/passes/ir_pass_lower_io.h>
#include <sm3/sm3_converter.h>
#include <util/util_byte_stream.h>
#include <util/util_log.h>
#include <util/util_md5.h>

#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#elif defined(__aarch64__)
// android_app.py arm_portable: AArch64's FPCR in MXCSR's place (0: round to nearest, no flush to
// zero, no trapped exception: the environment 0x1F80 and 0x037F name on x86).
static inline uint32_t sa2_fpcr_get() { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return uint32_t(v); }
static inline void sa2_fpcr_set(uint32_t v) { __asm__ volatile("msr fpcr, %0" : : "r"(uint64_t(v))); }
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <vector>

namespace dxvk {
// shader/extract_dxvk_shader.py's generated unit: DXVK's D3D9ShaderLowerLegacyInputPass, verbatim.
void d9gDxvkLowerLegacyInput(dxbc_spv::ir::Builder& builder, const D3D9ShaderOptions& options,
                             const D3D9ShaderAnalysis& analysis);
}

namespace d9g::shader {

using namespace dxbc_spv;

#ifndef D9G_TRANSLATOR_ID
#define D9G_TRANSLATOR_ID "unbuilt"
#endif

std::string translator_identity() { return D9G_TRANSLATOR_ID; }

// d9g_shader.h: defined here only (the translator's flags), never inline in the device's units.
Reflection::Reflection() = default;
Reflection::~Reflection() = default;
Reflection::Reflection(const Reflection&) = default;
Reflection::Reflection(Reflection&&) noexcept = default;
Reflection& Reflection::operator=(const Reflection&) = default;
Reflection& Reflection::operator=(Reflection&&) noexcept = default;
Translated::Translated() = default;
Translated::~Translated() = default;
Translated::Translated(const Translated&) = default;
Translated::Translated(Translated&&) noexcept = default;
Translated& Translated::operator=(const Translated&) = default;
Translated& Translated::operator=(Translated&&) noexcept = default;
Built::Built() = default;
Built::~Built() = default;
Built::Built(const Built&) = default;
Built::Built(Built&&) noexcept = default;
Built& Built::operator=(const Built&) = default;
Built& Built::operator=(Built&&) noexcept = default;

std::string Variant::key() const {
  std::ostringstream s;
  // hexadecimal with its prefix (0 alone for zero), so a reader parses each field as base 0
  s << std::hex << std::showbase << "bgra=" << bgra_mask << ";border_u=" << border_u << ";border_v=" << border_v
    << ";border_linear=" << border_linear << ";depth_clamp=" << int(depth_clamp) << ";sampler_modes=";
  if (sampler_modes)
    s << *sampler_modes;
  else
    s << "none";
  return s.str();
}

namespace {
// D3D9FORMAT values (DXVK's src/d3d9/d3d9_format.h D3D9Format) the sampler-mode rule reads.
constexpr uint32_t fourcc(char a, char b, char c, char d) {
  return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
}
constexpr uint32_t kFmtA8 = 28, kFmtL8 = 50, kFmtD16Lockable = 70, kFmtD32 = 71, kFmtD15S1 = 73, kFmtD24S8 = 75,
                   kFmtD24X8 = 77, kFmtD24X4S4 = 79, kFmtD16 = 80, kFmtL16 = 81, kFmtD32FLockable = 82, kFmtD24FS8 = 83,
                   kFmtD32Lockable = 84, kFmtR16F = 111, kFmtR32F = 114;
constexpr uint32_t kFmtDF24 = fourcc('D', 'F', '2', '4'), kFmtDF16 = fourcc('D', 'F', '1', '6'),
                   kFmtINTZ = fourcc('I', 'N', 'T', 'Z');
constexpr uint32_t kFetch4Enabled = fourcc('G', 'E', 'T', '4'), kFetch4Disabled = fourcc('G', 'E', 'T', '1');
constexpr uint32_t kD3dTexfPoint = 1;
}  // namespace

bool fetch4_after(bool enabled, uint32_t mip_lod_bias) {
  if (mip_lod_bias == kFetch4Enabled)
    return true;
  if (mip_lod_bias == kFetch4Disabled)
    return false;
  return enabled;
}

uint32_t dxvk_sampler_mode(const SamplerSlot& s) {
  if (!s.bound)
    return 0u;
  // IsDepthFormat (d3d9_format.h:288-302)
  const uint32_t f = s.d3dfmt;
  const bool depth = f == kFmtD16Lockable || f == kFmtD32 || f == kFmtD15S1 || f == kFmtD24S8 || f == kFmtD24X8 ||
                     f == kFmtD24X4S4 || f == kFmtD16 || f == kFmtD32FLockable || f == kFmtD24FS8 || f == kFmtD32Lockable ||
                     f == kFmtDF16 || f == kFmtDF24 || f == kFmtINTZ;
  const bool shadow = depth && f != kFmtINTZ && f != kFmtDF16 && f != kFmtDF24;
  if (shadow)
    return s.upgraded_to_d32f ? 3u : 2u;
  const bool singleChannel = f == kFmtINTZ || f == kFmtDF16 || f == kFmtDF24 || f == kFmtR16F || f == kFmtR32F ||
                             f == kFmtA8 || f == kFmtL8 || f == kFmtL16;
  return s.fetch4_enabled && singleChannel && s.mag_filter == kD3dTexfPoint ? 1u : 0u;
}

uint32_t sampler_modes_word(const SamplerSlot (&slots)[16], uint32_t sampler_mode_slots) {
  uint32_t word = 0;
  for (uint32_t r = 0; r < 16u; r++)
    if ((sampler_mode_slots >> r) & 1u)
      word |= dxvk_sampler_mode(slots[r]) << (2u * r);
  return word;
}

FpEnvGuard::FpEnvGuard() {
#if defined(__x86_64__) || defined(__i386__)
  m_mxcsr = _mm_getcsr();
  __asm__ volatile("fnstcw %0" : "=m"(m_x87));
#elif defined(__aarch64__)
  m_mxcsr = sa2_fpcr_get();
#endif
  std::string ignored;
  if (active_fault(ignored) == Fault::NoFpGuard)
    return;  // the guard's own control (a test run): the caller's environment stays
#if defined(__x86_64__) || defined(__i386__)
  _mm_setcsr(0x1F80u);  // all exceptions masked, round to nearest, no FTZ, no DAZ
  uint16_t cw = 0x037Fu;  // the x87 default: extended precision, round to nearest, masked
  __asm__ volatile("fldcw %0" : : "m"(cw));
#elif defined(__aarch64__)
  sa2_fpcr_set(0);
#endif
}

FpEnvGuard::~FpEnvGuard() {
#if defined(__x86_64__) || defined(__i386__)
  _mm_setcsr(m_mxcsr);
  __asm__ volatile("fldcw %0" : : "m"(m_x87));
#elif defined(__aarch64__)
  sa2_fpcr_set(m_mxcsr);
#endif
}

void CapturingLogger::message(util::LogLevel severity, const char* message) {
  if (severity >= util::LogLevel::eWarn) {
    if (!text.empty())
      text += "; ";
    text += message;
  }
  if (severity >= util::LogLevel::eError)
    errors++;
}

namespace {

dxvk::D3D9ShaderOptions d3d9_options(const ShaderProfile& profile) {
  // The device's options (D3D9DeviceEx::InitShaderOptions, d3d9_device.cpp:7886-7888): a hardware
  // vertex processing device, floatEmulation from host/player/dxvk.conf, no forced sampler types.
  dxvk::D3D9ShaderOptions o = {};
  o.isSWVP = false;
  o.d3d9FloatEmulation = profile.fast_float_emulation ? dxvk::D3D9FloatEmulation::Enabled
                                                      : dxvk::D3D9FloatEmulation::Strict;
  o.forceSamplerTypeSpecConstants = false;
  return o;
}

// DxvkIrShader::convertShader's CompileOptions (dxvk_shader_ir.cpp:2222-2264) for the profile.
ir::CompileOptions compile_options(const ShaderProfile& p) {
  ir::CompileOptions options;
  options.arithmeticOptions.lowerDot = true;
  options.arithmeticOptions.lowerSinCos = p.lower_sin_cos;
  options.arithmeticOptions.lowerMsad = true;
  options.arithmeticOptions.lowerUdiv = true;
  options.arithmeticOptions.lowerF32toF16 = p.lower_f32_to_f16;
  options.arithmeticOptions.lowerConvertFtoI = p.lower_ftoi;
  options.arithmeticOptions.lowerGsVertexCountIn = false;
  options.arithmeticOptions.hasNvUnsignedItoFBug = p.lower_itof;

  options.min16Options.enableFloat16 = p.supports_16bit_arithmetic;
  options.min16Options.enableInt16 = p.supports_16bit_arithmetic;

  options.resourceOptions.allowSubDwordScratchAndLds = true;
  options.resourceOptions.flattenLds = false;
  options.resourceOptions.flattenScratch = false;
  options.resourceOptions.structuredCbv = true;
  options.resourceOptions.structuredSrvUav = true;

  auto ssboAlignment = p.min_storage_buffer_alignment;
  options.bufferOptions.useTypedForRaw = ssboAlignment > 16u;
  options.bufferOptions.useTypedForStructured = ssboAlignment > 4u;
  options.bufferOptions.useTypedForSparseFeedback = true;
  options.bufferOptions.useRawForTypedAtomic = ssboAlignment <= 4u;
  options.bufferOptions.forceFormatForTypedUavRead = p.typed_r32_load_requires_format;
  options.bufferOptions.minStructureAlignment = ssboAlignment;

  options.scalarizeOptions.subDwordVectors = true;

  options.syncOptions.insertRovLocks = true;
  options.syncOptions.insertLdsBarriers = false;   // InsertSharedMemoryBarriers: D3D11 only
  options.syncOptions.insertUavBarriers = false;   // InsertResourceBarriers: D3D11 only

  options.derivativeOptions.hoistNontrivialDerivativeOps = true;
  options.derivativeOptions.hoistNontrivialImplicitLodOps = false;
  options.derivativeOptions.hoistDescriptorLoads = true;

  options.cseOptions.relocateDescriptorLoad = true;

  if (p.supports_resource_indexing)
    options.descriptorIndexing.optimizeDescriptorIndexing = true;
  return options;
}

std::string md5_hex(const void* a, size_t na, const void* b, size_t nb) {
  auto hasher = util::md5::Hasher();
  hasher.update(a, na);
  hasher.update(b, nb);
  auto digest = hasher.finalize();
  static const char* hex = "0123456789abcdef";
  std::string s;
  for (uint8_t byte : digest.data) {
    s += hex[byte >> 4];
    s += hex[byte & 15];
  }
  return s;
}

const char* builtin_name(ir::BuiltIn b) {
  std::ostringstream s;
  s << b;
  static thread_local std::string last;
  last = s.str();
  return last.c_str();
}

std::vector<IoVar> io_vars(const ir::IoMap& map) {
  std::vector<IoVar> out;
  for (const auto& e : map) {
    IoVar v;
    v.component_mask = e.getComponentMask();
    if (e.getType() == ir::IoEntryType::eBuiltIn) {
      v.builtin = true;
      v.builtin_name = builtin_name(e.getBuiltIn());
      v.location = uint32_t(e.getBuiltIn());
    } else {
      v.location = e.getLocationIndex();
      auto sem = map.getSemanticForEntry(e);
      v.semantic = sem.name;
      v.semantic_index = sem.index;
      if (e.getType() == ir::IoEntryType::ePerPatch)
        v.builtin_name = "per_patch";
    }
    out.push_back(std::move(v));
  }
  return out;
}

void ranges(const dxvk::D3D9ConstantBufferLayout& layout, std::vector<ConstantRange>& out) {
  for (uint32_t i = 0; i < layout.getRangeCount(); i++) {
    auto r = layout.getRange(i);
    out.push_back({r.srcIndex, r.dstIndex, r.count, r.isDynamicallyIndexed, r.isShaderDefined});
  }
}

// The BGRA variant: D3DCOLOR inputs read .zyxw (Variant::bgra_mask). After legalisation every
// input load is a scalar load whose address is a constant component index, so the swizzle is a
// permutation of those indices; any other form is refused rather than approximated.
bool apply_bgra(ir::Builder& b, uint32_t mask, std::string& error) {
  if (!mask)
    return true;
  std::vector<ir::SsaDef> inputs;
  for (auto it = b.getDeclarations().first; it != b.getDeclarations().second; ++it) {
    if (it->getOpCode() == ir::OpCode::eDclInput) {
      uint32_t location = uint32_t(it->getOperand(1u));
      if (location < 32u && (mask >> location) & 1u)
        inputs.push_back(it->getDef());
    }
  }
  for (auto input : inputs) {
    util::small_vector<ir::SsaDef, 64u> uses;
    b.getUses(input, uses);
    for (auto use : uses) {
      ir::Op op = b.getOp(use);
      if (op.getOpCode() != ir::OpCode::eInputLoad)
        continue;
      const auto& address = b.getOpForOperand(op, 1u);
      if (!op.getType().isScalarType() || !address || !address.isConstant() || address.getOperandCount() != 1u) {
        error = "a D3DCOLOR input load is not a scalar load at a constant component";
        return false;
      }
      uint32_t c = uint32_t(address.getOperand(0u));
      if (c < 3u) {
        op.setOperand(1u, b.makeConstant(2u - c));
        b.rewriteOp(use, std::move(op));
      }
    }
  }
  return true;
}

// Ruling 3's border fallback, the shader side (R1 U11): every 2D colour sample (ImageSample with a
// two-component coordinate, no depth reference, a four-component result) of a register in the
// variant's border set is blended with the register's border colour, read from the colour buffer
// (kBorderSpace, kPsBorderColours) at the register's index. The device makes that register's sampler
// CLAMP_TO_EDGE on its BORDER axes (d9g_sampler.h AddrDecision::fallback_border), so a coordinate
// outside the texture reads the edge texel, which the blend replaces:
//   point filtering (border_linear clear): D3D9 reads texel floor(c * n), the border outside
//     [0, n): inside = 0 <= c < 1 on every BORDER axis; the result is the sample where inside, the
//     colour elsewhere. Exact.
//   linear filtering (border_linear set): the bilinear footprint x = c * n - 0.5 covers texels
//     floor(x) and floor(x) + 1; the share of it inside [0, n) is clamp(min(c * n + 0.5,
//     n + 0.5 - c * n), 0, 1) on each BORDER axis, and the result is sample * k + colour * (1 - k),
//     k the product of the axes' shares. With a clamped sample this is the border blend D3D9 makes,
//     but n is the level-0 extent (ImageQuerySize at mip 0) and the share is computed in float where
//     the hardware quantises its weights: a named residue, measured (test_shaders.sh C8).
// Cube and volume samples, depth comparison (a depth reference), fetch4 (ImageGather) and image
// loads of a border register are left as they are, and each is recorded by kind in the reflection
// (Reflection::border_unwrapped_*): which of them a draw takes is the device's state at the draw (the
// spec data's sampler type and mode, the bound texture), so the device must leave unmapped, by name,
// a draw on a fallback device that would take one (R2-draws). In a module specialized at a sampler-mode
// word (Variant::sampler_modes) a register at mode 1, 2 or 3 keeps only its gather or its depth-compare
// sample, recorded the same way, and no colour sample to wrap: it is accounted for by that record. A
// border register the shader never samples in 2D, and does not read at such a mode, is refused: the
// variant key would name a register the module ignores.
bool apply_border(ir::Builder& b, const Variant& v, Reflection& refl, std::string& error) {
  const uint32_t regs = uint32_t(v.border_u) | uint32_t(v.border_v);
  if (!regs && !v.border_linear)
    return true;
  if (v.border_linear & ~regs) {
    error = "border_linear names a register with no BORDER axis";
    return false;
  }
  ir::SsaDef entry;
  struct Srv { uint32_t reg; bool twoD; };
  std::map<ir::SsaDef, Srv> srvs;   // every image declaration of space 0 -> its register, and whether it is 2D
  for (auto it = b.getDeclarations().first; it != b.getDeclarations().second; ++it) {
    if (it->getOpCode() == ir::OpCode::eEntryPoint)
      entry = it->getDef();
    if (it->getOpCode() == ir::OpCode::eDclSrv && uint32_t(it->getOperand(1u)) == 0u)
      srvs[it->getDef()] = {uint32_t(it->getOperand(2u)), ir::ResourceKind(it->getOperand(4u)) == ir::ResourceKind::eImage2D};
  }
  struct Site { ir::SsaDef sample, srv; uint32_t reg; };
  std::vector<Site> sites;
  uint32_t seen = 0;
  for (auto it = b.getCode().first; it != b.getCode().second; ++it) {
    const ir::OpCode code = it->getOpCode();
    if (code != ir::OpCode::eImageSample && code != ir::OpCode::eImageGather && code != ir::OpCode::eImageLoad)
      continue;
    const auto& desc = b.getOpForOperand(*it, 0u);
    if (desc.getOpCode() != ir::OpCode::eDescriptorLoad)
      continue;
    auto found = srvs.find(ir::SsaDef(desc.getOperand(0u)));
    if (found == srvs.end() || !((regs >> found->second.reg) & 1u))
      continue;
    const uint32_t bit = 1u << found->second.reg;
    if (code == ir::OpCode::eImageGather) {
      refl.border_unwrapped_gather |= bit;
      continue;
    }
    if (code == ir::OpCode::eImageLoad) {
      refl.border_unwrapped_other |= bit;
      continue;
    }
    const auto& coord = b.getOpForOperand(*it, 3u);
    const bool colour = it->getType().isBasicType() && it->getType().getBaseType(0u).getVectorSize() == 4u;
    const bool twoD = found->second.twoD && coord && coord.getType().isBasicType() &&
                      coord.getType().getBaseType(0u).getVectorSize() == 2u;
    const bool compare = bool(ir::SsaDef(it->getOperand(10u)));
    if (compare) {
      refl.border_unwrapped_depth_compare |= bit;
      continue;
    }
    if (!colour || !twoD) {
      refl.border_unwrapped_other |= bit;
      continue;
    }
    sites.push_back({it->getDef(), desc.getDef(), found->second.reg});
    seen |= bit;
  }
  // the registers a specialized word puts at mode 1, 2 or 3: their gather or depth-compare sample, recorded
  // above, is the read the device must leave unmapped
  uint32_t moded = 0;
  for (uint32_t r = 0; v.sampler_modes && r < 16u; r++)
    if ((*v.sampler_modes >> (2u * r)) & 3u)
      moded |= 1u << r;
  const uint32_t accounted = seen | (moded & (refl.border_unwrapped_depth_compare | refl.border_unwrapped_gather));
  if (regs & ~accounted) {
    for (uint32_t r = 0; r < 16u; r++)
      if ((regs & ~accounted) >> r & 1u) {
        error = "a border variant for sampler register " + std::to_string(r) + ", which the shader never samples in 2D" +
                ((moded >> r) & 1u ? " and does not read at its sampler mode" : "");
        return false;
      }
  }
  const ir::Type colours = ir::Type(ir::ScalarType::eF32, 4u).addArrayDimension(kBorderColourRegisters);
  ir::SsaDef cbv = b.add(ir::Op::DclCbv(colours, entry, kBorderSpace, 0u, 1u).setFlags(ir::OpFlag::eInBounds));
  b.add(ir::Op::DebugName(cbv, "borderColours"));
  const ir::Type f32 = ir::ScalarType::eF32, u32 = ir::ScalarType::eU32, bl = ir::ScalarType::eBool;
  const ir::Type f32x4 = ir::Type(ir::ScalarType::eF32, 4u), u32x2 = ir::Type(ir::ScalarType::eU32, 2u);
  const ir::Type size = ir::Type().addStructMember(ir::ScalarType::eU32, 2u).addStructMember(ir::ScalarType::eU32);
  const ir::SsaDef k0 = b.makeConstant(0.0f), k1 = b.makeConstant(1.0f), kHalf = b.makeConstant(0.5f);
  for (const Site& s : sites) {
    util::small_vector<ir::SsaDef, 16u> uses;
    b.getUses(s.sample, uses);
    ir::SsaDef at = s.sample;
    auto emit = [&](ir::Op op) { at = b.addAfter(at, std::move(op)); return at; };
    const ir::SsaDef coord = ir::SsaDef(b.getOp(s.sample).getOperand(3u));
    const bool linear = (v.border_linear >> s.reg) & 1u;
    ir::SsaDef extent[2];
    if (linear) {
      ir::SsaDef q = emit(ir::Op::ImageQuerySize(size, s.srv, b.makeConstant(0u)));
      ir::SsaDef wh = emit(ir::Op::CompositeExtract(u32x2, q, b.makeConstant(0u)));
      for (uint32_t a = 0; a < 2u; a++)
        extent[a] = emit(ir::Op::ConvertItoF(f32, emit(ir::Op::CompositeExtract(u32, wh, b.makeConstant(a)))));
    }
    ir::SsaDef weight, inside;   // linear: the product of the axes' shares; point: every axis inside
    for (uint32_t a = 0; a < 2u; a++) {
      const uint16_t axis = a ? v.border_v : v.border_u;
      if (!((axis >> s.reg) & 1u))
        continue;
      ir::SsaDef c = emit(ir::Op::CompositeExtract(f32, coord, b.makeConstant(a)));
      if (linear) {
        ir::SsaDef cn = emit(ir::Op::FMul(f32, c, extent[a]));
        ir::SsaDef lo = emit(ir::Op::FAdd(f32, cn, kHalf));
        ir::SsaDef hi = emit(ir::Op::FSub(f32, emit(ir::Op::FAdd(f32, extent[a], kHalf)), cn));
        ir::SsaDef share = emit(ir::Op::FClamp(f32, emit(ir::Op::FMin(f32, lo, hi)), k0, k1));
        weight = weight ? emit(ir::Op::FMul(f32, weight, share)) : share;
      } else {
        ir::SsaDef in = emit(ir::Op::BAnd(bl, emit(ir::Op::FGe(bl, c, k0)), emit(ir::Op::FLt(bl, c, k1))));
        inside = inside ? emit(ir::Op::BAnd(bl, inside, in)) : in;
      }
    }
    ir::SsaDef colourDesc = emit(ir::Op::DescriptorLoad(ir::ScalarType::eCbv, cbv, b.makeConstant(0u)));
    ir::SsaDef colourVec = emit(ir::Op::BufferLoad(f32x4, colourDesc, b.makeConstant(s.reg), 16u).setFlags(ir::OpFlag::eInBounds));
    ir::SsaDef rest = linear ? emit(ir::Op::FSub(f32, k1, weight)) : ir::SsaDef();
    ir::SsaDef parts[4];
    for (uint32_t i = 0; i < 4u; i++) {
      ir::SsaDef si = emit(ir::Op::CompositeExtract(f32, s.sample, b.makeConstant(i)));
      ir::SsaDef ci = emit(ir::Op::CompositeExtract(f32, colourVec, b.makeConstant(i)));
      parts[i] = linear ? emit(ir::Op::FAdd(f32, emit(ir::Op::FMul(f32, si, weight)), emit(ir::Op::FMul(f32, ci, rest))))
                        : emit(ir::Op::Select(f32, inside, si, ci));
    }
    ir::SsaDef result = emit(ir::Op::CompositeConstruct(f32x4, parts[0], parts[1], parts[2], parts[3]));
    for (ir::SsaDef use : uses) {
      ir::Op op = b.getOp(use);
      for (uint32_t k = 0; k < op.getFirstLiteralOperandIndex(); k++)
        if (ir::SsaDef(op.getOperand(k)) == s.sample)
          op.setOperand(k, result);
      b.rewriteOp(use, std::move(op));
    }
  }
  return true;
}

// The pixel shader's sampler-mode reads: the lowering pass's samplerMode function (DXVK's
// emitSpecConstantLoad of psSamplerModes, d3d9_shader.cpp:713-738 and :926-927, built by
// buildSpecConstantFunction :671-701: one u32 parameter, the register index, and a bit extract of spec
// constant 5) and its calls, as the lowering pass leaves them, before legalisation: the body is
// ParamLoad, IMul, IAdd, UBitExtract, ConsumeAs and Return, and each call's index is the first
// component of the sampler-state load's constant address (:540-545). Anything else is refused, never
// guessed at: the formula is evaluated from the body itself, per call.
struct ModeReads {
  ir::SsaDef function, formula;                        // the function, and the value it returns
  std::vector<ir::SsaDef> body;                        // the function's ops after its Function op
  std::vector<std::pair<ir::SsaDef, uint32_t>> calls;  // each call and its register index
  uint32_t slots = 0;                                  // bit r: a call reads register r's mode
};

constexpr uint32_t kSpecPsSamplerModes = 5u;   // offsetof(D3D9SpecData, psSamplerModes) / 4

bool is_u32(const ir::Type& t) {
  return t.isScalarType() && t.getBaseType(0u).getBaseType() == ir::ScalarType::eU32;
}

// A u32 the IR spells as a constant: a scalar constant, one component of a constant vector at a constant
// index, or either through ConsumeAs to u32.
bool u32_value(const ir::Builder& b, const ir::Op& op, uint32_t& v, int depth = 0) {
  if (depth > 4)
    return false;
  if (op.isConstant())
    return is_u32(op.getType()) && op.getOperandCount() == 1u && (v = uint32_t(op.getOperand(0u)), true);
  if (op.getOpCode() == ir::OpCode::eConsumeAs && is_u32(op.getType()))
    return u32_value(b, b.getOpForOperand(op, 0u), v, depth + 1);
  if (op.getOpCode() == ir::OpCode::eCompositeExtract && is_u32(op.getType()) && op.getOperandCount() == 2u) {
    const auto& vec = b.getOpForOperand(op, 0u);
    uint32_t k = 0;
    if (!vec.isConstant() || !u32_value(b, b.getOpForOperand(op, 1u), k, depth + 1) || k >= vec.getOperandCount() ||
        !vec.getType().isBasicType() || vec.getType().getBaseType(0u).getBaseType() != ir::ScalarType::eU32)
      return false;
    v = uint32_t(vec.getOperand(k));
    return true;
  }
  return false;
}

// The value of op `d` of the samplerMode body at (index, word); false where the body holds anything
// but the shape above.
bool eval_mode_op(const ir::Builder& b, ir::SsaDef d, const ModeReads& reads, ir::SsaDef word_decl, uint32_t index,
                  uint32_t word, int depth, uint32_t& v) {
  if (depth > 8)
    return false;
  const ir::Op& op = b.getOp(d);
  if (op.isConstant())
    return u32_value(b, op, v);
  if (d == word_decl) {
    v = word;
    return true;
  }
  if (!is_u32(op.getType()) || std::find(reads.body.begin(), reads.body.end(), d) == reads.body.end())
    return false;
  auto arg = [&](uint32_t k, uint32_t& out) {
    return eval_mode_op(b, ir::SsaDef(op.getOperand(k)), reads, word_decl, index, word, depth + 1, out);
  };
  uint32_t a = 0, c = 0, n = 0;
  switch (op.getOpCode()) {
    case ir::OpCode::eParamLoad:
      if (ir::SsaDef(op.getOperand(0u)) != reads.function)
        return false;
      v = index;
      return true;
    case ir::OpCode::eConsumeAs:
      return arg(0u, v);
    case ir::OpCode::eIMul:
      if (!arg(0u, a) || !arg(1u, c)) return false;
      v = a * c;
      return true;
    case ir::OpCode::eIAdd:
      if (!arg(0u, a) || !arg(1u, c)) return false;
      v = a + c;
      return true;
    case ir::OpCode::eUBitExtract:
      if (!arg(0u, a) || !arg(1u, c) || !arg(2u, n) || c >= 32u || n == 0u || n > 32u - c) return false;
      v = (a >> c) & (n == 32u ? ~0u : (1u << n) - 1u);
      return true;
    default:
      return false;
  }
}

bool find_mode_reads(ir::Builder& b, ModeReads& reads, ir::SsaDef& word_decl, std::string& error) {
  word_decl = ir::SsaDef();
  for (auto it = b.getDeclarations().first; it != b.getDeclarations().second; ++it)
    if (it->getOpCode() == ir::OpCode::eDclSpecConstant &&
        uint32_t(it->getOperand(it->getFirstLiteralOperandIndex())) == kSpecPsSamplerModes)
      word_decl = it->getDef();
  if (!word_decl)
    return true;   // the shader reads no sampler mode
  // every function's ops in order, and which function reads the word
  std::map<ir::SsaDef, std::vector<ir::SsaDef>> bodies;
  ir::SsaDef current;
  for (auto it = b.getCode().first; it != b.getCode().second; ++it) {
    if (it->getOpCode() == ir::OpCode::eFunction)
      current = it->getDef();
    else if (current)
      bodies[current].push_back(it->getDef());
    if (it->getOpCode() == ir::OpCode::eFunctionEnd)
      current = ir::SsaDef();
  }
  util::small_vector<ir::SsaDef, 16u> uses;
  b.getUses(word_decl, uses);
  for (ir::SsaDef use : uses) {
    const auto code = b.getOp(use).getOpCode();
    if (code == ir::OpCode::eDebugName || code == ir::OpCode::eDebugMemberName)
      continue;
    ir::SsaDef owner;
    for (const auto& [fn, ops] : bodies)
      if (std::find(ops.begin(), ops.end(), use) != ops.end())
        owner = fn;
    if (!owner || (reads.function && reads.function != owner)) {
      error = "psSamplerModes is read outside one samplerMode function";
      return false;
    }
    reads.function = owner;
  }
  if (!reads.function)
    return true;
  const ir::Op& fn = b.getOp(reads.function);
  if (!is_u32(fn.getType()) || fn.getOperandCount() != 1u) {
    error = "the samplerMode function is not a u32 function of one parameter";
    return false;
  }
  reads.body = bodies[reads.function];
  ir::SsaDef ret;
  for (ir::SsaDef d : reads.body) {
    const auto code = b.getOp(d).getOpCode();
    if (code == ir::OpCode::eReturn) {
      if (ret || b.getOp(d).getOperandCount() != 1u) {
        error = "the samplerMode function has no single return of a value";
        return false;
      }
      ret = d;
    } else if (code != ir::OpCode::eLabel && code != ir::OpCode::eFunctionEnd && code != ir::OpCode::eParamLoad &&
               code != ir::OpCode::eIMul && code != ir::OpCode::eIAdd && code != ir::OpCode::eUBitExtract &&
               code != ir::OpCode::eConsumeAs) {
      error = "the samplerMode function holds an op other than DXVK's bit extract of its index";
      return false;
    }
  }
  if (!ret) {
    error = "the samplerMode function returns nothing";
    return false;
  }
  reads.formula = ir::SsaDef(b.getOp(ret).getOperand(0u));
  util::small_vector<ir::SsaDef, 64u> callers;
  b.getUses(reads.function, callers);
  for (ir::SsaDef use : callers) {
    const ir::Op& op = b.getOp(use);
    if (op.getOpCode() == ir::OpCode::eDebugName ||
        (op.getOpCode() == ir::OpCode::eParamLoad && std::find(reads.body.begin(), reads.body.end(), use) != reads.body.end()))
      continue;   // its name, and its own parameter's load
    uint32_t index = 0;
    if (op.getOpCode() != ir::OpCode::eFunctionCall || op.getOperandCount() != 2u ||
        ir::SsaDef(op.getOperand(0u)) != reads.function || !u32_value(b, b.getOpForOperand(op, 1u), index) || index >= 16u) {
      error = "a samplerMode call whose register is not a constant 0-15";
      return false;
    }
    reads.calls.push_back({use, index});
    reads.slots |= 1u << index;
  }
  // the body is DXVK's two-bit field per register: evaluated on a word with one set field, it gives
  // that field, and on a word with every other bit set, nothing of its neighbours'
  for (uint32_t r = 0; r < 16u; r++) {
    for (uint32_t m = 0; m < 4u; m++) {
      uint32_t v = 0;
      if (!eval_mode_op(b, reads.formula, reads, word_decl, r, m << (2u * r), 0, v) || v != m ||
          !eval_mode_op(b, reads.formula, reads, word_decl, r, ~(3u << (2u * r)), 0, v) || v != 0u) {
        error = "the samplerMode function does not read register r's two bits at 2r";
        return false;
      }
    }
  }
  return true;
}

// The registers the module samples with a depth reference (an ImageSample with a reference operand)
// or gathers, by the sampler register of its image declaration.
void sampling_registers(const ir::Builder& b, uint32_t& dref, uint32_t& gather) {
  dref = gather = 0;
  for (auto it = b.getCode().first; it != b.getCode().second; ++it) {
    const ir::OpCode code = it->getOpCode();
    if (code != ir::OpCode::eImageSample && code != ir::OpCode::eImageGather)
      continue;
    const auto& desc = b.getOpForOperand(*it, 0u);
    if (desc.getOpCode() != ir::OpCode::eDescriptorLoad)
      continue;
    const auto& srv = b.getOpForOperand(desc, 0u);
    if (srv.getOpCode() != ir::OpCode::eDclSrv)
      continue;
    const uint32_t bit = 1u << (uint32_t(srv.getOperand(2u)) & 31u);
    if (code == ir::OpCode::eImageGather)
      gather |= bit;
    else if (ir::SsaDef(it->getOperand(10u)))
      dref |= bit;
  }
}

bool is_mode_fault(Fault f) {
  return f == Fault::ModesSlotShifted || f == Fault::ModesSkipped || f == Fault::ModesInverted;
}

// The sampler-mode specialization (Variant::sampler_modes), before legalisation: every
// samplerMode(r) call becomes the constant (word >> 2r) & 3, so that legalizeIr, which runs next on the
// module as it runs on the base, folds the compares (ArithmeticPass), resolves each constant branch and
// drops what it leaves unreachable (CleanupControlFlowPass), and removes the function and the word
// (RemoveUnusedPass). dxbc-spirv has no inliner, hence the call replacement. Nothing else changes:
// test/spv_specialize.py holds each module to its base as a pure deletion. Sets the reflection's
// sampler-mode registers, and the base's depth-compare and gather registers into base_dref and
// base_gather for check_sampler_modes.
bool specialize_sampler_modes(ir::Builder& b, const Variant& v, Stage stage, Reflection& refl, Fault fault,
                              uint32_t& base_dref, uint32_t& base_gather, std::string& error) {
  ir::SsaDef word_decl;
  ModeReads reads;
  if (stage == Stage::Pixel && !find_mode_reads(b, reads, word_decl, error))
    return false;
  refl.sampler_mode_slots = reads.slots;
  sampling_registers(b, base_dref, base_gather);
  if (!v.sampler_modes)
    return true;
  if (stage != Stage::Pixel) {
    // KEEP OUT: vertex shaders read vsSamplerModes (spec id 3, bits 8-15) the same way; no vertex
    // shader of the streams samples, so nothing specializes them
    error = "the sampler-mode specialization applies to pixel shaders only";
    return false;
  }
  const uint32_t word = *v.sampler_modes;
  uint32_t named = 0;
  for (uint32_t r = 0; r < 16u; r++)
    if ((word >> (2u * r)) & 3u)
      named |= 1u << r;
  for (uint32_t r = 0; r < 16u; r++)
    if ((named & ~reads.slots) >> r & 1u) {
      error = "a sampler-mode word that names register " + std::to_string(r) + ", whose mode the shader never reads";
      return false;
    }
  if (fault == Fault::ModesSkipped)
    return true;
  for (const auto& [call, r] : reads.calls) {
    uint32_t mode = 0;
    const uint32_t reg = fault == Fault::ModesSlotShifted ? (r + 1u) & 15u : r;
    if (!eval_mode_op(b, reads.formula, reads, word_decl, reg, word, 0, mode)) {
      error = "the samplerMode function did not evaluate";
      return false;
    }
    if (fault == Fault::ModesInverted)
      mode ^= 2u;
    b.rewriteDef(call, b.makeConstant(mode));
  }
  return true;
}

// After legalisation, what the word must have left: a register at mode 0 neither a depth-compare sample
// nor a gather, at 1 its gathers and no depth compare, at 2 and 3 its depth-compare samples and no
// gather. A module that keeps a dead path, or lost a live one, is refused here; a test run's sampler-mode
// fault is let through, as the instruments' to catch (test/spv_specialize.py, the Metal check, the
// frames). Sets the reflection's depth-compare and gather registers.
bool check_sampler_modes(const ir::Builder& b, const Variant& v, Reflection& refl, Fault fault, uint32_t base_dref,
                         uint32_t base_gather, std::string& error) {
  sampling_registers(b, refl.depth_compare_registers, refl.gather_registers);
  if (!v.sampler_modes || is_mode_fault(fault))
    return true;
  for (uint32_t r = 0; r < 16u; r++) {
    if (!((refl.sampler_mode_slots >> r) & 1u))
      continue;
    const uint32_t m = (*v.sampler_modes >> (2u * r)) & 3u, bit = 1u << r;
    const bool wantDref = m >= 2u && (base_dref & bit), wantGather = m == 1u && (base_gather & bit);
    if (bool(refl.depth_compare_registers & bit) != wantDref || bool(refl.gather_registers & bit) != wantGather) {
      error = "the sampler-mode specialization left register " + std::to_string(r) + " (mode " + std::to_string(m) +
              ") with " + (refl.depth_compare_registers & bit ? "a" : "no") + " depth-compare sample and " +
              (refl.gather_registers & bit ? "a" : "no") + " gather";
      return false;
    }
  }
  return true;
}

}  // namespace

Fault active_fault(std::string& error) {
  const char* f = std::getenv("SA2_D3D9GPU_SHADER_FAULT");
  if (!f || !*f)
    return Fault::None;
  // The device's one test-run switch (d9g_entry.cpp: SA2_D3D9GPU_TEST_RUN, which also gates the
  // capability override): a fault outside a test run is refused, never honoured.
  const char* test = std::getenv("SA2_D3D9GPU_TEST_RUN");
  if (!test || std::strcmp(test, "1") != 0) {
    error = "SA2_D3D9GPU_SHADER_FAULT is honoured only in a test run (SA2_D3D9GPU_TEST_RUN=1)";
    return Fault::None;
  }
  static const std::pair<const char*, Fault> kFaults[] = {
    {"umin_dropped", Fault::UminDropped}, {"point_size_f32", Fault::PointSizeF32}, {"fp16", Fault::Fp16},
    {"nocontraction_stripped", Fault::NoContractionStripped}, {"constants_swapped", Fault::ConstantsSwapped},
    {"samplers_swapped", Fault::SamplersSwapped}, {"denorm_ftz", Fault::DenormFtz},
    {"float_emulation_enabled", Fault::FloatEmulationEnabled}, {"linkage_skipped", Fault::LinkageSkipped}, {"no_fp_guard", Fault::NoFpGuard},
    {"lower_dot_off", Fault::LowerDotOff}, {"border_off", Fault::BorderOff},
    {"modes_slot_shifted", Fault::ModesSlotShifted}, {"modes_skipped", Fault::ModesSkipped},
    {"modes_inverted", Fault::ModesInverted},
  };
  for (const auto& [name, fault] : kFaults)
    if (!std::strcmp(f, name))
      return fault;
  error = std::string("unknown SA2_D3D9GPU_SHADER_FAULT ") + f;
  return Fault::None;
}

void apply_ir_fault(ir::Builder& b, Fault fault) {
  std::vector<ir::SsaDef> defs;
  for (auto it = b.getCode().first; it != b.getCode().second; ++it)
    defs.push_back(it->getDef());
  if (fault == Fault::UminDropped) {
    // DXVK's clamp of a dynamic constant index to the pushed count (d3d9_shader.cpp:378-390)
    for (auto d : defs)
      if (b.getOp(d).getOpCode() == ir::OpCode::eUMin)
        b.rewriteDef(d, ir::SsaDef(b.getOp(d).getOperand(0u)));
  } else if (fault == Fault::PointSizeF32) {
    // DXVK's 13.3 point-size decode, float(u16) / 8 (d3d9_shader.cpp:1058-1063), left undivided
    for (auto d : defs) {
      const auto& op = b.getOp(d);
      if (op.getOpCode() != ir::OpCode::eFDiv)
        continue;
      const auto& den = b.getOpForOperand(op, 1u);
      const auto& num = b.getOpForOperand(op, 0u);
      if (den.isConstant() && num.getOpCode() == ir::OpCode::eConvertItoF) {
        float v;
        uint32_t raw = uint32_t(den.getOperand(0u));
        std::memcpy(&v, &raw, 4);
        if (v == 8.0f)
          b.rewriteDef(d, num.getDef());
      }
    }
  } else if (fault == Fault::NoContractionStripped) {
    for (auto d : defs) {
      auto flags = b.getOp(d).getFlags();
      if (flags & ir::OpFlag::ePrecise)
        b.setOpFlags(d, flags - ir::OpFlag::ePrecise);
    }
  } else if (fault == Fault::ConstantsSwapped) {
    // Two members of the static constant buffer read each other's register.
    ir::SsaDef cbv;
    for (auto it = b.getDeclarations().first; it != b.getDeclarations().second; ++it)
      if (it->getOpCode() == ir::OpCode::eDclCbv && it->getType().isStructType())
        if (uint32_t(it->getOperand(2u)) == 3u || uint32_t(it->getOperand(2u)) == 6u)
          cbv = it->getDef();
    if (!cbv)
      return;
    for (auto d : defs) {
      ir::Op op = b.getOp(d);
      if (op.getOpCode() != ir::OpCode::eBufferLoad)
        continue;
      const auto& desc = b.getOpForOperand(op, 0u);
      if (desc.getOpCode() != ir::OpCode::eDescriptorLoad || ir::SsaDef(desc.getOperand(0u)) != cbv)
        continue;
      const auto& addr = b.getOpForOperand(op, 1u);
      if (!addr.isConstant())
        continue;
      uint32_t m = uint32_t(addr.getOperand(0u));
      if (m > 1u)
        continue;
      ir::Op na(addr);
      na.setOperand(0u, 1u - m);
      op.setOperand(1u, b.add(std::move(na)));
      b.rewriteOp(d, std::move(op));
    }
  }
}

// The no-ClipDistance build (ShaderProfile::no_clip_distance; host 2's own, for a device without
// shaderClipDistance). dxbc-spirv declares ClipDistance[6] in every vertex shader and its clipping
// function stores select(i < count, dot(position, plane i), 0.0) to element i (sm3_io_map.cpp
// dclClipPlanes, sm3_converter.cpp emitClipping), the count being the state's clip plane count
// (DXVK's spec word, D3DRS_CLIPPLANEENABLE's planes that are not all zero). The output, its stores and
// their debug names are removed before legalisation, whose dead-code passes then remove the
// distances, the clip-plane buffer and the count load that nothing reads any more. With no plane
// enabled every distance is 0.0, which clips nothing, so the module draws what the full one draws;
// the device refuses a draw with a plane enabled on this profile (d9g_device_draw.cpp). Any other use
// of the output refuses the shader, by name.
bool strip_clip_distance(ir::Builder& b, std::string& error) {
  std::vector<ir::SsaDef> decls;
  for (auto it = b.getDeclarations().first; it != b.getDeclarations().second; ++it)
    if (it->getOpCode() == ir::OpCode::eDclOutputBuiltIn &&
        ir::BuiltIn(it->getOperand(it->getFirstLiteralOperandIndex())) == ir::BuiltIn::eClipDistance)
      decls.push_back(it->getDef());
  for (ir::SsaDef d : decls) {
    std::vector<ir::SsaDef> uses;
    b.getUses(d, uses);
    std::sort(uses.begin(), uses.end());
    uses.erase(std::unique(uses.begin(), uses.end()), uses.end());
    for (ir::SsaDef u : uses) {
      const ir::OpCode code = b.getOp(u).getOpCode();
      if (code != ir::OpCode::eOutputStore && code != ir::OpCode::eDebugName && code != ir::OpCode::eDebugMemberName &&
          code != ir::OpCode::eSemantic) {
        error = "unmapped: the ClipDistance output has a use other than a store or a name (opcode " +
                std::to_string(uint32_t(code)) + "), so it cannot be removed for a device without shaderClipDistance";
        return false;
      }
    }
    for (ir::SsaDef u : uses)
      b.remove(u);
    b.remove(d);
  }
  return true;
}

std::string debug_name_for(const void* bytecode, size_t bytes, Stage stage, const ShaderProfile& profile) {
  // DXVK's key (d3d9_device.cpp:8018-8025, dxvk_shader_key.cpp:40-66): the stage and the md5 of the
  // bytecode followed by the device's D3D9ShaderOptions bytes.
  dxvk::D3D9ShaderOptions o = d3d9_options(profile);
  return std::string(stage == Stage::Vertex ? "vs." : "fs.") + md5_hex(bytecode, bytes, &o, sizeof(o));
}

Translated translate(const void* bytecode, size_t bytes, const ShaderProfile& profile, const Variant& variant,
                     bool disassemble) {
  Translated t;
  FpEnvGuard env;
  CapturingLogger logger;
  g_dxvk_errors.clear();   // what DXVK's units logged belongs to this translation only
  Fault fault = active_fault(t.error);
  if (!t.error.empty())
    return t;
  ShaderProfile faulted = profile;
  if (fault == Fault::Fp16)
    faulted.supports_16bit_arithmetic = true;
  if (fault == Fault::FloatEmulationEnabled)
    faulted.fast_float_emulation = true;  // d3d9.floatEmulation read as Enabled, not Strict

  if (variant.depth_clamp) {
    t.error = "the depth-clamp emulation is built only if the depth-clamp measurement finds a pixel clamp changes "
              "(ruling 3; the sdl-patch plan's H2): none is built";
    return t;
  }
  if (bytes < 4u || bytes % 4u) {
    t.error = "bytecode is not a whole number of tokens";
    return t;
  }
  uint32_t version = 0;
  std::memcpy(&version, bytecode, 4u);
  if ((version >> 16) != 0xFFFEu && (version >> 16) != 0xFFFFu) {
    t.error = "not a vertex or pixel shader version token";
    return t;
  }
  t.stage = (version >> 16) == 0xFFFEu ? Stage::Vertex : Stage::Pixel;
  if (t.stage == Stage::Pixel && variant.bgra_mask) {
    t.error = "a BGRA input mask applies to vertex shaders only";
    return t;
  }
  if (t.stage == Stage::Vertex && (variant.border_u | variant.border_v | variant.border_linear)) {
    t.error = "the border fallback applies to pixel shaders only (no vertex shader of the streams samples)";
    return t;
  }
  t.debug_name = debug_name_for(bytecode, bytes, t.stage, profile);
  dxvk::D3D9ShaderOptions options = d3d9_options(faulted);
  const bool isVs = t.stage == Stage::Vertex;

  // 1. DXVK's analysis (D3D9DeviceEx::CreateShaderModule, d3d9_device.cpp), on a hardware device.
  util::ByteReader analysisReader(bytecode, bytes);
  dxvk::D3D9ShaderAnalysis analysis(analysisReader, false);
  if (!analysis) {
    t.error = "D3D9ShaderAnalysis refused the bytecode: " + logger.text;
    return t;
  }

  // 2. Conversion with D3D9ShaderConverter::convertShader's options (d3d9_shader.cpp:1109-1121).
  sm3::Converter::Options convOptions = {};
  convOptions.name = t.debug_name.c_str();
  convOptions.includeDebugNames = true;
  convOptions.fastFloatEmulation = options.d3d9FloatEmulation == dxvk::D3D9FloatEmulation::Enabled;
  convOptions.isSWVP = options.isSWVP && isVs;
  convOptions.forceDynamicTextureType = options.forceSamplerTypeSpecConstants && !isVs;

  ir::Builder builder;
  {
    util::ByteReader reader(bytecode, analysis.GetLength());
    sm3::Converter converter(reader, convOptions);
    if (!converter.convertShader(builder)) {
      t.error = "sm3::Converter failed: " + logger.text;
      return t;
    }
  }

  // 3. DXVK's own lowering pass, verbatim (d3d9_shader.cpp:13-1091, run at :1129-1130).
  dxvk::d9gDxvkLowerLegacyInput(builder, options, analysis);

  // 3b. The sampler-mode specialization (host 2's own: DXVK leaves psSamplerModes to the driver, as a
  //     spec constant or a uniform branch, and never removes the code it makes dead): the word's
  //     constants in place of the samplerMode calls, for legalisation to fold.
  Reflection& r = t.reflection;
  uint32_t baseDref = 0, baseGather = 0;
  if (!specialize_sampler_modes(builder, variant, t.stage, r, fault, baseDref, baseGather, t.error))
    return t;

  // 3c. The no-ClipDistance build (host 2's own, a device without shaderClipDistance): the output and
  //     its stores removed, for legalisation to remove what computed them.
  if (isVs && faulted.no_clip_distance && !strip_clip_distance(builder, t.error))
    return t;

  // 4. DXVK's legalisation (DxvkIrShader::convertShader, dxvk_shader_ir.cpp:2202-2266). No
  //    transform feedback, no MSAA demotion: D3D9 sets neither.
  ir::CompileOptions compileOptions = compile_options(faulted);
  if (fault == Fault::LowerDotOff)
    compileOptions.arithmeticOptions.lowerDot = false;
  ir::legalizeIr(builder, compileOptions);
  apply_ir_fault(builder, fault);

  // 5. The BGRA variant (host 2's own; DXVK gets it from the vertex format).
  if (!apply_bgra(builder, variant.bgra_mask, t.error))
    return t;
  // 6. What the sampler-mode word left (3b): the paths it makes dead are gone, the live ones kept.
  if (!check_sampler_modes(builder, variant, r, fault, baseDref, baseGather, t.error))
    return t;
  // 7. Ruling 3's border fallback (host 2's own: DXVK has CLAMP_TO_BORDER on every device it runs on).
  if (fault != Fault::BorderOff && !apply_border(builder, variant, r, t.error))
    return t;

  // Reflection
  r.stage = t.stage;
  auto info = analysis.GetShaderInfo();
  r.major = info.getVersion().first;
  r.minor = info.getVersion().second;
  if (isVs) {
    const auto& sig = analysis.GetInputSignature();
    for (uint32_t i = 0; i < sig.size(); i++) {
      auto s = sig.get(i);
      r.input_signature.push_back({uint32_t(s.usage), uint32_t(s.index)});
    }
  }
  r.sampler_mask = analysis.GetSamplerMask();
  r.rt_mask = isVs ? 0u : analysis.GetRenderTargetMask();
  const auto* layout = analysis.GetConstantLayout();
  const auto& lf = layout->getLayout(dxvk::D3D9ConstantType::Float);
  const auto& li = layout->getLayout(dxvk::D3D9ConstantType::Int);
  const auto& lb = layout->getLayout(dxvk::D3D9ConstantType::Bool);
  r.floats_dynamic = lf.isDynamicallyIndexed();
  ranges(lf, r.float_ranges);
  ranges(li, r.int_ranges);
  ranges(lb, r.bool_ranges);
  auto& facts = r.constant_facts;
  facts.float_min_count = lf.computeConstantCount(0u);
  facts.float_max_count = lf.computeConstantCount(UINT32_MAX);
  facts.used_hardware_bool_mask = uint16_t(analysis.GetConstantsInfo().boolMask);
  uint32_t definedCount = 0;
  for (const auto& range : r.float_ranges)
    if (range.defined)
      definedCount = std::max(definedCount, range.src + range.count);
  facts.defined_float_bits.resize(definedCount);
  const auto* definedValues = lf.getShaderDefinedConstants();
  for (uint32_t i = 0; i < definedCount; i++)
    for (uint32_t component = 0; component < 4u; component++)
      std::memcpy(&facts.defined_float_bits[i][component], &definedValues[i][component], sizeof(uint32_t));
  uint32_t staticTotal = (lf.isDynamicallyIndexed() ? 0u : lf.computeConstantCount(0u)) +
                         li.computeConstantCount(0u) + lb.computeConstantCount(0u);
  r.static_buffer_is_dword_array = staticTotal && (staticTotal > ir::Type::MaxStructMembers || lb.computeConstantCount(0u));
  r.inputs = io_vars(ir::IoMap::forInputs(builder));
  r.outputs = io_vars(ir::IoMap::forOutputs(builder, 0u));
  if (!isVs) {
    for (auto it = builder.getDeclarations().first; it != builder.getDeclarations().second; ++it) {
      if (it->getOpCode() == ir::OpCode::eDclInput)
        r.flat_shading_inputs |= analysis.GetFlatShadingMask() & (1u << uint32_t(it->getOperand(it->getFirstLiteralOperandIndex())));
    }
  }
  for (auto it = builder.getDeclarations().first; it != builder.getDeclarations().second; ++it) {
    if (it->getOpCode() == ir::OpCode::eDclSpecConstant) {
      const uint32_t id = uint32_t(it->getOperand(it->getFirstLiteralOperandIndex()));
      if (id < kSpecDwords)
        r.spec_ids |= 1u << id;
    }
  }

  ir::Serializer serializer(builder);
  t.ir.resize(serializer.computeSerializedSize());
  if (!serializer.serialize(t.ir.data(), t.ir.size())) {
    t.error = "ir::Serializer failed";
    return t;
  }
  if (disassemble) {
    ir::Disassembler::Options dis = {};
    dis.resolveConstants = true;
    dis.showConstants = false;
    dis.coloredOutput = false;
    std::ostringstream text;
    ir::Disassembler(builder, dis).disassemble(text);
    t.ir_text = text.str();
  }

  if (logger.errors) {
    t.error = "dxbc-spirv logged errors: " + logger.text;
    return t;
  }
  if (!g_dxvk_errors.empty()) {
    t.error = "DXVK logged errors: " + g_dxvk_errors;
    return t;
  }
  t.ok = true;
  return t;
}

std::string CacheKey::text() const {
  return "bytecode=" + bytecode_sha256 + "\nstage=" + stage + "\nvariant=" + variant + "\ntranslator=" + translator +
         "\nprofile=" + profile_sha256 + "\nlinkage=" + linkage + "\n";
}

std::string CacheKey::sha256() const {
  Sha256 h;
  auto s = text();
  h.update(s.data(), s.size());
  return h.hex();
}

}  // namespace d9g::shader
