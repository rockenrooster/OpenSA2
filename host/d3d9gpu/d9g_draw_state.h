// R2 draw request and independently packed draw values.
#pragma once
#include "d9g_draw.h"
#include "d9g_pipeline.h"
#include "d9g_sampler.h"
#include "d9g_shader.h"
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace d9g {
class Device;
namespace draw {

struct BufferId { uint32_t value; };
struct TextureId { uint32_t value; };
struct BufferSlice { BufferId buffer; uint32_t offset, available_bytes; };
struct TextureLevel { TextureId texture; uint32_t level; uint32_t layer = 0; };   // layer: a cube face's
enum class IndexWidth : uint8_t { U16, U32 };

struct ProgramImage {
    shader::Built vertex, pixel;
    shader::Reflection vertex_reflection, pixel_reflection;
    shader::CacheKey vertex_identity, pixel_identity;
};
using Program = std::shared_ptr<const ProgramImage>;
struct Unmapped { std::vector<std::string> reasons; };
struct Failure { std::string operation, reason; };
using ProgramResult = std::variant<Program, Unmapped, Failure>;
struct SelectedVariants {
    shader::Variant vertex, pixel;
    bool flat_shading;
    uint32_t target_swizzle_mask;
    // The draw's spec words (pack_spec_words) where the pixel shader's module declares the spec
    // constant (Reflection::spec_ids), zero elsewhere: its build makes them constants.
    std::array<uint32_t, shader::kSpecDwords> pixel_spec{};
};
struct ShaderSource {
    const std::vector<uint32_t>& tokens;
    const shader::Translated& base;
    // The shader object's memo of its content's id in this ProgramImages (0 until the first draw
    // interns it): tokens and base IR are immutable for the object's life, so the per-draw lookup
    // keys on small integers. Null: the content is interned at every call (the same id, slower).
    uint32_t* source_id = nullptr;
};
class ProgramImages {
public:
    ProgramImages();
    ~ProgramImages();
    // The program for a draw, by exact identity: both shaders' bytecode and base IR (interned to
    // ids by content), the profile's record and the translator's identity (interned when the
    // profile's bytes change), and the variants' fields. Equal identities give the same Program.
    ProgramResult for_draw(const ShaderSource&, const ShaderSource&,
                          const shader::ShaderProfile&, const SelectedVariants&);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct UniformBytes {
    uint32_t count;
    std::array<std::vector<uint8_t>, 4> slots;
};
struct SampledTexture { TextureId texture; SamplerDesc sampler; };
struct StageBindings {
    std::vector<SampledTexture> samplers;
    UniformBytes uniforms;
};
struct Attachments {
    std::vector<TextureLevel> colours;
    std::optional<TextureLevel> depth_stencil;
};
struct DynamicState {
    float x, y, width, height, min_depth, max_depth;
    int32_t scissor_x, scissor_y;
    uint32_t scissor_width, scissor_height;
    std::array<float, 4> blend_constant;
    uint8_t stencil_reference;
};

struct NoRealVertices {};
struct BoundVertices { BufferSlice stream_zero; };
// A stream other than 0 that the pipeline reads (d9g_pipeline.cpp: per vertex, or per instance
// under SetStreamSourceFreq's INSTANCEDATA), at its SetStreamSource offset; stream 0 is the
// geometry's VertexSource. By stream, ascending.
struct StreamSlice { uint32_t stream; BufferSlice slice; };
struct OwnedVertices { std::vector<uint8_t> padded_bytes; };
using VertexSource = std::variant<NoRealVertices, BoundVertices, OwnedVertices>;
struct NonIndexed {
    VertexSource vertices;
    uint32_t count, first_vertex;
};
struct BufferIndexed {
    VertexSource vertices;
    BufferSlice indices;
    IndexWidth width;
    uint32_t count, first_index, instances;
    int32_t base_vertex;
};
struct OwnedIndexedUp {
    std::vector<uint8_t> bytes;
    uint32_t vertex_bytes, index_offset, index_bytes;
    IndexWidth width;
    uint32_t count, instances;
    // DXVK's indexed-UP prefix and padding, indices after the vertex allocation.
    // First index and base vertex are zero relative to this owned block.
};
using Geometry = std::variant<NonIndexed, BufferIndexed, OwnedIndexedUp>;

struct Values {
    Program program;
    PipelineDesc pipeline;
    Attachments attachments;
    Geometry geometry;
    std::optional<BufferSlice> semantic_null;
    std::vector<StreamSlice> streams;
    StageBindings vertex, pixel;
    DynamicState dynamic;
};
class DrawRequest {
public:
    const Values& values() const { return values_; }
private:
    explicit DrawRequest(Values values);
    Values values_;
    friend class ::d9g::Device;
};
using Prepared = std::variant<DrawRequest, Unmapped, Failure>;
struct Encoded { uint64_t uploaded_up_bytes; };
using Outcome = std::variant<Encoded, Failure>;

// Added to Gpu: virtual draw::Outcome draw(const draw::DrawRequest&) = 0;
// Device is the sole request factory. Its existing COM guards remain explicit.
// Pure packers return values; they cannot mutate or execute a partial request.
// Gpu resolves IDs and consumes bytes before returning. It retains no request.
// Internal SDL caches include exact shader identities and canonical mapped keys.
// Uniform holes are initialized; compact samplers follow Built's register order.
// Semantic null/filler bindings cover the complete highest-slot interval.
// Current-state selection never depends solely on census dirty bits.
// UP uploads precede the consuming draw, never wait for Present.
// Existing resource operations close incompatible passes and preserve order.


// Immediate views of Device-owned banks. Counts are registers, not scalar elements.
struct ConstantBank {
    const std::array<float, 4>* floats = nullptr;
    uint32_t float_count = 0;
    const std::array<int32_t, 4>* ints = nullptr;
    uint32_t int_count = 0;
    const int32_t* bools = nullptr;
    uint32_t bool_count = 0;
    uint32_t changed_float_count = 0;
};
struct FloatWriteFacts { bool changed; uint32_t changed_float_count; };
// The caller has already applied API validation and hardware truncation.
FloatWriteFacts float_write_facts(const void* previous_registers, const void* next_registers,
                                 uint32_t start, uint32_t effective_count,
                                 uint32_t previous_changed_float_count);

struct ClipPlanes {
    uint8_t count = 0;
    std::array<uint8_t, 96> bytes{};
};
ClipPlanes pack_clip_planes(const float (&planes)[6][4], uint32_t enabled_mask);

struct SharedPush {
    std::array<uint8_t, 3> fog_colour{};
    uint8_t alpha_reference = 0;
    float fog_distance_scale = 0.0f;
    float fog_distance_end = 0.0f;
    float fog_density = 0.0f;
};
// Fog disabled retains the last enabled values, as DXVK UpdateFog does.
SharedPush update_fog_push(const SharedPush& previous, bool enabled, uint32_t colour,
                          float start, float end, float density);

struct SpecFacts {
    LegacyData legacy;
    uint8_t clip_plane_count = 0, dref_scale = 0;
    bool fog_enabled = false, fog_use_z = false;
    uint8_t fog_vertex_mode = 0, fog_pixel_mode = 0;
    uint8_t sampler_projection_mask = 0;
    bool global_specular = false;
    uint8_t vs_sampler_types = 0, vs_sampler_modes = 0;
    uint32_t ps_sampler_types = 0, ps_sampler_modes = 0;
};
using SpecWords = std::array<uint32_t, shader::kSpecDwords>;
using SpecResult = std::variant<SpecWords, Unmapped>;
SpecResult pack_spec_words(const SpecFacts&, const shader::Reflection& vertex,
                          const shader::Reflection& pixel, const ConstantBank& vertex_bank,
                          const ConstantBank& pixel_bank);

struct TextureStageInput {
    uint32_t constant = 0;
    uint32_t bump00 = 0, bump01 = 0, bump10 = 0, bump11 = 0;
    uint32_t luminance_scale = 0, luminance_offset = 0;
};
struct UniformInputs {
    ConstantBank constants;
    // DXVK updates the VS push count only when dynamic float storage is nonempty.
    uint16_t previous_dynamic_float_count = 0;
    SpecWords spec{};
    SharedPush shared;
    LegacyData legacy;
    ClipPlanes clip_planes;
    std::array<TextureStageInput, 8> texture_stages{};
    // Already decoded RGBA after the view's border interpretation.
    std::array<std::array<float, 4>, shader::kBorderColourRegisters> border_colours{};
};
struct PackedUniforms {
    UniformBytes uniforms;
    uint16_t dynamic_float_count = 0;
};
using UniformResult = std::variant<PackedUniforms, Unmapped>;
// `storage` lends its slots' memory (a previous draw's, returned by the device): the result is the
// same as with none, every slot at or past the count empty.
UniformResult pack_uniforms(const shader::Reflection&, const shader::Built&, const UniformInputs&,
                            UniformBytes storage = {});

} // namespace draw
} // namespace d9g
