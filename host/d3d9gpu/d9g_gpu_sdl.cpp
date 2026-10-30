// SDL_gpu backend: resource IDs are resolved and geometry is encoded during draw().
// Copy and render passes are mutually exclusive and operations keep their call order.
// Readback submits and waits, including every preceding draw before Present.
// Transfer storage cycles, and so does each UP chunk at its first write in a command buffer
// (upload_up); ordinary resources never cycle.
// A whole clear waits (PendingClear) until a draw pass takes it as an attachment's load op or
// anything else that touches its subresource encodes it first, so the call order still holds.
#include "d9g.h"
#include "d9g_interp_spirv.h"
#include "d9g_props.h"
#include "tweaks/tex-filter.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <string_view>

// The Khronos headers of the pinned SDL tarball (test/extract_refs.py writes them beside the
// build's generated sources), for the one property query below; no Vulkan function is linked.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <cstring>
#include <unordered_map>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace d9g {

namespace {

constexpr uint32_t kRing = 64u << 20;

SDL_GPUTextureFormat sdl_format(GFmt f) {
    switch (f) {
    case GFmt::bgra8: return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    case GFmt::bc1: return SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM;
    case GFmt::bc2: return SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM;
    case GFmt::bc3: return SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM;
    case GFmt::d24s8: return SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
    case GFmt::d32s8: return SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
    default: return SDL_GPU_TEXTUREFORMAT_INVALID;
    }
}
bool is_depth(GFmt f) { return f == GFmt::d24s8 || f == GFmt::d32s8; }
size_t gfmt_level_bytes(GFmt f, uint32_t w, uint32_t h) {
    const size_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    switch (f) {
    case GFmt::bc1: return bw * bh * 8;
    case GFmt::bc2: case GFmt::bc3: return bw * bh * 16;
    default: return size_t(w) * h * 4;
    }
}
// One layer's level, packed: a volume's is its level's slices.
size_t level_upload_bytes(const TexDesc& d, uint32_t level) {
    const size_t slice = gfmt_level_bytes(d.fmt, level_dim(d.w, level), level_dim(d.h, level));
    return d.type == TexType::volume ? slice * level_dim(d.depth, level) : slice;
}
std::string prop(SDL_PropertiesID p, const char* name) {
    const char* v = SDL_GetStringProperty(p, name, nullptr);
    return v ? v : "";
}

// The project's SDL patch (patches/sdl3): its two device properties, by name, so a stock SDL's
// headers, which do not define them, build the same detection. The revision mark the patched
// build carries (tools/s5/build_host_deps.sh's SDL_VENDOR_INFO).
constexpr const char kPropBorder[] = "SDL.gpu.device.sa2recomp.sampler_clamp_to_border";
constexpr const char kPropClipAndClamp[] = "SDL.gpu.device.sa2recomp.depth_clip_and_clamp";
constexpr const char kPatchedMark[] = " (sa2-recomp sdl3-patches ";
#ifdef SDL_PROP_GPU_DEVICE_SA2RECOMP_SAMPLER_CLAMP_TO_BORDER_BOOLEAN
static_assert(std::string_view(SDL_PROP_GPU_DEVICE_SA2RECOMP_SAMPLER_CLAMP_TO_BORDER_BOOLEAN) == kPropBorder);
static_assert(std::string_view(SDL_PROP_GPU_DEVICE_SA2RECOMP_DEPTH_CLIP_AND_CLAMP_BOOLEAN) == kPropClipAndClamp);
#endif

bool test_switch(const char* env, const char* prop, const char** which);

// SA2_D3D9GPU_FLAT_PS=1 (on Android debug.sa2.flat_ps=1), a measuring switch: every pixel shader
// is this one, mid grey to location 0, with the original's sampler and uniform counts so the draws
// bind as before. The frame time it saves is what the translated pixel shaders cost (their ALU and
// texture fetches); the picture is wrong. glslc -fshader-stage=frag --target-env=vulkan1.1 -O of
//   #version 450
//   layout(location = 0) out vec4 o0;
//   void main() { o0 = vec4(0.5, 0.5, 0.5, 1.0); }
constexpr uint32_t kFlatPixelShader[] = {
    0x07230203, 0x00010300, 0x000d000a, 0x0000000d, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0006000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x00030010, 0x00000004,
    0x00000007, 0x00040047, 0x00000009, 0x0000001e, 0x00000000, 0x00020013, 0x00000002, 0x00030021,
    0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006,
    0x00000004, 0x00040020, 0x00000008, 0x00000003, 0x00000007, 0x0004003b, 0x00000008, 0x00000009,
    0x00000003, 0x0004002b, 0x00000006, 0x0000000a, 0x3f000000, 0x0004002b, 0x00000006, 0x0000000b,
    0x3f800000, 0x0007002c, 0x00000007, 0x0000000c, 0x0000000a, 0x0000000a, 0x0000000a, 0x0000000b,
    0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0003003e,
    0x00000009, 0x0000000c, 0x000100fd, 0x00010038,
};

class SdlGpu final : public Gpu {
public:
    // `off`: a test run's capability override (d9g_entry.cpp refuses it outside one), applied
    // after the detection: the detected capabilities stay as SDL reported them.
    SdlGpu(SDL_GPUDevice* dev, bool debug, const CapsOff& off) : dev_(dev) {
        info_.driver = SDL_GetGPUDeviceDriver(dev) ? SDL_GetGPUDeviceDriver(dev) : "";
        const SDL_PropertiesID p = SDL_GetGPUDeviceProperties(dev);
        info_.device_name = prop(p, SDL_PROP_GPU_DEVICE_NAME_STRING);
        info_.driver_name = prop(p, SDL_PROP_GPU_DEVICE_DRIVER_NAME_STRING);
        info_.driver_version = prop(p, SDL_PROP_GPU_DEVICE_DRIVER_VERSION_STRING);
        info_.driver_info = prop(p, SDL_PROP_GPU_DEVICE_DRIVER_INFO_STRING);
        info_.debug_mode = debug;
        // Run-time detection of the SDL patch's capabilities (absent: false, the fallback).
        const char* revision = SDL_GetRevision();
        info_.sdl_revision = revision ? revision : "";
        info_.sdl_patched = info_.sdl_revision.find(kPatchedMark) != std::string::npos;
        info_.sdl_capabilities_present = SDL_HasProperty(p, kPropBorder) && SDL_HasProperty(p, kPropClipAndClamp);
        info_.sampler_clamp_to_border_detected = SDL_GetBooleanProperty(p, kPropBorder, false);
        info_.depth_clip_and_clamp_detected = SDL_GetBooleanProperty(p, kPropClipAndClamp, false);
        apply_caps_off(info_, off);
        // DXVK's D24S8 fallback (d3d9_format.cpp): D32_FLOAT_S8_UINT where D24S8 cannot be
        // a depth target.
        depth_ = SDL_GPUTextureSupportsFormat(dev, SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT, SDL_GPU_TEXTURETYPE_2D,
                                              SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
                     ? GFmt::d24s8 : GFmt::d32s8;
        info_.depth_format = gfmt_name(depth_);
        // The BC formats a sampled 2D texture cannot be made of here (GpuInfo::bc_unsupported).
        for (GFmt f : {GFmt::bc1, GFmt::bc2, GFmt::bc3})
            if (!SDL_GPUTextureSupportsFormat(dev, sdl_format(f), SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER))
                info_.bc_unsupported += std::string(info_.bc_unsupported.empty() ? "" : ", ") + gfmt_name(f);
        SDL_GPUTransferBufferCreateInfo ri{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, kRing, 0};
        ring_ = SDL_CreateGPUTransferBuffer(dev, &ri);
        const char* flat_prop = nullptr;
        flat_ps_ = test_switch("SA2_D3D9GPU_FLAT_PS", "debug.sa2.flat_ps", &flat_prop);
        if (flat_ps_)
            std::fprintf(stderr, "d3d9gpu: every pixel shader is a flat grey one (%s), for measuring: the picture is wrong\n",
                         flat_prop ? "debug.sa2.flat_ps=1" : "SA2_D3D9GPU_FLAT_PS=1");
        // SA2_D3D9GPU_TINY_MIPS=1 (on Android debug.sa2.tiny_mips=1), the other measuring switch:
        // with the shaders unchanged and their textures read from the smallest level, the frame
        // time it saves is the texture fetches' share of what flat_ps saves.
        const char* tiny_prop = nullptr;
        tiny_mips_ = test_switch("SA2_D3D9GPU_TINY_MIPS", "debug.sa2.tiny_mips", &tiny_prop);
        if (tiny_mips_)
            std::fprintf(stderr, "d3d9gpu: every sampler reads its texture's smallest level (%s), for measuring: the "
                         "picture is blurred\n", tiny_prop ? "debug.sa2.tiny_mips=1" : "SA2_D3D9GPU_TINY_MIPS=1");
        const char* dump_prop = nullptr;
        dump_spirv_ = test_switch("SA2_D3D9GPU_DUMP_SPIRV", "debug.sa2.dump_spirv", &dump_prop);
        // SA2_PLAYER_INTERP=1 (on Android also debug.sa2.interp=1): frame interpolation (Interp).
        const char* interp_prop = nullptr;
        interp_ = test_switch("SA2_PLAYER_INTERP", "debug.sa2.interp", &interp_prop);
        if (interp_)
            std::fprintf(stderr, "d3d9gpu: %s: a frame is made between every two the game draws and shown at its "
                                 "Present, the drawn frame half a period later\n",
                         interp_prop ? "debug.sa2.interp=1" : "SA2_PLAYER_INTERP=1");
    }

    ~SdlGpu() override {
        finish();
        interp_release();
        for (auto& [id, t] : tex_) SDL_ReleaseGPUTexture(dev_, t.t);
        for (auto& [id, b] : buf_) SDL_ReleaseGPUBuffer(dev_, b.b);
        free_up_chunks();
        for (auto& [pair, cache] : pipelines_)
            for (auto& [hash, bucket] : cache)
                for (const PipelineEntry& e : bucket) SDL_ReleaseGPUGraphicsPipeline(dev_, e.pipeline);
        for (auto& [key, sampler] : samplers_) SDL_ReleaseGPUSampler(dev_, sampler);
        for (auto& [key, object] : shaders_) SDL_ReleaseGPUShader(dev_, object->shader);
        if (ring_) SDL_ReleaseGPUTransferBuffer(dev_, ring_);
        if (down_) SDL_ReleaseGPUTransferBuffer(dev_, down_);
        if (surface_gone_) {
            // SDL_ReleaseWindowFromGPUDevice, and SDL_DestroyGPUDevice through it, would destroy the
            // window's stale VkSurfaceKHR a second time (AfterAcquire::surface_gone): 0.1.0-alpha.3.1
            // crashed there on Android, in libvulkan's DestroySurfaceKHR, when the app's icon was
            // tapped during a session in the background. The device is left to the process's end.
            std::fprintf(stderr, "d3d9gpu: the GPU device is left undestroyed: SDL would destroy the window's "
                         "stale surface a second time\n");
            return;
        }
        if (win_) SDL_ReleaseWindowFromGPUDevice(dev_, win_);
        SDL_DestroyGPUDevice(dev_);
    }

    draw::Outcome draw(const draw::DrawRequest& request) override {
        try {
            if (poisoned_) return draw::Failure{"draw command buffer", err_};
            const draw::Values& v = request.values();
            auto fail = [&](const char* operation, const std::string& reason) -> draw::Outcome {
                err_ = std::string(operation) + ": " + reason;
                return draw::Failure{operation, reason};
            };
            if (!v.program) return fail("draw inputs", "no immutable program image");
            const PipelineDesc& p = v.pipeline;
            if (!p.unmapped.empty()) return fail("draw inputs", p.unmapped.front());
            if (p.rt_feedback) return fail("draw inputs", "render-target feedback is unsupported");
            if (p.n_vbs > 4 || p.n_attrs > 16 || p.n_ct > 4)
                return fail("draw inputs", "pipeline description exceeds its fixed arrays");
            if (v.attachments.colours.size() != p.n_ct || bool(v.attachments.depth_stencil) != p.has_ds)
                return fail("draw attachments", "attachments disagree with mapped pipeline");
            if (!p.n_ct && !p.has_ds) return fail("draw attachments", "no mapped render attachment");
            if (p.ms.sample_count != SDL_GPU_SAMPLECOUNT_1 || p.ms.enable_mask || p.ms.sample_mask)
                return fail("draw inputs", "unsupported multisample state");
            if (p.depth_clamp && !info_.depth_clip_and_clamp)
                return fail("draw inputs", "depth-clamp property requires the effective device capability");

            // The draw's scratch (members, so their storage is reused draw after draw).
            std::vector<SDL_GPUColorTargetInfo>& colours = draw_colours_;
            colours.assign(p.n_ct, SDL_GPUColorTargetInfo{});
            SDL_GPUDepthStencilTargetInfo depth{};
            std::string& attachment_key = draw_attachment_key_;
            attachment_key.clear();
            uint32_t render_w = UINT32_MAX, render_h = UINT32_MAX;
            auto resolve_attachment = [&](const draw::TextureLevel& a, bool ds, uint32_t slot) -> bool {
                auto it = tex_.find(a.texture.value);
                if (it == tex_.end()) { err_ = "unknown attachment texture"; return false; }
                const Tex& t = it->second;
                if (t.d.type == TexType::volume || a.layer >= (t.d.type == TexType::cube ? 6u : 1u)) {
                    err_ = "a volume texture, or a layer the texture does not have, as an attachment"; return false;
                }
                if (a.level >= t.d.levels || (ds && a.level > UINT8_MAX)) {
                    err_ = "attachment mip level is out of range"; return false;
                }
                if (is_depth(t.d.fmt) != ds || (!ds && !t.d.target) ||
                    sdl_format(t.d.fmt) != (ds ? p.ds_fmt : p.ct[slot].format)) {
                    err_ = "attachment usage or format disagrees with mapped pipeline"; return false;
                }
                render_w = std::min(render_w, level_dim(t.d.w, a.level));
                render_h = std::min(render_h, level_dim(t.d.h, a.level));
                append_u32(attachment_key, a.texture.value); append_u32(attachment_key, a.level); append_u32(attachment_key, a.layer);
                if (ds) {
                    depth.texture = t.t; depth.mip_level = Uint8(a.level); depth.layer = Uint8(a.layer);
                    depth.load_op = depth.stencil_load_op = SDL_GPU_LOADOP_LOAD;
                    depth.store_op = depth.stencil_store_op = SDL_GPU_STOREOP_STORE;
                } else {
                    colours[slot].texture = t.t; colours[slot].mip_level = a.level; colours[slot].layer_or_depth_plane = a.layer;
                    colours[slot].load_op = SDL_GPU_LOADOP_LOAD;
                    colours[slot].store_op = SDL_GPU_STOREOP_STORE;
                }
                return true;
            };
            append_u32(attachment_key, p.n_ct);
            for (uint32_t i = 0; i < p.n_ct; ++i)
                if (!resolve_attachment(v.attachments.colours[i], false, i)) return fail("draw attachments", err_);
            append_u32(attachment_key, p.has_ds);
            if (p.has_ds && !resolve_attachment(*v.attachments.depth_stencil, true, 0))
                return fail("draw attachments", err_);

            const auto& d = v.dynamic;
            if (!std::isfinite(d.x) || !std::isfinite(d.y) || !std::isfinite(d.width) ||
                !std::isfinite(d.height) || d.width <= 0 || d.height <= 0 ||
                !std::isfinite(d.min_depth) || !std::isfinite(d.max_depth) || d.min_depth < 0 ||
                d.max_depth > 1 || d.min_depth > d.max_depth || d.scissor_x < 0 || d.scissor_y < 0 ||
                d.scissor_width > INT32_MAX || d.scissor_height > INT32_MAX ||
                uint64_t(d.scissor_x) + d.scissor_width > render_w ||
                uint64_t(d.scissor_y) + d.scissor_height > render_h)
                return fail("draw dynamic state", "viewport or scissor is outside the executable domain");
            for (float value : d.blend_constant)
                if (!std::isfinite(value)) return fail("draw dynamic state", "nonfinite blend constant");

            auto validate_stage = [&](const shader::Built& b, const draw::StageBindings& bindings) -> bool {
                if (!b.ok || b.spirv.empty() || b.spirv[0] != 0x07230203u || b.num_samplers > 16 ||
                    b.num_samplers != b.sampler_registers.size() || b.num_uniform_buffers > 4) {
                    err_ = "invalid Built shader ABI"; return false;
                }
                if (bindings.samplers.size() != b.num_samplers || bindings.uniforms.count != b.num_uniform_buffers) {
                    err_ = "binding count disagrees with Built shader ABI"; return false;
                }
                uint32_t seen_registers = 0, seen_uniforms = 0;
                for (uint32_t reg : b.sampler_registers) {
                    if (reg >= 32 || (seen_registers & (1u << reg))) { err_ = "invalid compact sampler register order"; return false; }
                    seen_registers |= 1u << reg;
                }
                for (const auto& use : b.uniforms) {
                    if (use.slot >= b.num_uniform_buffers || !use.bytes || use.bytes > shader::kSdlUniformWindow ||
                        (seen_uniforms & (1u << use.slot)) || bindings.uniforms.slots[use.slot].size() < use.bytes) {
                        err_ = "uniform payload does not cover its declared slot"; return false;
                    }
                    seen_uniforms |= 1u << use.slot;
                }
                for (uint32_t i = 0; i < b.num_uniform_buffers; ++i)
                    if (bindings.uniforms.slots[i].empty() || bindings.uniforms.slots[i].size() > shader::kSdlUniformWindow) {
                        err_ = "uniform hole is uninitialized or payload exceeds 4 KiB"; return false;
                    }
                return true;
            };
            if (!validate_stage(v.program->vertex, v.vertex) || !validate_stage(v.program->pixel, v.pixel))
                return fail("draw bindings", err_);

            std::vector<SDL_GPUTextureSamplerBinding>& vertex_samplers = draw_vertex_samplers_;
            std::vector<SDL_GPUTextureSamplerBinding>& pixel_samplers = draw_pixel_samplers_;
            vertex_samplers.clear(); pixel_samplers.clear();
            auto resolve_samplers = [&](const draw::StageBindings& stage, const shader::Built& b,
                                        const shader::Reflection& reflection,
                                        std::vector<SDL_GPUTextureSamplerBinding>& out) -> bool {
                out.reserve(stage.samplers.size());
                for (size_t i = 0; i < stage.samplers.size(); ++i) {
                    const auto& binding = stage.samplers[i];
                    auto it = tex_.find(binding.texture.value);
                    if (it == tex_.end()) { err_ = "unknown sampled texture, including an unresolved null binding"; return false; }
                    const Tex& t = it->second;
                    if (!(t.usage & SDL_GPU_TEXTUREUSAGE_SAMPLER)) { err_ = "texture allocation has no sampled usage"; return false; }
                    // the device refuses a draw sampling a cube or volume texture (CUBE_TEXTURE_SAMPLE,
                    // VOLUME_TEXTURE_SAMPLE): the shaders' samplers here are 2D
                    if (t.d.type != TexType::flat) { err_ = "a cube or volume texture sampled"; return false; }
                    for (const auto& colour : v.attachments.colours)
                        if (binding.texture.value == colour.texture.value) { err_ = "sampled texture aliases a colour attachment"; return false; }
                    if (v.attachments.depth_stencil && binding.texture.value == v.attachments.depth_stencil->texture.value) {
                        err_ = "sampled texture aliases the depth attachment"; return false;
                    }
                    const uint32_t reg = b.sampler_registers[i];
                    const bool depth_read = (reflection.depth_compare_registers & (1u << reg)) != 0;
                    if (!binding.sampler.info || binding.sampler.fallback() || !binding.sampler.unmapped.empty()) {
                        err_ = "sampler description has an unfinished fallback or unmapped field"; return false;
                    }
                    if (depth_read != bool(binding.sampler.info->enable_compare) || (depth_read && !is_depth(t.d.fmt))) {
                        err_ = "shader depth-compare use disagrees with sampler or texture allocation"; return false;
                    }
                    SDL_GPUSampler* sampler = sampler_object(binding.sampler);
                    if (!sampler) return false;
                    out.push_back(SDL_GPUTextureSamplerBinding{t.t, sampler});
                }
                return true;
            };
            if (!resolve_samplers(v.vertex, v.program->vertex, v.program->vertex_reflection, vertex_samplers) ||
                !resolve_samplers(v.pixel, v.program->pixel, v.program->pixel_reflection, pixel_samplers))
                return fail("draw samplers", err_);

            bool real_slot = false, null_slot = false;
            uint32_t highest_slot = 0, pitch = 0, real_footprint = 0;
            uint32_t vb_slots = 0, attr_locations = 0, other_slots = 0;
            for (uint32_t i = 0; i < p.n_vbs; ++i) {
                const auto& vb = p.vbs[i];
                if (vb.slot >= 16 || (vb_slots & (1u << vb.slot)) || vb.instance_step_rate) {
                    return fail("draw vertices", "unsupported or duplicate vertex binding");
                }
                vb_slots |= 1u << vb.slot; highest_slot = std::max(highest_slot, vb.slot);
                if (vb.slot == 0) {
                    if (vb.input_rate != SDL_GPU_VERTEXINPUTRATE_VERTEX || !vb.pitch)
                        return fail("draw vertices", "real stream must have per-vertex nonzero pitch");
                    real_slot = true; pitch = vb.pitch;
                } else if (vb.slot == kNullStreamSlot) {
                    if (vb.pitch || vb.input_rate != SDL_GPU_VERTEXINPUTRATE_INSTANCE)
                        return fail("draw vertices", "semantic null must use zero pitch and per-instance rate");
                    null_slot = true;
                } else {
                    // a stream other than 0 (d9g_pipeline.cpp): per vertex, or per instance at SDL's
                    // one-instance step (INSTANCEDATA with divisor 1)
                    if (!vb.pitch || (vb.input_rate != SDL_GPU_VERTEXINPUTRATE_VERTEX &&
                                      vb.input_rate != SDL_GPU_VERTEXINPUTRATE_INSTANCE))
                        return fail("draw vertices", "a stream other than 0 must have a nonzero pitch and a vertex or instance rate");
                    other_slots |= 1u << vb.slot;
                }
            }
            if (null_slot != p.null_stream) return fail("draw vertices", "null-stream obligation disagrees with vertex bindings");
            for (uint32_t i = 0; i < p.n_attrs; ++i) {
                const auto& a = p.attrs[i];
                const uint32_t width = vertex_element_bytes(a.format);
                if (a.location >= 16 || (attr_locations & (1u << a.location)) || a.buffer_slot >= 16 ||
                    !(vb_slots & (1u << a.buffer_slot)) || !width || uint64_t(a.offset) + width > UINT32_MAX)
                    return fail("draw vertices", "invalid vertex attribute or missing binding");
                attr_locations |= 1u << a.location;
                if (a.buffer_slot == 0) real_footprint = std::max(real_footprint, a.offset + width);
                else if (a.buffer_slot == kNullStreamSlot && (a.offset || a.format != SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4))
                    return fail("draw vertices", "semantic null must read the first FLOAT4");
            }

            SDL_GPUBufferBinding real{}, index{}, filler{};
            uint32_t vertex_available = 0, count = 0, first_vertex = 0, first_index = 0, instances = 1;
            int32_t base_vertex = 0;
            bool indexed = false;
            draw::IndexWidth index_width = draw::IndexWidth::U16;
            const std::vector<uint8_t>* up_bytes = nullptr;
            uint32_t up_index_offset = 0;
            auto resolve_slice = [&](const draw::BufferSlice& slice, SDL_GPUBufferUsageFlags usage,
                                     SDL_GPUBufferBinding& out) -> bool {
                auto it = buf_.find(slice.buffer.value);
                if (it == buf_.end()) { err_ = "unknown buffer ID"; return false; }
                const Buf& buffer = it->second;
                if (!(buffer.usage & usage) || !slice.available_bytes || slice.offset >= buffer.bytes ||
                    uint64_t(slice.offset) + slice.available_bytes > buffer.bytes) {
                    err_ = "buffer slice is outside the native allocation or has incompatible usage"; return false;
                }
                out = SDL_GPUBufferBinding{buffer.b, slice.offset};
                return true;
            };
            auto resolve_vertices = [&](const draw::VertexSource& source) -> bool {
                if (const auto* bound = std::get_if<draw::BoundVertices>(&source)) {
                    if (!resolve_slice(bound->stream_zero, SDL_GPU_BUFFERUSAGE_VERTEX, real)) return false;
                    vertex_available = bound->stream_zero.available_bytes;
                } else if (const auto* owned = std::get_if<draw::OwnedVertices>(&source)) {
                    if (owned->padded_bytes.empty() || owned->padded_bytes.size() > UINT32_MAX - 63u) {
                        err_ = "invalid owned vertex allocation"; return false;
                    }
                    up_bytes = &owned->padded_bytes;
                    vertex_available = uint32_t(up_bytes->size());
                } else if (real_slot) { err_ = "real vertex input has no source"; return false; }
                return true;
            };
            if (const auto* non = std::get_if<draw::NonIndexed>(&v.geometry)) {
                count = non->count; first_vertex = non->first_vertex;
                if (!resolve_vertices(non->vertices)) return fail("draw geometry", err_);
            } else if (const auto* buffered = std::get_if<draw::BufferIndexed>(&v.geometry)) {
                indexed = true; count = buffered->count; first_index = buffered->first_index;
                instances = buffered->instances; base_vertex = buffered->base_vertex; index_width = buffered->width;
                if (index_width != draw::IndexWidth::U16 && index_width != draw::IndexWidth::U32)
                    return fail("draw indices", "unknown index width");
                if (!resolve_vertices(buffered->vertices) || !resolve_slice(buffered->indices, SDL_GPU_BUFFERUSAGE_INDEX, index))
                    return fail("draw geometry", err_);
                const uint32_t width = index_width == draw::IndexWidth::U16 ? 2u : 4u;
                if (buffered->indices.offset % width)
                    return fail("draw indices", "native index buffer binding offset is misaligned");
                const uint32_t native_available = buf_.at(buffered->indices.buffer.value).bytes - buffered->indices.offset;
                if ((uint64_t(first_index) + count) * width > native_available)
                    return fail("ROBUST_INDEX_BUFFER_OOB", "SDL has no robustBufferAccess2 for the required native index interval");
            } else {
                const auto& owned = std::get<draw::OwnedIndexedUp>(v.geometry);
                indexed = true; count = owned.count; instances = owned.instances; index_width = owned.width;
                if (index_width != draw::IndexWidth::U16 && index_width != draw::IndexWidth::U32)
                    return fail("draw indices", "unknown index width");
                up_bytes = &owned.bytes; up_index_offset = owned.index_offset;
                vertex_available = owned.vertex_bytes;
                const uint32_t width = index_width == draw::IndexWidth::U16 ? 2u : 4u;
                if (owned.bytes.empty() || owned.bytes.size() > UINT32_MAX - 63u || !owned.vertex_bytes ||
                    owned.index_offset < owned.vertex_bytes || owned.index_offset % width ||
                    uint64_t(owned.index_offset) + owned.index_bytes != owned.bytes.size() ||
                    uint64_t(count) * width > owned.index_bytes)
                    return fail("draw indices", "owned indexed-UP block has an invalid vertex/index interval");
                if (real_slot) {
                    for (uint32_t i = 0; i < count; ++i) {
                        uint32_t value = 0;
                        std::memcpy(&value, owned.bytes.data() + owned.index_offset + uint64_t(i) * width, width);
                        if (uint64_t(value) * pitch + real_footprint > vertex_available)
                            return fail("draw indices", "owned UP index addresses absent vertex data");
                    }
                }
            }
            if (!count || !instances) return fail("draw geometry", "empty geometry must be handled by Device");
            if (index_width != draw::IndexWidth::U16 && index_width != draw::IndexWidth::U32)
                return fail("draw indices", "unknown index width");
            // Ordinary buffers keep D3D's accepted robust fetch behavior. Root enables the
            // core robustBufferAccess feature in VulkanFeatures before this source is integrated.
            if (up_bytes && real_slot && vertex_available < real_footprint)
                return fail("draw vertices", "owned vertex source is shorter than its mapped attribute footprint");
            if (up_bytes && real_slot && !indexed &&
                (uint64_t(first_vertex) + count - 1u) * pitch + real_footprint > vertex_available)
                return fail("draw vertices", "owned nonindexed fetch interval is outside its supplied block");
            // the other streams' slices: one per binding, each inside its allocation
            std::array<SDL_GPUBufferBinding, 16> other{};
            uint32_t sliced = 0;
            for (const auto& stream : v.streams) {
                if (stream.stream >= 16 || !(other_slots & (1u << stream.stream)) || (sliced & (1u << stream.stream)))
                    return fail("draw vertices", "a stream slice for no binding of the pipeline's, or twice");
                if (!resolve_slice(stream.slice, SDL_GPU_BUFFERUSAGE_VERTEX, other[stream.stream])) return fail("draw vertices", err_);
                sliced |= 1u << stream.stream;
            }
            if (sliced != other_slots) return fail("draw vertices", "a pipeline binding of a stream other than 0 has no slice");
            // SDL binds slots 0..highest at once: a slot no binding names (the null stream's gap below
            // slot 15, or a stream gap) takes the semantic-null buffer
            const uint32_t span = (2u << highest_slot) - 1u;
            if (p.n_vbs && (null_slot || (vb_slots & span) != span)) {
                if (!v.semantic_null || !resolve_slice(*v.semantic_null, SDL_GPU_BUFFERUSAGE_VERTEX, filler))
                    return fail("draw vertices", v.semantic_null ? err_ : "missing semantic-null/filler buffer");
                if (v.semantic_null->available_bytes < 16)
                    return fail("draw vertices", "semantic-null/filler slice is shorter than zero FLOAT4");
            }

            ProgramObjects* objects = program_objects(v.program);
            if (!objects) return fail("draw shaders", err_);
            SDL_GPUGraphicsPipeline* pipeline = pipeline_object(p, *objects);
            if (!pipeline) return fail("draw pipeline", err_);
            uint64_t uploaded = 0;
            if (up_bytes) {
                SDL_GPUBufferBinding upload{};
                if (!stage_up(*up_bytes, upload)) return fail("draw UP upload", err_);
                uploaded = up_bytes->size();
                if (real_slot) real = upload;
                if (std::holds_alternative<draw::OwnedIndexedUp>(v.geometry))
                    index = SDL_GPUBufferBinding{upload.buffer, upload.offset + up_index_offset};
            }
            if (!open_pass(attachment_key, colours, p.has_ds ? &depth : nullptr, render_w, render_h))
                return fail("draw render pass", err_);
            // The draw joins the open pass's commands (flush_pass encodes them in one SDL render
            // pass), reduced against the state SDL will hold at that point of the pass (bound_):
            // a call that would set what is already set is left out, which leaves SDL's state as
            // it was and spares SDL's descriptor sets the refresh a pipeline or sampler bind forces.
            if (bound_.pipeline != pipeline) {
                record(Op::pipeline).u.pipeline = pipeline;
                bound_.pipeline = pipeline;
            }
            if (p.n_vbs) {
                std::array<SDL_GPUBufferBinding, 16> bindings{};
                for (uint32_t i = 0; i <= highest_slot; ++i) bindings[i] = filler;
                if (real_slot) bindings[0] = real;
                for (uint32_t i = 1; i < 16; ++i)
                    if (other_slots & (1u << i)) bindings[i] = other[i];
                record_vertex_buffers(bindings.data(), highest_slot + 1u);
            }
            if (indexed)
                record_index_buffer(index, index_width == draw::IndexWidth::U16 ? SDL_GPU_INDEXELEMENTSIZE_16BIT
                                                                                : SDL_GPU_INDEXELEMENTSIZE_32BIT);
            if (!vertex_samplers.empty()) record_samplers(0, vertex_samplers);
            if (!pixel_samplers.empty()) record_samplers(1, pixel_samplers);
            for (uint32_t i = 0; i < v.vertex.uniforms.count; ++i) record_uniform(0, i, v.vertex.uniforms.slots[i]);
            for (uint32_t i = 0; i < v.pixel.uniforms.count; ++i) record_uniform(1, i, v.pixel.uniforms.slots[i]);
            // Device supplied DXVK's half-pixel offset. SDL applies Vulkan's Y flip once.
            const SDL_GPUViewport viewport{d.x, d.y, d.width, d.height, d.min_depth, d.max_depth};
            const SDL_Rect scissor{d.scissor_x, d.scissor_y, int(d.scissor_width), int(d.scissor_height)};
            const SDL_FColor blend{d.blend_constant[0], d.blend_constant[1], d.blend_constant[2], d.blend_constant[3]};
            if (!bound_.has_dynamic || std::memcmp(&bound_.viewport, &viewport, sizeof viewport) != 0)
                record(Op::viewport).u.viewport = viewport;
            if (!bound_.has_dynamic || std::memcmp(&bound_.scissor, &scissor, sizeof scissor) != 0)
                record(Op::scissor).u.scissor = scissor;
            if (!bound_.has_dynamic || std::memcmp(&bound_.blend, &blend, sizeof blend) != 0)
                record(Op::blend).u.blend = blend;
            if (!bound_.has_dynamic || bound_.stencil != d.stencil_reference)
                record(Op::stencil).small = d.stencil_reference;
            bound_.viewport = viewport; bound_.scissor = scissor; bound_.blend = blend;
            bound_.stencil = d.stencil_reference; bound_.has_dynamic = true;
            Cmd& call = record(indexed ? Op::draw_indexed : Op::draw);
            if (indexed) call.u.draw = {count, instances, first_index, base_vertex};
            else call.u.draw = {count, 1, first_vertex, 0};
            return draw::Encoded{uploaded};
        } catch (const std::bad_alloc&) {
            err_ = "draw: no memory";
            return draw::Failure{"draw allocation", "out of memory"};
        }
    }

    uint32_t create_texture(const TexDesc& d) override {
        SDL_GPUTextureCreateInfo ci{};
        // A cube's six faces are its layers (D3DCUBEMAP_FACES' order is Vulkan's), a render-target
        // cube's faces colour targets by layer; a volume's depth its third dimension, never a target
        // (DXVK refuses a render-target volume).
        ci.type = d.type == TexType::cube ? SDL_GPU_TEXTURETYPE_CUBE
                : d.type == TexType::volume ? SDL_GPU_TEXTURETYPE_3D : SDL_GPU_TEXTURETYPE_2D;
        ci.format = sdl_format(d.fmt);
        ci.width = d.w;
        ci.height = d.h;
        ci.layer_count_or_depth = d.type == TexType::cube ? 6u : d.type == TexType::volume ? d.depth : 1u;
        ci.num_levels = d.levels;
        ci.sample_count = SDL_GPU_SAMPLECOUNT_1;
        if (!d.w || !d.h || !d.levels || ci.format == SDL_GPU_TEXTUREFORMAT_INVALID) {
            err_ = "invalid texture dimensions, levels or format"; return 0;
        }
        if (d.type != TexType::flat && ((d.type == TexType::volume && d.target) || is_depth(d.fmt) || !d.depth ||
                                        (d.type == TexType::cube && d.w != d.h))) {
            err_ = "a volume target, a cube or volume of a depth format, a cube not square or a volume of no depth"; return 0;
        }
        if (d.type != TexType::volume && d.depth != 1) { err_ = "a depth on a texture that is not a volume"; return 0; }
        if (is_depth(d.fmt)) ci.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET |
                                      (d.sampled ? SDL_GPU_TEXTUREUSAGE_SAMPLER : 0);
        else ci.usage = (d.target ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0) | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        if (!SDL_GPUTextureSupportsFormat(dev_, ci.format, ci.type, ci.usage)) {
            err_ = d.sampled && is_depth(d.fmt) ? "sampled depth format/usage is unsupported" : "texture format/usage is unsupported";
            return 0;
        }
        // SDL texture creation submits initialization commands and may defragment older
        // images. Submit prior uses first so those images cannot move ahead of our draws.
        if (cb_ && !submit(false)) return 0;
        SDL_GPUTexture* t = SDL_CreateGPUTexture(dev_, &ci);
        if (!t) { err_ = std::string("SDL_CreateGPUTexture: ") + SDL_GetError(); return 0; }
        const uint32_t id = next_++;
        tex_[id] = Tex{t, d, ci.usage};
        // Zero, as DXVK's new images are (dxvk_context.cpp initImage): targets by a CLEAR
        // pass per level, the rest by uploading zeros (every face of a cube, every slice of a volume).
        bool ok = true;
        const uint32_t layers = d.type == TexType::cube ? 6u : 1u;
        for (uint32_t layer = 0; layer < layers && ok; layer++)
            for (uint32_t l = 0; l < d.levels && ok; l++) {
                if (d.target || is_depth(d.fmt)) {
                    ClearValue v;
                    v.colour = !is_depth(d.fmt);
                    v.depth = v.stencil = is_depth(d.fmt);
                    ok = clear_layer(id, layer, l, v);
                } else {
                    const size_t n = level_upload_bytes(d, l);
                    if (zeros_.size() < n) zeros_.assign(n, 0);
                    ok = upload_subresource(id, layer, l, zeros_.data(), n);
                }
            }
        if (!ok) { release_texture(id); return 0; }
        return id;
    }

    void release_texture(uint32_t id) override {
        auto it = tex_.find(id);
        if (it == tex_.end()) return;
        end_render();
        end_copy();
        // its clears no one will see (and SDL may give a later texture the same handle)
        pending_clears_.erase(std::remove_if(pending_clears_.begin(), pending_clears_.end(),
                                             [&](const PendingClear& q) { return q.id == id; }),
                              pending_clears_.end());
        SDL_ReleaseGPUTexture(dev_, it->second.t);   // SDL defers it past the GPU's use
        tex_.erase(it);
    }

    uint32_t create_buffer(uint32_t bytes, bool index) override {
        if (!bytes) { err_ = "cannot create an empty GPU buffer"; return 0; }
        const uint32_t native_bytes = std::max(bytes, 4u); // SDL requires at least four bytes.
        SDL_GPUBufferCreateInfo ci{index ? SDL_GPU_BUFFERUSAGE_INDEX : SDL_GPU_BUFFERUSAGE_VERTEX, native_bytes, 0};
        SDL_GPUBuffer* b = SDL_CreateGPUBuffer(dev_, &ci);
        if (!b) { err_ = std::string("SDL_CreateGPUBuffer: ") + SDL_GetError(); return 0; }
        const uint32_t id = next_++;
        buf_[id] = Buf{b, native_bytes, ci.usage};
        // Zero-filled, as DXVK's InitBuffer leaves a new buffer (the critic's A24).
        if (zeros_.size() < native_bytes) zeros_.assign(native_bytes, 0);
        if (!upload_buffer(id, 0, zeros_.data(), native_bytes)) { release_buffer(id); return 0; }
        return id;
    }

    void release_buffer(uint32_t id) override {
        auto it = buf_.find(id);
        if (it == buf_.end()) return;
        end_render();
        end_copy();
        SDL_ReleaseGPUBuffer(dev_, it->second.b);
        buf_.erase(it);
    }

    bool upload_subresource(uint32_t id, uint32_t layer, uint32_t level, const uint8_t* data, size_t bytes) override {
        auto it = tex_.find(id);
        if (it == tex_.end()) { err_ = "upload to an unknown texture"; return false; }
        const TexDesc& d = it->second.d;
        if (level >= d.levels || layer >= (d.type == TexType::cube ? 6u : 1u) || !data || bytes > UINT32_MAX ||
            bytes != level_upload_bytes(d, level)) {
            err_ = "upload of a level with the wrong size";
            return false;
        }
        // The open pass first (it may take the subresource's pending clear as a load op), then the
        // upload overwrites the whole subresource: a colour clear still pending is never seen.
        if (!end_render()) return false;
        for (size_t i = 0; i < pending_clears_.size(); ++i) {
            const PendingClear& q = pending_clears_[i];
            if (q.id != id || q.layer != layer || q.level != level) continue;
            if (!q.ds) { pending_clears_.erase(pending_clears_.begin() + std::ptrdiff_t(i)); c_.clears_dropped++; }
            else if (!settle_clears(id, layer, level)) return false;
            break;
        }
        SDL_GPUTransferBuffer* tb = nullptr;
        uint32_t off = 0;
        if (!stage(data, bytes, &tb, &off)) return false;
        SDL_GPUTextureTransferInfo src{tb, off, 0, 0};
        // a volume's level: all of its slices (z 0, its depth); a cube face: its layer
        const uint32_t depth = d.type == TexType::volume ? level_dim(d.depth, level) : 1u;
        SDL_GPUTextureRegion dst{it->second.t, level, layer, 0, 0, 0, level_dim(d.w, level), level_dim(d.h, level), depth};
        SDL_GPUCopyPass* cp = copy_pass();
        if (!cp) { release_dedicated(tb); return false; }
        if (!encode_call("SDL_UploadToGPUTexture", [&] { SDL_UploadToGPUTexture(cp, &src, &dst, false); })) { release_dedicated(tb); return false; }
        release_dedicated(tb);
        c_.uploads++;
        c_.upload_bytes += bytes;
        return true;
    }

    bool upload_buffer(uint32_t id, uint32_t offset, const uint8_t* data, uint32_t bytes) override {
        auto it = buf_.find(id);
        if (it == buf_.end()) { err_ = "upload to an unknown buffer"; return false; }
        if (!bytes) return true;
        if (!data || uint64_t(offset) + bytes > it->second.bytes) {
            err_ = "buffer upload is outside the native allocation"; return false;
        }
        SDL_GPUTransferBuffer* tb = nullptr;
        uint32_t off = 0;
        if (!stage(data, bytes, &tb, &off)) return false;
        SDL_GPUTransferBufferLocation src{tb, off};
        SDL_GPUBufferRegion dst{it->second.b, offset, bytes};
        SDL_GPUCopyPass* cp = copy_pass();
        if (!cp) { release_dedicated(tb); return false; }
        if (!encode_call("SDL_UploadToGPUBuffer", [&] { SDL_UploadToGPUBuffer(cp, &src, &dst, false); })) { release_dedicated(tb); return false; }
        release_dedicated(tb);
        c_.uploads++;
        c_.upload_bytes += bytes;
        return true;
    }

    bool clear_layer(uint32_t id, uint32_t layer, uint32_t level, const ClearValue& v) override {
        auto it = tex_.find(id);
        if (it == tex_.end()) { err_ = "clear of an unknown texture"; return false; }
        if (it->second.d.type == TexType::volume || layer >= (it->second.d.type == TexType::cube ? 6u : 1u)) {
            err_ = "clear of a volume texture or of a layer the texture does not have"; return false;
        }
        if (level >= it->second.d.levels || (is_depth(it->second.d.fmt) && level > UINT8_MAX)) {
            err_ = "clear mip level is out of range"; return false;
        }
        // Not encoded here (PendingClear). The open pass's draws come before the clear, so it is
        // flushed first; an attachment of it whose aspects the clear covers is not stored, as
        // nothing can read that content before the clear (every reader encodes the clear first).
        const bool ds = is_depth(it->second.d.fmt);
        if (pass_open_) drop_cleared_stores(it->second.t, layer, level, ds, v);
        if (!end_render()) return false;
        PendingClear* q = nullptr;
        for (PendingClear& p : pending_clears_)
            if (p.id == id && p.layer == layer && p.level == level) { q = &p; break; }
        if (!q) {
            if (ds ? !(v.depth || v.stencil) : !v.colour) return true;   // loads every aspect: no change
            pending_clears_.push_back(PendingClear{});
            q = &pending_clears_.back();
            q->id = id; q->layer = layer; q->level = level; q->texture = it->second.t; q->ds = ds;
        } else if (ds ? (v.depth && q->depth) || (v.stencil && q->stencil) : v.colour && q->colour) {
            c_.clears_dropped++;   // a clear no draw saw, overwritten by this one
        }
        if (ds) {
            if (v.depth) { q->depth = true; q->z = v.z; }
            if (v.stencil) { q->stencil = true; q->s = Uint8(v.s); }
        } else if (v.colour) {
            q->colour = true;
            q->rgba = SDL_FColor{v.rgba[0], v.rgba[1], v.rgba[2], v.rgba[3]};
        }
        return true;
    }

    bool copy_layers(uint32_t src, uint32_t sa, uint32_t sl, uint32_t sx, uint32_t sy, uint32_t dst, uint32_t da, uint32_t dl,
                     uint32_t dx, uint32_t dy, uint32_t w, uint32_t h) override {
        auto a = tex_.find(src), b = tex_.find(dst);
        if (a == tex_.end() || b == tex_.end()) { err_ = "copy between unknown textures"; return false; }
        if (a->second.d.type == TexType::volume || b->second.d.type == TexType::volume ||
            sa >= (a->second.d.type == TexType::cube ? 6u : 1u) || da >= (b->second.d.type == TexType::cube ? 6u : 1u)) {
            err_ = "copy of a volume texture or of a layer a texture does not have"; return false;
        }
        if (sl >= a->second.d.levels || dl >= b->second.d.levels || a->second.d.fmt != b->second.d.fmt ||
            !w || !h || uint64_t(sx) + w > level_dim(a->second.d.w, sl) ||
            uint64_t(sy) + h > level_dim(a->second.d.h, sl) ||
            uint64_t(dx) + w > level_dim(b->second.d.w, dl) || uint64_t(dy) + h > level_dim(b->second.d.h, dl)) {
            err_ = "texture copy interval or format is invalid"; return false;
        }
        if (!settle_clears(src, sa, sl) || !settle_clears(dst, da, dl)) return false;
        SDL_GPUTextureLocation from{a->second.t, sl, sa, sx, sy, 0};
        SDL_GPUTextureLocation to{b->second.t, dl, da, dx, dy, 0};
        SDL_GPUCopyPass* cp = copy_pass();
        if (!cp) return false;
        if (!encode_call("SDL_CopyGPUTextureToTexture", [&] { SDL_CopyGPUTextureToTexture(cp, &from, &to, w, h, 1, false); })) { return false; }
        c_.copies++;
        return true;
    }

    bool download(uint32_t id, uint32_t level, uint8_t* out, size_t bytes) override {
        auto it = tex_.find(id);
        if (it == tex_.end()) { err_ = "download of an unknown texture"; return false; }
        const TexDesc& d = it->second.d;
        if (d.type != TexType::flat) { err_ = "download of a cube or volume texture"; return false; }
        if (level >= d.levels || !out || bytes > UINT32_MAX) { err_ = "invalid texture download interval"; return false; }
        const uint32_t lw = level_dim(d.w, level), lh = level_dim(d.h, level);
        if (d.fmt != GFmt::bgra8 || bytes != size_t(lw) * lh * 4) { err_ = "download of a non-BGRA8 level"; return false; }
        if (down_size_ < bytes) {
            if (down_) SDL_ReleaseGPUTransferBuffer(dev_, down_);
            SDL_GPUTransferBufferCreateInfo ci{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, uint32_t(bytes), 0};
            down_ = SDL_CreateGPUTransferBuffer(dev_, &ci);
            down_size_ = down_ ? uint32_t(bytes) : 0;
            if (!down_) { err_ = std::string("SDL_CreateGPUTransferBuffer: ") + SDL_GetError(); return false; }
        }
        if (!settle_clears(id, 0, level)) return false;
        SDL_GPUCopyPass* cp = copy_pass();
        if (!cp) return false;
        SDL_GPUTextureRegion from{it->second.t, level, 0, 0, 0, 0, lw, lh, 1};
        SDL_GPUTextureTransferInfo to{down_, 0, 0, 0};
        if (!encode_call("SDL_DownloadFromGPUTexture", [&] { SDL_DownloadFromGPUTexture(cp, &from, &to); })) { return false; }
        if (!submit(true)) return false;
        void* p = nullptr;
        if (!encode_call("SDL_MapGPUTransferBuffer(download)", [&] { p = SDL_MapGPUTransferBuffer(dev_, down_, false); })) {
            if (p) SDL_UnmapGPUTransferBuffer(dev_, down_);
            return false;
        }
        if (!p) { err_ = std::string("SDL_MapGPUTransferBuffer: ") + SDL_GetError(); return false; }
        std::memcpy(out, p, bytes);
        if (!encode_call("SDL_UnmapGPUTransferBuffer(download)", [&] { SDL_UnmapGPUTransferBuffer(dev_, down_); })) return false;
        c_.downloads++;
        return true;
    }

    bool present(uint32_t bb, PresentInfo* out) override {
        if (!out) { err_ = "no Present result destination"; return false; }
        *out = PresentInfo{};
        auto it = tex_.find(bb);
        if (it == tex_.end()) { err_ = "present of an unknown texture"; return false; }
        if (!end_render() || !settle_clears(bb, 0, 0) || !end_copy()) return false;
        presents_++;
        if (win_) {
            SDL_GPUCommandBuffer* cb = cmd();
            if (!cb) return false;
            // A frame the window has no surface for is drawn and submitted without the blit, and
            // Present succeeds (AfterAcquire, DXVK's rule): the command buffer holds the frame's
            // draws and uploads, which later frames need.
            const auto lost = [&](std::string why) {
                out->no_swapchain_texture = out->surface_lost = true;
                out->surface_lost_reason = std::move(why);
                return submit(false);
            };
            if (surface_gone_)
                return lost("SDL could not make the window's surface again earlier in this session, and its "
                            "swapchain is not acquired again (AfterAcquire::surface_gone)");
#ifdef __ANDROID__
            // Android destroys the window's surface while the app is in the background; SDL clears
            // the window's ANativeWindow property then (SDL_android.c onNativeSurfaceDestroyed) and
            // sets it once the new surface exists (onNativeSurfaceCreated). Without one, SDL's acquire
            // would try to remake the surface and fail, leaving it gone (surface_gone): no acquire is
            // made until the window has a surface again. SDL's acquire on the old swapchain then finds
            // it lost and remakes the surface and the swapchain at the acquire after it.
            if (!SDL_GetPointerProperty(SDL_GetWindowProperties(win_), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr))
                return lost("Android has destroyed the window's surface: the app is in the background, or closing");
#endif
            SDL_GPUTexture* sw = nullptr;
            Uint32 w = 0, h = 0;
            SDL_ClearError();
            bool acquired;
            if ((report().fault.bits & F_SURFACE_LOST) && presents_ >= 2 && presents_ <= 4) {
                // The negative control's fault: the acquire answers as it does on a lost surface.
                acquired = false;
                SDL_SetError("fault surface-lost: SDL_WaitAndAcquireGPUSwapchainTexture not called");
            } else {
                acquired = SDL_WaitAndAcquireGPUSwapchainTexture(cb, win_, &sw, &w, &h);
            }
            swapchain_acquired_ = sw != nullptr;
            const char* sdl_error = SDL_GetError();
            switch (after_acquire(acquired, sw != nullptr, sdl_error)) {
            case AfterAcquire::blit:
            case AfterAcquire::no_texture:
                break;
            case AfterAcquire::surface_lost: {
                std::string why = std::string("SDL_WaitAndAcquireGPUSwapchainTexture: ") +
                                  (sdl_error && *sdl_error ? sdl_error : "failed with no error");
                SDL_ClearError();
                return lost(std::move(why));
            }
            case AfterAcquire::surface_gone: {
                surface_gone_ = true;
                std::string why = std::string("SDL_WaitAndAcquireGPUSwapchainTexture: ") + sdl_error +
                                  " (SDL 3.4.16 keeps the destroyed surface: no swapchain is acquired again)";
                SDL_ClearError();
                return lost(std::move(why));
            }
            case AfterAcquire::encoding_error:
                if (!check_encoding_error("SDL_WaitAndAcquireGPUSwapchainTexture")) return false;
                err_ = "SDL_WaitAndAcquireGPUSwapchainTexture failed with a swapchain texture";
                discard_failed_commands();
                return false;
            }
            if (sw) {
                // The back buffer at its own aspect, centred, black bars on the sides (a wider
                // swapchain: a 19.5:9 phone, a resized window) or above and below (a taller one):
                // the blit's render pass clears the whole swapchain texture first. A swapchain of
                // the back buffer's aspect is covered whole, as before, with nothing cleared. The
                // player's picture place (host/player/player.cpp "The picture's place": the window's
                // properties "sa2.picture_y" and "sa2.picture_min_top") moves a letterboxed picture
                // up; a window without them is centred, as before.
                const Uint32 bw = it->second.d.w, bh = it->second.d.h;
                const SDL_PropertiesID wp = SDL_GetWindowProperties(win_);
                const FitRect dst = fit_rect(bw, bh, w, h, SDL_GetFloatProperty(wp, "sa2.picture_y", -1.0f),
                                             uint32_t(SDL_GetNumberProperty(wp, "sa2.picture_min_top", 0)));
                const bool whole = dst.x == 0 && dst.y == 0 && dst.w == w && dst.h == h;
                // Interpolating: the frame made between the last drawn frame and this one is shown
                // now, and this one is kept for present_pending (Interp).
                SDL_GPUTexture* shown = interp_ ? interp_make(cb, it->second.t, bw, bh, sdl_format(it->second.d.fmt))
                                                : it->second.t;
                SDL_GPUBlitInfo bi{};
                bi.source = SDL_GPUBlitRegion{shown, 0, 0, 0, 0, bw, bh};
                bi.destination = SDL_GPUBlitRegion{sw, 0, 0, dst.x, dst.y, dst.w, dst.h};
                bi.load_op = whole ? SDL_GPU_LOADOP_DONT_CARE : SDL_GPU_LOADOP_CLEAR;
                bi.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
                bi.flip_mode = SDL_FLIP_NONE;
                bi.filter = (dst.w == bw && dst.h == bh) ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
                bi.cycle = false;
                if (!encode_call("SDL_BlitGPUTexture", [&] { SDL_BlitGPUTexture(cb, &bi); })) return false;
                c_.blits++;
                // The window's present overlay (host/player/player.cpp "Gamepads": the touch
                // overlay): a function the player keeps in the window's properties, given the
                // frame and the swapchain texture it was just blitted to, draws over it in this
                // command buffer before the submission. A window without one presents as before.
                using PresentOverlay = void (*)(void*, SDL_GPUDevice*, SDL_GPUCommandBuffer*, SDL_GPUTexture*, Uint32,
                                                Uint32, SDL_GPUTexture*, Uint32, Uint32);
                if (void* overlay = SDL_GetPointerProperty(wp, "sa2.present_overlay", nullptr)) {
                    reinterpret_cast<PresentOverlay>(overlay)(SDL_GetPointerProperty(wp, "sa2.present_overlay.context", nullptr),
                        dev_, cb, it->second.t, it->second.d.w, it->second.d.h, sw, w, h);
                    SDL_ClearError();
                }
                out->presented = true;
                out->sw_w = w;
                out->sw_h = h;
            } else {
                out->no_swapchain_texture = true;
            }
        }
        return submit(false);
    }

    bool finish() override {
        const bool submitted = submit(true);
        // Even when there is no current command buffer, Present can have submissions pending.
        SDL_ClearError();
        const bool idle = SDL_WaitForGPUIdle(dev_);
        c_.waits++;
        const bool no_error = check_encoding_error("SDL_WaitForGPUIdle");
        if (idle && no_error) release_pending_fences();
        if (!idle && no_error) err_ = "SDL_WaitForGPUIdle failed";
        return submitted && idle && no_error;
    }

    bool attach_window(SDL_Window* w, bool vsync, std::string* mode) override {
        Report& r = report();
        r.claim_x87_before = x87_cw();
        r.claim_mxcsr_before = mxcsr();
        const bool ok = SDL_ClaimWindowForGPUDevice(dev_, w);
        r.claim_x87_after = x87_cw();
        r.claim_mxcsr_after = mxcsr();
        if (!ok) { err_ = std::string("SDL_ClaimWindowForGPUDevice: ") + SDL_GetError(); return false; }
        win_ = w;
        SDL_GPUPresentMode m = SDL_GPU_PRESENTMODE_VSYNC;
        if (!vsync) {
            if (SDL_WindowSupportsGPUPresentMode(dev_, w, SDL_GPU_PRESENTMODE_IMMEDIATE)) m = SDL_GPU_PRESENTMODE_IMMEDIATE;
            else if (SDL_WindowSupportsGPUPresentMode(dev_, w, SDL_GPU_PRESENTMODE_MAILBOX)) m = SDL_GPU_PRESENTMODE_MAILBOX;
        }
        *mode = m == SDL_GPU_PRESENTMODE_IMMEDIATE ? "IMMEDIATE" : m == SDL_GPU_PRESENTMODE_MAILBOX ? "MAILBOX" : "VSYNC";
        if (!SDL_SetGPUSwapchainParameters(dev_, w, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, m)) {
            err_ = std::string("SDL_SetGPUSwapchainParameters: ") + SDL_GetError();
            return false;
        }
        // Interpolation shows twice the game's frames, which only a display refreshing at about
        // twice 60 Hz can: Android's compositor takes one buffer a refresh, so on the 60 Hz panel
        // of a Moto One 5G Ace the second Present waited for the next and held the game to 30
        // frames a second (2026-10-29). Below kInterpMinHz it is off, said once.
        if (interp_) {
            const SDL_DisplayMode* dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(w));
            if (dm && dm->refresh_rate > 0.0f && dm->refresh_rate < kInterpMinHz) {
                std::fprintf(stderr, "d3d9gpu: SA2_PLAYER_INTERP: the display refreshes at %.0f Hz, below %.0f: "
                                     "frames are not interpolated\n", double(dm->refresh_rate), double(kInterpMinHz));
                interp_ = false;
            }
        }
        // The interpolation's second Present, which the player makes half a period after each
        // one (host/player/player.cpp, SA2_PLAYER_INTERP): a function in the window's properties,
        // as the player's present overlay is in the other direction.
        if (interp_) {
            const SDL_PropertiesID wp = SDL_GetWindowProperties(w);
            SDL_SetPointerProperty(wp, "sa2.interp.present_pending", reinterpret_cast<void*>(&SdlGpu::present_pending_thunk));
            SDL_SetPointerProperty(wp, "sa2.interp.context", this);
        }
        return true;
    }

    const GpuInfo& info() const override { return info_; }
    const GpuCounters& counters() const override { return c_; }
    const std::string& error() const override { return err_; }
    GFmt depth_format() const override { return depth_; }
    bool samples(GFmt f, TexType t) const override {
        const bool bc = f == GFmt::bc1 || f == GFmt::bc2 || f == GFmt::bc3;
        if (bc && info_.bc_forced_off) return false;
        const SDL_GPUTextureType type = t == TexType::cube ? SDL_GPU_TEXTURETYPE_CUBE
                                      : t == TexType::volume ? SDL_GPU_TEXTURETYPE_3D : SDL_GPU_TEXTURETYPE_2D;
        return SDL_GPUTextureSupportsFormat(dev_, sdl_format(f), type, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    }

private:
    struct Tex { SDL_GPUTexture* t; TexDesc d; SDL_GPUTextureUsageFlags usage; };
    struct Buf { SDL_GPUBuffer* b; uint32_t bytes; SDL_GPUBufferUsageFlags usage; };

    void discard_failed_commands() {
        poisoned_ = true;
        copy_ = nullptr;
        close_pass();
        up_copies_.clear();
        if (ring_map_) { SDL_UnmapGPUTransferBuffer(dev_, ring_); ring_map_ = nullptr; }
        if (cb_ && !swapchain_acquired_) {
            SDL_GPUCommandBuffer* failed = cb_;
            cb_ = nullptr;
            SDL_ClearError();
            if (!SDL_CancelGPUCommandBuffer(failed))
                err_ += std::string("; SDL_CancelGPUCommandBuffer: ") + SDL_GetError();
        }
        // Once a swapchain image was acquired, SDL forbids cancellation. The poisoned
        // command buffer remains quarantined until device teardown and is never submitted.
        free_up_chunks();
    }
    bool check_encoding_error(const char* operation) {
        const char* error = SDL_GetError();
        if (!error || !*error) return true;
        err_ = std::string(operation) + ": " + error;
        discard_failed_commands();
        return false;
    }
    template<class F> bool encode_call(const char* operation, F call) {
        if (poisoned_) return false;
        SDL_ClearError();
        call();
        return check_encoding_error(operation);
    }

    struct ShaderObject { SDL_GPUShader* shader; };
    struct ProgramObjects {
        draw::Program owner;
        ShaderObject* vertex;
        ShaderObject* pixel;
    };
    struct ShaderPair {
        const ShaderObject* vertex;
        const ShaderObject* pixel;
        bool operator==(const ShaderPair& o) const { return vertex == o.vertex && pixel == o.pixel; }
    };
    struct ShaderPairHash {
        size_t operator()(const ShaderPair& pair) const {
            return std::hash<const void*>{}(pair.vertex) ^ (std::hash<const void*>{}(pair.pixel) << 1u);
        }
    };
    // An UP chunk: its size, and the submission count (the command buffer) it was last written
    // and last allocated from in (UINT64_MAX: never written).
    struct UpChunk { SDL_GPUBuffer* buffer; uint32_t bytes; uint64_t written, used; };
    // The UP uploads not yet encoded (stage_up), from the ring into a chunk.
    struct UpCopy { SDL_GPUBuffer* buffer; uint32_t offset, destination, size, span; bool cycle; };
    // A command of a draw pass (record, replay): its variable part a span [at, at + n) of an arena.
    enum class Op : uint8_t { pipeline, vertex_buffers, index_buffer, vertex_samplers, fragment_samplers,
                              vertex_uniform, fragment_uniform, viewport, scissor, blend, stencil, draw, draw_indexed };
    struct Cmd {
        Op op = Op::draw;
        uint8_t small = 0;         // a uniform slot, an index element size, the stencil reference or a first vertex slot
        uint32_t at = 0, n = 0;
        union U {
            SDL_GPUGraphicsPipeline* pipeline;
            SDL_GPUBufferBinding buffer;
            SDL_GPUViewport viewport;
            SDL_Rect scissor;
            SDL_FColor blend;
            struct { uint32_t count, instances, first; int32_t base; } draw;
        } u{};
    };
    // A pipeline by its canonical key (key_bytes), in its hash's bucket.
    struct PipelineEntry { std::vector<uint8_t> key; SDL_GPUGraphicsPipeline* pipeline; };
    // A sampler by every field SDL_CreateGPUSampler reads and the border colour property.
    struct SamplerKey {
        std::array<uint32_t, 14> w;
        bool operator==(const SamplerKey& o) const { return same_bytes(w.data(), o.w.data(), sizeof w); }
    };
    struct SamplerKeyHash {
        size_t operator()(const SamplerKey& k) const { return size_t(memo_hash(k.w.data(), sizeof k.w)); }
    };

    static void append_u32(std::string& key, uint32_t value) {
        for (uint32_t i = 0; i < 4; ++i) key.push_back(char((value >> (8u * i)) & 255u));
    }
    static void append_text(std::string& key, const std::string& text) {
        append_u32(key, uint32_t(text.size())); key.append(text);
    }
    static uint32_t vertex_element_bytes(SDL_GPUVertexElementFormat format) {
        switch (format) {
        case SDL_GPU_VERTEXELEMENTFORMAT_INT: case SDL_GPU_VERTEXELEMENTFORMAT_UINT:
        case SDL_GPU_VERTEXELEMENTFORMAT_FLOAT: return 4;
        case SDL_GPU_VERTEXELEMENTFORMAT_INT2: case SDL_GPU_VERTEXELEMENTFORMAT_UINT2:
        case SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2: return 8;
        case SDL_GPU_VERTEXELEMENTFORMAT_INT3: case SDL_GPU_VERTEXELEMENTFORMAT_UINT3:
        case SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3: return 12;
        case SDL_GPU_VERTEXELEMENTFORMAT_INT4: case SDL_GPU_VERTEXELEMENTFORMAT_UINT4:
        case SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4: return 16;
        case SDL_GPU_VERTEXELEMENTFORMAT_BYTE2: case SDL_GPU_VERTEXELEMENTFORMAT_UBYTE2:
        case SDL_GPU_VERTEXELEMENTFORMAT_BYTE2_NORM: case SDL_GPU_VERTEXELEMENTFORMAT_UBYTE2_NORM: return 2;
        case SDL_GPU_VERTEXELEMENTFORMAT_BYTE4: case SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4:
        case SDL_GPU_VERTEXELEMENTFORMAT_BYTE4_NORM: case SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM:
        case SDL_GPU_VERTEXELEMENTFORMAT_SHORT2: case SDL_GPU_VERTEXELEMENTFORMAT_USHORT2:
        case SDL_GPU_VERTEXELEMENTFORMAT_SHORT2_NORM: case SDL_GPU_VERTEXELEMENTFORMAT_USHORT2_NORM:
        case SDL_GPU_VERTEXELEMENTFORMAT_HALF2: return 4;
        case SDL_GPU_VERTEXELEMENTFORMAT_SHORT4: case SDL_GPU_VERTEXELEMENTFORMAT_USHORT4:
        case SDL_GPU_VERTEXELEMENTFORMAT_SHORT4_NORM: case SDL_GPU_VERTEXELEMENTFORMAT_USHORT4_NORM:
        case SDL_GPU_VERTEXELEMENTFORMAT_HALF4: return 8;
        default: return 0;
        }
    }
    ShaderObject* shader_object(const shader::Built& b, const shader::CacheKey& identity, shader::Stage stage) {
        // Exact content is interned once per immutable ProgramImage. Hashing only indexes the
        // table; full identity, bindings and SPIR-V bytes decide equality.
        std::string key;
        append_u32(key, uint32_t(stage)); append_text(key, identity.text());
        append_text(key, shader::encode_bindings(b));
        append_u32(key, uint32_t(b.spirv.size()));
        for (uint32_t word : b.spirv) append_u32(key, word);
        auto found = shaders_.find(key);
        if (found != shaders_.end()) return found->second.get();
        SDL_GPUShaderCreateInfo ci{};
        ci.code_size = b.spirv.size() * sizeof(uint32_t);
        ci.code = reinterpret_cast<const Uint8*>(b.spirv.data());
        if (flat_ps_ && stage == shader::Stage::Pixel) {
            ci.code_size = sizeof kFlatPixelShader;
            ci.code = reinterpret_cast<const Uint8*>(kFlatPixelShader);
        }
        ci.entrypoint = "main"; ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
        ci.stage = stage == shader::Stage::Vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
        ci.num_samplers = b.num_samplers; ci.num_uniform_buffers = b.num_uniform_buffers;
        // SA2_D3D9GPU_DUMP_SPIRV=1 (on Android debug.sa2.dump_spirv=1), for reading the modules
        // offline (spirv-dis): each one SDL is given, into SA2_PLAY_OUT, in creation order.
        if (dump_spirv_) {
            const char* dir = std::getenv("SA2_PLAY_OUT");
            char path[512];
            std::snprintf(path, sizeof path, "%s/spirv-%03u-%s.spv", dir && *dir ? dir : ".", unsigned(shaders_.size()),
                          stage == shader::Stage::Vertex ? "vs" : "ps");
            if (FILE* f = std::fopen(path, "wb")) {
                std::fwrite(ci.code, 1, ci.code_size, f);
                std::fclose(f);
            }
        }
        auto object = std::make_unique<ShaderObject>(ShaderObject{nullptr});
        SDL_GPUShader* s = SDL_CreateGPUShader(dev_, &ci);
        if (!s) { err_ = std::string("SDL_CreateGPUShader: ") + SDL_GetError(); return nullptr; }
        c_.shaders_made++;
        object->shader = s;
        ShaderObject* result = object.get();
        try { shaders_.emplace(std::move(key), std::move(object)); }
        catch (...) { SDL_ReleaseGPUShader(dev_, s); throw; }
        return result;
    }
    ProgramObjects* program_objects(const draw::Program& program) {
        auto found = programs_.find(program.get());
        if (found != programs_.end()) return &found->second;
        ShaderObject* vs = shader_object(program->vertex, program->vertex_identity, shader::Stage::Vertex);
        if (!vs) return nullptr;
        ShaderObject* ps = shader_object(program->pixel, program->pixel_identity, shader::Stage::Pixel);
        if (!ps) return nullptr;
        return &programs_.emplace(program.get(), ProgramObjects{program, vs, ps}).first->second;
    }
    SDL_GPUGraphicsPipeline* pipeline_object(const PipelineDesc& desc, const ProgramObjects& program) {
        // key_of is the mapping's complete padding-free descriptor key. Interned module
        // identities retain exact executable equality, including PS variants and linkage.
        // The key's bytes on the stack and its hash index the pair's table; the bytes decide.
        static const PipelineInput kNoShaderIds{};
        uint8_t key[kPipelineKeyMaxBytes];
        const size_t n = key_bytes(desc, kNoShaderIds, false, key);
        auto& bucket = pipelines_[ShaderPair{program.vertex, program.pixel}][memo_hash(key, n)];
        for (const PipelineEntry& e : bucket)
            if (e.key.size() == n && same_bytes(e.key.data(), key, n)) return e.pipeline;
        CreateProps props(desc);
        if (!props.error().empty()) { err_ = props.error(); return nullptr; }
        auto ci = desc.create_info(props.id());
        ci.vertex_shader = program.vertex->shader; ci.fragment_shader = program.pixel->shader;
        const Uint64 t0 = SDL_GetTicksNS();
        SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(dev_, &ci);
        if (!pipeline) { err_ = std::string("SDL_CreateGPUGraphicsPipeline: ") + SDL_GetError(); return nullptr; }
        c_.pipelines_made++;
        c_.pipeline_make_ns += SDL_GetTicksNS() - t0;
        try { bucket.push_back(PipelineEntry{std::vector<uint8_t>(key, key + n), pipeline}); }
        catch (...) { SDL_ReleaseGPUGraphicsPipeline(dev_, pipeline); throw; }
        return pipeline;
    }
    SDL_GPUSampler* sampler_object(const SamplerDesc& desc) {
        const auto& fields = *desc.info;
        if (!std::isfinite(fields.mip_lod_bias) || !std::isfinite(fields.max_anisotropy) ||
            !std::isfinite(fields.min_lod) || !std::isfinite(fields.max_lod) || fields.min_lod < 0 ||
            fields.max_lod < fields.min_lod || (fields.enable_anisotropy && fields.max_anisotropy < 1)) {
            err_ = "invalid mapped sampler LOD or anisotropy interval"; return nullptr;
        }
        bool border = false;
        for (auto axis : {fields.address_mode_u, fields.address_mode_v, fields.address_mode_w})
            border |= axis == kAddressClampToBorder;
        if (border && (!info_.sampler_clamp_to_border || desc.border_colour < kBorderTransparentBlack ||
                       desc.border_colour > kBorderOpaqueWhite)) {
            err_ = "native border sampler requires an effective capability and explicit colour"; return nullptr;
        }
        if (!border && desc.border_colour != kBorderNone) {
            err_ = "border property on a sampler with no native border axis"; return nullptr;
        }
        auto bits = [](float value) { uint32_t b; std::memcpy(&b, &value, sizeof b); return b; };
        const SamplerKey key{{uint32_t(fields.min_filter), uint32_t(fields.mag_filter), uint32_t(fields.mipmap_mode),
            uint32_t(fields.address_mode_u), uint32_t(fields.address_mode_v), uint32_t(fields.address_mode_w),
            bits(fields.mip_lod_bias), bits(fields.max_anisotropy), bits(fields.min_lod), bits(fields.max_lod),
            uint32_t(fields.compare_op), uint32_t(fields.enable_anisotropy), uint32_t(fields.enable_compare),
            uint32_t(desc.border_colour)}};
        auto found = samplers_.find(key);
        if (found != samplers_.end()) return found->second;
        CreateProps props(desc);
        if (!props.error().empty()) { err_ = props.error(); return nullptr; }
        auto ci = desc.create_info(props.id());
        // A device made without samplerAnisotropy (make_sdl_gpu: one that lacks it) cannot filter
        // anisotropically, and SDL refuses the request there: the sampler is made without it, counted,
        // and said once. The streams recorded so far ask for none (D3DTEXF_ANISOTROPIC never set); the
        // host tweak tex-filter does.
        if (ci.enable_anisotropy && !info_.sampler_anisotropy) {
            if (!report().anisotropy_dropped_samplers) {
                std::fprintf(stderr, "d3d9gpu: a sampler asks for anisotropic filtering (%g) on a device made without "
                                     "samplerAnisotropy: made without it\n", double(ci.max_anisotropy));
                // the host tweak tex-filter asks for it on every mip-mapped stage (host/player/tweaks/tex-filter.h)
                if (sa2tw::on(sa2tw::SA2_TWEAK_TEX_FILTER))
                    std::fprintf(stderr, "sa2 host tweaks: texture filter 16x: this device has no samplerAnisotropy: "
                                         "its samplers are made without anisotropy, and the mip bias is kept\n");
            }
            report().anisotropy_dropped_samplers++;
            ci.enable_anisotropy = false;
            ci.max_anisotropy = 0.0f;
        }
        // The measuring switch tiny_mips_: every sampler reads its texture's smallest level, so the
        // texture fetches hit the cache and the shaders' arithmetic is left as it is.
        if (tiny_mips_) { ci.min_lod = ci.max_lod = 1000.0f; ci.enable_anisotropy = false; }
        SDL_GPUSampler* sampler = SDL_CreateGPUSampler(dev_, &ci);
        if (!sampler) { err_ = std::string("SDL_CreateGPUSampler: ") + SDL_GetError(); return nullptr; }
        try { samplers_.emplace(key, sampler); }
        catch (...) { SDL_ReleaseGPUSampler(dev_, sampler); throw; }
        return sampler;
    }
    bool upload_up(const std::vector<uint8_t>& bytes, SDL_GPUBufferBinding& binding) {
        // The chunks outlive the command buffer: each command buffer fills them from the first
        // again, only advancing, so later draws never overwrite an encoded draw's vertices or
        // indices. A chunk's first write in a command buffer cycles it (SDL's cycle flag), which
        // gives it a native buffer no earlier command buffer still reads (SDL keeps the ones it
        // made and hands back an idle one), and every later write and bind in the command buffer
        // goes to that one. So no buffer is created or released frame after frame.
        if (bytes.empty() || bytes.size() > UINT32_MAX - 63u) { err_ = "UP allocation is empty or too large"; return false; }
        const uint32_t need = (uint32_t(bytes.size()) + 63u) & ~63u;
        UpChunk* chunk = up_allocate(need);
        if (!chunk) return false;
        const uint32_t destination = up_next_ - need;
        SDL_GPUTransferBuffer* transfer = nullptr;
        uint32_t offset = 0;
        if (!stage(bytes.data(), bytes.size(), &transfer, &offset)) return false;
        SDL_GPUCopyPass* pass = copy_pass();
        if (!pass) { release_dedicated(transfer); return false; }
        SDL_GPUTransferBufferLocation source{transfer, offset};
        SDL_GPUBufferRegion target{chunk->buffer, destination, uint32_t(bytes.size())};
        const bool cycle = chunk->written != c_.submits;
        chunk->written = c_.submits;
        if (!encode_call("SDL_UploadToGPUBuffer", [&] { SDL_UploadToGPUBuffer(pass, &source, &target, cycle); })) { release_dedicated(transfer); return false; }
        release_dedicated(transfer);
        binding = SDL_GPUBufferBinding{chunk->buffer, destination};
        c_.uploads++; c_.upload_bytes += bytes.size();
        return true;
    }
    // `need` bytes of the command buffer's UP space: the current chunk's next bytes, or the next
    // chunk with room (a new one when none has), at up_next_ - need in the chunk returned.
    UpChunk* up_allocate(uint32_t need) {
        constexpr uint32_t kUpChunk = 1u << 20;
        while (up_cursor_ < up_chunks_.size() && need > up_chunks_[up_cursor_].bytes - up_next_) { up_cursor_++; up_next_ = 0; }
        if (up_cursor_ == up_chunks_.size()) {
            const uint32_t size = std::max(kUpChunk, need);
            SDL_GPUBufferCreateInfo ci{SDL_GPU_BUFFERUSAGE_VERTEX | SDL_GPU_BUFFERUSAGE_INDEX, size, 0};
            SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(dev_, &ci);
            if (!buffer) { err_ = std::string("SDL_CreateGPUBuffer(UP): ") + SDL_GetError(); return nullptr; }
            try { up_chunks_.push_back(UpChunk{buffer, size, UINT64_MAX, c_.submits}); }
            catch (...) { SDL_ReleaseGPUBuffer(dev_, buffer); throw; }
            up_next_ = 0;
        }
        UpChunk& chunk = up_chunks_[up_cursor_];
        chunk.used = c_.submits;
        up_next_ += need;
        return &chunk;
    }
    // At a submission, the command buffer's hold on the UP space ends: the next one fills the
    // chunks from the first again, and a chunk none has allocated from in kUpIdle submissions is
    // released (SDL defers that past the GPU's use).
    void release_up_chunks() {
        constexpr uint64_t kUpIdle = 600;
        up_cursor_ = 0; up_next_ = 0;
        size_t kept = 0;
        for (size_t i = 0; i < up_chunks_.size(); ++i) {
            if (c_.submits - up_chunks_[i].used > kUpIdle) SDL_ReleaseGPUBuffer(dev_, up_chunks_[i].buffer);
            else up_chunks_[kept++] = up_chunks_[i];
        }
        up_chunks_.resize(kept);
    }
    // Every chunk released (the device's end, or a discarded command buffer).
    void free_up_chunks() {
        for (const auto& chunk : up_chunks_) SDL_ReleaseGPUBuffer(dev_, chunk.buffer);
        up_chunks_.clear();
        up_cursor_ = 0; up_next_ = 0;
    }
    // The draw pass named by `key` (its attachments), open for the draw to join: the open one when
    // it is that pass, else a new one after the open one is flushed. Nothing reaches SDL until
    // flush_pass, so a DrawPrimitiveUP draw's upload no longer ends the pass it draws in.
    bool open_pass(const std::string& key, const std::vector<SDL_GPUColorTargetInfo>& colours,
                   const SDL_GPUDepthStencilTargetInfo* depth, uint32_t w, uint32_t h) {
        if (poisoned_) return false;
        if (pass_open_ && pass_key_.size() == key.size() && same_bytes(pass_key_.data(), key.data(), key.size())) return true;
        if (!flush_pass()) return false;
        pass_key_ = key;
        pass_colours_ = colours;
        pass_has_depth_ = depth != nullptr;
        if (depth) pass_depth_ = *depth;
        pass_w_ = w; pass_h_ = h;
        pass_open_ = true;
        bound_.reset();
        return true;
    }
    // Ends the draw pass (end_render's work): the pending UP uploads in a copy pass, then the open
    // pass's commands in one SDL render pass, LOAD and STORE on every attachment unless a pending
    // clear becomes the load op (take_pending_clears) or a clear right after drops the store
    // (clear_layer). After it no render pass is open, so a copy, clear, Present or submission may
    // follow.
    bool flush_pass() {
        if (poisoned_) return false;
        if (!encode_up_copies()) return false;
        if (!pass_open_) return true;
        if (cmds_.empty()) { close_pass(); return true; }
        if (!end_copy()) return false;
        if (!take_pending_clears()) return false;
        SDL_GPUCommandBuffer* cb = cmd();
        if (!cb) return false;
        SDL_ClearError();
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cb, pass_colours_.data(), uint32_t(pass_colours_.size()),
                                                         pass_has_depth_ ? &pass_depth_ : nullptr);
        if (!pass) {
            err_ = std::string("SDL_BeginGPURenderPass(draw): ") + SDL_GetError();
            discard_failed_commands();
            return false;
        }
        c_.render_passes++;
        replay(pass);
        SDL_EndGPURenderPass(pass);
        close_pass();
        return check_encoding_error("the draw pass's SDL calls");
    }
    bool end_render() { return flush_pass(); }
    void close_pass() {
        pass_open_ = false;
        pass_key_.clear();
        cmds_.clear(); rec_bindings_.clear(); rec_samplers_.clear(); rec_uniforms_.clear();
    }
    // flush_pass's share of the pending clears, before its SDL render pass begins: one of an
    // attachment becomes that attachment's load op (on a tiled GPU neither the clear's write-out
    // nor the pass's read-back happens), and one of a texture the pass's draws sample is encoded
    // first as its own pass. The pass's draws were all recorded after those clears (clear_layer
    // flushes the open pass), so every one of them sees the cleared contents. A load op acts on
    // the render area only, which SDL makes the smallest attachment's size: an attachment larger
    // than that (a depth buffer under a smaller target) has its clear encoded first instead.
    bool take_pending_clears() {
        for (size_t i = 0; i < pending_clears_.size();) {
            const PendingClear& q = pending_clears_[i];
            SDL_GPUColorTargetInfo* colour = nullptr;
            bool attached = false;
            if (q.ds) {
                attached = pass_has_depth_ && pass_depth_.texture == q.texture && pass_depth_.layer == q.layer &&
                           pass_depth_.mip_level == q.level;
            } else {
                for (SDL_GPUColorTargetInfo& c : pass_colours_)
                    if (c.texture == q.texture && c.layer_or_depth_plane == q.layer && c.mip_level == q.level) {
                        colour = &c;
                        attached = true;
                    }
            }
            const auto t = tex_.find(q.id);
            const bool whole = t != tex_.end() && level_dim(t->second.d.w, q.level) == pass_w_ &&
                               level_dim(t->second.d.h, q.level) == pass_h_;
            if (attached && whole) {
                if (q.ds) {
                    if (q.depth) { pass_depth_.load_op = SDL_GPU_LOADOP_CLEAR; pass_depth_.clear_depth = q.z; }
                    if (q.stencil) { pass_depth_.stencil_load_op = SDL_GPU_LOADOP_CLEAR; pass_depth_.clear_stencil = q.s; }
                } else {
                    colour->load_op = SDL_GPU_LOADOP_CLEAR;
                    colour->clear_color = q.rgba;
                }
                pending_clears_.erase(pending_clears_.begin() + std::ptrdiff_t(i));
                c_.clears_folded++;
                continue;
            }
            if (attached || std::any_of(rec_samplers_.begin(), rec_samplers_.end(),
                                        [&](const SDL_GPUTextureSamplerBinding& b) { return b.texture == q.texture; })) {
                if (!encode_pending_clear(i)) return false;
                continue;
            }
            ++i;
        }
        return true;
    }
    // clear_layer's part of the open pass: the stores of the aspects a whole clear of (t, layer,
    // level) is about to overwrite, which nothing reads in between.
    void drop_cleared_stores(SDL_GPUTexture* t, uint32_t layer, uint32_t level, bool ds, const ClearValue& v) {
        if (ds) {
            if (pass_has_depth_ && pass_depth_.texture == t && pass_depth_.layer == layer && pass_depth_.mip_level == level) {
                if (v.depth) pass_depth_.store_op = SDL_GPU_STOREOP_DONT_CARE;
                if (v.stencil) pass_depth_.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
            }
        } else if (v.colour) {
            for (SDL_GPUColorTargetInfo& c : pass_colours_)
                if (c.texture == t && c.layer_or_depth_plane == layer && c.mip_level == level)
                    c.store_op = SDL_GPU_STOREOP_DONT_CARE;
        }
    }
    // The pending clears of texture `id`'s subresource (layer, level) encoded, before an operation
    // outside a draw pass reads or writes it; the open pass may take one as a load op first.
    bool settle_clears(uint32_t id, uint32_t layer, uint32_t level) {
        auto hit = [&](const PendingClear& q) { return q.id == id && q.layer == layer && q.level == level; };
        if (std::none_of(pending_clears_.begin(), pending_clears_.end(), hit)) return true;
        if (!end_render()) return false;
        for (size_t i = 0; i < pending_clears_.size();) {
            if (!hit(pending_clears_[i])) { ++i; continue; }
            if (!encode_pending_clear(i)) return false;
        }
        return true;
    }
    // Pending clear `i` as its own render pass (the encoding clear_layer made at once before),
    // removed from the list. No SDL render pass may be open.
    bool encode_pending_clear(size_t i) {
        const PendingClear q = pending_clears_[i];
        pending_clears_.erase(pending_clears_.begin() + std::ptrdiff_t(i));
        if (!end_copy()) return false;
        SDL_GPUCommandBuffer* cb = cmd();
        if (!cb) return false;
        SDL_GPURenderPass* rp = nullptr;
        if (q.ds) {
            SDL_GPUDepthStencilTargetInfo ds{};
            ds.texture = q.texture;
            ds.clear_depth = q.z;
            ds.load_op = q.depth ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
            ds.store_op = SDL_GPU_STOREOP_STORE;
            ds.stencil_load_op = q.stencil ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
            ds.stencil_store_op = SDL_GPU_STOREOP_STORE;
            ds.cycle = false;
            ds.clear_stencil = q.s;
            ds.mip_level = Uint8(q.level);
            ds.layer = Uint8(q.layer);
            if (!encode_call("SDL_BeginGPURenderPass(clear)", [&] { rp = SDL_BeginGPURenderPass(cb, nullptr, 0, &ds); })) return false;
        } else {
            SDL_GPUColorTargetInfo ct{};
            ct.texture = q.texture;
            ct.mip_level = q.level;
            ct.layer_or_depth_plane = q.layer;
            ct.clear_color = q.rgba;
            ct.load_op = SDL_GPU_LOADOP_CLEAR;
            ct.store_op = SDL_GPU_STOREOP_STORE;
            ct.cycle = false;
            if (!encode_call("SDL_BeginGPURenderPass(clear)", [&] { rp = SDL_BeginGPURenderPass(cb, &ct, 1, nullptr); })) return false;
        }
        if (!rp) { err_ = std::string("SDL_BeginGPURenderPass: ") + SDL_GetError(); return false; }
        if (!encode_call("SDL_EndGPURenderPass(clear)", [&] { SDL_EndGPURenderPass(rp); })) return false;
        c_.render_passes++;
        return true;
    }
    Cmd& record(Op op) {
        cmds_.emplace_back();
        Cmd& c = cmds_.back();
        c.op = op;
        return c;
    }
    // Slots 0..n-1 as `b`, binding only the run of slots that differs from what the open pass
    // holds: a draw usually changes stream 0 alone, and SDL tracks each buffer of a bind, so
    // rebinding the semantic-null filler up to slot 15 at every draw cost the render thread more
    // than the draw's own buffers.
    void record_vertex_buffers(const SDL_GPUBufferBinding* b, uint32_t n) {
        auto same = [&](uint32_t i) { return bound_.vb[i].buffer == b[i].buffer && bound_.vb[i].offset == b[i].offset; };
        const uint32_t known = std::min(n, bound_.n_vbs);
        uint32_t first = 0, last = n;
        while (first < known && same(first)) ++first;
        if (first == n) return;
        while (last > first && last - 1 < known && same(last - 1)) --last;
        // The arena first, the command after: an allocation that throws (draw() turns bad_alloc
        // into a Failure) then leaves no command whose span has nothing behind it.
        const uint32_t at = uint32_t(rec_bindings_.size());
        rec_bindings_.insert(rec_bindings_.end(), b + first, b + last);
        Cmd& c = record(Op::vertex_buffers);
        c.small = uint8_t(first); c.at = at; c.n = last - first;
        std::copy(b + first, b + last, bound_.vb.begin() + first);
        bound_.n_vbs = std::max(bound_.n_vbs, last);
    }
    void record_index_buffer(const SDL_GPUBufferBinding& b, SDL_GPUIndexElementSize size) {
        if (bound_.has_index && bound_.index.buffer == b.buffer && bound_.index.offset == b.offset && bound_.index_size == size)
            return;
        Cmd& c = record(Op::index_buffer);
        c.u.buffer = b; c.small = uint8_t(size);
        bound_.index = b; bound_.index_size = size; bound_.has_index = true;
    }
    void record_samplers(int stage, const std::vector<SDL_GPUTextureSamplerBinding>& b) {
        const uint32_t n = uint32_t(b.size());
        if (bound_.n_samplers[stage] == n) {
            bool same = true;
            for (uint32_t i = 0; i < n && same; ++i) {
                const SDL_GPUTextureSamplerBinding& was = rec_samplers_[bound_.samplers[stage] + i];
                same = was.texture == b[i].texture && was.sampler == b[i].sampler;
            }
            if (same) return;
        }
        const uint32_t at = uint32_t(rec_samplers_.size());   // the arena first (record_vertex_buffers)
        rec_samplers_.insert(rec_samplers_.end(), b.begin(), b.end());
        Cmd& c = record(stage ? Op::fragment_samplers : Op::vertex_samplers);
        c.at = at; c.n = n;
        bound_.samplers[stage] = at; bound_.n_samplers[stage] = n;
    }
    // A uniform slot's bytes, pushed when they differ from what the pass last pushed there. SDL keeps
    // a slot's data for the command buffer; it is tracked per pass, so a push from outside the draw
    // passes (the present overlay's) is never mistaken for ours.
    void record_uniform(int stage, uint32_t slot, const std::vector<uint8_t>& bytes) {
        const uint32_t bit = 1u << (stage * 4 + slot);
        const uint32_t n = uint32_t(bytes.size());
        if ((bound_.uniform_valid & bit) && bound_.uniform_n[stage][slot] == n &&
            same_bytes(rec_uniforms_.data() + bound_.uniform_at[stage][slot], bytes.data(), n))
            return;
        const uint32_t at = uint32_t(rec_uniforms_.size());   // the arena first (record_vertex_buffers)
        rec_uniforms_.insert(rec_uniforms_.end(), bytes.begin(), bytes.end());
        Cmd& c = record(stage ? Op::fragment_uniform : Op::vertex_uniform);
        c.small = uint8_t(slot); c.at = at; c.n = n;
        bound_.uniform_at[stage][slot] = at; bound_.uniform_n[stage][slot] = n;
        bound_.uniform_valid |= bit;
    }
    void replay(SDL_GPURenderPass* pass) {
        for (const Cmd& c : cmds_) {
            switch (c.op) {
            case Op::pipeline: SDL_BindGPUGraphicsPipeline(pass, c.u.pipeline); break;
            case Op::vertex_buffers: SDL_BindGPUVertexBuffers(pass, c.small, rec_bindings_.data() + c.at, c.n); break;
            case Op::index_buffer: SDL_BindGPUIndexBuffer(pass, &c.u.buffer, SDL_GPUIndexElementSize(c.small)); break;
            case Op::vertex_samplers: SDL_BindGPUVertexSamplers(pass, 0, rec_samplers_.data() + c.at, c.n); break;
            case Op::fragment_samplers: SDL_BindGPUFragmentSamplers(pass, 0, rec_samplers_.data() + c.at, c.n); break;
            case Op::vertex_uniform: SDL_PushGPUVertexUniformData(cb_, c.small, rec_uniforms_.data() + c.at, c.n); break;
            case Op::fragment_uniform: SDL_PushGPUFragmentUniformData(cb_, c.small, rec_uniforms_.data() + c.at, c.n); break;
            case Op::viewport: SDL_SetGPUViewport(pass, &c.u.viewport); break;
            case Op::scissor: SDL_SetGPUScissor(pass, &c.u.scissor); break;
            case Op::blend: SDL_SetGPUBlendConstants(pass, c.u.blend); break;
            case Op::stencil: SDL_SetGPUStencilReference(pass, c.small); break;
            case Op::draw: SDL_DrawGPUPrimitives(pass, c.u.draw.count, 1, c.u.draw.first, 0); break;
            case Op::draw_indexed:
                SDL_DrawGPUIndexedPrimitives(pass, c.u.draw.count, c.u.draw.instances, c.u.draw.first, c.u.draw.base, 0);
                break;
            }
        }
    }
    // A DrawPrimitiveUP block into the command buffer's UP space, its upload left pending
    // (encode_up_copies) so the pass it draws in stays open; contiguous blocks (each padded to 64
    // bytes in both the ring and the chunk) become one upload. A block larger than the ring is
    // uploaded at once (upload_up), which ends the open pass as before.
    bool stage_up(const std::vector<uint8_t>& bytes, SDL_GPUBufferBinding& binding) {
        if (bytes.empty() || bytes.size() > UINT32_MAX - 63u) { err_ = "UP allocation is empty or too large"; return false; }
        if (bytes.size() > kRing) return upload_up(bytes, binding);
        const uint32_t need = (uint32_t(bytes.size()) + 63u) & ~63u;
        UpChunk* chunk = up_allocate(need);
        if (!chunk) return false;
        const uint32_t destination = up_next_ - need;
        SDL_GPUTransferBuffer* transfer = nullptr;
        uint32_t offset = 0;
        if (!stage(bytes.data(), bytes.size(), &transfer, &offset, 64)) return false;
        const bool cycle = chunk->written != c_.submits;
        chunk->written = c_.submits;
        UpCopy* last = up_copies_.empty() ? nullptr : &up_copies_.back();
        if (!cycle && last && last->buffer == chunk->buffer && last->offset + last->span == offset &&
            last->destination + last->span == destination) {
            last->size = last->span + uint32_t(bytes.size());
            last->span += need;
        } else {
            up_copies_.push_back(UpCopy{chunk->buffer, offset, destination, uint32_t(bytes.size()), need, cycle});
        }
        binding = SDL_GPUBufferBinding{chunk->buffer, destination};
        c_.uploads++; c_.upload_bytes += bytes.size();
        return true;
    }
    // The pending UP uploads, from the ring, in a copy pass: before the render pass of any draw that
    // reads them (flush_pass), and before the ring is mapped anew (stage), whose cycling would give
    // a later upload a different buffer than the one they were written to.
    bool encode_up_copies() {
        if (up_copies_.empty()) return true;
        SDL_GPUCopyPass* pass = open_copy();
        if (!pass) return false;
        SDL_ClearError();
        for (const UpCopy& u : up_copies_) {
            SDL_GPUTransferBufferLocation source{ring_, u.offset};
            SDL_GPUBufferRegion target{u.buffer, u.destination, u.size};
            SDL_UploadToGPUBuffer(pass, &source, &target, u.cycle);
        }
        up_copies_.clear();
        return check_encoding_error("SDL_UploadToGPUBuffer(UP)");
    }

    SDL_GPUCommandBuffer* cmd() {
        if (poisoned_) return nullptr;
        if (!cb_) {
            if (!encode_call("SDL_AcquireGPUCommandBuffer", [&] { cb_ = SDL_AcquireGPUCommandBuffer(dev_); })) return nullptr;
            if (!cb_) { err_ = std::string("SDL_AcquireGPUCommandBuffer: ") + SDL_GetError(); return nullptr; }
            c_.command_buffers++;
        }
        return cb_;
    }
    SDL_GPUCopyPass* copy_pass() {
        if (!end_render()) return nullptr;
        return open_copy();
    }
    // A copy pass, the logical draw pass left as it is (no SDL render pass is open outside flush_pass).
    SDL_GPUCopyPass* open_copy() {
        if (poisoned_) return nullptr;
        if (!copy_) {
            SDL_GPUCommandBuffer* cb = cmd();
            if (!cb) return nullptr;
            if (!encode_call("SDL_BeginGPUCopyPass", [&] { copy_ = SDL_BeginGPUCopyPass(cb); })) return nullptr;
            if (!copy_) err_ = std::string("SDL_BeginGPUCopyPass: ") + SDL_GetError();
        }
        return copy_;
    }
    bool end_copy() {
        if (poisoned_) return false;
        if (copy_) {
            SDL_GPUCopyPass* pass = copy_; copy_ = nullptr;
            return encode_call("SDL_EndGPUCopyPass", [&] { SDL_EndGPUCopyPass(pass); });
        }
        return true;
    }
    // `bytes` copied into the ring (or a transfer buffer of their own when larger); the
    // buffer and offset to upload from.
    bool stage(const void* data, size_t bytes, SDL_GPUTransferBuffer** tb, uint32_t* off, uint32_t align = 16) {
        if (!data || !bytes || bytes > UINT32_MAX) { err_ = "invalid transfer input size"; return false; }
        if (bytes > kRing) {
            SDL_GPUTransferBufferCreateInfo ci{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, uint32_t(bytes), 0};
            SDL_GPUTransferBuffer* t = nullptr;
            if (!encode_call("SDL_CreateGPUTransferBuffer(stage)", [&] { t = SDL_CreateGPUTransferBuffer(dev_, &ci); })) {
                if (t) SDL_ReleaseGPUTransferBuffer(dev_, t);
                return false;
            }
            void* p = nullptr;
            if (t && !encode_call("SDL_MapGPUTransferBuffer(stage)", [&] { p = SDL_MapGPUTransferBuffer(dev_, t, false); })) {
                if (p) SDL_UnmapGPUTransferBuffer(dev_, t);
                SDL_ReleaseGPUTransferBuffer(dev_, t);
                return false;
            }
            if (!p) { err_ = std::string("a dedicated transfer buffer: ") + SDL_GetError(); if (t) SDL_ReleaseGPUTransferBuffer(dev_, t); return false; }
            std::memcpy(p, data, bytes);
            if (!encode_call("SDL_UnmapGPUTransferBuffer(stage)", [&] { SDL_UnmapGPUTransferBuffer(dev_, t); })) {
                SDL_ReleaseGPUTransferBuffer(dev_, t); return false;
            }
            *tb = t;
            *off = 0;
            dedicated_ = t;
            c_.dedicated_transfers++;
            return true;
        }
        if (!ring_) { err_ = "no transfer ring"; return false; }
        if (ring_map_ && uint64_t(ring_off_) + bytes > kRing) {
            if (!encode_up_copies()) return false;
            ring_map_ = nullptr;
            if (!encode_call("SDL_UnmapGPUTransferBuffer(ring)", [&] { SDL_UnmapGPUTransferBuffer(dev_, ring_); })) return false;
        }
        if (!ring_map_) {
            if (!encode_call("SDL_MapGPUTransferBuffer(ring)", [&] {
                    ring_map_ = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(dev_, ring_, true));
                })) return false;
            if (!ring_map_) { err_ = std::string("SDL_MapGPUTransferBuffer: ") + SDL_GetError(); return false; }
            ring_off_ = 0;
            c_.ring_cycles++;
        }
        std::memcpy(ring_map_ + ring_off_, data, bytes);
        *tb = ring_;
        *off = ring_off_;
        ring_off_ = uint32_t((uint64_t(ring_off_) + bytes + align - 1u) & ~uint64_t(align - 1u));
        return true;
    }
    void release_dedicated(SDL_GPUTransferBuffer* tb) {
        if (tb && tb == dedicated_) { SDL_ReleaseGPUTransferBuffer(dev_, tb); dedicated_ = nullptr; }
    }
    void release_pending_fences() {
        for (uint32_t i = 0; i < pending_count_; ++i) SDL_ReleaseGPUFence(dev_, pending_[i]);
        pending_count_ = 0;
    }
    bool submit(bool wait) {
        if (!end_render() || !end_copy()) return false;
        if (ring_map_) {
            ring_map_ = nullptr;
            if (!encode_call("SDL_UnmapGPUTransferBuffer(submit)", [&] { SDL_UnmapGPUTransferBuffer(dev_, ring_); })) return false;
        }
        if (!cb_) { release_up_chunks(); return true; }
        // Swapchain acquisition throttles windowed frames, but a headless stream has no
        // such wait. Bound submissions so command buffers and their transient resources
        // cannot accumulate faster than the GPU completes them.
        if (pending_count_ == pending_.size()) {
            SDL_ClearError();
            const bool ready = SDL_WaitForGPUFences(dev_, true, pending_.data(), 1);
            c_.waits++;
            if (!check_encoding_error("SDL_WaitForGPUFences(backpressure)")) return false;
            if (!ready) {
                err_ = "SDL_WaitForGPUFences(backpressure) failed";
                discard_failed_commands();
                return false;
            }
            SDL_ReleaseGPUFence(dev_, pending_[0]);
            pending_[0] = pending_[1];
            --pending_count_;
        }
        SDL_GPUCommandBuffer* cb = cb_;
        cb_ = nullptr;
        swapchain_acquired_ = false;
        c_.submits++;
        release_up_chunks();
        SDL_ClearError();
        SDL_GPUFence* f = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
        if (f) pending_[pending_count_++] = f;
        if (!check_encoding_error("SDL_SubmitGPUCommandBufferAndAcquireFence")) return false;
        if (!f) { err_ = "SDL_SubmitGPUCommandBufferAndAcquireFence failed"; discard_failed_commands(); return false; }
        if (!wait) return true;
        SDL_ClearError();
        const bool ok = SDL_WaitForGPUFences(dev_, true, &f, 1);
        const bool no_error = check_encoding_error("SDL_WaitForGPUFences");
        // All earlier submissions on SDL's graphics queue completed before this fence.
        if (ok && no_error) release_pending_fences();
        c_.waits++;
        if (!ok && no_error) { err_ = "SDL_WaitForGPUFences failed"; discard_failed_commands(); }
        return ok && no_error;
    }

    // ------------------------------------------------------------ frame interpolation
    //
    // SA2_PLAYER_INTERP=1 (on Android also debug.sa2.interp=1): one frame made between every two the
    // game draws, from the two pictures alone (host/d3d9gpu/interp_shaders.py has the passes and
    // why): at a Present the back buffer is copied into the ring, its luma made at 1/scale, each
    // 4 x 4 luma block's motion into the previous frame found, and the frame half way between them
    // drawn and shown; the drawn frame is shown by present_pending, which the player calls half a
    // period later (host/player/player.cpp). Every pass is on the render thread, in its command
    // buffers. A device that cannot make the passes' targets shows each drawn frame as before.
    static constexpr float kInterpMismatch = 0.08f;   // a block's mean luma difference past which it did not match
    static constexpr float kInterpMinHz = 110.0f;     // the least refresh rate that shows every frame made

    static int present_pending_thunk(void* self) { return static_cast<SdlGpu*>(self)->present_pending(); }

    // The drawn frame the last Present kept, shown now: 1 shown, 0 nothing to show (or no surface
    // to show it on), -1 an SDL failure (said once on stderr).
    int present_pending() {
        Interp& p = ip_;
        const int slot = p.pending;
        p.pending = -1;
        if (slot < 0 || !win_ || surface_gone_ || poisoned_) return 0;
        const SDL_PropertiesID wp = SDL_GetWindowProperties(win_);
#ifdef __ANDROID__
        if (!SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr)) return 0;
#endif
        SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(dev_);
        if (!cb) return interp_fail("SDL_AcquireGPUCommandBuffer");
        SDL_GPUTexture* sw = nullptr;
        Uint32 w = 0, h = 0;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(cb, win_, &sw, &w, &h)) {
            SDL_CancelGPUCommandBuffer(cb);
            return 0;
        }
        if (sw) {
            const FitRect dst = fit_rect(p.w, p.h, w, h, SDL_GetFloatProperty(wp, "sa2.picture_y", -1.0f),
                                         uint32_t(SDL_GetNumberProperty(wp, "sa2.picture_min_top", 0)));
            const bool whole = dst.x == 0 && dst.y == 0 && dst.w == w && dst.h == h;
            SDL_GPUBlitInfo bi{};
            bi.source = SDL_GPUBlitRegion{p.frame[slot], 0, 0, 0, 0, p.w, p.h};
            bi.destination = SDL_GPUBlitRegion{sw, 0, 0, dst.x, dst.y, dst.w, dst.h};
            bi.load_op = whole ? SDL_GPU_LOADOP_DONT_CARE : SDL_GPU_LOADOP_CLEAR;
            bi.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
            bi.flip_mode = SDL_FLIP_NONE;
            bi.filter = (dst.w == p.w && dst.h == p.h) ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
            SDL_BlitGPUTexture(cb, &bi);
            using PresentOverlay = void (*)(void*, SDL_GPUDevice*, SDL_GPUCommandBuffer*, SDL_GPUTexture*, Uint32,
                                            Uint32, SDL_GPUTexture*, Uint32, Uint32);
            if (void* overlay = SDL_GetPointerProperty(wp, "sa2.present_overlay", nullptr))
                reinterpret_cast<PresentOverlay>(overlay)(SDL_GetPointerProperty(wp, "sa2.present_overlay.context", nullptr),
                    dev_, cb, p.frame[slot], p.w, p.h, sw, w, h);
        }
        if (!SDL_SubmitGPUCommandBuffer(cb)) return interp_fail("SDL_SubmitGPUCommandBuffer");
        SDL_ClearError();
        if (!sw) return 0;
        p.pending_shown++;
        return 1;
    }

    int interp_fail(const char* what) {
        if (!ip_.failed) std::fprintf(stderr, "d3d9gpu: frame interpolation: %s: %s; drawn frames are shown alone from now on\n",
                                      what, SDL_GetError());
        ip_.failed = true;
        ip_.pending = -1;
        return -1;
    }

    // The passes' shaders, pipelines and samplers (once), and their targets at this back buffer's
    // size and format (again when either changes). False: interpolation is off for the session.
    bool interp_ready(uint32_t bw, uint32_t bh, SDL_GPUTextureFormat fmt) {
        Interp& p = ip_;
        if (p.failed) return false;
        if (!p.vs) {
            auto shader = [&](const uint32_t* code, size_t bytes, SDL_GPUShaderStage stage, Uint32 samplers) {
                SDL_GPUShaderCreateInfo ci{};
                ci.code = reinterpret_cast<const Uint8*>(code);
                ci.code_size = bytes;
                ci.entrypoint = "main";
                ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
                ci.stage = stage;
                ci.num_samplers = samplers;
                ci.num_uniform_buffers = stage == SDL_GPU_SHADERSTAGE_FRAGMENT ? 1 : 0;
                return SDL_CreateGPUShader(dev_, &ci);
            };
            using namespace sa2_interp_spirv;
            p.vs = shader(kVertex, sizeof kVertex, SDL_GPU_SHADERSTAGE_VERTEX, 0);
            p.fs[0] = shader(kLuma, sizeof kLuma, SDL_GPU_SHADERSTAGE_FRAGMENT, 1);
            p.fs[1] = shader(kMotion, sizeof kMotion, SDL_GPU_SHADERSTAGE_FRAGMENT, 2);
            p.fs[2] = shader(kWarp, sizeof kWarp, SDL_GPU_SHADERSTAGE_FRAGMENT, 3);
            if (!p.vs || !p.fs[0] || !p.fs[1] || !p.fs[2]) return interp_fail("SDL_CreateGPUShader") == 0;
            SDL_GPUSamplerCreateInfo si{};
            si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;
            si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
            si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
            p.linear = SDL_CreateGPUSampler(dev_, &si);
            si.min_filter = si.mag_filter = SDL_GPU_FILTER_NEAREST;
            p.nearest = SDL_CreateGPUSampler(dev_, &si);
            if (!p.linear || !p.nearest) return interp_fail("SDL_CreateGPUSampler") == 0;
        }
        if (p.frame[0] && p.w == bw && p.h == bh && p.fmt == fmt) return true;
        interp_release_targets();
        const SDL_GPUTextureUsageFlags rt = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        for (SDL_GPUTextureFormat f : {fmt, SDL_GPU_TEXTUREFORMAT_R8_UNORM, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT})
            if (!SDL_GPUTextureSupportsFormat(dev_, f, SDL_GPU_TEXTURETYPE_2D, rt)) return interp_fail("a target format") == 0;
        p.w = bw; p.h = bh; p.fmt = fmt;
        p.scale = bw >= 1800 ? 8 : bw >= 900 ? 4 : 2;
        p.lw = (bw + p.scale - 1) / p.scale; p.lh = (bh + p.scale - 1) / p.scale;
        const uint32_t b = uint32_t(sa2_interp_spirv::kBlock);
        p.mw = (p.lw + b - 1) / b; p.mh = (p.lh + b - 1) / b;
        auto texture = [&](SDL_GPUTextureFormat f, uint32_t w, uint32_t h) {
            SDL_GPUTextureCreateInfo ci{};
            ci.type = SDL_GPU_TEXTURETYPE_2D; ci.format = f; ci.usage = rt;
            ci.width = w; ci.height = h; ci.layer_count_or_depth = 1; ci.num_levels = 1;
            ci.sample_count = SDL_GPU_SAMPLECOUNT_1;
            return SDL_CreateGPUTexture(dev_, &ci);
        };
        for (int i = 0; i < 2; i++) {
            p.frame[i] = texture(fmt, bw, bh);
            p.luma[i] = texture(SDL_GPU_TEXTUREFORMAT_R8_UNORM, p.lw, p.lh);
        }
        p.motion = texture(SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, p.mw, p.mh);
        p.made = texture(fmt, bw, bh);
        if (!p.frame[0] || !p.frame[1] || !p.luma[0] || !p.luma[1] || !p.motion || !p.made)
            return interp_fail("SDL_CreateGPUTexture") == 0;
        const SDL_GPUTextureFormat target[3] = {SDL_GPU_TEXTUREFORMAT_R8_UNORM, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, fmt};
        for (int i = 0; i < 3; i++) {
            if (p.pipe[i] && p.pipe_fmt[i] == target[i]) continue;
            if (p.pipe[i]) SDL_ReleaseGPUGraphicsPipeline(dev_, p.pipe[i]);
            SDL_GPUColorTargetDescription ct{};
            ct.format = target[i];
            SDL_GPUGraphicsPipelineCreateInfo pi{};
            pi.vertex_shader = p.vs;
            pi.fragment_shader = p.fs[i];
            pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
            pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
            pi.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
            pi.target_info.color_target_descriptions = &ct;
            pi.target_info.num_color_targets = 1;
            p.pipe[i] = SDL_CreateGPUGraphicsPipeline(dev_, &pi);
            p.pipe_fmt[i] = target[i];
            if (!p.pipe[i]) return interp_fail("SDL_CreateGPUGraphicsPipeline") == 0;
        }
        p.have_prev = false;
        p.pending = -1;
        return true;
    }

    // One fullscreen pass of pipeline `which` into `target`.
    bool interp_pass(SDL_GPUCommandBuffer* cb, SDL_GPUTexture* target, int which,
                     std::initializer_list<SDL_GPUTextureSamplerBinding> tex, const void* u, Uint32 n) {
        SDL_GPUColorTargetInfo ct{};
        ct.texture = target;
        ct.load_op = SDL_GPU_LOADOP_DONT_CARE;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cb, &ct, 1, nullptr);
        if (!rp) return interp_fail("SDL_BeginGPURenderPass") == 0;
        SDL_BindGPUGraphicsPipeline(rp, ip_.pipe[which]);
        SDL_BindGPUFragmentSamplers(rp, 0, tex.begin(), Uint32(tex.size()));
        SDL_PushGPUFragmentUniformData(cb, 0, u, n);
        SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
        SDL_EndGPURenderPass(rp);
        return true;
    }

    // At a Present (present(), its swapchain texture acquired): the back buffer `bb` into the ring,
    // and the texture to show now: the frame made between the last drawn frame and this one, or
    // `bb` itself at the first Present or when the passes cannot run.
    SDL_GPUTexture* interp_make(SDL_GPUCommandBuffer* cb, SDL_GPUTexture* bb, uint32_t bw, uint32_t bh,
                                SDL_GPUTextureFormat fmt) {
        Interp& p = ip_;
        p.pending = -1;
        if (!interp_ready(bw, bh, fmt)) return bb;
        const int cur = p.cur, prev = cur ^ 1;
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
        if (!cp) { interp_fail("SDL_BeginGPUCopyPass"); return bb; }
        SDL_GPUTextureLocation from{bb, 0, 0, 0, 0, 0}, to{p.frame[cur], 0, 0, 0, 0, 0};
        SDL_CopyGPUTextureToTexture(cp, &from, &to, bw, bh, 1, false);
        SDL_EndGPUCopyPass(cp);
        const float luma_u[8] = {1.0f / float(bw), 1.0f / float(bh), 0, 0, float(p.scale), 0, 0, 0};
        if (!interp_pass(cb, p.luma[cur], 0, {{p.frame[cur], p.linear}}, luma_u, sizeof luma_u)) return bb;
        SDL_GPUTexture* shown = bb;
        if (p.have_prev) {
            const float motion_u[4] = {float(p.lw), float(p.lh), float(p.mw), float(p.mh)};
            const float warp_u[4] = {float(p.scale) / float(bw), float(p.scale) / float(bh), kInterpMismatch, 0};
            if (!interp_pass(cb, p.motion, 1, {{p.luma[cur], p.nearest}, {p.luma[prev], p.nearest}}, motion_u,
                             sizeof motion_u) ||
                !interp_pass(cb, p.made, 2, {{p.frame[prev], p.linear}, {p.frame[cur], p.linear}, {p.motion, p.linear}},
                             warp_u, sizeof warp_u))
                return bb;
            shown = p.made;
            p.pending = cur;
            p.made_frames++;
        }
        p.cur = prev;
        p.have_prev = true;
        return shown;
    }

    void interp_release_targets() {
        Interp& p = ip_;
        for (SDL_GPUTexture** t : {&p.frame[0], &p.frame[1], &p.luma[0], &p.luma[1], &p.motion, &p.made})
            if (*t) { SDL_ReleaseGPUTexture(dev_, *t); *t = nullptr; }
    }

    void interp_release() {
        Interp& p = ip_;
        interp_release_targets();
        for (SDL_GPUGraphicsPipeline*& g : p.pipe) if (g) { SDL_ReleaseGPUGraphicsPipeline(dev_, g); g = nullptr; }
        for (SDL_GPUShader*& s : p.fs) if (s) { SDL_ReleaseGPUShader(dev_, s); s = nullptr; }
        if (p.vs) { SDL_ReleaseGPUShader(dev_, p.vs); p.vs = nullptr; }
        if (p.linear) { SDL_ReleaseGPUSampler(dev_, p.linear); p.linear = nullptr; }
        if (p.nearest) { SDL_ReleaseGPUSampler(dev_, p.nearest); p.nearest = nullptr; }
        if (win_) {
            const SDL_PropertiesID wp = SDL_GetWindowProperties(win_);
            SDL_ClearProperty(wp, "sa2.interp.present_pending");
            SDL_ClearProperty(wp, "sa2.interp.context");
        }
    }

    SDL_GPUDevice* dev_;
    SDL_Window* win_ = nullptr;
    std::unordered_map<uint32_t, Tex> tex_;
    std::unordered_map<uint32_t, Buf> buf_;
    std::unordered_map<std::string, std::unique_ptr<ShaderObject>> shaders_;
    std::unordered_map<const draw::ProgramImage*, ProgramObjects> programs_;
    std::unordered_map<ShaderPair, std::unordered_map<uint64_t, std::vector<PipelineEntry>>, ShaderPairHash> pipelines_;
    std::unordered_map<SamplerKey, SDL_GPUSampler*, SamplerKeyHash> samplers_;
    std::vector<UpChunk> up_chunks_;
    size_t up_cursor_ = 0;           // the command buffer's chunk, and its next free byte
    uint32_t up_next_ = 0;
    // draw()'s scratch: cleared at each draw, never read across draws
    std::vector<SDL_GPUColorTargetInfo> draw_colours_;
    std::string draw_attachment_key_;
    std::vector<SDL_GPUTextureSamplerBinding> draw_vertex_samplers_, draw_pixel_samplers_;
    uint32_t next_ = 1;
    SDL_GPUCommandBuffer* cb_ = nullptr;
    std::array<SDL_GPUFence*, 2> pending_{};
    uint32_t pending_count_ = 0;
    bool poisoned_ = false, swapchain_acquired_ = false;
    bool surface_gone_ = false;      // AfterAcquire::surface_gone: no swapchain acquire is made again
    bool flat_ps_ = false;           // the measuring switch: kFlatPixelShader for every pixel shader
    bool tiny_mips_ = false;         // the measuring switch: every sampler at its texture's smallest level
    bool dump_spirv_ = false;        // each SPIR-V module SDL is given, into SA2_PLAY_OUT
    bool interp_ = false;            // SA2_PLAYER_INTERP: frame interpolation (Interp)
    struct Interp {
        SDL_GPUTexture* frame[2]{};      // the last two drawn frames, copies of the back buffer
        SDL_GPUTexture* luma[2]{};       // their luma at 1/scale
        SDL_GPUTexture* motion = nullptr;
        SDL_GPUTexture* made = nullptr;  // the frame between them
        SDL_GPUShader* vs = nullptr;
        SDL_GPUShader* fs[3]{};          // luma, motion, warp
        SDL_GPUGraphicsPipeline* pipe[3]{};
        SDL_GPUTextureFormat pipe_fmt[3]{};
        SDL_GPUSampler* linear = nullptr;
        SDL_GPUSampler* nearest = nullptr;
        SDL_GPUTextureFormat fmt = SDL_GPU_TEXTUREFORMAT_INVALID;
        uint32_t w = 0, h = 0, lw = 0, lh = 0, mw = 0, mh = 0, scale = 4;
        int cur = 0;                     // the ring slot the next drawn frame goes to
        bool have_prev = false;
        int pending = -1;                // the slot present_pending shows, or -1
        bool failed = false;
        uint64_t made_frames = 0, pending_shown = 0;
    } ip_;
    uint64_t presents_ = 0;          // present() calls (the surface-lost fault's frames)
    SDL_GPUCopyPass* copy_ = nullptr;
    // The open draw pass (open_pass, flush_pass): its attachments, and its draws' commands with
    // their variable parts in the three arenas (a command's span of one, [at, at + n)).
    bool pass_open_ = false, pass_has_depth_ = false;
    uint32_t pass_w_ = 0, pass_h_ = 0;   // its render area: the smallest attachment level's size
    std::string pass_key_;
    std::vector<SDL_GPUColorTargetInfo> pass_colours_;
    SDL_GPUDepthStencilTargetInfo pass_depth_{};
    std::vector<Cmd> cmds_;
    std::vector<SDL_GPUBufferBinding> rec_bindings_;
    std::vector<SDL_GPUTextureSamplerBinding> rec_samplers_;
    std::vector<uint8_t> rec_uniforms_;
    // The whole clears clear_layer has not encoded, one per subresource (a later clear of it
    // merges in); a frame holds a handful, so a list.
    struct PendingClear {
        uint32_t id = 0, layer = 0, level = 0;
        SDL_GPUTexture* texture = nullptr;
        bool ds = false;                                // a depth-stencil texture
        bool colour = false, depth = false, stencil = false;
        SDL_FColor rgba{};
        float z = 0.0f;
        Uint8 s = 0;
    };
    std::vector<PendingClear> pending_clears_;
    // What the open pass's commands so far leave SDL holding (open_pass resets it): spans of the
    // arenas for the vertex buffers, samplers and uniform slots.
    struct Bound {
        SDL_GPUGraphicsPipeline* pipeline = nullptr;
        std::array<SDL_GPUBufferBinding, 16> vb{};   // slots 0..n_vbs-1 as bound
        uint32_t n_vbs = 0;
        SDL_GPUBufferBinding index{};
        SDL_GPUIndexElementSize index_size = SDL_GPU_INDEXELEMENTSIZE_16BIT;
        bool has_index = false;
        uint32_t samplers[2] = {0, 0}, n_samplers[2] = {0, 0};
        uint32_t uniform_at[2][4] = {}, uniform_n[2][4] = {};
        uint32_t uniform_valid = 0;               // bit stage * 4 + slot
        SDL_GPUViewport viewport{};
        SDL_Rect scissor{};
        SDL_FColor blend{};
        uint8_t stencil = 0;
        bool has_dynamic = false;
        void reset() { *this = Bound{}; }
    } bound_;
    std::vector<UpCopy> up_copies_;
    SDL_GPUTransferBuffer* ring_ = nullptr;
    SDL_GPUTransferBuffer* dedicated_ = nullptr;
    uint8_t* ring_map_ = nullptr;
    uint32_t ring_off_ = 0;
    SDL_GPUTransferBuffer* down_ = nullptr;
    uint32_t down_size_ = 0;
    std::vector<uint8_t> zeros_;
    GpuInfo info_;
    GpuCounters c_;
    std::string err_;
    GFmt depth_ = GFmt::d24s8;
};

}  // namespace

// One vkGetPhysicalDeviceProperties query of the device SDL_gpu chose, matched by its name, on
// a throwaway instance through the loader SDL_gpu loaded (SDL_Vulkan_GetVkGetInstanceProcAddr):
// the vendor ID and pointSizeRange[1] (R2b, d9g_gpu.h GpuInfo), and the device's apiVersion and type
// with the loader's version (the 1.1 profile's record, and make_sdl_gpu's CPU-device rule). It changes nothing SDL_gpu
// holds; the x87 control word and MXCSR are recorded around it with SDL_CreateGPUDevice's.
void query_vulkan_device(SdlGpu* gpu) {
    GpuInfo& info = const_cast<GpuInfo&>(gpu->info());
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    if (!gipa) { info.vk_query = std::string("no vkGetInstanceProcAddr: ") + SDL_GetError(); return; }
    auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!create) { info.vk_query = "no vkCreateInstance"; return; }
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    const uint16_t x87 = x87_cw();
    const uint32_t mx = mxcsr();
    if (create(&ci, nullptr, &inst) != VK_SUCCESS) { info.vk_query = "vkCreateInstance failed"; return; }
    // the loader's version (GpuInfo::vk_instance_api): 1.0 where it has no vkEnumerateInstanceVersion
    info.vk_instance_api = VK_API_VERSION_1_0;
    if (auto version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(gipa(VK_NULL_HANDLE, "vkEnumerateInstanceVersion")))
        version(&info.vk_instance_api);
    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(inst, "vkEnumeratePhysicalDevices"));
    auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(inst, "vkGetPhysicalDeviceProperties"));
    auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(inst, "vkDestroyInstance"));
    uint32_t n = 0;
    if (enumerate && props && enumerate(inst, &n, nullptr) == VK_SUCCESS && n) {
        std::vector<VkPhysicalDevice> devs(n);
        if (enumerate(inst, &n, devs.data()) == VK_SUCCESS) {
            int matches = 0;
            for (VkPhysicalDevice pd : devs) {
                VkPhysicalDeviceProperties p{};
                props(pd, &p);
                if (info.device_name != p.deviceName) continue;
                if (matches++) continue;
                info.vk_vendor_id = p.vendorID;
                info.vk_device_id = p.deviceID;
                info.vk_point_size_max = p.limits.pointSizeRange[1];
                info.vk_device_api = p.apiVersion;
                info.vk_device_type = uint32_t(p.deviceType);
            }
            if (matches == 1) { info.vk_query_ok = true; info.vk_query = "vkGetPhysicalDeviceProperties"; }
            else info.vk_query = std::to_string(matches) + " physical devices named " + info.device_name;
        }
    } else {
        info.vk_query = "vkEnumeratePhysicalDevices found none";
    }
    if (destroy) destroy(inst, nullptr);
    Report& r = report();
    if (x87_cw() != x87 || mxcsr() != mx) {   // the device's fpu record reports any change
        r.x87_after = x87_cw();
        r.mxcsr_after = mxcsr();
    }
}

// The facts DXVK's shader options are computed from (R1, d9g_shader_profile.h), read from the
// device SDL_gpu chose, matched by its name, by d9g_shader_vulkan.cpp's own query on a throwaway
// instance through the loader SDL_gpu loaded, at the level the device was made at (`level`: at vk11
// no Vulkan 1.2 or 1.3 structure is read). It changes nothing SDL_gpu holds; the x87 control word and
// MXCSR are recorded around it as around the properties query above.
void query_shader_facts(SdlGpu* gpu, shader::ApiLevel level) {
    GpuInfo& info = const_cast<GpuInfo&>(gpu->info());
    const uint16_t x87 = x87_cw();
    const uint32_t mx = mxcsr();
    const shader::DeviceFactsQuery q = shader::query_device_facts(
        reinterpret_cast<void*>(SDL_Vulkan_GetVkGetInstanceProcAddr()), info.device_name, level);
    info.shader_facts_ok = q.ok;
    info.shader_facts_query = q.how;
    info.shader_facts = q.facts;
    Report& r = report();
    if (x87_cw() != x87 || mxcsr() != mx) {
        r.x87_after = x87_cw();
        r.mxcsr_after = mxcsr();
    }
}

namespace {

// One SDL_CreateGPUDeviceWithProperties with `features` and SDL's own opt-out demands `sdl`, the x87
// control word and MXCSR recorded around it (`first`: before this attempt; a retry keeps the first
// attempt's, so it hides no change). `hardware_only` (DeviceRequest::hardware_only, the CPU step down's
// 1.1 request alone): SDL_gpu refuses every device that is not a GPU.
SDL_GPUDevice* create_device(bool debug, shader::VulkanFeatures& features, const shader::VulkanFeatures::SdlDemands& sdl,
                             bool hardware_only, bool first) {
    Report& r = report();
    SDL_GPUVulkanOptions vo{};
    vo.vulkan_api_version = features.api_version();
    vo.feature_list = features.feature_list();
    vo.vulkan_10_physical_device_features = features.features10();
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, debug);
    SDL_SetStringProperty(props, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, "vulkan");
    SDL_SetPointerProperty(props, SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER, &vo);
    // SDL_gpu asks for shaderClipDistance itself unless told not to (SDL_gpu_vulkan.c
    // VULKAN_INTERNAL_PrepareVulkan: an opt-out feature, on by default), so a device without it is
    // refused whatever the options say: off with ours (the default request leaves SDL's default).
    // depthClamp, drawIndirectFirstInstance and samplerAnisotropy are opt-out the same way: off where
    // `sdl` drops them (the default request leaves SDL's defaults, so the properties set are as before)
    if (!features.clip_distance())
        SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_CLIP_DISTANCE_BOOLEAN, false);
    if (!sdl.depth_clamp)
        SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_DEPTH_CLAMPING_BOOLEAN, false);
    if (!sdl.indirect_first_instance)
        SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_INDIRECT_DRAW_FIRST_INSTANCE_BOOLEAN, false);
    if (!sdl.anisotropy)
        SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_ANISOTROPY_BOOLEAN, false);
    if (hardware_only)
        SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_VULKAN_REQUIRE_HARDWARE_ACCELERATION_BOOLEAN, true);
    // SA2_D3D9GPU_PIPELINE_CACHE=<file>: the Vulkan pipeline cache's file (patches/sdl3 0002), so a
    // session's pipelines are compiled once and the next session finds them there. A stock SDL
    // ignores the property.
    if (const char* cache = std::getenv("SA2_D3D9GPU_PIPELINE_CACHE"); cache && *cache)
        SDL_SetStringProperty(props, "SDL.gpu.device.create.sa2recomp.pipeline_cache_path", cache);
    if (first) {
        r.x87_before = x87_cw();
        r.mxcsr_before = mxcsr();
    }
    SDL_GPUDevice* dev = props ? SDL_CreateGPUDeviceWithProperties(props) : nullptr;
    r.x87_after = x87_cw();
    r.mxcsr_after = mxcsr();
    r.fpu_measured = true;
    if (props) SDL_DestroyProperties(props);
    return dev;
}

// What each Vulkan physical device lacks of `features` and `sdl` (VulkanFeatures::lacking), through
// the loader SDL loads for it; SDL_gpu refused every device, so this names why. *lacks: the optional
// groups and SDL demands some device lacks; every one when the devices could not be read.
std::string lacking_features(const shader::VulkanFeatures& features, const shader::VulkanFeatures::SdlDemands& sdl,
                             bool* fits, shader::VulkanFeatures::Lacks* lacks) {
    *fits = false;
    *lacks = shader::VulkanFeatures::Lacks::all();
    if (!SDL_Vulkan_LoadLibrary(nullptr)) return std::string("the Vulkan loader: ") + SDL_GetError();
    std::string how;
    shader::VulkanFeatures::Lacks read;
    std::string out =
        features.lacking(reinterpret_cast<void*>(SDL_Vulkan_GetVkGetInstanceProcAddr()), fits, &how, &read, sdl);
    SDL_Vulkan_UnloadLibrary();
    if (!out.empty()) *lacks = read;
    return out.empty() ? "the devices' features could not be read (" + how + ")" : out;
}

// The names of SDL's opt-out demands `sdl` drops ("" none), as the report gives them.
std::string sdl_dropped(const shader::VulkanFeatures::SdlDemands& sdl) {
    std::string out;
    auto add = [&](bool dropped, const char* name) {
        if (dropped) out += std::string(out.empty() ? "" : ", ") + name;
    };
    add(!sdl.depth_clamp, "depthClamp");
    add(!sdl.indirect_first_instance, "drawIndirectFirstInstance");
    add(!sdl.anisotropy, "samplerAnisotropy");
    return out;
}

// A switch for testing a fallback on a device that does not need it: the environment variable `env`
// set to 1, or on Android, where the app's session gives the in-process player no game.env.extra line
// but SA2_PLAYER_PROF's, the system property `prop` set to 1 (adb shell setprop, no root). *which
// names the one that asked ("SA2_D3D9GPU_NO_BC=1", "debug.sa2.no_bc=1").
bool test_switch(const char* env, const char* prop, const char** which) {
    const char* v = std::getenv(env);
    if (v && std::strcmp(v, "1") == 0) {
        *which = nullptr;
        return true;
    }
#ifdef __ANDROID__
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get(prop, value) > 0 && std::strcmp(value, "1") == 0) {
        *which = prop;
        return true;
    }
#else
    (void)prop;
#endif
    return false;
}

// The SDL global property a live session's player sets (host/player/player.cpp, --interactive):
// GpuInfo::live_session.
constexpr const char kLiveSessionProperty[] = "sa2.live_session";

}  // namespace

// SDL_CreateGPUDeviceWithProperties with the Vulkan driver named (the critic's A26), SPIR-V as the
// only shader format, and SDL_GPUVulkanOptions asking for Vulkan 1.3 and the features the
// translated modules need (R1, shader::VulkanFeatures: R0's chain and what the validation layer
// named on it). This is the change make_sdl_gpu's earlier comment said R1 would make; the host
// comparison (test_d3d9gpu.sh steps 3 and 8) is rerun with it (the R1 plan's check C10). The x87
// control word and MXCSR are recorded around the call.
//
// SDL_gpu refuses a device that lacks any feature asked for (SDL_gpu_vulkan.c
// VULKAN_INTERNAL_ValidateOptInFeatures and VULKAN_INTERNAL_IsDeviceSuitable). When it refuses the full
// request, the features each device lacks are read, and the device is asked for again without the
// optional groups some device lacks (all of them when the devices could not be read): the sub-dword
// features (VulkanFeatures(false, ..)), which many low-end GPUs and SwiftShader lack, the translator
// then reading the state buffer as 32-bit words (DeviceFacts::sub_dword_access,
// ShaderProfile::state_words); shaderClipDistance (VulkanFeatures(.., false), and SDL's own request of it
// off: create_device), which Mali GPUs lack (the user's Pixel 9 Pro's Mali-G715, 2026-10-12), the
// translator then building no ClipDistance output (DeviceFacts::clip_distance,
// ShaderProfile::no_clip_distance); scalarBlockLayout, which no module needs (d9g_shader_vulkan.cpp); and
// SDL's own opt-out demands (VulkanFeatures::SdlDemands, off with their
// SDL_PROP_GPU_DEVICE_CREATE_FEATURE_*_BOOLEAN): depthClamp, which the Galaxy A05's Mali-G52 MC2 lacks
// (GitHub #3; the device then has no depth clip and clamp: GpuInfo::depth_clamp_clip_only),
// drawIndirectFirstInstance (host 2 issues no indirect draw) and samplerAnisotropy (sampler_object). If
// that is refused too while an optional group is still asked for, once more without all of them. A
// device that lacks any other feature, ours or SDL's own, is refused with every device's list. Each
// one dropped is said once on stderr. A test run's override (CapsOff sub_dword) asks without the
// sub-dword features from the start; SA2_D3D9GPU_NO_CLIPDIST=1 (any run: the fallback a Mali GPU
// takes, on a device that has the feature) without shaderClipDistance; SA2_D3D9GPU_NO_DEPTH_CLAMP=1
// without SDL's depthClamp. On Android the system properties debug.sa2.no_clipdist=1 and
// debug.sa2.no_depth_clamp=1 ask the same (test_switch).
//
// The Vulkan 1.1 profile (shader::ApiLevel::vk11; GitHub #5's Adreno 610 offers Vulkan 1.1 only): where
// SDL_gpu refuses the full 1.3 request and no device takes the 1.3 profile (each reports below 1.3 or
// lacks one of its four 1.2 and 1.3 features: VulkanFeatures::Lacks::api13), the device is asked for at
// API 1.1 with Vulkan 1.0 features (and the clip distance and SDL demands some device lacks dropped),
// and every module is lowered to SPIR-V 1.3 (ShaderProfile::vk11); a refusal at 1.1 is retried as at
// 1.3. Where SDL_gpu settles on a CPU device (lavapipe) at 1.3 while a GPU takes only the 1.1 profile,
// the GPU is asked for at 1.1 instead, GPUs only (SDL_PROP_GPU_DEVICE_CREATE_VULKAN_REQUIRE_HARDWARE_
// ACCELERATION_BOOLEAN); where SDL_gpu takes no GPU at 1.1 either (a non-conformant driver such as hasvk,
// which it refuses whatever the version), the CPU device's 1.3 request is made again, as SDL_gpu chose it,
// and said once on stderr. A device SDL_gpu makes at 1.3 although it reports below 1.3 (its driver fills
// the 1.3 feature structures SDL_gpu checks) keeps the 1.3 profile it had before the 1.1 profile existed,
// named on stderr and in the report (GpuInfo::vk13_below_device). A device that reports no Vulkan 1.1 is
// refused by name before SDL_gpu is asked at 1.1. SA2_D3D9GPU_VK11=1 (on Android debug.sa2.vk11=1) asks at 1.1 from the start,
// on any device. The choice is shader::next_device_request's; each step down is said once on stderr and
// recorded (GpuInfo::vk11, vk11_reason, vk11_forced).
std::unique_ptr<Gpu> make_sdl_gpu(bool debug, const CapsOff& off, std::string* err) {
    const char* clip_prop = nullptr;
    const bool clip_forced_off = test_switch("SA2_D3D9GPU_NO_CLIPDIST", "debug.sa2.no_clipdist", &clip_prop);
    if (clip_forced_off)
        std::fprintf(stderr, "d3d9gpu: %s: the Vulkan device is made without shaderClipDistance and the vertex shaders "
                             "write no ClipDistance\n", clip_prop ? "debug.sa2.no_clipdist=1" : "SA2_D3D9GPU_NO_CLIPDIST=1");
    // SA2_D3D9GPU_NO_BC=1 (on Android also debug.sa2.no_bc=1): no BC format is sampled, each DXT
    // texture decoded to BGRA8 on the CPU as on a GPU without them (a Mali's), on any device.
    const char* bc_prop = nullptr;
    const bool bc_forced_off = test_switch("SA2_D3D9GPU_NO_BC", "debug.sa2.no_bc", &bc_prop);
    if (bc_forced_off)
        std::fprintf(stderr, "d3d9gpu: %s: no BC format is sampled; DXT textures are decoded to BGRA8 on the CPU\n",
                     bc_prop ? "debug.sa2.no_bc=1" : "SA2_D3D9GPU_NO_BC=1");
    // SA2_D3D9GPU_NO_DEPTH_CLAMP=1 (on Android also debug.sa2.no_depth_clamp=1): the device is made
    // without SDL's depthClamp, as on a GPU without it (the Galaxy A05's), on any device, so it has no
    // depth clip and clamp, and the draws are clipped without the clamp in any run.
    const char* clamp_prop = nullptr;
    const bool clamp_forced_off = test_switch("SA2_D3D9GPU_NO_DEPTH_CLAMP", "debug.sa2.no_depth_clamp", &clamp_prop);
    if (clamp_forced_off)
        std::fprintf(stderr, "d3d9gpu: %s: the Vulkan device is made without depthClamp, so with no depth clip and "
                             "clamp, and the draws are clipped at the near and far planes without the clamp\n",
                     clamp_prop ? "debug.sa2.no_depth_clamp=1" : "SA2_D3D9GPU_NO_DEPTH_CLAMP=1");
    // SA2_D3D9GPU_VK11=1 (on Android also debug.sa2.vk11=1): the device is made at Vulkan 1.1 and the
    // shaders lowered to SPIR-V 1.3, as on a GPU whose driver offers only 1.1 (an Adreno 610), on any device.
    const char* vk11_prop = nullptr;
    const bool vk11_forced = test_switch("SA2_D3D9GPU_VK11", "debug.sa2.vk11", &vk11_prop);
    const std::string vk11_switch = !vk11_forced ? "" : vk11_prop ? "debug.sa2.vk11=1" : "SA2_D3D9GPU_VK11=1";
    if (vk11_forced)
        std::fprintf(stderr, "d3d9gpu: %s: the Vulkan device is made at API 1.1 and the shaders are lowered to SPIR-V 1.3\n",
                     vk11_switch.c_str());
    // d3d9.floatEmulation. host/player/dxvk.conf pins Strict, which gives every multiply a zero test
    // and two selects per component; DXVK's own Auto keeps Strict only on drivers with a native
    // legacy multiply (RADV, Intel's Mesa driver) and takes Enabled elsewhere, Adreno and Mali among
    // them, where Strict cost the Moto One 5G Ace about 9% of its frame rate at 2400x1080 (52.3 to
    // 57.0 fps idle in City Escape, 2026-10-29). A live session takes Enabled (GpuInfo::
    // fast_float_emulation, below); SA2_D3D9GPU_STRICT_FLOAT=1 (on Android debug.sa2.strict_float=1)
    // keeps Strict in one, and SA2_D3D9GPU_FAST_FLOAT=1 (debug.sa2.fast_float=1) asks Enabled in any run.
    const char* fast_float_prop = nullptr;
    const bool fast_float = test_switch("SA2_D3D9GPU_FAST_FLOAT", "debug.sa2.fast_float", &fast_float_prop);
    const char* strict_float_prop = nullptr;
    const bool strict_float = test_switch("SA2_D3D9GPU_STRICT_FLOAT", "debug.sa2.strict_float", &strict_float_prop);
    using shader::DeviceAnswer;
    using shader::DeviceRequest;
    using shader::DeviceStep;
    DeviceRequest req = shader::first_device_request(vk11_forced, off.sub_dword, clip_forced_off, clamp_forced_off);
    auto features_for = [](const DeviceRequest& r) {
        return std::make_unique<shader::VulkanFeatures>(r.sub_dword, r.clip_distance, r.scalar_layout, r.level);
    };
    std::unique_ptr<shader::VulkanFeatures> features = features_for(req);
    auto level_name = [](const DeviceRequest& r) { return r.level == shader::ApiLevel::vk11 ? "1.1" : "1.3"; };
    // What the devices lack of the request (read at its level's first refusal; for the vk11 switch before
    // its first request), and the text of each level's reading for the error and the report.
    shader::VulkanFeatures::Lacks lacks;
    bool fits = false, read = false;
    std::string lacking, lacking13, why, vk11_reason = vk11_forced ? "forced (" + vk11_switch + ")" : "";
    if (vk11_forced) {
        lacking = lacking_features(*features, req.sdl, &fits, &lacks);
        read = true;
        if (shader::next_device_request(req, DeviceAnswer::unasked, 0, lacks, fits).kind == DeviceStep::refuse_api11) {
            *err = "no Vulkan device reports 1.1 or later (" + lacking + "): the renderer needs Vulkan 1.1";
            return nullptr;
        }
    }
    // Each request's part of the error: the first, a step down, a retry within a level.
    std::string label = std::string("SDL_CreateGPUDeviceWithProperties (Vulkan ") + level_name(req) + ", ";
    bool first = true, refused_once = false, cpu_step_down = false;
    int retry = 0;
    std::unique_ptr<SdlGpu> gpu;
    // The CPU step down's way back (DeviceStep::restore): the 1.3 request SDL_gpu accepted on the CPU device
    // (DeviceStep::fallback), the CPU device's name, and what the report and the errors held then.
    struct CpuDevice13 {
        DeviceRequest req;
        std::string device_name, lacking, lacking13;
        bool refused_once = false;
    } cpu13;
    std::string refusal;   // SDL_GetError() of the last refusal
    for (;;) {
        SDL_GPUDevice* dev = create_device(debug, *features, req.sdl, req.hardware_only, first);
        first = false;
        DeviceAnswer answer = DeviceAnswer::refused;
        shader::VulkanFeatures::Lacks cpu_lacks;
        bool cpu_fits = false;
        std::string cpu_lacking;
        if (dev) {
            gpu = std::make_unique<SdlGpu>(dev, debug, off);
            // An SDL marked as patched reports the patch's properties, and an unmarked one none: either
            // other way, the library is not the configuration it says it is (an unmarked patched SDL,
            // or a mark without the patch), and nothing runs on it.
            const GpuInfo& g = gpu->info();
            if (g.sdl_patched != g.sdl_capabilities_present) {
                *err = "the SDL3 loaded (" + g.sdl_revision + ") is " + (g.sdl_patched ? "marked as patched" : "not marked as patched") +
                       " but " + (g.sdl_capabilities_present ? "reports" : "does not report") +
                       " the patch's device properties (patches/sdl3)";
                return nullptr;
            }
            query_vulkan_device(gpu.get());
            answer = DeviceAnswer::accepted;
            if (g.vk_device_type == VK_PHYSICAL_DEVICE_TYPE_CPU) {
                answer = DeviceAnswer::accepted_cpu;
                // SDL_gpu chose a CPU device at 1.3: read whether a GPU would take the 1.1 profile instead
                // (not on the CPU device's 1.3 gone back to, which is kept)
                if (req.level == shader::ApiLevel::vk13 && !req.cpu_settled)
                    cpu_lacking = lacking_features(*features, req.sdl, &cpu_fits, &cpu_lacks);
            }
        } else {
            refused_once = true;
            refusal = SDL_GetError();
            why += label + features->describe() + (sdl_dropped(req.sdl).empty() ? "" : ", SDL without " + sdl_dropped(req.sdl)) +
                   "): " + refusal;
            if (!read) {
                lacking = lacking_features(*features, req.sdl, &fits, &lacks);
                if (req.level == shader::ApiLevel::vk13) lacking13 = lacking;
                read = true;
            }
        }
        const bool cpu = answer == DeviceAnswer::accepted_cpu;
        const DeviceStep step = shader::next_device_request(req, answer, retry, cpu ? cpu_lacks : lacks, cpu ? cpu_fits : fits);
        if (step.kind == DeviceStep::done) break;
        if (step.kind == DeviceStep::refuse_api11) {
            *err = "no Vulkan device reports 1.1 or later (" + lacking + "): the renderer needs Vulkan 1.1; " + why;
            return nullptr;
        }
        if (step.kind == DeviceStep::give_up) {
            *err = why + "; the features each Vulkan device lacks: " + (lacking13.empty() ? lacking : lacking13) +
                   (!lacking13.empty() && lacking != lacking13 ? "; at Vulkan 1.1: " + lacking : "");
            return nullptr;
        }
        if (step.kind == DeviceStep::restore) {
            // No GPU took the GPU-only 1.1 request (SDL_gpu refuses a non-conformant driver, or one without a
            // present queue, which the reading does not see): the CPU device's 1.3, as SDL_gpu chose it, with
            // the report it had then (no vk11, the lacks as they were).
            std::fprintf(stderr, "d3d9gpu: no GPU took the Vulkan 1.1 request either (%s): the CPU device %s is made at "
                                 "API 1.3, as SDL_gpu chose it\n",
                         cpu ? ("SDL_gpu chose the CPU device " + gpu->info().device_name + " again").c_str() : refusal.c_str(),
                         cpu13.device_name.c_str());
            gpu.reset();
            req = cpu13.req;
            refused_once = cpu13.refused_once;
            lacking = cpu13.lacking;
            lacking13 = cpu13.lacking13;
            vk11_reason.clear();
            cpu_step_down = false;
            label = "; the CPU device at Vulkan 1.3 again (";
            retry = 0;
            features = features_for(req);
            continue;
        }
        if (step.step_down) {
            if (cpu) {
                cpu13 = {step.fallback, gpu->info().device_name, lacking, lacking13, refused_once};
                vk11_reason = "SDL_gpu chose the CPU device " + gpu->info().device_name + " at Vulkan 1.3, and a GPU takes "
                              "only the 1.1 profile (" + cpu_lacking + ")";
                lacking = cpu_lacking;
                cpu_step_down = true;
                // said once a GPU takes it (after the loop): where none does, the CPU device keeps its 1.3
                label = "; at Vulkan 1.1, GPUs only (";
            } else {
                vk11_reason = "no Vulkan device takes the 1.3 profile (" + lacking + ")";
                std::fprintf(stderr, "d3d9gpu: %s: the device is made at API 1.1 and the shaders are lowered to SPIR-V 1.3\n",
                             vk11_reason.c_str());
                label = "; at Vulkan 1.1 (";
            }
            gpu.reset();   // the CPU device's, if any
            retry = 0;
            read = false;
        } else {
            label = "; without " + step.dropped + " (";
            retry = step.retry + 1;
        }
        req = step.request;
        features = features_for(req);
    }
    const bool vk11 = features->level() == shader::ApiLevel::vk11;
    if (vk11 && cpu_step_down)
        std::fprintf(stderr, "d3d9gpu: %s: the device is made at API 1.1 on the GPU %s and the shaders are lowered to "
                             "SPIR-V 1.3\n", vk11_reason.c_str(), gpu->info().device_name.c_str());
    // SDL_gpu made the device at 1.3 on a driver that reports below it (it compares no version, only the
    // feature structures, which some drivers fill above their own version): run as before the 1.1 profile
    // existed, its facts read in full (query_device_facts at vk13), and named here and in the report.
    const std::string below13 = vk11 ? "" : shader::device_below13_note(gpu->info().vk_device_api);
    if (!below13.empty()) std::fprintf(stderr, "d3d9gpu: %s\n", below13.c_str());
    // What each retry dropped, said once (and at vk11 not the sub-dword features or scalarBlockLayout, which
    // the 1.1 profile goes without on any device: the step down said so). The devices' lacks are reported
    // where they changed the request: after a refusal, or the step down from a CPU device; the vk11 switch's
    // reading before its first request is not.
    const bool lacking_used = refused_once || cpu_step_down;
    if (lacking_used) {
        if (!vk11 && !features->sub_dword() && !off.sub_dword)
            std::fprintf(stderr, "d3d9gpu: the Vulkan device lacks the sub-dword features (%s): the shaders read their "
                                 "state as 32-bit words\n", lacking.c_str());
        if (!vk11 && !features->scalar_layout())
            std::fprintf(stderr, "d3d9gpu: the Vulkan device lacks scalarBlockLayout (%s): made without it, which no "
                                 "shader needs\n", lacking.c_str());
        if (!features->clip_distance() && !clip_forced_off)
            std::fprintf(stderr, "d3d9gpu: the Vulkan device lacks shaderClipDistance (%s): the vertex shaders write no "
                                 "ClipDistance, and a draw with a user clip plane enabled is refused\n", lacking.c_str());
        if (!req.sdl.depth_clamp && !clamp_forced_off)
            std::fprintf(stderr, "d3d9gpu: the Vulkan device lacks depthClamp (%s): made without it, so with no depth "
                                 "clip and clamp\n", lacking.c_str());
        if (!req.sdl.indirect_first_instance)
            std::fprintf(stderr, "d3d9gpu: the Vulkan device lacks drawIndirectFirstInstance (%s): made without it, "
                                 "which no draw needs (no indirect draw is issued)\n", lacking.c_str());
        if (!req.sdl.anisotropy)
            std::fprintf(stderr, "d3d9gpu: the Vulkan device lacks samplerAnisotropy (%s): made without it; a sampler "
                                 "that asks for anisotropic filtering is made without it\n", lacking.c_str());
    }
    const shader::VulkanFeatures::SdlDemands& sdl = req.sdl;
    const GpuInfo& g = gpu->info();
    GpuInfo& info = const_cast<GpuInfo&>(gpu->info());
    info.vulkan_options = features->describe();
    info.sub_dword_access = features->sub_dword();
    info.clip_distance = features->clip_distance();
    info.clip_distance_forced_off = clip_forced_off;
    info.bc_forced_off = bc_forced_off;
    info.vulkan_lacking = lacking_used ? lacking : "";
    info.sdl_features_dropped = sdl_dropped(sdl);
    info.sampler_anisotropy = sdl.anisotropy;
    info.depth_clamp_forced_off = clamp_forced_off;
    info.vk11 = vk11;
    info.vk11_reason = vk11 ? vk11_reason : "";
    info.vk11_forced = vk11_switch;
    info.vk13_below_device = !below13.empty();
    info.live_session = SDL_GetBooleanProperty(SDL_GetGlobalProperties(), kLiveSessionProperty, false);
    info.depth_clamp_clip_only = !info.depth_clip_and_clamp && (info.live_session || clamp_forced_off);
    info.fast_float_emulation = fast_float || (info.live_session && !strict_float);
    if (fast_float || strict_float)
        std::fprintf(stderr, "d3d9gpu: %s: the shaders are translated with d3d9.floatEmulation = %s\n",
                     fast_float ? (fast_float_prop ? "debug.sa2.fast_float=1" : "SA2_D3D9GPU_FAST_FLOAT=1")
                                : (strict_float_prop ? "debug.sa2.strict_float=1" : "SA2_D3D9GPU_STRICT_FLOAT=1"),
                     info.fast_float_emulation ? "Enabled" : "Strict");
    // Without depth clip and clamp every pipeline owes the clamp (R_DEPTH_CLAMP), so say once which
    // way the draws go: clip-only, or refused by name.
    if (!info.depth_clip_and_clamp) {
        const char* why_not = !info.sdl_capabilities_present ? "the SDL3 loaded is not the patched one (patches/sdl3)"
                              : info.depth_clip_and_clamp_detected ? "forced off by a test run (SA2_D3D9GPU_CAPS_OFF)"
                              : !sdl.depth_clamp ? "the device was made without depthClamp"
                                                 : "the device has no depthClipEnable (VK_EXT_depth_clip_enable) beside depthClamp";
        std::fprintf(stderr, "d3d9gpu: no depth clip and clamp (%s): %s\n", why_not,
                     info.depth_clamp_clip_only
                         ? "the draws are clipped at the near and far planes without the clamp"
                         : "outside a live session each draw is refused (DEPTH_CLAMP_FALLBACK_OWED); "
                           "SA2_D3D9GPU_NO_DEPTH_CLAMP=1 draws them clip-only");
    }
    query_shader_facts(gpu.get(), features->level());
    // the profile's facts are what the device was made with, not what it could have been
    info.shader_facts.sub_dword_access = features->sub_dword();
    info.shader_facts.clip_distance = features->clip_distance();
    info.shader_facts.vk11 = vk11;
    if (!g.shader_facts_ok) {
        *err = "the shader translator's device facts could not be read (" + g.shader_facts_query +
               "): without them it cannot compile as DXVK compiles on this device";
        return nullptr;
    }
    return gpu;
}

}  // namespace d9g
