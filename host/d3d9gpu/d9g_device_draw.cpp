// Resolve current D3D state and consume each draw before any later resource operation.
#include "d9g.h"
#include "tweaks/tex-filter.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <type_traits>

namespace d9g {
namespace {
float as_float(uint32_t bits) { float f; std::memcpy(&f, &bits, sizeof f); return f; }
std::array<float, 4> colour(uint32_t bits) {
    return {float((bits >> 16) & 255) / 255, float((bits >> 8) & 255) / 255,
            float(bits & 255) / 255, float(bits >> 24) / 255};
}
// DXVK's sampler options at their defaults (d9g_state.h), or, with the host tweak tex-filter on,
// its anisotropy and mip bias as d3d9.samplerAnisotropy and d3d9.samplerLodBias
// (host/player/tweaks/tex-filter.h). The tweak's bit is set once, before any device is made (the
// player's main), so mapped_sampler's memo holds descriptions of one setting.
SamplerOptions sampler_options() {
    SamplerOptions o;
    if (sa2tw::on(sa2tw::SA2_TWEAK_TEX_FILTER)) {
        o.sampler_anisotropy = sa2tw::tex_filter::kAnisotropy;
        o.sampler_lod_bias = sa2tw::tex_filter::kLodBias;
    }
    return o;
}
bool reads_stream_zero(const PipelineDesc& p) {
    for (uint32_t i = 0; i < p.n_attrs; i++) if (p.attrs[i].buffer_slot == 0) return true;
    return false;
}
uint32_t attribute_bytes(SDL_GPUVertexElementFormat f) {
    switch (f) {
    case SDL_GPU_VERTEXELEMENTFORMAT_FLOAT: case SDL_GPU_VERTEXELEMENTFORMAT_INT:
    case SDL_GPU_VERTEXELEMENTFORMAT_UINT: return 4;
    case SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2: case SDL_GPU_VERTEXELEMENTFORMAT_INT2:
    case SDL_GPU_VERTEXELEMENTFORMAT_UINT2: return 8;
    case SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3: case SDL_GPU_VERTEXELEMENTFORMAT_INT3:
    case SDL_GPU_VERTEXELEMENTFORMAT_UINT3: return 12;
    case SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4: case SDL_GPU_VERTEXELEMENTFORMAT_INT4:
    case SDL_GPU_VERTEXELEMENTFORMAT_UINT4: return 16;
    case SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4: case SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM:
    case SDL_GPU_VERTEXELEMENTFORMAT_SHORT2: case SDL_GPU_VERTEXELEMENTFORMAT_SHORT2_NORM:
    case SDL_GPU_VERTEXELEMENTFORMAT_USHORT2_NORM: case SDL_GPU_VERTEXELEMENTFORMAT_HALF2: return 4;
    case SDL_GPU_VERTEXELEMENTFORMAT_SHORT4: case SDL_GPU_VERTEXELEMENTFORMAT_SHORT4_NORM:
    case SDL_GPU_VERTEXELEMENTFORMAT_USHORT4_NORM: case SDL_GPU_VERTEXELEMENTFORMAT_HALF4: return 8;
    default: return 0;
    }
}
const draw::VertexSource* source(const draw::Geometry& geometry) {
    if (auto p = std::get_if<draw::NonIndexed>(&geometry)) return &p->vertices;
    if (auto p = std::get_if<draw::BufferIndexed>(&geometry)) return &p->vertices;
    return nullptr;
}
static_assert(sizeof(DeviceParams) == 12, "a new DeviceParams field must join same_params");
bool same_params(const DeviceParams& a, const DeviceParams& b) {
    return a.d24s8_supported == b.d24s8_supported && a.point_size_max == b.point_size_max &&
           a.sampler_clamp_to_border == b.sampler_clamp_to_border && a.depth_clip_and_clamp == b.depth_clip_and_clamp;
}
constexpr size_t kMappedPipelines = 256, kMappedSamplers = 256;   // powers of two
}

const Device::MappedPipeline& Device::mapped_pipeline(const PipelineInput& in) {
    static_assert(std::is_trivially_copyable<PipelineInput>::value, "a pipeline input is compared by its bytes");
    if (mapped_pipelines_.empty()) mapped_pipelines_.resize(kMappedPipelines);
    const DeviceParams dp = device_params();
    const uint32_t faults = report().fault.map_bits;
    MappedPipeline& m = mapped_pipelines_[memo_hash(&in, sizeof in) & (kMappedPipelines - 1)];
    if (!m.valid || m.faults != faults || !same_params(m.dp, dp) || !same_bytes(&m.in, &in, sizeof in)) {
        m.valid = false;
        m.desc = map_pipeline(in, dp, faults);
        m.legacy = map_legacy(in, faults);
        std::memcpy(static_cast<void*>(&m.in), &in, sizeof in);
        m.dp = dp;
        m.faults = faults;
        m.valid = true;
    }
    return m;
}

const Device::MappedPipeline& Device::draw_pipeline(uint32_t method, uint32_t prim, uint32_t stride0) {
    // prep_pipeline_ is the table entry the last call returned: only this function calls
    // mapped_pipeline, so the entry has held that input since.
    const bool recomputed = prep_dirty_ || !prep_pipeline_ || method != prep_method_ || prim != prep_prim_ ||
                            stride0 != prep_stride_ || report().fault.map_bits != prep_pipeline_->faults ||
                            !same_params(device_params(), prep_pipeline_->dp);
    if (recomputed) {
        prep_pipeline_ = &mapped_pipeline(pipeline_input(method, prim, stride0));
        prep_dirty_ = false;
        prep_method_ = method;
        prep_prim_ = prim;
        prep_stride_ = stride0;
    }
#ifdef D9G_PREP_CHECK
    const PipelineInput in = pipeline_input(method, prim, stride0);
    if (!same_bytes(&in, &prep_pipeline_->in, sizeof in)) {
        std::fprintf(stderr, "D9G_PREP_CHECK: a reused pipeline input differs at draw %llu of frame %llu\n",
                     (unsigned long long)report().frame_draws, (unsigned long long)(report().presents + 1));
        std::abort();
    }
    static uint64_t checked = 0, reused = 0;
    reused += !recomputed;
    if (++checked % 1000000 == 0)
        std::fprintf(stderr, "D9G_PREP_CHECK: %llu draws, %llu reused a pipeline input equal to their own\n",
                     (unsigned long long)checked, (unsigned long long)reused);
#endif
    return *prep_pipeline_;
}

const SamplerDesc& Device::mapped_sampler(const SamplerInput& in) {
    static_assert(std::is_trivially_copyable<SamplerInput>::value, "a sampler input is compared by its bytes");
    if (mapped_samplers_.empty()) mapped_samplers_.resize(kMappedSamplers);
    const uint32_t faults = report().fault.map_bits;
    MappedSampler& m = mapped_samplers_[memo_hash(&in, sizeof in) & (kMappedSamplers - 1)];
    if (!m.valid || m.faults != faults || !same_params(m.dp, dp_) || !same_bytes(&m.in, &in, sizeof in)) {
        m.valid = false;
        m.desc = map_sampler(in, sampler_options(), dp_, faults);
        std::memcpy(static_cast<void*>(&m.in), &in, sizeof in);
        m.dp = dp_;
        m.faults = faults;
        m.valid = true;
    }
    return m.desc;
}

void Device::backend_failure(const std::string& operation, const std::string& reason) {
    Report& r = report();
    const std::string text = operation + ": " + reason;
    if (r.failure.empty()) r.failure = text;
    if (r.draw_failures.size() < 40 &&
        std::find(r.draw_failures.begin(), r.draw_failures.end(), text) == r.draw_failures.end())
        r.draw_failures.push_back(text);
}

draw::VertexSource Device::bound_vertices() const {
    const auto& s = streams_[0];
    if (!s.vb) return draw::NoRealVertices{};
    const uint32_t available = s.offset <= s.vb->length() ? s.vb->length() - s.offset : 0;
    return draw::BoundVertices{{{s.vb->gpu_id()}, s.offset, available}};
}

draw::Prepared Device::prepare_draw(uint32_t method, uint32_t prim, uint32_t stride, draw::Geometry geometry) {
    using namespace draw;
    // UpdateFog retains prior push bytes while disabled, including calls whose geometry is unmapped.
    shared_push_ = update_fog_push(shared_push_, rs_[D3DRS_FOGENABLE] != 0, rs_[D3DRS_FOGCOLOR],
        as_float(rs_[D3DRS_FOGSTART]), as_float(rs_[D3DRS_FOGEND]), as_float(rs_[D3DRS_FOGDENSITY]));
    if (!vs_ || !ps_) return Unmapped{{"FIXED_FUNCTION_SHADER"}};
    if (can_swvp()) return Unmapped{{"SOFTWARE_VERTEX_PROCESSING_LAYOUT"}};
    Values v;
    // the last draw's storage, emptied (spare_values_)
    auto reuse = [](auto& to, auto& from) { to.swap(from); to.clear(); };
    reuse(v.attachments.colours, spare_values_.attachments.colours);
    reuse(v.vertex.samplers, spare_values_.vertex.samplers);
    reuse(v.pixel.samplers, spare_values_.pixel.samplers);
    const MappedPipeline& mapped = draw_pipeline(method, prim, stride);
    v.pipeline = mapped.desc;
    const LegacyData legacy = mapped.legacy;
    std::vector<std::string> reasons = v.pipeline.unmapped;
    // Without depth clip and clamp the pipeline owes the clamp (R_DEPTH_CLAMP): a live session (or
    // SA2_D3D9GPU_NO_DEPTH_CLAMP=1) draws it clipped at the near and far planes, D3D9's own clip,
    // without the clamp DXVK adds; anywhere else it is refused by name. A pipeline with depth clip
    // off (the map-depth-clip-off fault) needs depthClamp itself, which such a device may have been
    // made without, so it is refused there too.
    if ((v.pipeline.residues & R_DEPTH_CLAMP) && (!depth_clamp_clip_only_ || !v.pipeline.rs.enable_depth_clip))
        reasons.push_back("DEPTH_CLAMP_FALLBACK_OWED");
    if (v.pipeline.rt_feedback) reasons.push_back("RT_FEEDBACK");
    if (!v.pipeline.n_ct && !v.pipeline.has_ds) reasons.push_back("NO_FRAMEBUFFER_ATTACHMENT");
    if (!reads_stream_zero(v.pipeline)) {
        // An unread stream cannot constrain a shader that fetches no real inputs.
        if (auto* dp = std::get_if<NonIndexed>(&geometry)) {
            if (std::holds_alternative<BoundVertices>(dp->vertices)) dp->vertices = NoRealVertices{};
        } else if (auto* dip = std::get_if<BufferIndexed>(&geometry)) {
            if (std::holds_alternative<BoundVertices>(dip->vertices)) dip->vertices = NoRealVertices{};
        }
    }
    const VertexSource* vertices = source(geometry);
    if (vertices && std::holds_alternative<NoRealVertices>(*vertices) && reads_stream_zero(v.pipeline))
        reasons.push_back("DECLARED_UNBOUND_STREAM_ZERO");
    if (const auto* indexed = std::get_if<BufferIndexed>(&geometry)) {
        if (!indexed->indices.buffer.value) reasons.push_back("UNBOUND_INDEX_BUFFER");
        const uint32_t width = indexed->width == IndexWidth::U16 ? 2 : 4;
        if ((uint64_t(indexed->first_index) + indexed->count) * width > indexed->indices.available_bytes)
            reasons.push_back("ROBUST_INDEX_BUFFER_OOB");
    }
    // The vertex indices the draw fetches, [low, high] (base vertex applied): what every per-vertex
    // stream is held to. Computed once, at the first stream that needs it.
    bool range_known = false;
    int64_t range_low = 0, range_high = 0;
    auto vertex_range = [&](int64_t* lo, int64_t* hi) {
        if (!range_known) {
            range_known = true;
            int64_t low = 0, high = 0;
            if (const auto* dp = std::get_if<NonIndexed>(&geometry)) {
                low = dp->first_vertex;
                high = low + dp->count - 1;
            } else if (const auto* dip = std::get_if<BufferIndexed>(&geometry)) {
                if (v.pipeline.fan_as_list) {
                    low = dip->base_vertex;
                    high = low + dip->count / 3 + 1;
                } else if (indices_ && dip->indices.buffer.value == indices_->gpu_id()) {
                    if (indices_->locked()) reasons.push_back("LOCKED_INDEX_BUFFER");
                    const uint32_t width = dip->width == IndexWidth::U16 ? 2 : 4;
                    const auto& data = indices_->source_bytes();
                    const uint64_t end = (uint64_t(dip->first_index) + dip->count) * width;
                    if (end <= data.size() && dip->count) {
                        uint32_t min_index = UINT32_MAX, max_index = 0;
                        for (uint32_t i = 0; i < dip->count; i++) {
                            uint32_t index = 0;
                            std::memcpy(&index, data.data() + (uint64_t(dip->first_index) + i) * width, width);
                            min_index = std::min(min_index, index); max_index = std::max(max_index, index);
                        }
                        low = int64_t(min_index) + dip->base_vertex;
                        high = int64_t(max_index) + dip->base_vertex;
                    }
                }
            }
            range_low = low;
            range_high = high;
        }
        *lo = range_low;
        *hi = range_high;
    };
    auto footprint_of = [&](uint32_t slot) {
        uint32_t footprint = 0;
        for (uint32_t i = 0; i < v.pipeline.n_attrs; i++) {
            const auto& a = v.pipeline.attrs[i];
            if (a.buffer_slot == slot) footprint = std::max(footprint, a.offset + attribute_bytes(a.format));
        }
        return footprint;
    };
    // Core robustness makes fetches legal, but does not promise DXVK robustness2's exact
    // zero values. Keep those unproved reads named until completed-pixel controls establish them.
    if (vertices && reads_stream_zero(v.pipeline)) {
        if (const auto* bound = std::get_if<BoundVertices>(vertices)) {
            const uint32_t footprint = footprint_of(0);
            int64_t low = 0, high = 0;
            vertex_range(&low, &high);
            if (low < 0 || high < 0 || uint64_t(high) * stride + footprint > bound->stream_zero.available_bytes)
                reasons.push_back("ROBUST_VERTEX_BUFFER_OOB");
        }
    }
    // DXVK's instance count: stream 0's (Setting & 0x7fffff, at least 1) where a stream is instanced
    // (GetInstanceCount :7359-7362) and the pipeline reads an instanced stream (DrawIndexedPrimitive's
    // CS check :3114-3126, GenerateDrawInfo :7347-7356 for the indexed UP draw); 1 where none is, and
    // for every non-indexed draw (:3049, :3170).
    uint32_t instanced_read = 0;
    for (uint32_t i = 0; i < v.pipeline.n_vbs; i++) {
        const auto& b = v.pipeline.vbs[i];
        if (b.slot != 0 && b.slot != kNullStreamSlot && b.input_rate == SDL_GPU_VERTEXINPUTRATE_INSTANCE)
            instanced_read |= 1u << b.slot;
    }
    const bool indexed_method = method == M_DIP || method == M_DIPUP;
    const uint32_t instances = indexed_method && instanced_read ? std::max(streams_[0].freq & 0x7FFFFFu, 1u) : 1u;
    // Streams 1-14 the pipeline reads (map_pipeline): each one's vertex buffer at its SetStreamSource
    // offset, held to its fetch interval as stream 0 is: the draw's vertex range for a per-vertex
    // stream, its instances for a per-instance one. A UP draw's data is stream 0's alone: a UP draw
    // reading another stream is refused by name.
    std::vector<StreamSlice> other_streams;
    for (uint32_t i = 0; i < v.pipeline.n_vbs; i++) {
        const uint32_t st = v.pipeline.vbs[i].slot;
        if (st == 0 || st == kNullStreamSlot) continue;
        if (method == M_DPUP || method == M_DIPUP) { reasons.push_back("UP_DRAW_READS_STREAM " + std::to_string(st)); continue; }
        const Stream& bound = streams_[st];
        if (!bound.vb) { reasons.push_back("DECLARED_UNBOUND_STREAM " + std::to_string(st)); continue; }
        const uint32_t available = bound.offset <= bound.vb->length() ? bound.vb->length() - bound.offset : 0;
        const uint32_t footprint = footprint_of(st), pitch = v.pipeline.vbs[i].pitch;
        int64_t low = 0, high = int64_t(instances) - 1;
        if (v.pipeline.vbs[i].input_rate != SDL_GPU_VERTEXINPUTRATE_INSTANCE) vertex_range(&low, &high);
        if (low < 0 || high < 0 || uint64_t(high) * pitch + footprint > available)
            reasons.push_back("ROBUST_VERTEX_BUFFER_OOB stream " + std::to_string(st));
        other_streams.push_back(StreamSlice{st, BufferSlice{{bound.vb->gpu_id()}, bound.offset, available}});
    }
    if (!reasons.empty()) return Unmapped{std::move(reasons)};

    const auto& vs_base = vs_->translated().reflection;
    const auto& ps_base = ps_->translated().reflection;
    shader::SamplerSlot modes[16]{};
    std::array<SamplerDesc, 21> sampler_desc;
    std::array<std::array<float, 4>, 16> border_colours{};
    uint32_t ps_types = 0, ps_modes = 0;
    uint8_t vs_types = 0, vs_modes = 0;
    SelectedVariants variants;
    variants.vertex.bgra_mask = v.pipeline.bgra_mask;
    variants.flat_shading = rs_[D3DRS_SHADEMODE] == D3DSHADE_FLAT;
    variants.target_swizzle_mask = 0; // map_pipeline refuses nonidentity target swizzles.
    auto slot_facts = [&](uint32_t stage) {
        shader::SamplerSlot slot;
        slot.bound = tex_[stage] != nullptr;
        if (slot.bound) {
            slot.d3dfmt = tex_[stage]->image()->d3dfmt;
            slot.upgraded_to_d32f = tex_[stage]->image()->fi->gpu == GFmt::d24s8 && gpu_->depth_format() == GFmt::d32s8;
        }
        slot.fetch4_enabled = stage < 16 && fetch4_[stage];
        slot.mag_filter = ss_[stage][D3DSAMP_MAGFILTER];
        return slot;
    };
    for (uint32_t st = 0; st < 21; st++) {
        const uint32_t r = st < 16 ? st : st >= 17 ? st - 17 : 0;
        const bool used = st < 16 ? ((ps_base.sampler_mask >> r) & 1u) :
            st >= 17 ? ((vs_base.sampler_mask >> r) & 1u) : false;
        shader::SamplerSlot slot = slot_facts(st);
        const uint32_t mode = used ? shader::dxvk_sampler_mode(slot) : 0;
        const uint32_t type = used && slot.bound ? 0 : 3; // This device creates 2D textures only.
        if (st < 16) {
            modes[st] = slot;
            ps_types |= type << (2 * r);
            ps_modes |= mode << (2 * r);
        } else if (st >= 17) {
            vs_types |= uint8_t(type << (2 * r));
            vs_modes |= uint8_t(mode << (2 * r));
        }
        if (!used) continue;
        // a cube or volume texture where the shader samples: the translated samplers here are 2D (no
        // game shader samples another kind), so the draw is refused by name
        if (const ObjectBase* other = tex_other_[st]) {
            reasons.push_back(std::string(other->texture_image()->type == TexType::cube ? "CUBE_TEXTURE_SAMPLE" : "VOLUME_TEXTURE_SAMPLE") +
                              " stage " + std::to_string(st));
            continue;
        }
        SamplerInput si = sampler_input(st);
        si.depth = mode >= 2; // DXVK's shadow classification, not generic depth format membership.
        sampler_desc[st] = mapped_sampler(si);
        reasons.insert(reasons.end(), sampler_desc[st].unmapped.begin(), sampler_desc[st].unmapped.end());
        if (tex_[st] && tex_[st]->GetLOD()) reasons.push_back("TEXTURE_LOD_VIEW stage " + std::to_string(st));
        if (!sampler_desc[st].fallback()) continue;
        if (st >= 16) { reasons.push_back("VERTEX_BORDER_FALLBACK"); continue; }
        if (mode != 0 || si.cube) { reasons.push_back("BORDER_UNWRAPPED_SAMPLE stage " + std::to_string(st)); continue; }
        auto& desc = sampler_desc[st];
        if (desc.axes[0] == AddrDecision::fallback_border) variants.pixel.border_u |= uint16_t(1u << r);
        if (desc.axes[1] == AddrDecision::fallback_border) variants.pixel.border_v |= uint16_t(1u << r);
        if (desc.fields.min_filter == SDL_GPU_FILTER_LINEAR || desc.fields.mag_filter == SDL_GPU_FILTER_LINEAR)
            variants.pixel.border_linear |= uint16_t(1u << r);
        border_colours[r] = colour(ss_[st][D3DSAMP_BORDERCOLOR]);
        if (const Texture* t = tex_[st]) {
            // The supported colour swizzle is X8's forced alpha; border uses the inverse view.
            if (t->image()->fi->x8) border_colours[r][3] = 1;
            // A8's (0, 0, 0, R) view: the inverse puts the colour's alpha in R, the view returns it as alpha.
            if (t->image()->fi->expand == Expand::a8) border_colours[r][0] = border_colours[r][1] = border_colours[r][2] = 0;
        }
        for (uint32_t axis = 0; axis < 3; axis++)
            if (desc.axes[axis] == AddrDecision::fallback_border) si.ss[D3DSAMP_ADDRESSU + axis] = D3DTADDRESS_CLAMP;
        desc = mapped_sampler(si);
        reasons.insert(reasons.end(), desc.unmapped.begin(), desc.unmapped.end());
    }
    if (!reasons.empty()) return Unmapped{std::move(reasons)};
    variants.pixel.sampler_modes = shader::sampler_modes_word(modes, ps_base.sampler_mode_slots);
    auto program = programs_.for_draw({vs_->tokens(), vs_->translated(), vs_->program_source_id()},
                                     {ps_->tokens(), ps_->translated(), ps_->program_source_id()},
                                     shader_profile_, variants);
    if (const auto* u = std::get_if<Unmapped>(&program)) return *u;
    if (const auto* f = std::get_if<Failure>(&program)) return *f;
    v.program = std::get<Program>(std::move(program));
    // A device made at Vulkan 1.1 takes SPIR-V 1.3 at most. The vk11 profile lowers every module to it
    // (d9g_shader_abi.cpp lower_vk11), so a module above it means a profile that disagrees with the
    // device: refused by name, never handed to the driver.
    if (vk11_)
        for (const shader::Built* b : {&v.program->vertex, &v.program->pixel})
            if (b->spirv.size() > 1 && b->spirv[1] > 0x00010300u)
                return Unmapped{{"a SPIR-V 1." + std::to_string((b->spirv[1] >> 8) & 0xffu) +
                                 " module on a device made at Vulkan 1.1"}};
    if (auto* dip = std::get_if<BufferIndexed>(&geometry); dip && indexed_method) dip->instances = instances;
    if (auto* up = std::get_if<OwnedIndexedUp>(&geometry)) up->instances = instances;
    v.geometry = std::move(geometry);
    v.streams = std::move(other_streams);
    for (uint32_t i = 0; i < v.pipeline.n_ct; i++)
        v.attachments.colours.push_back({{rt_[i]->image()->gpu}, rt_[i]->level(), rt_[i]->face()});
    if (v.pipeline.has_ds) v.attachments.depth_stencil = TextureLevel{{ds_->image()->gpu}, ds_->level(), ds_->face()};
    // SDL binds slots 0 to the highest at once: the null stream's slot, and a slot between two streams
    // the pipeline reads, take the semantic-null buffer (d9g_gpu_sdl.cpp)
    uint32_t slots = 0, highest = 0;
    for (uint32_t i = 0; i < v.pipeline.n_vbs; i++) {
        slots |= 1u << v.pipeline.vbs[i].slot;
        highest = std::max(highest, v.pipeline.vbs[i].slot);
    }
    const uint32_t span = (2u << highest) - 1u;
    if (v.pipeline.null_stream || (v.pipeline.n_vbs && (slots & span) != span)) {
        if (!semantic_null_gpu_) semantic_null_gpu_ = gpu_->create_buffer(65536, false);
        if (!semantic_null_gpu_) return Failure{"create semantic-null buffer", gpu_->error()};
        v.semantic_null = BufferSlice{{semantic_null_gpu_}, 0, 65536};
    }
    auto bind_samplers = [&](const shader::Built& built, bool pixel, StageBindings& bindings) -> bool {
        bindings.samplers.reserve(built.sampler_registers.size());
        for (uint32_t r : built.sampler_registers) {
            const uint32_t st = pixel ? r : r + 17;
            if (st >= 21) { reasons.push_back("SAMPLER_REGISTER " + std::to_string(st)); return false; }
            uint32_t id = tex_[st] ? tex_[st]->image()->gpu : 0;
            if (!tex_[st]) {
                if (!dummy_texture_gpu_) dummy_texture_gpu_ = create_gpu_texture(TexDesc{1, 1, 1, GFmt::bgra8, false, true});
                id = dummy_texture_gpu_;
            }
            if (!id) return false;
            bindings.samplers.push_back({{id}, sampler_desc[st]});
        }
        return true;
    };
    if (!bind_samplers(v.program->vertex, false, v.vertex) || !bind_samplers(v.program->pixel, true, v.pixel)) {
        if (!reasons.empty()) return Unmapped{std::move(reasons)};
        return Failure{"bind sampled texture", gpu_->error()};
    }
    ConstantBank vb{vs_f_.data(), uint32_t(vs_f_.size()), vs_i_.data(), uint32_t(vs_i_.size()),
        vs_b_.data(), uint32_t(vs_b_.size()), changed_float_count_};
    ConstantBank pb{ps_f_.data(), uint32_t(ps_f_.size()), ps_i_.data(), uint32_t(ps_i_.size()),
        ps_b_.data(), uint32_t(ps_b_.size()), 0};
    const ClipPlanes planes = pack_clip_planes(clip_, rs_[D3DRS_CLIPPLANEENABLE]);
    // On a device without shaderClipDistance no vertex module writes ClipDistance
    // (ShaderProfile::no_clip_distance), so a draw with a user clip plane enabled would draw what the
    // plane clips: it is refused by name. SA2 enables none (the streams' scans).
    if (planes.count && shader_profile_.no_clip_distance)
        return Unmapped{{"USER_CLIP_PLANES without shaderClipDistance"}};
    SpecFacts spec;
    spec.legacy = legacy;
    spec.clip_plane_count = planes.count;
    spec.fog_enabled = rs_[D3DRS_FOGENABLE] != 0;
    spec.fog_vertex_mode = uint8_t(rs_[D3DRS_FOGVERTEXMODE]);
    spec.fog_pixel_mode = uint8_t(rs_[D3DRS_FOGTABLEMODE]);
    spec.global_specular = rs_[D3DRS_SPECULARENABLE] != 0;
    for (uint32_t r = 0; r < 8; r++)
        if (tex_[r] && ((ps_base.sampler_mask >> r) & 1u) &&
            (tss_[r][D3DTSS_TEXTURETRANSFORMFLAGS - 1] & D3DTTFF_PROJECTED)) spec.sampler_projection_mask |= uint8_t(1u << r);
    spec.vs_sampler_types = vs_types; spec.vs_sampler_modes = vs_modes;
    spec.ps_sampler_types = ps_types; spec.ps_sampler_modes = ps_modes;
    const auto spec_result = pack_spec_words(spec, v.program->vertex_reflection, v.program->pixel_reflection, vb, pb);
    if (const auto* u = std::get_if<Unmapped>(&spec_result)) return *u;
    UniformInputs uniforms;
    uniforms.spec = std::get<SpecWords>(spec_result);
    uniforms.shared = shared_push_;
    uniforms.shared.alpha_reference = spec.legacy.alpha_ref;
    uniforms.legacy = spec.legacy;
    uniforms.clip_planes = planes;
    uniforms.border_colours = border_colours;
    uniforms.previous_dynamic_float_count = dynamic_float_count_;
    for (uint32_t i = 0; i < 8; i++) uniforms.texture_stages[i] = {
        tss_[i][D3DTSS_CONSTANT - 1], tss_[i][D3DTSS_BUMPENVMAT00 - 1], tss_[i][D3DTSS_BUMPENVMAT01 - 1],
        tss_[i][D3DTSS_BUMPENVMAT10 - 1], tss_[i][D3DTSS_BUMPENVMAT11 - 1],
        tss_[i][D3DTSS_BUMPENVLSCALE - 1], tss_[i][D3DTSS_BUMPENVLOFFSET - 1]};
    uniforms.constants = vb;
    auto packed_vs = pack_uniforms(v.program->vertex_reflection, v.program->vertex, uniforms,
                                   std::move(spare_values_.vertex.uniforms));
    if (const auto* u = std::get_if<Unmapped>(&packed_vs)) return *u;
    auto& vs_uniforms = std::get<PackedUniforms>(packed_vs);
    dynamic_float_count_ = vs_uniforms.dynamic_float_count;
    v.vertex.uniforms = std::move(vs_uniforms.uniforms);
    uniforms.constants = pb;
    auto packed_ps = pack_uniforms(v.program->pixel_reflection, v.program->pixel, uniforms,
                                   std::move(spare_values_.pixel.uniforms));
    if (const auto* u = std::get_if<Unmapped>(&packed_ps)) return *u;
    v.pixel.uniforms = std::get<PackedUniforms>(std::move(packed_ps)).uniforms;
    const float minz = std::clamp(vp_.MinZ, 0.0f, 1.0f);
    const float bias = vp_.MinZ >= 0.5f ? 0 : 0.001f;
    auto& d = v.dynamic;
    d.x = float(vp_.X) + 0.5f; d.y = float(vp_.Y) + 0.5f;
    d.width = float(vp_.Width); d.height = float(vp_.Height);
    d.min_depth = minz; d.max_depth = std::clamp(std::max(vp_.MaxZ, vp_.MinZ + bias), 0.0f, 1.0f);
    const int32_t vx = int32_t(vp_.X), vy = int32_t(vp_.Y);
    int32_t x = vx, y = vy, right = int32_t(vp_.X + vp_.Width), bottom = int32_t(vp_.Y + vp_.Height);
    if (rs_[D3DRS_SCISSORTESTENABLE]) {
        x = std::max(vx, std::max(0, int32_t(sc_.left)));
        y = std::max(vy, std::max(0, int32_t(sc_.top)));
        right = std::min(right, std::max(x, int32_t(sc_.right)));
        bottom = std::min(bottom, std::max(y, int32_t(sc_.bottom)));
    }
    d.scissor_x = x; d.scissor_y = y;
    d.scissor_width = uint32_t(right - x); d.scissor_height = uint32_t(bottom - y);
    d.blend_constant = colour(rs_[D3DRS_BLENDFACTOR]);
    d.stencil_reference = uint8_t(rs_[D3DRS_STENCILREF]);
    return DrawRequest(std::move(v));
}

void Device::execute_draw(const char* name, uint32_t method, uint32_t prim, uint32_t stride, draw::Geometry geometry) {
    DrawClass cls = DrawClass::BackendFailed;
    try {
        auto prepared = prepare_draw(method, prim, stride, std::move(geometry));
        if (const auto* u = std::get_if<draw::Unmapped>(&prepared)) {
            cls = DrawClass::Unmapped;
            for (const auto& reason : u->reasons) {
                auto& r = report();
                if (r.draw_failures.size() < 40 &&
                    std::find(r.draw_failures.begin(), r.draw_failures.end(), reason) == r.draw_failures.end())
                    r.draw_failures.push_back(reason);
            }
        } else if (const auto* f = std::get_if<draw::Failure>(&prepared)) {
            backend_failure(f->operation, f->reason);
        } else if (report().fault.bits & F_GEOMETRY_NOOP) {
            cls = DrawClass::Suppressed;
        } else {
            auto& request = std::get<draw::DrawRequest>(prepared);
            const bool clip_only = (request.values().pipeline.residues & R_DEPTH_CLAMP) != 0;
            auto outcome = gpu_->draw(request);
            spare_values_ = std::move(request.values_);   // the Gpu retains no request
            if (const auto* encoded = std::get_if<draw::Encoded>(&outcome)) {
                cls = DrawClass::Encoded;
                if (clip_only) report().depth_clamp_clip_only_draws++;
                if (encoded->uploaded_up_bytes) report().up_arena_uploads++;
                report().up_uploaded_bytes += encoded->uploaded_up_bytes;
            } else {
                const auto& failure = std::get<draw::Failure>(outcome);
                backend_failure(failure.operation, failure.reason);
            }
        }
    } catch (const std::exception& e) { backend_failure("draw preparation", e.what()); }
      catch (...) { backend_failure("draw preparation", "unknown exception"); }
    count_draw(name, prim, cls);
    // The census (the keys file and the report's pipeline counts) is for replays and the state
    // check. A live session leaves it out (its keys file lists no inputs): it re-derives each changed
    // draw's pipeline input and interns string keys, 0.85 ms of a frame's render thread on a
    // Snapdragon 750G in City Escape (player_prof, inclusive).
    if (!gpu_ || !gpu_->info().live_session) {
        try { note_draw_state(method, prim, stride); }
        catch (const std::exception& e) { backend_failure("draw state census", e.what()); }
        catch (...) { backend_failure("draw state census", "unknown exception"); }
    }
}
}
