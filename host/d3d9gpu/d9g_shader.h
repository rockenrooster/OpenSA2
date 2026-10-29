// Host 2's shader translator (R1, ruling 2: "reuse DXVK's shader compiler";
// Documentation/decisions/2026-10-04-S5-user-rulings.md).
//
// A Direct3D 9 shader's bytecode goes through DXVK 3.1.1's own compiler, in DXVK's order:
// D3D9ShaderAnalysis, dxbc-spirv's sm3::Converter with DXVK's options, DXVK's
// D3D9ShaderLowerLegacyInputPass (extracted verbatim at build time by shader/extract_dxvk_shader.py),
// ir::legalizeIr with DXVK's CompileOptions for the device's profile, and dxbc-spirv's SPIR-V
// builder with DXVK's SPIR-V options. What is host 2's own is the SDL ABI (d9g_shader_abi.cpp):
// spec constants and push data become one uniform buffer, and descriptors take SDL_gpu's sets.
// What the compiler produces is shared with DXVK; what host 2 feeds it (constant packing, spec
// and push values, vertex input, sampler bindings) stays the device's own and is checked against
// DXVK's by the state check.
//
// The two steps mirror DXVK's DxvkIrShader: translate() is convertShader() (up to the serialized
// IR, plus the reflection the device needs), build() is getCode(): unlinked (DXVK's
// getCode(nullptr, nullptr), the GPL variant) or linked against the previous stage's outputs
// (getCode(bindings, &linkage)). Every compile runs under the default floating-point environment
// (MXCSR 0x1F80, x87 control word 0x037F), because dxbc-spirv folds constants on the host CPU and
// DXVK compiles on worker threads that have that environment.
#pragma once

#include "d9g_shader_profile.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace d9g::shader {

enum class Stage : uint8_t { Vertex = 0, Pixel = 1 };

// SDL_gpu's Vulkan binding model (SDL_gpu.h, SDL_CreateGPUShader): sampled textures in set 0
// (vertex) or 2 (fragment), uniform buffers in set 1 (vertex) or 3 (fragment). Uniform slots are
// fixed per kind, so the device's pushes need no per-shader table; an unused slot below the
// highest used one is declared to SDL and never read.
enum UniformSlot : uint32_t {
  kVsStaticConstants = 0, kVsDynamicConstants = 1, kVsClipPlanes = 2, kVsState = 3,
  kPsStaticConstants = 0, kPsState = 1, kPsTextureStages = 2, kPsBorderColours = 3,
};

// The border fallback's colours (ruling 3, R1 U11; Variant::border_u/border_v): one f32x4 per pixel
// sampler register 0-15, RGBA in [0, 1], at the register's index, in uniform slot kPsBorderColours.
// The device fills it with the colour DXVK's border sampler returns for the register (D3DSAMP_BORDERCOLOR
// as DXVK decodes it for the view): the shader only blends with what it is given.
constexpr uint32_t kBorderColourRegisters = 16;
constexpr uint32_t kBorderColourBytes = kBorderColourRegisters * 16;

// The state buffer: DXVK's spec data (D3D9SpecData, src/d3d9/d3d9_state.h:105, seven dwords, spec
// id N at byte 4N) followed by its push data (D3D9SharedPushData at byte 28, D3D9VsPushData at
// byte 44 in vertex shaders), scalar layout. The layout is fixed per stage whatever the shader reads.
constexpr uint32_t kSpecDwords = 7;
constexpr uint32_t kStateSharedPushOffset = 28;
constexpr uint32_t kStateVsPushOffset = 44;
constexpr uint32_t kStateBytesVs = 52, kStateBytesPs = 44;
// SDL binds each uniform slot with a 4 KiB window (MAX_UBO_SECTION_SIZE, SDL_gpu_vulkan.c:71).
constexpr uint32_t kSdlUniformWindow = 4096;

struct Variant {
  // Vertex shaders: input locations (signature indices) whose D3DCOLOR data arrive as the bytes
  // B, G, R, A in an RGBA8 attribute (SDL_gpu has no BGRA8 vertex format): the shader reads them
  // .zyxw. DXVK gets the same values from VK_FORMAT_B8G8R8A8_UNORM.
  uint32_t bgra_mask = 0;
  // Pixel shaders, ruling 3's border fallback (R1 U11): bit r, pixel sampler register r is
  // D3DTADDRESS_BORDER on its U (border_u) or V (border_v) axis on a device without the SDL patch's
  // CLAMP_TO_BORDER (d9g_sampler.h AddrDecision::fallback_border), whose sampler the device then makes
  // CLAMP_TO_EDGE on that axis. Each 2D sample of the register is blended with its border colour
  // (kPsBorderColours) by the sample's coverage of the texture on those axes: an inside test where
  // border_linear's bit r is clear (point filtering: exact), the bilinear footprint's share inside the
  // level-0 extent where it is set (linear filtering: a named residue, measured by test_shaders.sh C8).
  uint16_t border_u = 0, border_v = 0, border_linear = 0;
  // Reserved for ruling 3's depth-clamp emulation, which R1 builds only if the depth-clamp
  // measurement (the sdl-patch plan's H2) finds a pixel clamp changes; translate() refuses it.
  bool depth_clamp = false;
  // Pixel shaders, the sampler-mode specialization: DXVK's psSamplerModes word (D3D9SpecData spec id 5,
  // src/d3d9/d3d9_state.h:136-140, built by updateSamplers :277-296), two bits per sampler register r at
  // bits 2r and 2r+1: 0 a plain sample, 1 fetch4 (a gather), 2 a depth-compare sample, 3 a depth-compare
  // sample with its reference clamped to [0, 1]. Off (the base), the module keeps DXVK's uniform branch on
  // the word read from the state buffer: the identity object C3's GPL half compares with DXVK's dump. On,
  // each samplerMode(r) call (the lowering pass's spec-constant function, d3d9_shader.cpp:926-927) becomes
  // the constant (word >> 2r) & 3 and dxbc-spirv's own folding removes the branches that word makes dead:
  // a register at mode 0 keeps no depth-compare sample and no gather, so neither the Vulkan validation
  // layer (VUID-vkCmdDraw-None-06479) nor SPIRV-Cross (which types an image with any depth-compare use as
  // depth2d) sees one on a colour texture. The word must name only registers the module reads a mode for
  // (Reflection::sampler_mode_slots): a caller masks DXVK's word by them, so one module has one key.
  // Empty is the base (the key's sampler_modes=none); a word, 0 included, is the specialization.
  std::optional<uint32_t> sampler_modes;
  std::string key() const;
};

// DXVK's sampler mode for one pixel sampler register at a draw (D3D9DeviceEx's slot tracking, as
// d3d9_state.h:277-296 packs it): 2 (depth compare) for a depth texture DXVK samples as a shadow map
// (D3D9CommonTexture::DetermineShadowState, d3d9_common_texture.cpp:514-521: a depth format but INTZ, DF16
// and DF24), 3 where that texture's format is emulated with a 32-bit float depth format
// (IsUpgradedToD32f, :42-43), 1 (fetch4) where the stage's fetch4 switch is on (fetch4_after), the
// texture's format is single-channel (DetermineFetch4Compatibility, :524-532) and the magnification
// filter is point (UpdateActiveFetch4, d3d9_device.cpp:6471-6488; programmable pixel shaders only, :7469)
// and the texture is not a shadow map (updateSamplers: fetch4Mask & ~drefMask), and 0 otherwise, an
// unbound register included (nullOrUnusedMask, d3d9_device.cpp:7466).
struct SamplerSlot {
  bool bound = false;
  uint32_t d3dfmt = 0;                // D3DFORMAT of the bound texture
  bool upgraded_to_d32f = false;      // the device stores that depth format as D32_SFLOAT(_S8_UINT)
  bool fetch4_enabled = false;        // the stage's fetch4 switch: fetch4_after over every MIPMAPLODBIAS write
  uint32_t mag_filter = 0;            // D3DSAMP_MAGFILTER
};
// DXVK's fetch4 switch for a sampler stage (D3D9DeviceEx::SetStateSamplerState, d3d9_device.cpp:4579-4588,
// its fetch4SamplerState bit): a D3DSAMP_MIPMAPLODBIAS write of GET4 turns it on, of GET1 off, and any
// other value leaves it as it was. It is sticky: the value D3DSAMP_MIPMAPLODBIAS holds now does not say
// whether fetch4 is on, so a device keeps the switch per stage and applies this to each write.
bool fetch4_after(bool enabled, uint32_t mip_lod_bias);
uint32_t dxvk_sampler_mode(const SamplerSlot& slot);
// The word over registers 0-15 (the slots' modes at bits 2r), masked by the module's sampler-mode
// registers (Reflection::sampler_mode_slots), as Variant::sampler_modes takes it.
uint32_t sampler_modes_word(const SamplerSlot (&slots)[16], uint32_t sampler_mode_slots);

struct ApiConstantIndex { uint32_t value; };
struct DefinedFloatIndex { uint32_t value; };
using ConstantSource = std::variant<ApiConstantIndex, DefinedFloatIndex>;

struct ConstantRange {
  // An API register unless defined is true. A defined float range uses the compact index in
  // ConstantFacts::defined_float_bits, not the register named by the shader's def instruction.
  uint32_t src = 0;
  uint32_t dst = 0;    // first member (static buffer) or element (dynamic buffer)
  uint32_t count = 0;
  bool dynamic = false;   // D3D9ConstantRange::isDynamicallyIndexed
  bool defined = false;   // D3D9ConstantRange::isShaderDefined: filled from the shader's def values
  ConstantSource source() const {
    if (defined)
      return DefinedFloatIndex{src};
    return ApiConstantIndex{src};
  }
};

// Owned layout facts copied while DXVK's analysis is alive. Static def values fold into IR;
// this compact array covers only the shader-defined source ranges in float_ranges.
struct ConstantFacts {
  std::vector<std::array<uint32_t, 4>> defined_float_bits;
  uint32_t float_min_count = 0, float_max_count = 0;
  uint16_t used_hardware_bool_mask = 0;
};

struct UniformUse {
  uint32_t slot = 0;
  uint32_t bytes = 0;  // the declared size (array or struct, scalar layout)
  std::string kind;    // "static", "dynamic", "clip_planes", "state", "texture_stages"
};

struct IoVar {
  bool builtin = false;
  std::string builtin_name;   // dxbc-spirv BuiltIn, for built-ins
  uint32_t location = 0;
  uint32_t component_mask = 0;
  std::string semantic;
  uint32_t semantic_index = 0;
};

// The special members of Reflection, Translated and Built are defined once, in the translator
// (d9g_shader.cpp), not inline: the device's units are compiled with the device's flags and the
// translator's with DXVK's (-D_GLIBCXX_ASSERTIONS=1), so an inline one would be defined weakly by
// both, which host/d3d9gpu/build.sh's "both define" check refuses.
struct Reflection {
  Reflection();
  ~Reflection();
  Reflection(const Reflection&);
  Reflection(Reflection&&) noexcept;
  Reflection& operator=(const Reflection&);
  Reflection& operator=(Reflection&&) noexcept;
  Stage stage = Stage::Vertex;
  uint32_t major = 0, minor = 0;
  // Vertex shaders: DXVK's analysis's input signature (D3D9ShaderAnalysis::GetInputSignature, in
  // dcl order): the D3DDECLUSAGE and usage index of signature location i.
  std::vector<std::pair<uint32_t, uint32_t>> input_signature;
  uint32_t sampler_mask = 0;   // GetSamplerMask
  uint32_t rt_mask = 0;        // GetRenderTargetMask (pixel shaders)
  // DXVK's constant layout (D3D9ConstantBufferCopy, d3d9_constant_copy.h)
  bool floats_dynamic = false;
  std::vector<ConstantRange> float_ranges, int_ranges, bool_ranges;
  bool static_buffer_is_dword_array = false;  // DXVK's SWVP-style fallback (d3d9_shader.cpp:300-315)
  // I/O after legalisation, for linkage (ir::IoMap::forInputs / forOutputs)
  std::vector<IoVar> inputs, outputs;
  // Pixel shaders: the analysis's flat-shading mask (GetFlatShadingMask) on the user input
  // locations the shader declares, DXVK's metadata.flatShadingInputs (dxvk_shader_ir.cpp:602-611).
  uint32_t flat_shading_inputs = 0;
  // A border variant (Variant::border_u/border_v): the registers of its border set with a read the
  // emulation leaves as it is, by kind. dxbc-spirv's SM3 sampling function holds, beside the 2D colour
  // sample the emulation wraps, a depth-compare sample (taken where the spec data's samplerMode says a
  // depth texture) and a gather (fetch4, samplerMode 1) on the same register, and a ps_1_x register
  // can carry 2D, volume and cube declarations at once. None of those is wrapped, so a draw that
  // takes one of them on a fallback device reads edge texels where D3D9 reads the border: the device
  // must leave such a draw unmapped, by name (R2-draws: a BORDER sampler on a device without
  // CLAMP_TO_BORDER whose texture is a depth texture, fetch4-enabled, or not 2D). Specialized at a
  // sampler-mode word (Variant::sampler_modes), a register keeps only what its mode takes: at 1 its
  // gather, at 2 or 3 its depth-compare sample, recorded here, and no colour sample to wrap.
  uint32_t border_unwrapped_depth_compare = 0;   // a sample with a depth reference
  uint32_t border_unwrapped_gather = 0;          // a gather (fetch4)
  uint32_t border_unwrapped_other = 0;           // any other read: a non-2D or non-colour sample, a load
  // Pixel shaders: bit r, the module reads sampler register r's mode (a samplerMode(r) call before any
  // specialization): the registers Variant::sampler_modes may name.
  uint32_t sampler_mode_slots = 0;
  // Bit r, the module as built samples register r with a depth reference, or gathers it. In the base
  // each 2D or cube register of sampler_mode_slots has both (dxbc-spirv's SM3 sampling function,
  // sm3_resources.cpp:597-724); in a specialized module only the registers at mode 2 or 3 keep the
  // first and only those at mode 1 the second.
  uint32_t depth_compare_registers = 0;
  uint32_t gather_registers = 0;
  ConstantFacts constant_facts;
  // Bit i: the module declares spec constant i, D3D9SpecData's dword i: the words a build with
  // spec values (build's `spec`) makes constants, and the words the device keys that build on.
  uint32_t spec_ids = 0;
};

struct Translated {
  Translated();
  ~Translated();
  Translated(const Translated&);
  Translated(Translated&&) noexcept;
  Translated& operator=(const Translated&);
  Translated& operator=(Translated&&) noexcept;
  bool ok = false;
  std::string error;
  Stage stage = Stage::Vertex;
  std::string debug_name;
  std::vector<uint8_t> ir;    // the serialized IR (dxbc-spirv ir::Serializer), DXVK's m_ir
  std::string ir_text;        // its disassembly, only where translate() is asked for it (the tools)
  Reflection reflection;
};

// DxvkShaderLinkage's fields that matter to D3D9 pairs (src/dxvk/dxvk_shader.h), as
// DxvkGraphicsPipeline builds them (dxvk_graphics.cpp:880-915): the previous stage is the vertex
// shader (its outputs, its SemanticIo flag), flat shading is D3DRS_SHADEMODE == D3DSHADE_FLAT
// while the pixel shader has flat-shading inputs, and render-target swizzles are identity for
// every format the recorded streams use; a non-identity swizzle is unmapped, never approximated.
struct Linkage {
  const Translated* vs = nullptr;
  bool flat_shade_mode = false;
  uint32_t rt_nonidentity_swizzle_mask = 0;
};

struct Built {
  Built();
  ~Built();
  Built(const Built&);
  Built(Built&&) noexcept;
  Built& operator=(const Built&);
  Built& operator=(Built&&) noexcept;
  bool ok = false;
  std::string error;
  std::vector<uint32_t> spirv;
  bool linked = false;
  bool io_rewritten = false;   // the linkage resolved mismatched or semantic I/O
  // What SDL_CreateGPUShader is told, and what the device binds and pushes.
  std::vector<uint32_t> sampler_registers;  // binding order: SDL sampler slot i is register [i]
  std::vector<UniformUse> uniforms;
  uint32_t num_samplers = 0;
  uint32_t num_uniform_buffers = 0;         // highest used slot + 1
  bool dynamic_capped = false;              // DXVK's count + 1 exceeded SDL's window
};

// A refusal the translator reports (an unknown version token, an analysis or converter failure, a
// variant it does not take) returns ok false with the reason. Bytecode that trips one of
// dxbc-spirv's own assertions aborts the process instead, as it does in DXVK's player: the refusal
// claim covers reported refusals only. disassemble: fill ir_text (the device never asks for it).
Translated translate(const void* bytecode, size_t bytes, const ShaderProfile& profile, const Variant& variant,
                     bool disassemble = false);
// spec: null for the module DXVK's pipeline library builds (every spec constant read from the state
// buffer), or kSpecDwords words: each spec constant the module declares (Reflection::spec_ids)
// becomes that word, a constant, as DXVK's optimized pipelines specialize theirs.
Built build(const Translated& shader, const ShaderProfile& profile, const Linkage* linkage,
            const uint32_t* spec = nullptr);

// The vk11 profile's lowering (ShaderProfile::vk11; d9g_shader_abi.cpp), which build() runs on each
// module it builds with that profile. dxbc-spirv builds SPIR-V 1.6 with PhysicalStorageBuffer64 and
// the Vulkan memory model, and the alpha test (and D3D9 texkill) as a demote; a device SDL_gpu makes at
// Vulkan 1.1 takes SPIR-V 1.3 and has Vulkan 1.0 features only. The module becomes SPIR-V 1.3, Logical
// GLSL450, with the two memory-model capabilities dropped, the entry point's interface trimmed to its
// inputs and outputs, and each [OpLabel, OpDemoteToHelperInvocation, OpBranch] block made [OpLabel,
// OpKill] (with the demote capability dropped); the ID bound is unchanged. Everything else must already
// be a 1.0 construct, from an allowlist of capabilities (ClipDistance only where `clip_distance`), storage
// classes, execution modes and operand bits; a demote is lowered only in a fragment shader's entry
// function, never in a loop, never where an OpPhi reads its block, and never where a derivative or an
// implicit-LOD sample can run after it (a texkill before a texld: OpKill would leave the quad's
// derivatives undefined). Returns "" when lowered. Otherwise `words` is unchanged and the reason is
// "unmapped: vk11: <what>" for a construct the 1.1 device cannot take (the draw is unmapped by name), or
// starts "vk11 lowering:" for a module that is not the translator's (malformed, or a SPIR-V version
// other than 1.6: a pin change), which is a failure.
std::string lower_vk11(std::vector<uint32_t>& words, Stage stage, bool clip_distance);

// DxvkShaderIo::checkStageCompatibility over the two stages' I/O (src/dxvk/dxvk_shader_io.cpp:43-93),
// with DXVK's own code: false means DXVK's linked variant rewrites the pixel shader's I/O.
bool stages_compatible(const Translated& vs, const Translated& ps, bool match_semantics);

// The translator's identity for the shader cache: the sha256 of the dxbc-spirv archive and of the
// translator's sources, fixed at build time (D9G_TRANSLATOR_ID).
std::string translator_identity();

// The shader cache (R1 U5, d9g_shader_cache.cpp), under the repository's private/ only.
struct CacheKey {
  std::string bytecode_sha256, stage, variant, translator, profile_sha256, linkage;
  std::string text() const;
  std::string sha256() const;
};
struct CacheStats { uint32_t hits = 0, misses = 0, verified = 0, corrupt = 0; };

// A cache entry: the module and what the device binds for it (Built's binding fields), so a hit
// needs no build at all.
struct CachedModule {
  std::vector<uint32_t> spirv;
  std::string bindings;   // encode_bindings(Built)
  bool operator==(const CachedModule& o) const { return spirv == o.spirv && bindings == o.bindings; }
  bool operator!=(const CachedModule& o) const { return !(*this == o); }
};
std::string encode_bindings(const Built& b);
bool decode_bindings(const std::string& text, Built& b);

class Cache {
 public:
  // root must resolve inside <repo>/private; anything else is refused (ok() false).
  Cache(const std::string& root, const std::string& repo_root, bool verify);
  bool ok() const { return m_ok; }
  const std::string& error() const { return m_error; }
  // Returns the entry for key, compiling it with compile(CachedModule&) on a miss; in verify mode a
  // hit is recompiled and compared byte for byte, and a differing or corrupt entry is replaced and
  // counted.
  template <class F> bool get(const CacheKey& key, CachedModule& module, F compile);
  const CacheStats& stats() const { return m_stats; }
 private:
  bool load(const CacheKey& key, CachedModule& module);
  bool store(const CacheKey& key, const CachedModule& module);
  std::string path_for(const CacheKey& key) const;
  std::string m_root;
  bool m_verify = false, m_ok = false;
  std::string m_error;
  CacheStats m_stats;
};

template <class F> bool Cache::get(const CacheKey& key, CachedModule& module, F compile) {
  CachedModule cached;
  bool hit = load(key, cached);
  if (hit && !m_verify) {
    m_stats.hits++;
    module = std::move(cached);
    return true;
  }
  if (!compile(module))
    return false;
  if (hit) {
    m_stats.verified++;
    if (cached != module) {
      m_stats.corrupt++;
      return store(key, module);
    }
    m_stats.hits++;
    return true;
  }
  m_stats.misses++;
  return store(key, module);
}

}  // namespace d9g::shader
