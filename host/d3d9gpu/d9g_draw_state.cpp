// R2 shader artifacts and independent API-value packing. DXVK 3.1.1's constant layout
// is shared compiler metadata; no DXVK constant-copy helper is called here.
#include "d9g_draw_state.h"
#include "sha256.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <type_traits>
#include <unordered_map>

namespace d9g::draw {
namespace {

void u16(std::vector<uint8_t>& dst, size_t off, uint16_t value) {
    dst[off] = uint8_t(value); dst[off + 1] = uint8_t(value >> 8);
}
void u32(std::vector<uint8_t>& dst, size_t off, uint32_t value) {
    for (uint32_t i = 0; i < 4; ++i) dst[off + i] = uint8_t(value >> (8 * i));
}
uint32_t bits(float value) {
    uint32_t result; std::memcpy(&result, &value, sizeof result); return result;
}
void append_u32(std::string& out, uint32_t value) {
    for (uint32_t i = 0; i < 4; ++i) out.push_back(char(value >> (8 * i)));
}
void append_blob(std::string& out, const void* data, size_t bytes) {
    // A fixed-width length makes concatenated identities unambiguous.
    uint64_t n = bytes;
    for (uint32_t i = 0; i < 8; ++i) out.push_back(char(n >> (8 * i)));
    if (bytes) out.append(static_cast<const char*>(data), bytes);
}
void append_text(std::string& out, const std::string& text) { append_blob(out, text.data(), text.size()); }
void append_tokens(std::string& out, const std::vector<uint32_t>& tokens) {
    append_u32(out, uint32_t(tokens.size()));
    for (auto token : tokens) append_u32(out, token);
}
std::string sha(const void* data, size_t size) {
    Sha256 h; h.update(data, size); return h.hex();
}
std::string token_sha(const std::vector<uint32_t>& tokens) {
    std::string bytes;
    for (auto token : tokens) append_u32(bytes, token);
    return sha(bytes.data(), bytes.size());
}
ProgramResult program_error(const std::string& operation, const std::string& error) {
    if (error.compare(0, 9, "unmapped:") == 0) return Unmapped{{error}};
    return Failure{operation, error};
}
uint32_t range_end(const std::vector<shader::ConstantRange>& ranges, bool& valid) {
    uint32_t end = 0;
    for (const auto& r : ranges) {
        if (r.count > UINT32_MAX - r.dst) { valid = false; return 0; }
        end = std::max(end, r.dst + r.count);
    }
    return end;
}
bool bool_bits(const ConstantBank& bank, uint16_t mask, uint16_t& result) {
    result = 0;
    for (uint32_t i = 0; i < 16; ++i) {
        if (!(mask & (1u << i))) continue;
        if (!bank.bools || i >= bank.bool_count) return false;
        // D3D9 BOOL writes are truth tests; noncanonical nonzero values set the bit.
        if (bank.bools[i]) result |= uint16_t(1u << i);
    }
    return true;
}

} // namespace

namespace {
// A draw's program identity as integers: the two shaders' content ids, the profile's id and
// every field of both variants (Variant::key()'s fields), the flat-shading and swizzle words.
// Equal keys are equal identities: each id is interned from exact content.
static_assert(sizeof(shader::Variant) == 20, "a new shader::Variant field must join ProgramKey");
struct ProgramKey {
    std::array<uint32_t, 13 + shader::kSpecDwords> w{};
    bool operator==(const ProgramKey& o) const { return same_bytes(w.data(), o.w.data(), sizeof w); }
};
struct ProgramKeyHash {
    size_t operator()(const ProgramKey& k) const {
        uint64_t h = 0xcbf29ce484222325ull;
        for (uint32_t x : k.w) { h ^= x; h *= 0x100000001b3ull; h ^= h >> 29; }
        return size_t(h);
    }
};
void variant_words(const shader::Variant& v, uint32_t* w) {
    w[0] = v.bgra_mask;
    w[1] = uint32_t(v.border_u) | uint32_t(v.border_v) << 16;
    w[2] = uint32_t(v.border_linear) | uint32_t(v.depth_clamp) << 16 | uint32_t(v.sampler_modes.has_value()) << 17;
    w[3] = v.sampler_modes.value_or(0);
}
} // namespace

struct ProgramImages::Impl {
    std::unordered_map<ProgramKey, Program, ProgramKeyHash> images;
    // shader content (bytecode, base IR) -> id, from 1
    std::unordered_map<std::string, uint32_t> sources;
    // profile record and translator identity -> id, from 1; the profile last seen, by its bytes
    std::unordered_map<std::string, uint32_t> profiles;
    const std::string translator = shader::translator_identity();
    unsigned char last_profile[sizeof(shader::ShaderProfile)]{};
    uint32_t profile_id = 0;
    std::string profile_text;

    uint32_t source_id(const ShaderSource& s) {
        if (s.source_id && *s.source_id) return *s.source_id;
        std::string content;
        append_tokens(content, s.tokens);
        append_blob(content, s.base.ir.data(), s.base.ir.size());
        const uint32_t id = sources.emplace(std::move(content), uint32_t(sources.size() + 1)).first->second;
        if (s.source_id) *s.source_id = id;
        return id;
    }
    uint32_t profile(const shader::ShaderProfile& p) {
        static_assert(std::is_trivially_copyable<shader::ShaderProfile>::value, "the profile is compared by its bytes");
        // Any byte that differs (a field, or padding) re-reads the record: never a stale id.
        if (profile_id && same_bytes(last_profile, &p, sizeof p)) return profile_id;
        profile_text = p.record();
        std::string identity;
        append_text(identity, profile_text); append_text(identity, translator);
        profile_id = profiles.emplace(std::move(identity), uint32_t(profiles.size() + 1)).first->second;
        std::memcpy(last_profile, &p, sizeof p);
        return profile_id;
    }
};
ProgramImages::ProgramImages() : impl_(std::make_unique<Impl>()) {}
ProgramImages::~ProgramImages() = default;

ProgramResult ProgramImages::for_draw(const ShaderSource& vs_source, const ShaderSource& ps_source,
                                     const shader::ShaderProfile& profile, const SelectedVariants& variants) {
    if (variants.target_swizzle_mask) return Unmapped{{"render-target swizzle is not identity"}};
    if (variants.vertex.depth_clamp || variants.pixel.depth_clamp)
        return Unmapped{{"reserved depth-clamp shader variant is unsupported"}};
    if (!vs_source.base.ok || !ps_source.base.ok)
        return Unmapped{{"draw shader base translation failed"}};
    if (vs_source.base.stage != shader::Stage::Vertex || ps_source.base.stage != shader::Stage::Pixel)
        return Unmapped{{"draw shader source has the wrong stage"}};
    if (vs_source.tokens.empty() || ps_source.tokens.empty())
        return Unmapped{{"draw shader source is empty"}};

    ProgramKey identity;
    identity.w[0] = impl_->source_id(vs_source);
    identity.w[1] = impl_->source_id(ps_source);
    identity.w[2] = impl_->profile(profile);
    variant_words(variants.vertex, &identity.w[3]);
    variant_words(variants.pixel, &identity.w[7]);
    identity.w[11] = variants.flat_shading;
    identity.w[12] = variants.target_swizzle_mask;
    for (uint32_t i = 0; i < shader::kSpecDwords; ++i) identity.w[13 + i] = variants.pixel_spec[i];
    auto hit = impl_->images.find(identity);
    if (hit != impl_->images.end()) return hit->second;
    const std::string& profile_text = impl_->profile_text;
    const std::string& translator = impl_->translator;

    const shader::Variant base_variant;
    auto vertex = variants.vertex.key() == base_variant.key() ? vs_source.base
        : shader::translate(vs_source.tokens.data(), vs_source.tokens.size() * sizeof(uint32_t), profile, variants.vertex);
    if (!vertex.ok) return program_error("translate vertex shader", vertex.error);
    auto pixel = variants.pixel.key() == base_variant.key() ? ps_source.base
        : shader::translate(ps_source.tokens.data(), ps_source.tokens.size() * sizeof(uint32_t), profile, variants.pixel);
    if (!pixel.ok) return program_error("translate pixel shader", pixel.error);
    const auto& r = pixel.reflection;
    if (r.border_unwrapped_depth_compare || r.border_unwrapped_gather || r.border_unwrapped_other)
        return Unmapped{{"border fallback has an unwrapped depth, gather or non-2D read"}};

    auto image = std::make_shared<ProgramImage>();
    image->vertex = shader::build(vertex, profile, nullptr);
    if (!image->vertex.ok) return program_error("build vertex shader", image->vertex.error);
    shader::Linkage linkage{&vertex, variants.flat_shading, variants.target_swizzle_mask};
    // The pixel shader specialized at its spec words: the work per pixel is the work per vertex
    // times the scene's overdraw, and a phone's GPU spends most of a frame there. The vertex shader
    // keeps reading its words from the state buffer, so its variants stay as many as before.
    image->pixel = shader::build(pixel, profile, &linkage, variants.pixel_spec.data());
    // A specialization the translator refuses is built unspecialized, as before, and said once: the
    // draw still draws, and the image, cached under this key, is not built again at the next draw.
    if (!image->pixel.ok) {
        static bool said = false;
        if (!said) {
            said = true;
            std::fprintf(stderr, "d3d9gpu: a pixel shader could not be specialized (%s): built unspecialized\n",
                         image->pixel.error.c_str());
        }
        image->pixel = shader::build(pixel, profile, &linkage);
    }
    if (!image->pixel.ok) return program_error("build pixel shader", image->pixel.error);
    image->vertex_reflection = vertex.reflection;
    image->pixel_reflection = pixel.reflection;
    const auto profile_sha = sha(profile_text.data(), profile_text.size());
    image->vertex_identity = {token_sha(vs_source.tokens), "vertex", variants.vertex.key(), translator, profile_sha, "unlinked"};
    // The owning VS key and exact selected IR include linkage inputs and profile semantics.
    std::string link_identity;
    append_text(link_identity, image->vertex_identity.text());
    append_blob(link_identity, vertex.ir.data(), vertex.ir.size());
    append_u32(link_identity, variants.flat_shading);
    append_u32(link_identity, variants.target_swizzle_mask);
    image->pixel_identity = {token_sha(ps_source.tokens), "pixel", variants.pixel.key(), translator, profile_sha,
                            "linked:" + sha(link_identity.data(), link_identity.size())};
    Program owned = std::move(image);
    impl_->images.emplace(std::move(identity), owned);
    return owned;
}

DrawRequest::DrawRequest(Values values) : values_(std::move(values)) {}

FloatWriteFacts float_write_facts(const void* previous, const void* next, uint32_t start,
                                 uint32_t count, uint32_t previous_count) {
    const bool changed = count && !same_bytes(previous, next, size_t(count) * 16);
    return {changed, changed ? std::max(previous_count, start + count) : previous_count};
}

ClipPlanes pack_clip_planes(const float (&planes)[6][4], uint32_t mask) {
    ClipPlanes out;
    // DXVK UpdateClipPlanes compares all 16 bytes to an all-zero plane. Negative zero counts.
    const std::array<uint8_t, 16> zero{};
    for (uint32_t i = 0; i < 6; ++i) {
        if (!(mask & (1u << i)) || same_bytes(planes[i], zero.data(), 16)) continue;
        std::memcpy(out.bytes.data() + 16 * out.count++, planes[i], 16);
    }
    return out;
}

SharedPush update_fog_push(const SharedPush& previous, bool enabled, uint32_t colour,
                          float start, float end, float density) {
    SharedPush out = previous;
    if (enabled) {
        out.fog_colour = {uint8_t(colour), uint8_t(colour >> 8), uint8_t(colour >> 16)};
        out.fog_distance_scale = end != start ? 1.0f / (end - start) : std::numeric_limits<float>::max();
        out.fog_distance_end = end;
        out.fog_density = density;
    }
    return out;
}

SpecResult pack_spec_words(const SpecFacts& in, const shader::Reflection& vertex,
                          const shader::Reflection& pixel, const ConstantBank& vs_bank,
                          const ConstantBank& ps_bank) {
    uint16_t vs_bools, ps_bools;
    if (!bool_bits(vs_bank, vertex.constant_facts.used_hardware_bool_mask, vs_bools) ||
        !bool_bits(ps_bank, pixel.constant_facts.used_hardware_bool_mask, ps_bools))
        return Unmapped{{"hardware BOOL constant source is unavailable"}};
    const uint32_t alpha = (in.legacy.alpha_compare_vk & 15u) | ((in.legacy.alpha_precision & 15u) << 4);
    SpecWords out{};
    out[0] = alpha | (uint32_t(in.legacy.point_sprite) << 8) |
             (uint32_t(in.clip_plane_count) << 16) | (uint32_t(in.dref_scale) << 24);
    out[1] = uint32_t(in.fog_enabled) |
             (uint32_t(in.fog_enabled && !in.fog_pixel_mode ? in.fog_vertex_mode : 0) << 8) |
             (uint32_t(in.fog_enabled ? in.fog_pixel_mode : 0) << 16) |
             (uint32_t(in.fog_enabled && in.fog_use_z) << 24);
    out[2] = uint32_t(in.sampler_projection_mask) | (uint32_t(in.global_specular) << 8) |
             (uint32_t(in.legacy.point_scale) << 16) | (uint32_t(pixel.major >= 3u) << 24);
    out[3] = uint32_t(in.vs_sampler_types) | (uint32_t(in.vs_sampler_modes) << 8) | (uint32_t(vs_bools) << 16);
    out[4] = in.ps_sampler_types;
    out[5] = in.ps_sampler_modes;
    out[6] = ps_bools;
    return out;
}

UniformResult pack_uniforms(const shader::Reflection& r, const shader::Built& built, const UniformInputs& in,
                            UniformBytes storage) {
    auto refuse = [](const std::string& reason) -> UniformResult { return Unmapped{{reason}}; };
    if (!built.ok) return refuse("uniform packing requires a built shader");
    if (built.num_uniform_buffers > 4) return refuse("shader requires more than four SDL uniform slots");
    if (!r.bool_ranges.empty()) return refuse("software BOOL constant layout is unsupported");
    if (r.floats_dynamic && r.stage != shader::Stage::Vertex) return refuse("dynamic pixel float constants are unsupported");
    const auto& facts = r.constant_facts;
    if (facts.float_min_count > facts.float_max_count) return refuse("float constant bounds are invalid");
    if (!r.floats_dynamic && facts.float_min_count != facts.float_max_count)
        return refuse("static float constant bounds are not fixed");
    bool ranges_valid = true;
    const uint32_t int_count = range_end(r.int_ranges, ranges_valid);
    const uint32_t float_extent = range_end(r.float_ranges, ranges_valid);
    if (!ranges_valid || float_extent > facts.float_max_count) return refuse("constant ranges exceed exported bounds");
    const uint32_t float_count = r.floats_dynamic
        ? std::clamp(in.constants.changed_float_count, facts.float_min_count, facts.float_max_count)
        : facts.float_min_count;
    if (r.floats_dynamic && uint64_t(float_count + 1ull) * 16 > shader::kSdlUniformWindow)
        return refuse("dynamic float count plus zero sentinel exceeds SDL's 4 KiB uniform window");
    const uint64_t static_bytes64 = uint64_t(r.floats_dynamic ? 0 : float_count) * 16 + uint64_t(int_count) * 16;
    if (static_bytes64 > shader::kSdlUniformWindow) return refuse("static constants exceed SDL's 4 KiB uniform window");
    const size_t static_bytes = size_t(static_bytes64);
    PackedUniforms out;
    out.uniforms = std::move(storage);
    for (auto& slot : out.uniforms.slots) slot.clear();
    out.dynamic_float_count = r.floats_dynamic && float_count
        ? uint16_t(float_count) : in.previous_dynamic_float_count;
    out.uniforms.count = built.num_uniform_buffers;
    // SDL declares all slots through the highest used slot. Supply initialized hole bytes too.
    // Each slot's storage is allocated once, at the larger of the hole and its declared size.
    std::array<size_t, 4> capacity{16, 16, 16, 16};
    for (const auto& use : built.uniforms)
        if (use.slot < 4 && use.bytes <= shader::kSdlUniformWindow) capacity[use.slot] = std::max<size_t>(capacity[use.slot], use.bytes);
    for (uint32_t i = 0; i < out.uniforms.count; ++i) {
        out.uniforms.slots[i].reserve(capacity[i]);
        out.uniforms.slots[i].assign(16, 0);
    }
    std::array<bool, 4> seen{};
    for (const auto& use : built.uniforms) {
        if (use.slot >= out.uniforms.count || use.slot >= 4 || seen[use.slot]) return refuse("uniform slot metadata is invalid");
        if (!use.bytes || use.bytes > shader::kSdlUniformWindow) return refuse("declared uniform bytes exceed SDL's 4 KiB window");
        seen[use.slot] = true;
        auto& bytes = out.uniforms.slots[use.slot];
        bytes.assign(use.bytes, 0);
        const bool vs = r.stage == shader::Stage::Vertex;
        if (use.kind == "static" || use.kind == "dynamic") {
            const bool dynamic = use.kind == "dynamic";
            if (use.slot != (dynamic ? shader::kVsDynamicConstants : 0u) || (dynamic && (!vs || !r.floats_dynamic)))
                return refuse("constant uniform occupies the wrong slot");
            const size_t needed = dynamic ? size_t(float_count + 1u) * 16 : static_bytes;
            if (needed > bytes.size()) return refuse("constant data exceeds its declared uniform size");
            if (dynamic || !r.floats_dynamic) {
                for (const auto& range : r.float_ranges) {
                    if (range.dynamic != r.floats_dynamic) return refuse("float range indexing disagrees with reflection");
                    const auto count = std::min(range.count, float_count - std::min(range.dst, float_count));
                    if (!count) continue;
                    const auto source = range.source();
                    if (auto api = std::get_if<shader::ApiConstantIndex>(&source)) {
                        if (!in.constants.floats || api->value > in.constants.float_count || count > in.constants.float_count - api->value)
                            return refuse("API float constant source range is unavailable");
                        std::memcpy(bytes.data() + size_t(range.dst) * 16, in.constants.floats + api->value, size_t(count) * 16);
                    } else {
                        const auto index = std::get<shader::DefinedFloatIndex>(source).value;
                        if (index > facts.defined_float_bits.size() || count > facts.defined_float_bits.size() - index)
                            return refuse("shader-defined float constant source range is unavailable");
                        std::memcpy(bytes.data() + size_t(range.dst) * 16, facts.defined_float_bits.data() + index, size_t(count) * 16);
                    }
                }
            }
            if (!dynamic) {
                const size_t int_offset = size_t(r.floats_dynamic ? 0 : float_count) * 16;
                for (const auto& range : r.int_ranges) {
                    if (range.defined || range.dynamic) return refuse("integer constant range is not API static data");
                    if (!in.constants.ints || range.src > in.constants.int_count || range.count > in.constants.int_count - range.src)
                        return refuse("API integer constant source range is unavailable");
                    std::memcpy(bytes.data() + int_offset + size_t(range.dst) * 16,
                                in.constants.ints + range.src, size_t(range.count) * 16);
                }
            }
        } else if (use.kind == "state") {
            if (use.slot != (vs ? shader::kVsState : shader::kPsState) ||
                use.bytes != (vs ? shader::kStateBytesVs : shader::kStateBytesPs))
                return refuse("state uniform does not match its scalar ABI");
            for (uint32_t i = 0; i < shader::kSpecDwords; ++i) u32(bytes, 4 * i, in.spec[i]);
            bytes[28] = in.shared.fog_colour[0]; bytes[29] = in.shared.fog_colour[1];
            bytes[30] = in.shared.fog_colour[2]; bytes[31] = in.shared.alpha_reference;
            u32(bytes, 32, bits(in.shared.fog_distance_scale));
            u32(bytes, 36, bits(in.shared.fog_distance_end)); u32(bytes, 40, bits(in.shared.fog_density));
            if (vs) {
                u16(bytes, 44, out.dynamic_float_count); u16(bytes, 46, in.legacy.point_size);
                u16(bytes, 48, in.legacy.point_min); u16(bytes, 50, in.legacy.point_max);
            }
        } else if (use.kind == "clip_planes") {
            if (!vs || use.slot != shader::kVsClipPlanes || use.bytes != in.clip_planes.bytes.size())
                return refuse("clip-plane uniform does not match its ABI");
            std::copy(in.clip_planes.bytes.begin(), in.clip_planes.bytes.end(), bytes.begin());
        } else if (use.kind == "texture_stages") {
            if (vs || use.slot != shader::kPsTextureStages || use.bytes != 8 * 32)
                return refuse("texture-stage uniform does not match its ABI");
            for (uint32_t i = 0; i < 8; ++i) {
                const auto& stage = in.texture_stages[i]; const auto offset = size_t(i) * 32;
                u32(bytes, offset, stage.constant);
                // DXVK PrepareDraw transposes the 2x2 bump matrix before uploading it.
                u32(bytes, offset + 8, stage.bump00); u32(bytes, offset + 12, stage.bump10);
                u32(bytes, offset + 16, stage.bump01); u32(bytes, offset + 20, stage.bump11);
                u32(bytes, offset + 24, stage.luminance_scale); u32(bytes, offset + 28, stage.luminance_offset);
            }
        } else if (use.kind == "border_colours") {
            if (vs || use.slot != shader::kPsBorderColours || use.bytes != shader::kBorderColourBytes)
                return refuse("border-colour uniform does not match its ABI");
            for (uint32_t i = 0; i < shader::kBorderColourRegisters; ++i)
                for (uint32_t component = 0; component < 4; ++component)
                    u32(bytes, size_t(i) * 16 + component * 4, bits(in.border_colours[i][component]));
        } else return refuse("unknown uniform kind: " + use.kind);
    }
    return out;
}

} // namespace d9g::draw
