// d9g_device.cpp: IDirect3DDevice9 for R2a. Every rule below is DXVK 3.1.1's
// (private/s5-tools/src/dxvk-3.1.1/src/d3d9/d3d9_device.cpp unless a file is named),
// cited by line; the answers are DXVK's, which check d of tools/s5/compare_hosts.py
// holds the player's accounting to. Draws are validated, answered and counted, never
// drawn: shaders are translated at creation by DXVK's compiler (R1, ruling 2), and the draws that
// use them are R2-draws'.
#include "d9g.h"

#include "sha256.h"
#include "texcodec.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <exception>
#include <stdexcept>
#ifdef _WIN32
#include <cerrno>
// The C library's rename refuses an existing destination on Windows, where rename(2) replaces
// it; a report is published as a temporary file renamed over the last one. 0, or -1 with errno.
static int sa2_replace_rename(const char* from, const char* to) {
    std::error_code ec;
    std::filesystem::rename(std::filesystem::path(from), std::filesystem::path(to), ec);
    if (!ec) return 0;
    errno = ec.default_error_condition().category() == std::generic_category() ? ec.default_error_condition().value()
                                                                               : EIO;
    return -1;
}
#endif

namespace d9g {

namespace {
constexpr uint32_t kRS_SCISSOR = 174, kRS_SRGBWRITE = 194;
// The Present fault's colour: not black in any channel, so a frame a Clear did not
// cover shows it.
constexpr float kScribble[4] = {0.25f, 0.5f, 0.75f, 1.0f};

// InvalidSampler and RemapSamplerState (d3d9_util.h:48-76): 0-15 pixel, 256 the
// displacement map, 257-260 vertex; stored at 0..20.
bool invalid_sampler(DWORD s) { return (s >= 16 && s < D3DDMAPSAMPLER) || s > D3DVERTEXTEXTURESAMPLER3; }
DWORD remap_sampler(DWORD s) { return s >= D3DDMAPSAMPLER ? 16 + (s - D3DDMAPSAMPLER) : s; }

const char* kMethodDP = "DrawPrimitive";
const char* kMethodDIP = "DrawIndexedPrimitive";
const char* kMethodDPUP = "DrawPrimitiveUP";
const char* kMethodDIPUP = "DrawIndexedPrimitiveUP";
}  // namespace

// ------------------------------------------------------------------ CreateDevice's parameters
HRESULT check_present_parameters(const D3DPRESENT_PARAMETERS& pp, bool* ni, std::string* why) {
    *ni = false;
    // ValidatePresentationParameters (d3d9_interface.cpp:475-530), a non-Ex device.
    if (pp.SwapEffect > D3DSWAPEFFECT_COPY || pp.SwapEffect == 0) { *why = "SwapEffect"; return D3DERR_INVALIDCALL; }
    if (pp.BackBufferCount > D3DPRESENT_BACK_BUFFERS_MAX) { *why = "BackBufferCount"; return D3DERR_INVALIDCALL; }
    if (pp.SwapEffect == D3DSWAPEFFECT_COPY && pp.BackBufferCount > 1) { *why = "COPY with more than one back buffer"; return D3DERR_INVALIDCALL; }
    *ni = true;
    if (!pp.Windowed) { *why = "fullscreen presentation"; return D3DERR_INVALIDCALL; }
    if (pp.MultiSampleType != D3DMULTISAMPLE_NONE) { *why = "a multisampled back buffer"; return D3DERR_INVALIDCALL; }
    // SwapWithFrontBuffer (d3d9_swapchain.h:245-255): DISCARD with one back buffer in a
    // window, and COPY, never rotate; anything else does, which this device does not.
    const UINT count = pp.BackBufferCount ? pp.BackBufferCount : 1;
    const bool rotates = !(pp.SwapEffect == D3DSWAPEFFECT_COPY ||
                           (pp.SwapEffect == D3DSWAPEFFECT_DISCARD && count == 1 && pp.Windowed));
    if (rotates) { *why = "a swap chain DXVK rotates (more than one back buffer, or FLIP)"; return D3DERR_INVALIDCALL; }
    const uint32_t fmt = pp.BackBufferFormat;
    if (fmt != D3DFMT_A8R8G8B8 && fmt != D3DFMT_X8R8G8B8 && fmt != D3DFMT_UNKNOWN) {
        *why = "back buffer format " + std::to_string(fmt);
        return D3DERR_INVALIDCALL;
    }
    if (pp.EnableAutoDepthStencil && pp.AutoDepthStencilFormat != D3DFMT_D24S8) {
        *why = "automatic depth-stencil format " + std::to_string(uint32_t(pp.AutoDepthStencilFormat));
        return D3DERR_INVALIDCALL;
    }
    *ni = false;
    return D3D_OK;
}

// ------------------------------------------------------------------ setup
Device::Device(std::unique_ptr<Gpu> gpu, const DeviceSetup& s) : gpu_(std::move(gpu)), pp_(s.pp), behavior_(s.behavior) {
    // Constant files, sized as DXVK's (d3d9_caps.h; d3d9_device.h:1339-1361).
    vs_f_.assign(can_swvp() ? 8192 : 256, {0, 0, 0, 0});
    vs_i_.assign(can_swvp() ? 2048 : 16, {0, 0, 0, 0});
    vs_b_.assign(can_swvp() ? 2048 : 16, 0);
    ps_f_.assign(224, {0, 0, 0, 0});
    ps_i_.assign(16, {0, 0, 0, 0});
    ps_b_.assign(16, 0);
    swvp_ = only_swvp();
    // DXVK's ResetState defaults (d3d9_device.cpp:8633-8857, d9g_state.h state_defaults), every
    // render state and sampler state it sets; the rest start at 0. POINTSIZE_MAX's is the
    // device's pointSizeRange[1] (GpuInfo's Vulkan query; 0 without it, which the report says).
    auto_depth_ = pp_.EnableAutoDepthStencil != FALSE;
    dp_.d24s8_supported = gpu_->depth_format() == GFmt::d24s8;
    dp_.point_size_max = gpu_->info().vk_point_size_max;
    // the SDL patch's capabilities as the GPU uses them (a test run's override applied: d9g_gpu.h)
    dp_.sampler_clamp_to_border = gpu_->info().sampler_clamp_to_border;
    dp_.depth_clip_and_clamp = gpu_->info().depth_clip_and_clamp;
    depth_clamp_clip_only_ = gpu_->info().depth_clamp_clip_only && !dp_.depth_clip_and_clamp;
    vk11_ = gpu_->info().vk11;
    vk_query_ok_ = gpu_->info().vk_query_ok;
    report().pipeline.device_params = vk_query_ok_;
    // R1: DXVK's shader options for this device, from the facts the GPU read (d9g_gpu.h); without
    // them every shader is refused at creation, by name (translate_shader)
    shader_profile_ok_ = gpu_->info().shader_facts_ok;
    if (shader_profile_ok_) {
        shader_profile_ = shader::determine_profile(gpu_->info().shader_facts);
        const std::string rec = shader_profile_.record();
        Sha256 h;
        h.update(rec.data(), rec.size());
        report().shader_tx.profile_sha256 = h.hex();
    }
    // The device's name, which the keys file carries (write_keys_file) and the state check holds
    // to its parameters': Direct3DCreate9 records the SDL_gpu device's (d9g_entry.cpp); a device
    // made without it (the unit test's CPU model) names its own GPU here, so a keys file never
    // names no device (the R2b fold's checker-logic finding 1).
    // The GPU's whole account (its capabilities among it) where Direct3DCreate9 recorded none.
    if (report().gpu.device_name.empty()) report().gpu = gpu_->info();
#ifdef _WIN32
    // mingw-w64's DWORD is unsigned long (32 bits under LLP64), DXVK Native's uint32_t: the same
    // words in another type, so they are copied through uint32_t arrays rather than aliased.
    static_assert(sizeof(DWORD) == sizeof(uint32_t), "DWORD is 32 bits");
    for (auto& st : ss_) {
        uint32_t rs[256], ss[14];
        std::copy(std::begin(rs_), std::end(rs_), rs);
        std::copy(std::begin(st), std::end(st), ss);
        state_defaults(rs, ss, dp_, auto_depth_);
        std::copy(std::begin(rs), std::end(rs), rs_);
        std::copy(std::begin(ss), std::end(ss), st);
    }
#else
    for (auto& st : ss_) state_defaults(rs_, st, dp_, auto_depth_);
#endif
    for (auto& s2 : streams_) s2.freq = 1;
}

uint32_t Device::make_gpu_image(Image& img) {
    TexDesc d;
    d.w = img.w; d.h = img.h; d.levels = img.levels;
    d.fmt = img.fi->gpu == GFmt::d24s8 ? gpu_->depth_format() : img.fi->gpu;
    d.target = (img.usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) != 0;
    d.sampled = img.is_texture || !img.fi->depth;
    d.type = img.type;
    d.depth = img.depth;
    // A DXT format the GPU cannot sample (Mali GPUs have no BC format; GpuInfo::bc_unsupported, or
    // the forced switch): the texture is BGRA8 and each upload decodes the store's blocks on the CPU
    // (upload_image_subresource), the mip chain kept. DXT1's one-bit alpha decodes as BC1_RGBA's (a
    // transparent black third colour where c0 <= c1), DXT3's and DXT5's alpha as BC2's and BC3's.
    img.decoded = img.fi->block_bytes != 0 && !gpu_->samples(d.fmt, d.type);
    if (img.decoded) {
        d.fmt = GFmt::bgra8;
        report().decoded_textures[img.fi->name]++;
        if (std::find(decode_logged_.begin(), decode_logged_.end(), img.d3dfmt) == decode_logged_.end()) {
            decode_logged_.push_back(img.d3dfmt);
            const GpuInfo& g = gpu_->info();
            std::fprintf(stderr, "d3d9gpu: %s is not supported by the GPU%s; decoded to BGRA8 on the CPU\n", img.fi->name,
                         g.bc_forced_off ? " (forced: SA2_D3D9GPU_NO_BC=1 or debug.sa2.no_bc=1)" : "");
        }
    }
    const uint32_t t = create_gpu_texture(d);
    if (t && !d.target) {
        uint64_t bytes = 0;
        for (uint32_t l = 0; l < img.levels; l++)
            bytes += level_bytes(level_dim(img.w, l), level_dim(img.h, l), *img.fi) * img.level_depth(l) * img.layers;
        report().zero_init_bytes += bytes;
    }
    // A new BC image is zero blocks (DXVK's initImage), which DXT1 decodes to opaque black (a zero
    // block's c0 <= c1, index 0: c0, alpha 1): the decoded texture's levels start as that, not as the
    // backend's zero texels. DXT3's and DXT5's zero blocks decode to zero texels, as made. A zero
    // R5G6B5 texel samples as opaque black too (the format has no alpha); a zero A8 one as zero.
    const bool opaque_zero = img.fi->expand == Expand::r5g6b5 && !d.target;
    if (t && (img.decoded || opaque_zero)) {
        const uint8_t zero[16] = {};
        std::vector<uint8_t> px;
        if (opaque_zero) px = {0, 0, 0, 0xFF};
        else if (!tex::decode_level_sampled(zero, 1, 1, img.d3dfmt, 0, &px)) { release_gpu_texture(t); return 0; }
        if (px[0] || px[1] || px[2] || px[3]) {
            std::vector<uint8_t> fill;
            for (uint32_t layer = 0; layer < img.layers; layer++)
                for (uint32_t l = 0; l < img.levels; l++) {
                    const size_t n = size_t(level_dim(img.w, l)) * level_dim(img.h, l) * img.level_depth(l);
                    fill.resize(n * 4);
                    for (size_t i = 0; i < n; i++) std::memcpy(&fill[i * 4], px.data(), 4);
                    if (!gpu_->upload_subresource(t, layer, l, fill.data(), fill.size())) {
                        release_gpu_texture(t);
                        return 0;
                    }
                }
        }
    }
    return t;
}

uint32_t Device::create_gpu_texture(const TexDesc& d) {
    const uint32_t t = gpu_->create_texture(d);
    if (t) {
        const uint64_t n = texture_bytes(d);
        gpu_texture_bytes_[t] = n;
        Report& r = report();
        r.gpu_texture_bytes += n;
        r.gpu_texture_bytes_peak = std::max(r.gpu_texture_bytes_peak, r.gpu_texture_bytes);
    }
    return t;
}

void Device::release_gpu_texture(uint32_t t) {
    if (!t) return;
    auto it = gpu_texture_bytes_.find(t);
    if (it != gpu_texture_bytes_.end()) {
        report().gpu_texture_bytes -= it->second;
        gpu_texture_bytes_.erase(it);
    }
    gpu_->release_texture(t);
}

bool Device::upload_image_subresource(Image& img, uint32_t sub) {
    if (!img.gpu || sub >= img.store.size() || img.store[sub].empty()) return false;
    const std::vector<uint8_t>* src = &img.store[sub];
    std::vector<uint8_t> filled;
    if (img.decoded) {
        // The whole level decoded (a partial lock's rectangle is a part of it), slice by slice for a volume.
        const auto t0 = std::chrono::steady_clock::now();
        const uint32_t level = sub % img.levels, lw = level_dim(img.w, level), lh = level_dim(img.h, level);
        const uint32_t slices = img.level_depth(level);
        const size_t in_slice = level_bytes(lw, lh, *img.fi), out_slice = size_t(lw) * lh * 4;
        bool ok = src->size() == in_slice * slices;
        if (ok && slices == 1) {
            ok = tex::decode_level_sampled(src->data(), lw, lh, img.d3dfmt, 0, &filled);
        } else if (ok) {
            filled.resize(out_slice * slices);
            std::vector<uint8_t> one;
            for (uint32_t z = 0; z < slices && ok; z++) {
                ok = tex::decode_level_sampled(src->data() + in_slice * z, lw, lh, img.d3dfmt, 0, &one);
                if (ok) std::memcpy(filled.data() + out_slice * z, one.data(), out_slice);
            }
        }
        if (!ok) {
            backend_failure("texture level decode", std::string("the ") + img.fi->name + " level could not be decoded");
            return false;
        }
        Report& r = report();
        r.decoded_levels++;
        r.decoded_block_bytes += src->size();
        r.decode_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        src = &filled;
    } else if (img.fi->x8) {
        filled = *src;
        x8_alpha_fill(filled.data(), filled.size());
        src = &filled;
    } else if (img.fi->expand != Expand::none) {
        // R5G6B5 and A8: the store's packed pixels (every slice of a volume level) as BGRA8.
        const size_t pixels = src->size() / img.fi->pixel_bytes;
        filled.resize(pixels * 4);
        expand_to_bgra8(img.fi->expand, src->data(), pixels, filled.data());
        src = &filled;
    }
    const bool ok = gpu_->upload_subresource(img.gpu, sub / img.levels, sub % img.levels, src->data(), src->size());
    if (ok) {
        report().texture_level_uploads++;
        report().texture_upload_bytes += src->size();
    } else backend_failure("texture level upload", gpu_->error());
    return ok;
}

// NormalizeTextureProperties (d3d9_common_texture.cpp:126-300), the cases a stream
// can reach, and this device's formats and pools.
HRESULT Device::make_texture_image(uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t fmt,
                                   uint32_t pool, bool lockable, bool is_texture, bool multisample,
                                   std::unique_ptr<Image>* out, TexType type, uint32_t depth) {
    const FormatInfo* fi = format_info(fmt);
    if (!fi) {
        char b[96];
        std::snprintf(b, sizeof b, "format %u (0x%08x)", fmt, fmt);
        not_implemented_value(b);
        return D3DERR_INVALIDCALL;
    }
    // :142-146: a cube of a depth format (d3d9.supportCubeDepthFormats is false, d3d9_options.cpp:54)
    if (type == TexType::cube && fi->depth) return D3DERR_INVALIDCALL;
    if (multisample) { not_implemented_value("multisampling"); return D3DERR_INVALIDCALL; }
    if (w == 0 || h == 0 || depth == 0) return D3DERR_INVALIDCALL;                         // :174
    if (fi->block_bytes && ((w & 3) || (h & 3))) return D3DERR_INVALIDCALL;              // :179-185
    if (pool == D3DPOOL_MANAGED && (usage & D3DUSAGE_DYNAMIC)) return D3DERR_INVALIDCALL; // :216
    if (usage & D3DUSAGE_WRITEONLY) return D3DERR_INVALIDCALL;                           // :220
    const uint32_t rtds = D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL;
    if (pool != D3DPOOL_DEFAULT && (usage & rtds)) return D3DERR_INVALIDCALL;            // :226
    if (pool == D3DPOOL_DEFAULT && (usage & rtds) && (usage & D3DUSAGE_DYNAMIC)) return D3DERR_INVALIDCALL;   // :231
    const bool plain = !is_texture && !(usage & rtds);
    if (!plain && pool != D3DPOOL_DEFAULT && fi->depth) return D3DERR_INVALIDCALL;       // :242
    if ((fi->depth && (usage & D3DUSAGE_RENDERTARGET)) || (!fi->depth && (usage & D3DUSAGE_DEPTHSTENCIL)))
        return D3DERR_INVALIDCALL;                                                          // :248
    if (type == TexType::volume && (usage & D3DUSAGE_RENDERTARGET)) return D3DERR_INVALIDCALL;   // :255-257
    if (type == TexType::volume && pool == D3DPOOL_SCRATCH && (usage & D3DUSAGE_DYNAMIC))
        return D3DERR_INVALIDCALL;                                                          // :260-264
    if ((usage & D3DUSAGE_AUTOGENMIPMAP) && !is_texture) return D3DERR_INVALIDCALL;     // :270
    if (usage & D3DUSAGE_AUTOGENMIPMAP) { not_implemented_value("D3DUSAGE_AUTOGENMIPMAP"); return D3DERR_INVALIDCALL; }
    if (pool != D3DPOOL_DEFAULT && pool != D3DPOOL_MANAGED && pool != D3DPOOL_SYSTEMMEM && pool != D3DPOOL_SCRATCH) {
        not_implemented_value("pool " + std::to_string(pool));
        return D3DERR_INVALIDCALL;
    }
    // A render-target cube is made (each face's surface a colour target by layer: the game's
    // render-target creator makes one, GetCubeMapSurface then SetRenderTarget); a volume of a depth
    // format has no attachment here, by name (DXVK refuses a render-target volume, :255-257).
    if (type == TexType::volume && fi->depth) { not_implemented_value("a volume texture of a depth format"); return D3DERR_INVALIDCALL; }
    // R5G6B5 and A8 are made BGRA8 from their packed store on upload: a draw into one would leave
    // BGRA8 texels that are not the format's, so a render target of one (CreateRenderTarget's too)
    // is not implemented, by name.
    if (fi->expand != Expand::none && (usage & D3DUSAGE_RENDERTARGET)) {
        not_implemented_value(std::string("a render target of ") + fi->name + " (the GPU image is BGRA8, expanded only on upload)");
        return D3DERR_INVALIDCALL;
    }
    const uint32_t max_levels = full_chain(std::max(w, depth), h);                       // :277-284 (computeMipLevelCount of w, h, d)
    if (levels == 0 || levels > max_levels) levels = max_levels;

    auto img = std::make_unique<Image>();
    img->w = w; img->h = h; img->levels = levels; img->usage = usage; img->pool = pool; img->d3dfmt = fmt;
    img->fi = fi; img->lockable = lockable; img->is_texture = is_texture;
    img->type = type; img->depth = depth; img->layers = type == TexType::cube ? 6u : 1u;
    const uint32_t subs = levels * img->layers;
    img->locked.assign(subs, 0);
    img->lock_flags.assign(subs, 0);
    // The GPU keeps every DEFAULT and MANAGED image; a CPU store is kept where the image
    // is lockable (MANAGED, SYSTEMMEM, SCRATCH, DYNAMIC; a lockable render target or
    // depth surface would need a readback at LockRect, which no stream makes: not kept,
    // so such a lock is not implemented, by name).
    const bool on_gpu = pool == D3DPOOL_DEFAULT || pool == D3DPOOL_MANAGED;
    const bool store = lockable && !(usage & rtds);
    if (store) {
        img->store.resize(subs);
        for (uint32_t sub = 0; sub < subs; sub++) {
            const uint32_t l = sub % levels;
            img->store[sub].assign(level_bytes(level_dim(w, l), level_dim(h, l), *fi) * img->level_depth(l), 0);
        }
    }
    if (on_gpu) {
        img->gpu = make_gpu_image(*img);
        if (!img->gpu) {
            std::fprintf(stderr, "d3d9gpu: texture %ux%u %s: %s\n", w, h, fi->name, gpu_->error().c_str());
            return D3DERR_OUTOFVIDEOMEMORY;
        }
    }
    *out = std::move(img);
    return D3D_OK;
}

bool Device::init(std::string* why) {
    // The implicit back buffer: one, never rotated (check_present_parameters refused
    // any case DXVK would rotate), with RENDERTARGET usage; not lockable (DXVK's need
    // D3DPRESENTFLAG_LOCKABLE_BACKBUFFER, which no stream sets: not implemented).
    if (pp_.Flags & D3DPRESENTFLAG_LOCKABLE_BACKBUFFER) not_implemented_value("D3DPRESENTFLAG_LOCKABLE_BACKBUFFER");
    std::unique_ptr<Image> img;
    const uint32_t fmt = pp_.BackBufferFormat == D3DFMT_UNKNOWN ? uint32_t(D3DFMT_X8R8G8B8) : uint32_t(pp_.BackBufferFormat);
    HRESULT hr = make_texture_image(pp_.BackBufferWidth, pp_.BackBufferHeight, 1, D3DUSAGE_RENDERTARGET, fmt,
                                    D3DPOOL_DEFAULT, false, false, false, &img);
    if (FAILED(hr)) { *why = "the back buffer could not be made: " + gpu_->error(); return false; }
    bb_ = new Surface(this, std::move(img), false);
    bb_->owned_ = true;
    bb_->pub_ = 0;
    if (pp_.EnableAutoDepthStencil) {
        hr = make_texture_image(pp_.BackBufferWidth, pp_.BackBufferHeight, 1, D3DUSAGE_DEPTHSTENCIL,
                                uint32_t(pp_.AutoDepthStencilFormat), D3DPOOL_DEFAULT, false, false, false, &img);
        if (FAILED(hr)) { *why = "the automatic depth-stencil could not be made: " + gpu_->error(); return false; }
        auto_ds_ = new Surface(this, std::move(img), false);
        auto_ds_->owned_ = true;
        auto_ds_->pub_ = 0;
    }
    // ResetState: render target 0 the back buffer (viewport and scissor its size), the
    // automatic depth-stencil bound.
    bind(rt_[0], bb_);
    if (auto_ds_) bind(ds_, auto_ds_);
    vp_ = D3DVIEWPORT9{0, 0, pp_.BackBufferWidth, pp_.BackBufferHeight, 0.0f, 1.0f};
    sc_ = RECT{0, 0, LONG(pp_.BackBufferWidth), LONG(pp_.BackBufferHeight)};
    if (!gpu_->finish()) { *why = "the GPU did not finish the device's setup: " + gpu_->error(); return false; }
    return true;
}

Device::~Device() {
    destroying_ = true;
    if (!gpu_->finish()) backend_failure("device teardown submit", gpu_->error());
    for (auto& r : rt_) bind(r, static_cast<Surface*>(nullptr));
    bind(ds_, static_cast<Surface*>(nullptr));
    for (auto& t : tex_) bind(t, static_cast<Texture*>(nullptr));
    for (auto& t : tex_other_) bind(t, static_cast<ObjectBase*>(nullptr));
    for (auto& s : streams_) bind(s.vb, static_cast<VertexBuffer*>(nullptr));
    bind(indices_, static_cast<IndexBuffer*>(nullptr));
    bind(decl_, static_cast<VertexDecl*>(nullptr));
    bind(vs_, static_cast<VertexShader*>(nullptr));
    bind(ps_, static_cast<PixelShader*>(nullptr));
    for (auto& [fvf, d] : fvf_decls_) { d->orphan(); delete d; }
    fvf_decls_.clear();
    // What the caller never released: counted, and cut loose from the device.
    std::vector<ObjectBase*> left(live_.begin(), live_.end());
    for (ObjectBase* o : left) {
        report().live_at_destroy[o->kind()]++;
        o->orphan();
    }
    live_.clear();
    delete bb_;
    delete auto_ds_;
    for (auto& [key, t] : scratch_) release_gpu_texture(t);
    if (semantic_null_gpu_) gpu_->release_buffer(semantic_null_gpu_);
    if (dummy_texture_gpu_) release_gpu_texture(dummy_texture_gpu_);
    if (fan_gpu_) gpu_->release_buffer(fan_gpu_);
    if (!gpu_->finish()) backend_failure("device teardown finish", gpu_->error());
    report().gpu_counters = gpu_->counters();
    gpu_.reset();   // the SDL_gpu device goes, and the window is released, before the caller's window
    write_report("destroyed", true);
}

void Device::enlist(ObjectBase* o, const char* kind) {
    live_.insert(o);
    report().created[kind]++;
}

void Device::delist(ObjectBase* o) { live_.erase(o); }

void Device::write_report(const char* phase, bool complete) {
    Report& r = report();
    r.phase = phase;
    r.complete = complete;
    if (gpu_) r.gpu_counters = gpu_->counters();
    write_keys_file();
    r.write();
}

template <class T>
void Device::bind(T*& slot, T* obj) {
    if (slot == obj) return;
    if (obj) obj->add_priv();
    T* old = slot;
    slot = obj;
    if (old) old->release_priv();
}

HRESULT STDMETHODCALLTYPE Device::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return answer(I_DEVICE, 0, E_POINTER);
    *ppv = nullptr;
    const GUID unk = __uuidof(IUnknown), own = __uuidof(IDirect3DDevice9);
    if (!std::memcmp(&riid, &unk, sizeof(GUID)) || !std::memcmp(&riid, &own, sizeof(GUID))) {
        AddRef();
        *ppv = static_cast<IDirect3DDevice9*>(this);
        return answer(I_DEVICE, 0, S_OK);
    }
    return answer(I_DEVICE, 0, E_NOINTERFACE);
}

ULONG STDMETHODCALLTYPE Device::AddRef() { return ++refs_; }

ULONG STDMETHODCALLTYPE Device::Release() {
    if (!refs_) return 0;
    const ULONG n = --refs_;
    if (!n) delete this;
    return n;
}

// ------------------------------------------------------------------ frames
void Device::close_frame() {
    Report& r = report();
    const uint64_t frame = r.presents + 1;
    // A live session's runs stop at kLiveRuns (about five minutes of play, each frame's draw count
    // differing): the report holds them all, and its write grows with them on the render thread.
    // Replays and tests, which compare them, keep every run.
    constexpr size_t kLiveRuns = 20000;
    if (!r.per_frame.empty() && r.per_frame.back()[2] == r.frame_draws && r.per_frame.back()[1] + 1 == frame)
        r.per_frame.back()[1] = frame;
    else if (r.per_frame.size() < kLiveRuns || !gpu_ || !gpu_->info().live_session)
        r.per_frame.push_back({frame, frame, r.frame_draws});
    r.frame_draws = 0;
    // Preserve the census's DXVK CPU layout account; GPU uploads occurred at each draw.
    const auto& arena = up_frame_.bytes();
    r.up_frame_peak = std::max<uint64_t>(r.up_frame_peak, arena.size());
    r.up_arena_bytes += arena.size();
    r.up_pad_bytes += up_frame_.pad_bytes();
    up_frame_.reset();
}

// PresentEx (:539-551) through the implicit swap chain: the back buffer to the window.
HRESULT STDMETHODCALLTYPE Device::Present(const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty) {
    if (src || dst || wnd || dirty) {
        not_implemented_value("Present with a rectangle, a window override or a dirty region");
        return answer(I_DEVICE, 17, D3DERR_INVALIDCALL);
    }
    close_frame();
    Report& r = report();
    PresentInfo pi;
    const bool ok = gpu_->present(bb_->image()->gpu, &pi);
    r.presents++;
    if (pi.presented) r.presented++;
    if (pi.no_swapchain_texture) r.no_swapchain_texture++;
    if (pi.sw_w) { r.swapchain_w = pi.sw_w; r.swapchain_h = pi.sw_h; }
    // A frame without the window's surface (PresentInfo::surface_lost) was drawn and submitted, and is
    // answered D3D_OK, as DXVK answers it (d3d9_swapchain.cpp:107-200): counted, and said on stderr
    // where a run of them starts and where the surface is back.
    if (pi.surface_lost) {
        r.present_surface_lost++;
        if (!surface_lost_run_++)
            std::fprintf(stderr, "d3d9gpu: Present %llu: the window has no surface (%s): frames are drawn and submitted, "
                         "not shown, until SDL makes it again\n", (unsigned long long)r.presents,
                         pi.surface_lost_reason.c_str());
    } else if (surface_lost_run_ && pi.presented) {
        std::fprintf(stderr, "d3d9gpu: Present %llu: the window's surface is back after %llu frames without it\n",
                     (unsigned long long)r.presents, (unsigned long long)surface_lost_run_);
        surface_lost_run_ = 0;
    }
    if (!ok) {
        r.present_failures++;
        std::fprintf(stderr, "d3d9gpu: Present failed: %s\n", gpu_->error().c_str());
    }
    if (r.fault.bits & F_PRESENT_SCRIBBLE) {
        ClearValue v;
        v.colour = true;
        std::memcpy(v.rgba, kScribble, sizeof kScribble);
        gpu_->clear(bb_->image()->gpu, 0, v);
        r.scribbles++;
    }
    if (r.presents % 100 == 0 && report_due(r)) write_report("replay", false);
    return answer(I_DEVICE, 17, ok ? D3D_OK : D3DERR_DRIVERINTERNALERROR);
}

// Whether the 100th Present rewrites the report. A replay or a test: always. A live session
// (GpuInfo::live_session): its first 600 Presents, then every 3600th (a minute at 60 fps), and the
// first after a new failure, so the crash report (host/android CrashReport.java) still finds the
// device and any failure. Each write builds the report and the keys file on the render thread:
// about 5 ms on a Snapdragon 750G's big core three minutes in, and more as per_frame grows.
bool Device::report_due(const Report& r) {
    if (!gpu_ || !gpu_->info().live_session || r.presents <= 600) return true;
    const uint64_t failures = r.present_failures + r.backend_failed_draws + r.clear_host_failures +
                              r.draw_failures.size() + (r.failure.empty() ? 0 : 1);
    if (failures != reported_failures_) {
        reported_failures_ = failures;
        return true;
    }
    return r.presents % 3600 == 0;
}

// GetBackBuffer (:553-563, d3d9_swapchain.cpp:473-497).
HRESULT STDMETHODCALLTYPE Device::GetBackBuffer(UINT swap_chain, UINT index, D3DBACKBUFFER_TYPE type,
                                                IDirect3DSurface9** pp) {
    if (pp) *pp = nullptr;
    if (swap_chain != 0 || !pp || index >= 1) return answer(I_DEVICE, 18, D3DERR_INVALIDCALL);
    bb_->AddRef();
    *pp = bb_;
    return answer(I_DEVICE, 18, D3D_OK);
}

// ------------------------------------------------------------------ resources
HRESULT STDMETHODCALLTYPE Device::CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                                IDirect3DTexture9** pp, HANDLE* shared) {
    if (pp) *pp = nullptr;
    if (!pp) return answer(I_DEVICE, 23, D3DERR_INVALIDCALL);
    if (shared) { not_implemented_value("CreateTexture with pSharedHandle"); return answer(I_DEVICE, 23, D3DERR_INVALIDCALL); }
    // :624-628: lockable unless DEFAULT without DYNAMIC.
    const bool lockable = pool != D3DPOOL_DEFAULT || (usage & D3DUSAGE_DYNAMIC);
    std::unique_ptr<Image> img;
    const HRESULT hr = make_texture_image(w, h, levels, usage, fmt, pool, lockable, true, false, &img);
    if (FAILED(hr)) return answer(I_DEVICE, 23, hr);
    *pp = new Texture(this, std::move(img));
    return answer(I_DEVICE, 23, D3D_OK);
}

// CreateVolumeTexture (:675-741): lockable unless DEFAULT without DYNAMIC.
HRESULT STDMETHODCALLTYPE Device::CreateVolumeTexture(UINT w, UINT h, UINT depth, UINT levels, DWORD usage, D3DFORMAT fmt,
                                                      D3DPOOL pool, IDirect3DVolumeTexture9** pp, HANDLE* shared) {
    if (pp) *pp = nullptr;
    if (!pp) return answer(I_DEVICE, 24, D3DERR_INVALIDCALL);
    if (shared && pool != D3DPOOL_DEFAULT) return answer(I_DEVICE, 24, D3DERR_INVALIDCALL);   // :690-691
    if (shared) { not_implemented_value("CreateVolumeTexture with pSharedHandle"); return answer(I_DEVICE, 24, D3DERR_INVALIDCALL); }
    const bool lockable = pool != D3DPOOL_DEFAULT || (usage & D3DUSAGE_DYNAMIC);
    std::unique_ptr<Image> img;
    const HRESULT hr = make_texture_image(w, h, levels, usage, fmt, pool, lockable, true, false, &img, TexType::volume, depth);
    if (FAILED(hr)) return answer(I_DEVICE, 24, hr);
    *pp = new VolumeTexture(this, std::move(img));
    return answer(I_DEVICE, 24, D3D_OK);
}

// CreateCubeTexture (:744-810): six faces of EdgeLength; lockable unless DEFAULT without DYNAMIC
// (DXVK's vendor-format exception names formats this device does not make).
HRESULT STDMETHODCALLTYPE Device::CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                                    IDirect3DCubeTexture9** pp, HANDLE* shared) {
    if (pp) *pp = nullptr;
    if (!pp) return answer(I_DEVICE, 25, D3DERR_INVALIDCALL);
    if (shared && pool != D3DPOOL_DEFAULT) return answer(I_DEVICE, 25, D3DERR_INVALIDCALL);   // :757-758
    if (shared) { not_implemented_value("CreateCubeTexture with pSharedHandle"); return answer(I_DEVICE, 25, D3DERR_INVALIDCALL); }
    const bool lockable = pool != D3DPOOL_DEFAULT || (usage & D3DUSAGE_DYNAMIC);
    std::unique_ptr<Image> img;
    const HRESULT hr = make_texture_image(edge, edge, levels, usage, fmt, pool, lockable, true, false, &img, TexType::cube);
    if (FAILED(hr)) return answer(I_DEVICE, 25, hr);
    *pp = new CubeTexture(this, std::move(img));
    return answer(I_DEVICE, 25, D3D_OK);
}

// CreateVertexBuffer, CreateIndexBuffer (:811-913) and ValidateBufferProperties
// (d3d9_common_buffer.cpp:50-70); the GPU buffer zero-filled (InitBuffer).
HRESULT STDMETHODCALLTYPE Device::CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
                                                     IDirect3DVertexBuffer9** pp, HANDLE* shared) {
    if (pp) *pp = nullptr;
    if (!pp) return answer(I_DEVICE, 26, D3DERR_INVALIDCALL);
    if (shared && pool != D3DPOOL_DEFAULT) return answer(I_DEVICE, 26, D3DERR_NOTAVAILABLE);
    if (shared) { not_implemented_value("CreateVertexBuffer with pSharedHandle"); return answer(I_DEVICE, 26, D3DERR_INVALIDCALL); }
    if (length == 0 || pool == D3DPOOL_SCRATCH || (pool == D3DPOOL_MANAGED && (usage & D3DUSAGE_DYNAMIC)) ||
        (usage & (D3DUSAGE_AUTOGENMIPMAP | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_RENDERTARGET)))
        return answer(I_DEVICE, 26, D3DERR_INVALIDCALL);
    const uint32_t buf = gpu_->create_buffer(length, false);
    if (!buf) return answer(I_DEVICE, 26, D3DERR_INVALIDCALL);
    report().zero_init_bytes += length;
    *pp = new VertexBuffer(this, length, usage, pool, fvf, buf);
    return answer(I_DEVICE, 26, D3D_OK);
}

HRESULT STDMETHODCALLTYPE Device::CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                                    IDirect3DIndexBuffer9** pp, HANDLE* shared) {
    if (pp) *pp = nullptr;
    if (!pp) return answer(I_DEVICE, 27, D3DERR_INVALIDCALL);
    if (shared && pool != D3DPOOL_DEFAULT) return answer(I_DEVICE, 27, D3DERR_NOTAVAILABLE);
    if (shared) { not_implemented_value("CreateIndexBuffer with pSharedHandle"); return answer(I_DEVICE, 27, D3DERR_INVALIDCALL); }
    if (length == 0 || pool == D3DPOOL_SCRATCH || (pool == D3DPOOL_MANAGED && (usage & D3DUSAGE_DYNAMIC)) ||
        (usage & (D3DUSAGE_AUTOGENMIPMAP | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_RENDERTARGET)))
        return answer(I_DEVICE, 27, D3DERR_INVALIDCALL);
    const uint32_t buf = gpu_->create_buffer(length, true);
    if (!buf) return answer(I_DEVICE, 27, D3DERR_INVALIDCALL);
    report().zero_init_bytes += length;
    *pp = new IndexBuffer(this, length, usage, pool, fmt, buf);
    return answer(I_DEVICE, 27, D3D_OK);
}

// CreateRenderTargetEx (:4275-4334): RENDERTARGET, DEFAULT, one level.
HRESULT STDMETHODCALLTYPE Device::CreateRenderTarget(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD q,
                                                     WINBOOL lockable, IDirect3DSurface9** pp, HANDLE* shared) {
    if (pp) *pp = nullptr;
    if (!pp) return answer(I_DEVICE, 28, D3DERR_INVALIDCALL);
    if (shared) { not_implemented_value("CreateRenderTarget with pSharedHandle"); return answer(I_DEVICE, 28, D3DERR_INVALIDCALL); }
    std::unique_ptr<Image> img;
    const HRESULT hr = make_texture_image(w, h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, lockable != FALSE,
                                          false, ms != D3DMULTISAMPLE_NONE, &img);
    if (FAILED(hr)) return answer(I_DEVICE, 28, hr);
    *pp = new Surface(this, std::move(img));
    return answer(I_DEVICE, 28, D3D_OK);
}

// CreateDepthStencilSurfaceEx (:4421-4473): DEPTHSTENCIL, DEFAULT, lockable only for
// the lockable depth formats (none of this device's).
HRESULT STDMETHODCALLTYPE Device::CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms,
                                                            DWORD q, WINBOOL discard, IDirect3DSurface9** pp,
                                                            HANDLE* shared) {
    if (pp) *pp = nullptr;
    if (!pp) return answer(I_DEVICE, 29, D3DERR_INVALIDCALL);
    if (shared) { not_implemented_value("CreateDepthStencilSurface with pSharedHandle"); return answer(I_DEVICE, 29, D3DERR_INVALIDCALL); }
    const FormatInfo* fi = format_info(fmt);
    if (discard && fi && !fi->depth) return answer(I_DEVICE, 29, D3DERR_INVALIDCALL);   // :286-288
    std::unique_ptr<Image> img;
    const HRESULT hr = make_texture_image(w, h, 1, D3DUSAGE_DEPTHSTENCIL, fmt, D3DPOOL_DEFAULT, false, false,
                                          ms != D3DMULTISAMPLE_NONE, &img);
    if (FAILED(hr)) return answer(I_DEVICE, 29, hr);
    *pp = new Surface(this, std::move(img));
    return answer(I_DEVICE, 29, D3D_OK);
}

// CreateOffscreenPlainSurfaceEx (:4339-4417): always lockable; a depth format must be a
// lockable one. SYSTEMMEM and SCRATCH only here (CPU stores); DEFAULT and MANAGED are
// not implemented, by name.
HRESULT STDMETHODCALLTYPE Device::CreateOffscreenPlainSurface(UINT w, UINT h, D3DFORMAT fmt, D3DPOOL pool,
                                                              IDirect3DSurface9** pp, HANDLE* shared) {
    if (pp) *pp = nullptr;
    if (!pp) return answer(I_DEVICE, 36, D3DERR_INVALIDCALL);
    if (shared) { not_implemented_value("CreateOffscreenPlainSurface with pSharedHandle"); return answer(I_DEVICE, 36, D3DERR_INVALIDCALL); }
    const FormatInfo* fi = format_info(fmt);
    if (fi && fi->depth) return answer(I_DEVICE, 36, D3DERR_INVALIDCALL);   // D24S8 is not a lockable depth format
    if (pool != D3DPOOL_SYSTEMMEM && pool != D3DPOOL_SCRATCH) {
        not_implemented_value("an offscreen plain surface in pool " + std::to_string(uint32_t(pool)));
        return answer(I_DEVICE, 36, D3DERR_INVALIDCALL);
    }
    std::unique_ptr<Image> img;
    const HRESULT hr = make_texture_image(w, h, 1, 0, fmt, pool, true, false, false, &img);
    if (FAILED(hr)) return answer(I_DEVICE, 36, hr);
    *pp = new Surface(this, std::move(img));
    return answer(I_DEVICE, 36, D3D_OK);
}

// GetRenderTargetData (:1134-1199): a GPU image's level read into a SYSTEMMEM store.
HRESULT STDMETHODCALLTYPE Device::GetRenderTargetData(IDirect3DSurface9* rt, IDirect3DSurface9* dst) {
    Surface* s = static_cast<Surface*>(rt);
    Surface* d = static_cast<Surface*>(dst);
    if (!s || !d) return answer(I_DEVICE, 32, D3DERR_INVALIDCALL);
    if (rt == dst) return answer(I_DEVICE, 32, D3D_OK);
    Image& si = *s->image();
    Image& di = *d->image();
    if (si.d3dfmt != di.d3dfmt) return answer(I_DEVICE, 32, D3DERR_INVALIDCALL);
    if (s->width() != d->width() || s->height() != d->height()) return answer(I_DEVICE, 32, D3DERR_INVALIDCALL);
    if (di.pool == D3DPOOL_DEFAULT) {
        not_implemented_value("GetRenderTargetData into a DEFAULT-pool surface (DXVK's StretchRect)");
        return answer(I_DEVICE, 32, D3DERR_INVALIDCALL);
    }
    if (si.type != TexType::flat) {
        not_implemented_value("GetRenderTargetData of a cube face (the readback is of a 2D level)");
        return answer(I_DEVICE, 32, D3DERR_INVALIDCALL);
    }
    if (si.fi->expand != Expand::none) {
        // its GPU texels are BGRA8, not the destination store's packed format
        not_implemented_value(std::string("GetRenderTargetData of a ") + si.fi->name +
                              " surface (the GPU image is BGRA8, expanded only on upload)");
        return answer(I_DEVICE, 32, D3DERR_INVALIDCALL);
    }
    if (!si.gpu || si.fi->gpu != GFmt::bgra8 || di.store.size() <= d->subresource() || di.store[d->subresource()].empty()) {
        not_implemented_value(std::string("GetRenderTargetData of a ") + si.fi->name + " surface " +
                              (si.gpu ? "into a surface with no CPU store" : "with no GPU image"));
        return answer(I_DEVICE, 32, D3DERR_INVALIDCALL);
    }
    std::vector<uint8_t>& out = di.store[d->subresource()];
    if (!gpu_->download(si.gpu, s->level(), out.data(), out.size())) {
        std::fprintf(stderr, "d3d9gpu: readback failed: %s\n", gpu_->error().c_str());
        return answer(I_DEVICE, 32, D3DERR_DRIVERINTERNALERROR);
    }
    report().readbacks++;
    return answer(I_DEVICE, 32, D3D_OK);
}

// ------------------------------------------------------------------ targets
// SetRenderTarget and SetRenderTargetInternal (:1638-1782): render target 0 sets the
// viewport and scissor to its size even when it is already bound.
HRESULT STDMETHODCALLTYPE Device::SetRenderTarget(DWORD index, IDirect3DSurface9* surface) {
    Surface* s = static_cast<Surface*>(surface);
    if (!s && index == 0) return answer(I_DEVICE, 37, D3DERR_INVALIDCALL);
    if (s && s->device() != this) return answer(I_DEVICE, 37, D3DERR_INVALIDCALL);
    if (index >= 4) return answer(I_DEVICE, 37, D3DERR_INVALIDCALL);
    if (s && !(s->image()->usage & D3DUSAGE_RENDERTARGET)) return answer(I_DEVICE, 37, D3DERR_INVALIDCALL);
    if (index == 0) {
        vp_ = D3DVIEWPORT9{0, 0, s->width(), s->height(), 0.0f, 1.0f};
        sc_ = RECT{0, 0, LONG(s->width()), LONG(s->height())};
    }
    if (rt_[index] != s) pipe_dirty_ = prep_dirty_ = true;
    bind(rt_[index], s);
    return answer(I_DEVICE, 37, D3D_OK);
}

// GetRenderTarget (:1785-1797): index > 4 refused (DXVK's bound; index 4 reads past
// its array, so not implemented here), none bound D3DERR_NOTFOUND.
HRESULT STDMETHODCALLTYPE Device::GetRenderTarget(DWORD index, IDirect3DSurface9** pp) {
    if (pp) *pp = nullptr;
    if (!pp || index > 4) return answer(I_DEVICE, 38, D3DERR_INVALIDCALL);
    if (index == 4) { not_implemented_value("GetRenderTarget(4)"); return answer(I_DEVICE, 38, D3DERR_INVALIDCALL); }
    if (!rt_[index]) return answer(I_DEVICE, 38, D3DERR_NOTFOUND);
    rt_[index]->AddRef();
    *pp = rt_[index];
    return answer(I_DEVICE, 38, D3D_OK);
}

// SetDepthStencilSurface (:1799-1830).
HRESULT STDMETHODCALLTYPE Device::SetDepthStencilSurface(IDirect3DSurface9* surface) {
    Surface* s = static_cast<Surface*>(surface);
    if (s && !(s->image()->usage & D3DUSAGE_DEPTHSTENCIL)) return answer(I_DEVICE, 39, D3DERR_INVALIDCALL);
    if (ds_ != s) pipe_dirty_ = prep_dirty_ = true;
    bind(ds_, s);
    return answer(I_DEVICE, 39, D3D_OK);
}

HRESULT STDMETHODCALLTYPE Device::GetDepthStencilSurface(IDirect3DSurface9** pp) {
    if (pp) *pp = nullptr;
    if (!pp) return answer(I_DEVICE, 40, D3DERR_INVALIDCALL);
    if (!ds_) return answer(I_DEVICE, 40, D3DERR_NOTFOUND);
    ds_->AddRef();
    *pp = ds_;
    return answer(I_DEVICE, 40, D3D_OK);
}

// BeginScene, EndScene (:1852-1885).
HRESULT STDMETHODCALLTYPE Device::BeginScene() {
    if (in_scene_) return answer(I_DEVICE, 41, (report().fault.bits & F_SCENE_ANSWER) ? D3D_OK : D3DERR_INVALIDCALL);
    in_scene_ = true;
    return answer(I_DEVICE, 41, D3D_OK);
}

HRESULT STDMETHODCALLTYPE Device::EndScene() {
    if (!in_scene_) return answer(I_DEVICE, 42, D3DERR_INVALIDCALL);
    in_scene_ = false;
    return answer(I_DEVICE, 42, D3D_OK);
}

// ------------------------------------------------------------------ Clear
HRESULT STDMETHODCALLTYPE Device::Clear(DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR colour, float z,
                                        DWORD stencil) {
    report().clear_calls++;
    return answer(I_DEVICE, 43, clear_impl(count, rects, flags, colour, z, stencil));
}

// One subresource cleared by DXVK's per-image rule (d9g_clear.h): a full clear is a
// render pass with CLEAR for the cleared aspects and LOAD for the rest; a partial one
// copies the region into a scratch texture of the same format, clears it there the
// same way and copies it back (no shader and no SDL patch; exact for unorm8 colour and
// for D24 at Z 0.0 and 1.0, the copies moving both depth-stencil aspects).
Device::ClearDone Device::clear_subresource(Surface* s, const ImageClear& ic, const ClearValue& v, bool depth) {
    Image& img = *s->image();
    Report& r = report();
    if (ic.kind == ClearKind::skip) { r.clear_skipped_images++; return ClearDone::done; }
    if (!img.gpu) {
        not_implemented_value("Clear of a surface with no GPU image");
        return ClearDone::not_implemented;
    }
    const auto host = [](bool ok) { return ok ? ClearDone::done : ClearDone::host_failed; };
    if (ic.kind == ClearKind::full) {
        (depth ? r.clear_full_depth : r.clear_full_colour)++;
        return host(gpu_->clear_layer(img.gpu, s->face(), s->level(), v));
    }
    (depth ? r.clear_partial_depth : r.clear_partial_colour)++;
    if (ic.past_edge) {
        r.clear_past_edge++;
        not_implemented_value("a partial Clear running past its image's edge (DXVK's clamps only the extent)");
        return ClearDone::not_implemented;
    }
    const GFmt g = img.fi->gpu == GFmt::d24s8 ? gpu_->depth_format() : img.fi->gpu;
    const std::array<uint32_t, 3> key{uint32_t(g), ic.box.w, ic.box.h};
    uint32_t& scratch = scratch_[key];
    if (!scratch) {
        TexDesc d;
        d.w = ic.box.w; d.h = ic.box.h; d.levels = 1; d.fmt = g; d.target = true; d.sampled = false;
        scratch = create_gpu_texture(d);
        if (!scratch) return ClearDone::host_failed;
    }
    const uint32_t x = uint32_t(ic.box.x), y = uint32_t(ic.box.y);
    return host(gpu_->copy_layers(img.gpu, s->face(), s->level(), x, y, scratch, 0, 0, 0, 0, ic.box.w, ic.box.h) &&
                gpu_->clear(scratch, 0, v) &&
                gpu_->copy_layers(scratch, 0, 0, 0, 0, img.gpu, s->face(), s->level(), x, y, ic.box.w, ic.box.h));
}

// Clear (:1894-2075), in DXVK's order.
HRESULT Device::clear_impl(DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR colour, float z, DWORD stencil) {
    Report& r = report();
    if (!count && rects) { r.clear_zero_count_rects++; return D3D_OK; }                // :1902
    if (!ds_ && (flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL))) { r.clear_refused++; return D3DERR_INVALIDCALL; }   // :1909
    if ((flags & D3DCLEAR_TARGET) && rs_[kRS_SRGBWRITE]) {
        // DXVK clears through the render target's sRGB view (:1917, :2024): not here.
        not_implemented_value("Clear with D3DRS_SRGBWRITEENABLE set");
        return D3DERR_INVALIDCALL;
    }
    if (r.fault.bits & F_CLEAR_NOOP) { r.clear_fault_noop++; return D3D_OK; }
    ClearInput in;
    in.vp_x = vp_.X; in.vp_y = vp_.Y; in.vp_w = vp_.Width; in.vp_h = vp_.Height;
    in.sc_left = sc_.left; in.sc_top = sc_.top; in.sc_right = sc_.right; in.sc_bottom = sc_.bottom;
    in.scissor = rs_[kRS_SCISSOR] != 0;
    in.count = count;
    in.rects = reinterpret_cast<const int32_t*>(rects);
    const std::vector<ClearBox> regions = clear_regions(in);
    if (regions.empty()) { r.clear_empty++; return D3D_OK; }
    ClearValue depth_v, colour_v;
    if (ds_ && (flags & D3DCLEAR_ZBUFFER) && ds_->image()->fi->depth) depth_v.depth = true;
    if (ds_ && (flags & D3DCLEAR_STENCIL) && ds_->image()->fi->stencil) depth_v.stencil = true;
    depth_v.z = z;
    depth_v.s = stencil;
    colour_v.colour = true;
    decode_d3dcolor((r.fault.bits & F_CLEAR_COLOUR) ? (colour ^ 0x00010101u) : colour, colour_v.rgba);
    bool refused = false, host_failed = false;
    const auto note = [&](ClearDone c) {
        refused |= c == ClearDone::not_implemented;
        host_failed |= c == ClearDone::host_failed;
    };
    for (const ClearBox& box : regions) {
        r.clear_regions++;
        // ClearViewRect (:2013-2036): the depth-stencil first, then render targets 0..3.
        if (depth_v.depth || depth_v.stencil)
            note(clear_subresource(ds_, clear_image(box, ds_->width(), ds_->height()), depth_v, true));
        if (flags & D3DCLEAR_TARGET)
            for (Surface* t : rt_)
                if (t) note(clear_subresource(t, clear_image(box, t->width(), t->height()), colour_v, false));
    }
    // DXVK's Clear answers D3DERR_INVALIDCALL only at :1909 (above); what its context then does with the
    // clear never reaches the answer. A host GPU failure is the host's: named on stderr (the first 20)
    // and in the report (clears.host_failures, the failure), and the guest is answered D3D_OK. A case
    // this device does not implement is refused by name (not_implemented_value), as before.
    if (host_failed) {
        r.clear_host_failures++;
        backend_failure("Clear", gpu_->error());
        if (r.clear_host_failures <= 20)
            std::fprintf(stderr, "d3d9gpu: Clear: host failure, answered D3D_OK as DXVK answers it%s: %s\n",
                         r.clear_host_failures == 20 ? " (later ones are counted in the report only)" : "",
                         gpu_->error().c_str());
    }
    return refused ? D3DERR_INVALIDCALL : D3D_OK;
}

// ------------------------------------------------------------------ state
// SetStateTransform (:4652-4670); a NULL matrix crashes DXVK: not implemented here.
HRESULT STDMETHODCALLTYPE Device::SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* m) {
    if (!m) { not_implemented_value("SetTransform with a NULL matrix"); return answer(I_DEVICE, 44, D3DERR_INVALIDCALL); }
    transforms_[uint32_t(state)] = *m;
    return answer(I_DEVICE, 44, D3D_OK);
}

// SetViewport (:2112-2137): MaxZ is MinZ + 0.001 unless above MinZ.
HRESULT STDMETHODCALLTYPE Device::SetViewport(const D3DVIEWPORT9* vp) {
    if (!vp) return answer(I_DEVICE, 47, D3DERR_INVALIDCALL);
    vp_ = *vp;
    vp_.MaxZ = vp->MinZ < vp->MaxZ ? vp->MaxZ : vp->MinZ + 0.001f;
    return answer(I_DEVICE, 47, D3D_OK);
}

HRESULT STDMETHODCALLTYPE Device::GetViewport(D3DVIEWPORT9* vp) {
    if (!vp) return answer(I_DEVICE, 48, D3DERR_INVALIDCALL);
    *vp = vp_;
    return answer(I_DEVICE, 48, D3D_OK);
}

HRESULT STDMETHODCALLTYPE Device::SetMaterial(const D3DMATERIAL9* m) {      // :2152-2165
    if (!m) return answer(I_DEVICE, 49, D3DERR_INVALIDCALL);
    material_ = *m;
    return answer(I_DEVICE, 49, D3D_OK);
}

HRESULT STDMETHODCALLTYPE Device::SetLight(DWORD index, const D3DLIGHT9* l) {   // :2180-2200
    if (!l) return answer(I_DEVICE, 51, D3DERR_INVALIDCALL);
    lights_[index] = {true, *l};
    return answer(I_DEVICE, 51, D3D_OK);
}

HRESULT STDMETHODCALLTYPE Device::LightEnable(DWORD index, WINBOOL enable) {     // :2224-2245
    light_enabled_[index] = enable != FALSE;
    return answer(I_DEVICE, 53, D3D_OK);
}

HRESULT STDMETHODCALLTYPE Device::SetClipPlane(DWORD index, const float* plane) {   // :2267-2295
    if (!plane) return answer(I_DEVICE, 55, D3DERR_INVALIDCALL);
    if (index >= 6) index = 5;
    std::memcpy(clip_[index], plane, sizeof clip_[index]);
    return answer(I_DEVICE, 55, D3D_OK);
}

// SetRenderState (:2314-2672): states above 255, or 1-6, answer D3D_OK unstored.
HRESULT STDMETHODCALLTYPE Device::SetRenderState(D3DRENDERSTATETYPE state, DWORD value) {
    if (state > 255 || (state < D3DRS_ZENABLE && state != 0)) return answer(I_DEVICE, 57, D3D_OK);
    if (rs_[state] != value) {
        rs_[state] = value;
        // a state the pipeline input or the census tuple holds (d9g_state.h); prepare_draw's
        // reused pipeline only the input's (draw_pipeline)
        const bool input = pipeline_rs_index(state) >= 0;
        if (input) prep_dirty_ = true;
        if (input ||
            std::find(std::begin(kCensusExtraRenderStates), std::end(kCensusExtraRenderStates), state) !=
                std::end(kCensusExtraRenderStates))
            pipe_dirty_ = true;
    }
    return answer(I_DEVICE, 57, D3D_OK);
}

HRESULT STDMETHODCALLTYPE Device::GetRenderState(D3DRENDERSTATETYPE state, DWORD* value) {
    if (!value) return answer(I_DEVICE, 58, D3DERR_INVALIDCALL);
    if (state > 255 || (state < D3DRS_ZENABLE && state != 0)) return answer(I_DEVICE, 58, D3DERR_INVALIDCALL);
    *value = rs_[state];
    return answer(I_DEVICE, 58, D3D_OK);
}

// SetTexture (:2802-2810, SetStateTexture :4598-4649). The object is one of this device's three
// texture kinds: a 2D texture binds at tex_, a cube or volume texture at tex_other_ (the stage's
// other slot emptied).
HRESULT STDMETHODCALLTYPE Device::SetTexture(DWORD stage, IDirect3DBaseTexture9* t) {
    if (invalid_sampler(stage)) return answer(I_DEVICE, 65, D3D_OK);
    Texture*& slot = tex_[remap_sampler(stage)];
    ObjectBase*& other = tex_other_[remap_sampler(stage)];
    Texture* nt = dynamic_cast<Texture*>(t);
    ObjectBase* no = nullptr;
    if (t && !nt) {
        no = dynamic_cast<CubeTexture*>(t);
        if (!no) no = dynamic_cast<VolumeTexture*>(t);
        if (!no) { not_implemented_value("SetTexture of an object not this device's"); return answer(I_DEVICE, 65, D3DERR_INVALIDCALL); }
    }
    // the hazard bits (pipeline_input) read which render-target and depth-stencil textures are
    // bound (rt_tex_sampled, rt_tex_stages, ds_tex_sampled), a cube whose face is the target too
    auto hazard_tex = [](const Texture* x) {
        return x && (x->image()->usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL));
    };
    auto hazard_other = [](const ObjectBase* x) {
        const Image* i = x ? x->texture_image() : nullptr;
        return i && (i->usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL));
    };
    if (other != no && (hazard_other(other) || hazard_other(no))) pipe_dirty_ = prep_dirty_ = true;
    if (other != no) sampler_dirty_ |= 1u << remap_sampler(stage);
    bind(other, no);
    if (slot != nt && (hazard_tex(slot) || hazard_tex(nt))) pipe_dirty_ = prep_dirty_ = true;
    if (slot != nt) sampler_dirty_ |= 1u << remap_sampler(stage);
    bind(slot, nt);
    return answer(I_DEVICE, 65, D3D_OK);
}

// SetStateTextureStageState (:4673-4720): the stage and type clamped, never refused.
HRESULT STDMETHODCALLTYPE Device::SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) {
    const DWORD s = std::min<DWORD>(stage, 7), t = std::min<DWORD>(DWORD(type) - 1, 31);
    tss_[s][t] = value;
    return answer(I_DEVICE, 67, D3D_OK);
}

// SetSamplerState (:2860-2870, SetStateSamplerState :4556-4595). The state is stored
// raw: no SDL_GPUSampler is made in R2a (the border question is the user's).
HRESULT STDMETHODCALLTYPE Device::SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) {
    if (invalid_sampler(sampler)) return answer(I_DEVICE, 69, D3D_OK);
    if (uint32_t(type) >= 14) {
        // DXVK writes samplerStates[s][Type] past its 14 entries.
        not_implemented_value("SetSamplerState type " + std::to_string(uint32_t(type)));
        return answer(I_DEVICE, 69, D3DERR_INVALIDCALL);
    }
    const uint32_t stage = remap_sampler(sampler);
    if (type == D3DSAMP_MIPMAPLODBIAS) fetch4_[stage] = shader::fetch4_after(fetch4_[stage], value);
    ss_[stage][type] = value;
    sampler_dirty_ |= 1u << remap_sampler(sampler);
    return answer(I_DEVICE, 69, D3D_OK);
}

HRESULT STDMETHODCALLTYPE Device::SetScissorRect(const RECT* r) {           // :2953-2970
    if (!r) return answer(I_DEVICE, 75, D3DERR_INVALIDCALL);
    sc_ = *r;
    return answer(I_DEVICE, 75, D3D_OK);
}

HRESULT STDMETHODCALLTYPE Device::GetScissorRect(RECT* r) {
    if (!r) return answer(I_DEVICE, 76, D3DERR_INVALIDCALL);
    *r = sc_;
    return answer(I_DEVICE, 76, D3D_OK);
}

HRESULT STDMETHODCALLTYPE Device::SetSoftwareVertexProcessing(WINBOOL software) {   // :2985-2997
    if (software && !can_swvp()) return answer(I_DEVICE, 77, D3DERR_INVALIDCALL);
    if (!software && only_swvp()) return answer(I_DEVICE, 77, D3DERR_INVALIDCALL);
    swvp_ = software != FALSE;
    return answer(I_DEVICE, 77, D3D_OK);
}

// ------------------------------------------------------------------ draws
// Draw classification preserves D3D's refused/empty answers and distinguishes execution.
void Device::count_draw(const char* method, uint32_t type, DrawClass cls) {
    Report& r = report();
    const bool accepted = cls != DrawClass::Refused && cls != DrawClass::Empty;
    if (accepted && (r.fault.bits & F_COUNT_DRAW) && !fault_draw_dropped_) {
        fault_draw_dropped_ = true; r.fault_uncounted++; return;
    }
    auto* by = &r.draws.empty;
    switch (cls) {
    case DrawClass::Encoded: by = &r.draws.encoded; break;
    case DrawClass::Unmapped: by = &r.draws.unmapped; break;
    case DrawClass::BackendFailed: by = &r.draws.backend_failed; break;
    case DrawClass::Suppressed: by = &r.draws.fault_suppressed; break;
    case DrawClass::Refused: by = &r.draws.refused; break;
    case DrawClass::Empty: break;
    }
    // Allocate the account cell before changing its matching scalars. Reporting
    // failures must not escape into the COM handler and classify this draw twice.
    try { (*by)[method][primitive_name(type)]++; }
    catch (...) { if (r.failure.empty()) r.failure = "draw accounting"; return; }
    switch (cls) {
    case DrawClass::Encoded: r.drawn++; break;
    case DrawClass::Unmapped: r.unmapped_draws++; break;
    case DrawClass::BackendFailed: r.backend_failed_draws++; break;
    case DrawClass::Suppressed: r.suppressed_draws++; break;
    case DrawClass::Refused: case DrawClass::Empty: break;
    }
    if (accepted) r.accepted_draws++;
    r.draws_total++; r.frame_draws++;
}

bool Device::use_fan_cache(uint32_t vertices) {
    if (vertices <= fan_vertices_) return true;
    uint32_t n = 64;
    while (n < vertices && n <= UINT32_MAX / 2) n *= 2;
    const std::vector<uint32_t> idx = fan_cache_indices(n, report().fault.map_bits);
    const uint32_t next = gpu_->create_buffer(uint32_t(idx.size() * 4), true);
    if (!next || !gpu_->upload_buffer(next, 0, reinterpret_cast<const uint8_t*>(idx.data()), uint32_t(idx.size() * 4))) {
        if (next) gpu_->release_buffer(next);
        backend_failure("fan index upload", gpu_->error()); return false;
    }
    if (fan_gpu_) gpu_->release_buffer(fan_gpu_);
    fan_gpu_ = next; fan_vertices_ = n;
    report().fan_cache_uploads++; report().fan_cache_vertices = n;
    return true;
}

// DrawPrimitive and DrawIndexedPrimitive retain DXVK's declaration/empty guards.
HRESULT STDMETHODCALLTYPE Device::DrawPrimitive(D3DPRIMITIVETYPE type, UINT start, UINT count) {
    if (!decl_) { count_draw(kMethodDP, type, DrawClass::Refused); return answer(I_DEVICE, 81, D3DERR_INVALIDCALL); }
    if (!count) { count_draw(kMethodDP, type, DrawClass::Empty); return answer(I_DEVICE, 81, D3D_OK); }
    try {
        const DrawArgs a = draw_args(DM_DP, type, start, count, 0, 0);
        if (a.fan_cache) {
            if (!use_fan_cache(vertex_count(type, count))) throw std::runtime_error(gpu_->error());
            execute_draw(kMethodDP, M_DP, type, streams_[0].stride,
                draw::BufferIndexed{bound_vertices(), {{fan_gpu_}, 0, fan_vertices_ >= 2 ? (fan_vertices_ - 2) * 12 : 0},
                draw::IndexWidth::U32, a.count, 0, 1, a.vertex_offset});
        } else execute_draw(kMethodDP, M_DP, type, streams_[0].stride,
                draw::NonIndexed{bound_vertices(), a.count, a.first_vertex});
    } catch (const std::exception& e) {
        backend_failure("DrawPrimitive", e.what()); count_draw(kMethodDP, type, DrawClass::BackendFailed);
    } catch (...) { backend_failure("DrawPrimitive", "unknown exception"); count_draw(kMethodDP, type, DrawClass::BackendFailed); }
    return answer(I_DEVICE, 81, D3D_OK);
}

HRESULT STDMETHODCALLTYPE Device::DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT base, UINT min_index, UINT vertices,
                                                       UINT start, UINT count) {
    if (!decl_) { count_draw(kMethodDIP, type, DrawClass::Refused); return answer(I_DEVICE, 82, D3DERR_INVALIDCALL); }
    if (!count || !vertices) { count_draw(kMethodDIP, type, DrawClass::Empty); return answer(I_DEVICE, 82, D3D_OK); }
    (void)min_index; // Source preparation uses a stored GPU buffer; base/start remain the GPU offsets.
    try {
        const uint32_t fmt = indices_ ? indices_->format() : D3DFMT_INDEX16;
        const DrawArgs a = draw_args(DM_DIP, type, start, count, base, fmt);
        execute_draw(kMethodDIP, M_DIP, type, streams_[0].stride,
            draw::BufferIndexed{bound_vertices(), {{indices_ ? indices_->gpu_id() : 0}, 0, indices_ ? indices_->length() : 0},
            a.index_size == 2 ? draw::IndexWidth::U16 : draw::IndexWidth::U32, a.count, a.first_index, 1, a.vertex_offset});
    } catch (const std::exception& e) {
        backend_failure("DrawIndexedPrimitive", e.what()); count_draw(kMethodDIP, type, DrawClass::BackendFailed);
    } catch (...) { backend_failure("DrawIndexedPrimitive", "unknown exception"); count_draw(kMethodDIP, type, DrawClass::BackendFailed); }
    return answer(I_DEVICE, 82, D3D_OK);
}

HRESULT STDMETHODCALLTYPE Device::DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride) {
    if (!stride || !decl_) { count_draw(kMethodDPUP, type, DrawClass::Refused); return answer(I_DEVICE, 83, D3DERR_INVALIDCALL); }
    if (!count) { count_draw(kMethodDPUP, type, DrawClass::Empty); return answer(I_DEVICE, 83, D3D_OK); }
    try {
        const uint32_t n = vertex_count(type, count);
        const UpBlock b = up_frame_.add(data, n, stride, decl_->size0());
        report().up_vertex_bytes += b.data_bytes;
        const auto& bytes = up_frame_.bytes();
        draw::OwnedVertices owned{{bytes.begin() + b.vb_offset, bytes.begin() + b.vb_offset + b.buffer_bytes}};
        const DrawArgs a = draw_args(DM_DPUP, type, 0, count, 0, 0);
        if (a.fan_cache) {
            if (!use_fan_cache(n)) throw std::runtime_error(gpu_->error());
            execute_draw(kMethodDPUP, M_DPUP, type, stride,
                draw::BufferIndexed{std::move(owned), {{fan_gpu_}, 0, fan_vertices_ >= 2 ? (fan_vertices_ - 2) * 12 : 0},
                draw::IndexWidth::U32, a.count, 0, 1, 0});
        } else execute_draw(kMethodDPUP, M_DPUP, type, stride, draw::NonIndexed{std::move(owned), a.count, 0});
    } catch (const std::exception& e) {
        backend_failure("DrawPrimitiveUP", e.what()); count_draw(kMethodDPUP, type, DrawClass::BackendFailed);
    } catch (...) { backend_failure("DrawPrimitiveUP", "unknown exception"); count_draw(kMethodDPUP, type, DrawClass::BackendFailed); }
    bind(streams_[0].vb, static_cast<VertexBuffer*>(nullptr));
    streams_[0].offset = streams_[0].stride = 0;
    return answer(I_DEVICE, 83, D3D_OK);
}

HRESULT STDMETHODCALLTYPE Device::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT min_index, UINT vertices,
                                                         UINT count, const void* indices, D3DFORMAT index_fmt,
                                                         const void* data, UINT stride) {
    if (!stride || !decl_) { count_draw(kMethodDIPUP, type, DrawClass::Refused); return answer(I_DEVICE, 84, D3DERR_INVALIDCALL); }
    if (!count || !vertices) { count_draw(kMethodDIPUP, type, DrawClass::Empty); return answer(I_DEVICE, 84, D3D_OK); }
    try {
        const uint32_t isz = index_fmt == D3DFMT_INDEX16 ? 2 : 4;
        const uint32_t ibytes = vertex_count(type, count) * isz;
        const UpBlock b = up_frame_.add(data, min_index + vertices, stride, decl_->size0(), indices, ibytes);
        report().up_vertex_bytes += b.data_bytes; report().up_index_bytes += ibytes;
        const auto& bytes = up_frame_.bytes();
        // the instance count is prepare_draw's (DXVK's GenerateDrawInfo: instanced streams the pipeline reads)
        draw::OwnedIndexedUp owned{{bytes.begin() + b.vb_offset, bytes.begin() + b.ib_offset + ibytes},
            b.buffer_bytes, b.buffer_bytes, ibytes, isz == 2 ? draw::IndexWidth::U16 : draw::IndexWidth::U32,
            vertex_count(type, count), 1};
        execute_draw(kMethodDIPUP, M_DIPUP, type, stride, std::move(owned));
    } catch (const std::exception& e) {
        backend_failure("DrawIndexedPrimitiveUP", e.what()); count_draw(kMethodDIPUP, type, DrawClass::BackendFailed);
    } catch (...) { backend_failure("DrawIndexedPrimitiveUP", "unknown exception"); count_draw(kMethodDIPUP, type, DrawClass::BackendFailed); }
    bind(streams_[0].vb, static_cast<VertexBuffer*>(nullptr));
    streams_[0].offset = streams_[0].stride = 0;
    bind(indices_, static_cast<IndexBuffer*>(nullptr));
    return answer(I_DEVICE, 84, D3D_OK);
}

// ------------------------------------------------------------------ declarations
// CreateVertexDeclaration (:3406-3429): the elements up to Stream 0xFF.
HRESULT STDMETHODCALLTYPE Device::CreateVertexDeclaration(const D3DVERTEXELEMENT9* elements,
                                                          IDirect3DVertexDeclaration9** pp) {
    if (pp) *pp = nullptr;
    if (!pp || !elements) return answer(I_DEVICE, 86, D3DERR_INVALIDCALL);
    std::vector<D3DVERTEXELEMENT9> v;
    for (const D3DVERTEXELEMENT9* e = elements; e->Stream != 0xFF; ++e) v.push_back(*e);
    *pp = new VertexDecl(this, std::move(v), 0);
    report().decls++;
    return answer(I_DEVICE, 86, D3D_OK);
}

HRESULT STDMETHODCALLTYPE Device::SetVertexDeclaration(IDirect3DVertexDeclaration9* decl) {   // :3432-3466
    if (decl_ != static_cast<VertexDecl*>(decl)) pipe_dirty_ = prep_dirty_ = true;
    bind(decl_, static_cast<VertexDecl*>(decl));
    return answer(I_DEVICE, 87, D3D_OK);
}

// SetFVF (:3488-3505): 0 changes nothing; otherwise the FVF's declaration, one per
// value, the device's own.
HRESULT STDMETHODCALLTYPE Device::SetFVF(DWORD fvf) {
    if (!fvf) return answer(I_DEVICE, 89, D3D_OK);
    VertexDecl*& d = fvf_decls_[fvf];
    if (!d) {
        d = new VertexDecl(this, {}, fvf);
        report().fvf_decls++;
    }
    if (decl_ != d) pipe_dirty_ = prep_dirty_ = true;
    bind(decl_, d);
    return answer(I_DEVICE, 89, D3D_OK);
}

// ------------------------------------------------------------------ shaders
namespace {
// A vertex shader's input signature as the keys file writes it: [[usage, usage index], ...].
std::string signature_json(const VsInput* sig, size_t n) {
    std::string s = "[";
    for (size_t i = 0; i < n; i++)
        s += std::string(i ? ", " : "") + "[" + std::to_string(sig[i].usage) + ", " + std::to_string(sig[i].usage_index) + "]";
    return s + "]";
}
}  // namespace

void Device::note_shader_bytecode(bool vertex, uint32_t id, const std::vector<uint32_t>& tokens,
                                  const shader::Reflection& reflection) {
    std::vector<std::string>& v = vertex ? vs_sha256_ : ps_sha256_;
    std::vector<std::string>& rv = vertex ? vs_reflection_ : ps_reflection_;
    if (!id) return;
    if (v.size() < id) v.resize(id);
    if (rv.size() < id) rv.resize(id);
    Sha256 h;
    h.update(tokens.data(), tokens.size() * sizeof(uint32_t));
    v[id - 1] = h.hex();
    // the reflection the pipeline input takes (pipeline_input): a vertex shader's input signature,
    // a pixel shader's sampler and output masks, as JSON
    if (vertex) {
        std::vector<VsInput> sig;
        for (const auto& [usage, index] : reflection.input_signature) sig.push_back(VsInput{uint8_t(usage), uint8_t(index)});
        rv[id - 1] = signature_json(sig.data(), sig.size());
    } else {
        rv[id - 1] = "[" + std::to_string(reflection.sampler_mask) + ", " + std::to_string(reflection.rt_mask) + "]";
    }
}

// The bytecode by its sha256 into SA2_D3D9GPU_SHADER_DUMP's directory (d9g_entry.cpp), once per
// distinct shader, written whole or not at all (a temporary file, then a rename).
void Device::dump_shader_bytecode(const std::vector<uint32_t>& tokens) {
    Report& r = report();
    if (r.shader_tx.dump_dir.empty()) return;
    Sha256 h;
    h.update(tokens.data(), tokens.size() * sizeof(uint32_t));
    const std::string path = r.shader_tx.dump_dir + "/" + h.hex() + ".bin";
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) return;
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    const bool ok = f && std::fwrite(tokens.data(), sizeof(uint32_t), tokens.size(), f) == tokens.size();
    if (f && std::fclose(f) != 0) r.shader_tx.dump_failures++;
#ifdef _WIN32
    else if (!ok || sa2_replace_rename(tmp.c_str(), path.c_str()) != 0) r.shader_tx.dump_failures++;
#else
    else if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) r.shader_tx.dump_failures++;
#endif
    else r.shader_tx.dumped++;
}

// DXVK's compiler on the bytecode (ruling 2; d9g_shader.h translate), with this device's profile and
// the base variant. A refusal is the device's not-implemented case and the call's failure: a shader
// the second host cannot translate must never become a silent draw (the R1 plan's section 3). That
// holds for the refusals the translator reports; bytecode that trips one of dxbc-spirv's assertions
// aborts the process, as it aborts DXVK's.
std::shared_ptr<const shader::Translated> Device::translate_shader(const std::vector<uint32_t>& tokens, std::string* why) {
    Report& r = report();
    dump_shader_bytecode(tokens);
    std::shared_ptr<shader::Translated> t;
    if (!shader_profile_ok_) {
        *why = "no shader profile (" + gpu_->info().shader_facts_query + ")";
    } else if (can_swvp()) {
        // DXVK compiles every shader of a device that can do software vertex processing with
        // D3D9ShaderOptions::isSWVP = CanSWVP() (d3d9_device.cpp:7886) and analyses it with CanSWVP()
        // (:7904): other constant layouts, and other debug names. The translator mirrors a hardware
        // device only (d9g_shader.cpp d3d9_options), so it refuses rather than compile as DXVK would not.
        *why = "unmapped: a device that can do software vertex processing (DXVK's isSWVP, d3d9_device.cpp:7886, :7904) "
               "is not mirrored by the shader translator";
    } else {
        t = std::make_shared<shader::Translated>(shader::translate(tokens.data(), tokens.size() * sizeof(uint32_t),
                                                                   shader_profile_, shader::Variant{}));
        if (!t->ok) *why = "the shader translator refused it: " + t->error;
    }
    if (!t || !t->ok) {
        r.shader_tx.failures++;
        if (r.shader_tx.failure_reasons.size() < 10) r.shader_tx.failure_reasons.push_back(*why);
        return nullptr;
    }
    r.shader_tx.translated++;
    return t;
}

HRESULT STDMETHODCALLTYPE Device::CreateVertexShader(const DWORD* fn, IDirect3DVertexShader9** pp) {   // :3523-3548
    if (!pp) return answer(I_DEVICE, 91, D3DERR_INVALIDCALL);
    bool ni = false;
    std::string why;
    const size_t n = shader_tokens(fn, true, &ni, &why);
    if (!n) {
        if (ni) not_implemented_value("CreateVertexShader: " + why);
        return answer(I_DEVICE, 91, D3DERR_INVALIDCALL);
    }
    std::vector<uint32_t> tokens(fn, fn + n);
    std::shared_ptr<const shader::Translated> t = translate_shader(tokens, &why);
    if (!t) {
        not_implemented_value("CreateVertexShader: " + why);
        return answer(I_DEVICE, 91, D3DERR_INVALIDCALL);
    }
    *pp = new VertexShader(this, std::move(tokens), std::move(t));
    report().vs++;
    report().vs_bytes += n * 4;
    return answer(I_DEVICE, 91, D3D_OK);
}

HRESULT STDMETHODCALLTYPE Device::SetVertexShader(IDirect3DVertexShader9* s) {
    if (vs_ != static_cast<VertexShader*>(s)) pipe_dirty_ = prep_dirty_ = true;
    bind(vs_, static_cast<VertexShader*>(s));
    return answer(I_DEVICE, 92, D3D_OK);
}

HRESULT STDMETHODCALLTYPE Device::CreatePixelShader(const DWORD* fn, IDirect3DPixelShader9** pp) {   // :3882-3906
    if (pp) *pp = nullptr;
    if (!pp) return answer(I_DEVICE, 106, D3DERR_INVALIDCALL);
    bool ni = false;
    std::string why;
    const size_t n = shader_tokens(fn, false, &ni, &why);
    if (!n) {
        if (ni) not_implemented_value("CreatePixelShader: " + why);
        return answer(I_DEVICE, 106, D3DERR_INVALIDCALL);
    }
    std::vector<uint32_t> tokens(fn, fn + n);
    std::shared_ptr<const shader::Translated> t = translate_shader(tokens, &why);
    if (!t) {
        not_implemented_value("CreatePixelShader: " + why);
        return answer(I_DEVICE, 106, D3DERR_INVALIDCALL);
    }
    *pp = new PixelShader(this, std::move(tokens), std::move(t));
    report().ps++;
    report().ps_bytes += n * 4;
    return answer(I_DEVICE, 106, D3D_OK);
}

HRESULT STDMETHODCALLTYPE Device::SetPixelShader(IDirect3DPixelShader9* s) {
    if (ps_ != static_cast<PixelShader*>(s)) pipe_dirty_ = prep_dirty_ = true;
    bind(ps_, static_cast<PixelShader*>(s));
    return answer(I_DEVICE, 107, D3D_OK);
}

// SetShaderConstants (:8049-8100) with DXVK's register counts (d3d9_device.h:1339-1361):
// the software count bounds the call, a vertex shader without software processing
// stores no more than the hardware count, and NULL data is refused after both.
template <int Stage, int Kind, class T>
HRESULT Device::set_constants(UINT start, const T* data, UINT count) {
    const bool vs = Stage == 0;
    const uint32_t soft = Kind == 0 ? (vs ? 8192u : 224u) : (vs ? 2048u : 16u);
    if (start > soft || count > soft - start) return D3DERR_INVALIDCALL;
    if (!count) return D3D_OK;
    if (vs && !can_swvp()) {
        const uint32_t hard = Kind == 0 ? 256u : 16u;
        if (start >= hard) return D3D_OK;
        count = std::min(hard - start, count);
    }
    if (!data) return D3DERR_INVALIDCALL;
    if constexpr (Stage == 0 && Kind == 0)
        changed_float_count_ = draw::float_write_facts(vs_f_[start].data(), data, start, count, changed_float_count_).changed_float_count;
    for (UINT i = 0; i < count; i++) {
        if constexpr (Kind == 0) std::memcpy((vs ? vs_f_ : ps_f_)[start + i].data(), data + 4 * i, 16);
        else if constexpr (Kind == 1) std::memcpy((vs ? vs_i_ : ps_i_)[start + i].data(), data + 4 * i, 16);
        else (vs ? vs_b_ : ps_b_)[start + i] = data[i];
    }
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetVertexShaderConstantF(UINT s, const float* d, UINT n) {
    return answer(I_DEVICE, 94, set_constants<0, 0>(s, d, n));
}
HRESULT STDMETHODCALLTYPE Device::SetVertexShaderConstantI(UINT s, const int* d, UINT n) {
    return answer(I_DEVICE, 96, set_constants<0, 1>(s, d, n));
}
HRESULT STDMETHODCALLTYPE Device::SetVertexShaderConstantB(UINT s, const WINBOOL* d, UINT n) {
    return answer(I_DEVICE, 98, set_constants<0, 2>(s, d, n));
}
HRESULT STDMETHODCALLTYPE Device::SetPixelShaderConstantF(UINT s, const float* d, UINT n) {
    return answer(I_DEVICE, 109, set_constants<1, 0>(s, d, n));
}
HRESULT STDMETHODCALLTYPE Device::SetPixelShaderConstantI(UINT s, const int* d, UINT n) {
    return answer(I_DEVICE, 111, set_constants<1, 1>(s, d, n));
}
HRESULT STDMETHODCALLTYPE Device::SetPixelShaderConstantB(UINT s, const WINBOOL* d, UINT n) {
    return answer(I_DEVICE, 113, set_constants<1, 2>(s, d, n));
}

// ------------------------------------------------------------------ streams
// SetStreamSource (:3695-3756): stream 16 or more refused; NULL unbinds.
HRESULT STDMETHODCALLTYPE Device::SetStreamSource(UINT stream, IDirect3DVertexBuffer9* vb, UINT offset, UINT stride) {
    if (stream >= 16) return answer(I_DEVICE, 100, D3DERR_INVALIDCALL);
    Stream& s = streams_[stream];
    bind(s.vb, static_cast<VertexBuffer*>(vb));
    s.offset = vb ? offset : 0;
    const UINT new_stride = vb ? stride : 0;
    // a draw compares stream 0's stride itself (note_draw_state); another stream's is part of the
    // input where the declaration names the stream (pipeline_input)
    if (stream != 0 && s.stride != new_stride && decl_names_stream(stream)) pipe_dirty_ = prep_dirty_ = true;
    s.stride = new_stride;
    return answer(I_DEVICE, 100, D3D_OK);
}

// SetStreamSourceFreq (:3789-3828).
HRESULT STDMETHODCALLTYPE Device::SetStreamSourceFreq(UINT stream, UINT setting) {
    if (stream >= 16) return answer(I_DEVICE, 102, D3DERR_INVALIDCALL);
    const bool indexed = setting & D3DSTREAMSOURCE_INDEXEDDATA, instanced = setting & D3DSTREAMSOURCE_INSTANCEDATA;
    if ((stream == 0 && instanced) || (instanced && indexed) || setting == 0) return answer(I_DEVICE, 102, D3DERR_INVALIDCALL);
    if (stream == 0 && streams_[0].freq != setting) pipe_dirty_ = prep_dirty_ = true;
    if (stream != 0 && streams_[stream].freq != setting && decl_names_stream(stream)) pipe_dirty_ = prep_dirty_ = true;
    streams_[stream].freq = setting;
    return answer(I_DEVICE, 102, D3D_OK);
}

bool Device::decl_names_stream(uint32_t stream) const {
    if (!decl_) return false;
    for (const D3DVERTEXELEMENT9& e : decl_->elements())
        if (e.Stream == stream) return true;
    return false;
}

HRESULT STDMETHODCALLTYPE Device::SetIndices(IDirect3DIndexBuffer9* ib) {   // :3846-3866
    bind(indices_, static_cast<IndexBuffer*>(ib));
    return answer(I_DEVICE, 104, D3D_OK);
}


// ------------------------------------------------------------------ pipeline state (R2b)
// At every skipped draw the device computes what R2 will draw with: the D3D9 state a pipeline
// is a function of (PipelineInput), its SDL_gpu description and key (d9g_pipeline), the
// shader-side values (LegacyData) and every sampler stage's description (d9g_sampler). Nothing
// is created. The mapping runs when the state changed (the dirty bits the setters above set,
// and the draw's method, type and stride); otherwise the draw only counts. The distinct records
// go to the keys file beside the report (write_keys_file), the counts to the report's
// "pipeline" block. The shaders' parts of the input are their reflection (R1's translator, DXVK's
// analysis): the vertex shader's input signature, which the vertex input follows (U9), and the pixel
// shader's written targets and sampled slots. Every stage's sampler is described (a superset of the
// stages the pixel shader samples: R2-draws binds the ones it reads).
DeviceParams Device::device_params() const { return dp_; }

PipelineInput Device::pipeline_input(uint32_t method, uint32_t prim, uint32_t stride0) const {
    // Every byte set, padding included, so equal inputs are equal bytes (mapped_pipeline).
    PipelineInput in;
    std::memset(static_cast<void*>(&in), 0, sizeof in);
    in.freq0 = 1;
    for (size_t i = 0; i < kNumPipelineRS; i++) in.rs[i] = rs_[kPipelineRenderStates[i]];
    in.prim = prim;
    in.method = uint8_t(method);
    in.stride0 = stride0;
    in.freq0 = streams_[0].freq;
    if (decl_) {
        const auto& el = decl_->elements();
        in.n_elems = uint8_t(std::min<size_t>(el.size(), 16));
        in.decl_overflow = el.size() > 16;
        for (size_t i = 0; i < in.n_elems; i++) {
            in.elems[i] = VertexElem{el[i].Stream, el[i].Offset, el[i].Type, el[i].Method, el[i].Usage, el[i].UsageIndex};
            const uint32_t s = el[i].Stream;
            if (s >= 1 && s < 16) { in.stream_stride[s] = streams_[s].stride; in.stream_freq[s] = streams_[s].freq; }
        }
        in.fvf_decl = decl_->fvf() != 0;
        in.decl_id = decl_->id();
    }
    auto target = [](const Surface* s) {
        TargetDesc t;
        if (!s) return t;
        t.fmt = s->image()->d3dfmt;
        t.w = s->width();
        t.h = s->height();
        t.ms = 0;   // this device makes no multisampled surface
        t.level = s->level();
        t.bound = true;
        t.texture = s->container() != nullptr || s->cube() != nullptr;
        return t;
    };
    for (uint32_t i = 0; i < 4; i++) in.rt[i] = target(rt_[i]);
    in.ds = target(ds_);
    // a render target that is level 0 of a texture bound at some stage (d3d9_device.cpp:6789-6801)
    // (a cube face's: its cube bound at the stage, DXVK's common-texture test)
    for (uint32_t i = 0; i < 4; i++) {
        const Surface* r = rt_[i];
        if (!r || !(r->container() || r->cube()) || r->level() != 0) continue;
        for (uint32_t st = 0; st < 21; st++)
            if ((r->container() && tex_[st] == r->container()) ||
                (r->cube() && tex_other_[st] == static_cast<const ObjectBase*>(r->cube()))) {
                in.rt_tex_sampled |= 1u << i;
                in.rt_tex_stages |= 1u << st;
            }
    }
    if (ds_ && ds_->container())
        for (const Texture* t : tex_)
            if (t && t == ds_->container()) in.ds_tex_sampled = 1;
    // the shaders' reflection (d9g_shader.h Reflection, DXVK's D3D9ShaderAnalysis)
    if (vs_) {
        const auto& sig = vs_->translated().reflection.input_signature;
        in.vs_inputs_overflow = sig.size() > in.vs_inputs.size();
        in.vs_n_inputs = uint8_t(std::min(sig.size(), in.vs_inputs.size()));
        for (size_t i = 0; i < in.vs_n_inputs; i++) in.vs_inputs[i] = VsInput{uint8_t(sig[i].first), uint8_t(sig[i].second)};
    }
    if (ps_) {
        in.ps_rt_mask = ps_->translated().reflection.rt_mask;
        in.ps_sampler_mask = ps_->translated().reflection.sampler_mask;
    }
    in.vs_id = vs_ ? vs_->id() : 0;
    in.ps_id = ps_ ? ps_->id() : 0;
    return in;
}

SamplerInput Device::sampler_input(uint32_t stage) const {
    SamplerInput in;   // every byte set, padding included (mapped_sampler)
    std::memset(static_cast<void*>(&in), 0, sizeof in);
    for (uint32_t i = 0; i < 14; i++) in.ss[i] = ss_[stage][i];
    const Image* img = tex_[stage] ? tex_[stage]->image() : tex_other_[stage] ? tex_other_[stage]->texture_image() : nullptr;
    if (img) {
        in.tex = true;
        in.levels = img->levels;
        in.cube = img->type == TexType::cube;
        in.depth = img->fi->depth;    // DXVK's m_textureSlotTracking.depth
        in.tex_fmt = img->d3dfmt;
    }
    return in;
}

namespace {
std::string bytes_of(const void* p, size_t n) { return std::string(static_cast<const char*>(p), n); }

// An input's census key into b (its previous contents replaced; its capacity kept, so a draw's
// census allocates nothing once b has grown).
void input_bytes(std::string& b, const PipelineInput& in, const uint32_t* extra, size_t n_extra) {
    auto put = [&](const void* p, size_t n) { b.append(static_cast<const char*>(p), n); };
    b.clear();
    put(in.rs.data(), sizeof(uint32_t) * in.rs.size());
    const uint32_t head[] = {in.prim, in.method, in.stride0, in.freq0, in.n_elems, in.decl_overflow, in.fvf_decl,
                             in.rt_tex_sampled, in.rt_tex_stages, in.ds_tex_sampled, in.ps_rt_mask, in.vs_id, in.ps_id,
                             in.decl_id, in.ps_sampler_mask};
    put(head, sizeof head);
    for (uint32_t i = 0; i < in.n_elems; i++) {
        const VertexElem& e = in.elems[i];
        const uint32_t v[] = {e.stream, e.offset, e.type, e.method, e.usage, e.usage_index};
        put(v, sizeof v);
    }
    const uint32_t vsh[] = {in.vs_n_inputs, in.vs_inputs_overflow};
    put(vsh, sizeof vsh);
    put(in.stream_stride.data(), sizeof(uint32_t) * in.stream_stride.size());
    put(in.stream_freq.data(), sizeof(uint32_t) * in.stream_freq.size());
    for (uint32_t i = 0; i < in.vs_n_inputs; i++) {
        const uint32_t v[] = {in.vs_inputs[i].usage, in.vs_inputs[i].usage_index};
        put(v, sizeof v);
    }
    auto tgt = [&](const TargetDesc& t) {
        const uint32_t v[] = {t.fmt, t.w, t.h, t.ms, t.quality, t.level, t.bound, t.texture};
        put(v, sizeof v);
    };
    for (const TargetDesc& t : in.rt) tgt(t);
    tgt(in.ds);
    put(extra, sizeof(uint32_t) * n_extra);
}
uint32_t intern(KeyIds& m, const std::string& k, bool* fresh) {
    auto it = m.find(k);
    if (it != m.end()) { *fresh = false; return it->second; }
    const uint32_t id = uint32_t(m.size());
    m.emplace(k, id);
    *fresh = true;
    return id;
}
}  // namespace

void Device::note_draw_state(uint32_t method, uint32_t prim, uint32_t stride0) {
    Report& r = report();
    Report::Pipeline& P = r.pipeline;
    const uint64_t frame = r.presents + 1, draw = r.frame_draws;
    if (pipe_dirty_ || method != last_method_ || prim != last_prim_ || stride0 != last_stride_ || cur_input_ < 0) {
        P.recomputes++;
        pipe_dirty_ = false;
        last_method_ = method;
        last_prim_ = prim;
        last_stride_ = stride0;
        const PipelineInput in = pipeline_input(method, prim, stride0);
        std::array<uint32_t, std::size(kCensusExtraRenderStates)> extra{};
        for (size_t i = 0; i < extra.size(); i++) extra[i] = rs_[kCensusExtraRenderStates[i]];
        bool fresh = false;
        input_bytes(census_key_, in, extra.data(), extra.size());
        const uint32_t id = intern(input_ids_, census_key_, &fresh);
        if (fresh) {
            InputRec rec;
            rec.in = in;
            rec.extra = extra;
            rec.first_frame = frame;
            rec.first_draw = draw;
            const uint32_t faults = r.fault.map_bits;
            const PipelineDesc d = map_pipeline(in, device_params(), faults);
            const LegacyData l = map_legacy(in, faults);
            rec.unmapped = d.unmapped;
            const PipelineKey k = key_of(d, in, true), sk = key_of(d, in, false);
            bool fk = false, fs = false, fl = false;
            rec.key = intern(key_ids_, bytes_of(k.bytes.data(), k.bytes.size()), &fk);
            rec.state_key = intern(state_key_ids_, bytes_of(sk.bytes.data(), sk.bytes.size()), &fs);
            const std::vector<uint8_t> lb = legacy_bytes(l);
            rec.legacy = intern(legacy_ids_, bytes_of(lb.data(), lb.size()), &fl);
            if (fk) {
                const uint32_t flags = (d.bgra_mask ? 1u : 0u) | ((d.residues & R_DEPTH_CLAMP) ? 2u : 0u) |
                                       (d.rt_feedback ? 4u : 0u) | (d.depth_clamp ? 8u : 0u);
                key_flags_.push_back(flags);
                if (flags & 1u) P.bgra_keys++;
                if (flags & 2u) P.fallback_depth_clamp_keys++;
                if (flags & 4u) P.rt_feedback_keys++;
                if (flags & 8u) P.depth_clamp_property_keys++;
            }
            if (!d.unmapped.empty()) {
                P.unmapped_inputs++;
                for (const std::string& u : d.unmapped)
                    if (P.unmapped_values.size() < 40 &&
                        std::find(P.unmapped_values.begin(), P.unmapped_values.end(), u) == P.unmapped_values.end())
                        P.unmapped_values.push_back(u);
            }
            inputs_.push_back(std::move(rec));
            P.distinct_inputs = inputs_.size();
            P.distinct_keys = key_ids_.size();
            P.distinct_state_keys = state_key_ids_.size();
            P.distinct_legacy = legacy_ids_.size();
        }
        cur_input_ = int32_t(id);
    }
    inputs_[size_t(cur_input_)].draws++;
    P.draws_mapped++;
    if (prim == D3DPT_TRIANGLEFAN) P.fan_as_list_draws++;
    // the sampler stages
    if (sampler_dirty_ || cur_sampler_set_ < 0) {
        P.sampler_recomputes++;
        for (uint32_t st = 0; st < 21; st++) {
            if (!(sampler_dirty_ & (1u << st)) && cur_sampler_set_ >= 0) continue;
            const SamplerInput in = sampler_input(st);
            uint8_t b[kSamplerInputBytes];
            sampler_input_bytes(in, b);
            census_key_.assign(reinterpret_cast<const char*>(b), sizeof b);
            bool fresh = false;
            const uint32_t id = intern(sampler_ids_, census_key_, &fresh);
            if (fresh) {
                SamplerRec rec;
                rec.in = in;
                rec.first_frame = frame;
                rec.first_draw = draw;
                const SamplerDesc d = map_sampler(in, SamplerOptions{}, device_params(), r.fault.map_bits);
                rec.unmapped = d.unmapped;
                rec.fallback_border = d.fallback();
                rec.border_colour = d.border_colour;
                if (rec.fallback_border && in.tex) P.fallback_border_samplers++;
                if (rec.border_colour != kBorderNone && in.tex) P.border_mapped_samplers++;
                if (!d.unmapped.empty()) {
                    P.unmapped_samplers++;
                    for (const std::string& u : d.unmapped)
                        if (P.unmapped_values.size() < 40 &&
                            std::find(P.unmapped_values.begin(), P.unmapped_values.end(), u) == P.unmapped_values.end())
                            P.unmapped_values.push_back(u);
                }
                samplers_.push_back(std::move(rec));
                P.distinct_samplers = samplers_.size();
            }
            stage_sampler_[st] = int32_t(id);
            samplers_[id].stages |= 1u << st;
        }
        sampler_dirty_ = 0;
        bool fresh = false;
        census_key_.assign(reinterpret_cast<const char*>(stage_sampler_.data()), sizeof stage_sampler_);
        const uint32_t sid = intern(sampler_set_ids_, census_key_, &fresh);
        if (fresh) {
            sampler_sets_.push_back(stage_sampler_);
            sampler_set_draws_.push_back(0);
            P.distinct_sampler_sets = sampler_sets_.size();
        }
        cur_sampler_set_ = int32_t(sid);
    }
    sampler_set_draws_[size_t(cur_sampler_set_)]++;
    for (uint32_t st = 0; st < 21; st++) {
        const SamplerRec& s = samplers_[size_t(sampler_sets_[size_t(cur_sampler_set_)][st])];
        if (s.fallback_border && s.in.tex) P.fallback_border_stage_draws++;
    }
}

// The keys file (schema sa2-d3d9gpu-keys/v3: v2 and the shaders' reflection, each input's vertex-shader
// signature and each shader's by creation id; v2: v1 and the SDL patch's capabilities): beside the report
// ("<report minus .json>.keys.json"),
// written with it (atomically, the report's rule-9 refusal), holding every distinct pipeline
// input and sampler stage with its ids and draws: the tuples test/state_check.cpp checks and
// test/scan_projection.py projects onto the stream census.
void Device::write_keys_file() {
    Report& r = report();
    Report::Pipeline& P = r.pipeline;
    if (r.path.empty() || !r.path_refused.empty()) return;
    std::string path = r.path;
    if (path.size() > 5 && path.compare(path.size() - 5, 5, ".json") == 0) path.resize(path.size() - 5);
    path += ".keys.json";
    P.keys_refused = refuse_output(path);
    if (!P.keys_refused.empty()) return;
    // the draw counts per sampler record, from the sets
    for (SamplerRec& s : samplers_) s.draws = 0;
    for (size_t i = 0; i < sampler_sets_.size(); i++)
        for (int32_t id : sampler_sets_[i]) samplers_[size_t(id)].draws += sampler_set_draws_[i];
    std::string o = "{\n  \"schema\": \"sa2-d3d9gpu-keys/v3\",\n  \"tool\": \"host/d3d9gpu libsa2_d3d9gpu.a\",\n";
    auto u = [](uint64_t v) { return std::to_string(v); };
    auto b = [](bool v) { return std::string(v ? "true" : "false"); };
    o += "  \"complete\": " + b(r.complete) + ",\n";
    o += "  \"library_sha256\": \"" + r.library_sha256 + "\",\n";
    o += "  \"map_faults\": " + u(r.fault.map_bits) + ",\n";
    const DeviceParams dp = device_params();
    char buf[256];
    std::snprintf(buf, sizeof buf, "%.9g", double(dp.point_size_max));
    // the device's name, which the state check holds to its parameters' (and a sweep axis's
    // parameters to the CPU model's): the R2b review's checker-logic findings 4 and 7. The report
    // keeps it past the GPU's lifetime (the constructor fills it where Direct3DCreate9 did not).
    std::string name = "\"";
    for (unsigned char c : report().gpu.device_name) {
        if (c == '"' || c == '\\') { name += '\\'; name += char(c); }
        else if (c >= 0x20) name += char(c);
    }
    name += "\"";
    // the SDL patch's capabilities the descriptions were mapped with, and a test run's override,
    // which the state check holds to its parameters' (a keys file made under one is a test leg)
    const std::string& ov = report().gpu.capability_override;
    o += "  \"device\": {\"device_name\": " + name + ", \"d24s8_supported\": " + b(dp.d24s8_supported) +
         ", \"point_size_max\": " + buf + ", \"vk_query_ok\": " + b(vk_query_ok_) + ", \"auto_depth\": " + b(auto_depth_) +
         ", \"sampler_clamp_to_border\": " + b(dp.sampler_clamp_to_border) + ", \"depth_clip_and_clamp\": " +
         b(dp.depth_clip_and_clamp) + ", \"capability_override\": " + (ov.empty() ? std::string("null") : "\"" + ov + "\"") +
         "},\n";
    o += "  \"pipeline_render_states\": [";
    for (size_t i = 0; i < kNumPipelineRS; i++) o += (i ? ", " : "") + u(kPipelineRenderStates[i]);
    o += "],\n  \"census_extra_render_states\": [";
    for (size_t i = 0; i < std::size(kCensusExtraRenderStates); i++) o += (i ? ", " : "") + u(kCensusExtraRenderStates[i]);
    o += "],\n  \"inputs\": [";
    for (size_t n = 0; n < inputs_.size(); n++) {
        const InputRec& rec = inputs_[n];
        const PipelineInput& in = rec.in;
        o += std::string(n ? "," : "") + "\n    {\"id\": " + u(n) + ", \"rs\": [";
        for (size_t i = 0; i < in.rs.size(); i++) o += (i ? ", " : "") + u(in.rs[i]);
        o += "], \"extra\": [";
        for (size_t i = 0; i < rec.extra.size(); i++) o += (i ? ", " : "") + u(rec.extra[i]);
        o += "], \"prim\": " + u(in.prim) + ", \"method\": " + u(in.method) + ", \"stride0\": " + u(in.stride0) +
             ", \"freq0\": " + u(in.freq0);
        // the other streams the declaration names, [stream, stride, frequency]: only where it names one,
        // so an input of stream 0 alone is written as before
        std::string streams;
        for (uint32_t st = 1; st < 16; st++) {
            bool named = false;
            for (uint32_t i = 0; i < in.n_elems; i++) named |= in.elems[i].stream == st;
            if (named) streams += (streams.empty() ? "[" : ", [") + u(st) + ", " + u(in.stream_stride[st]) + ", " + u(in.stream_freq[st]) + "]";
        }
        if (!streams.empty()) o += ", \"streams\": [" + streams + "]";
        o += ", \"elems\": [";
        for (uint32_t i = 0; i < in.n_elems; i++) {
            const VertexElem& e = in.elems[i];
            o += std::string(i ? ", " : "") + "[" + u(e.stream) + ", " + u(e.offset) + ", " + u(e.type) + ", " + u(e.method) +
                 ", " + u(e.usage) + ", " + u(e.usage_index) + "]";
        }
        auto tgt = [&](const TargetDesc& t) {
            return "[" + u(t.fmt) + ", " + u(t.w) + ", " + u(t.h) + ", " + u(t.ms) + ", " + u(t.quality) + ", " + u(t.level) +
                   ", " + b(t.bound) + ", " + b(t.texture) + "]";
        };
        o += "], \"decl_overflow\": " + b(in.decl_overflow) + ", \"fvf\": " + b(in.fvf_decl) + ", \"rt\": [" +
             tgt(in.rt[0]) + ", " + tgt(in.rt[1]) + ", " + tgt(in.rt[2]) + ", " + tgt(in.rt[3]) + "], \"ds\": " + tgt(in.ds) +
             ", \"rt_tex_sampled\": " + u(in.rt_tex_sampled) + ", \"rt_tex_stages\": " + u(in.rt_tex_stages) +
             ", \"ds_tex_sampled\": " + u(in.ds_tex_sampled) +
             ", \"ps_rt_mask\": " + u(in.ps_rt_mask) + ", \"ps_sampler_mask\": " + u(in.ps_sampler_mask) +
             ", \"vs_inputs\": " + signature_json(in.vs_inputs.data(), in.vs_n_inputs) +
             ", \"vs_inputs_overflow\": " + b(in.vs_inputs_overflow) +
             ", \"vs\": " + u(in.vs_id) + ", \"ps\": " + u(in.ps_id) +
             ", \"decl\": " + u(in.decl_id) + ", \"key\": " + u(rec.key) + ", \"state_key\": " + u(rec.state_key) +
             ", \"legacy\": " + u(rec.legacy) + ", \"draws\": " + u(rec.draws) + ", \"first\": [" + u(rec.first_frame) +
             ", " + u(rec.first_draw) + "], \"unmapped\": " + u(rec.unmapped.size()) + "}";
    }
    o += "\n  ],\n  \"samplers\": [";
    for (size_t n = 0; n < samplers_.size(); n++) {
        const SamplerRec& rec = samplers_[n];
        o += std::string(n ? "," : "") + "\n    {\"id\": " + u(n) + ", \"ss\": [";
        for (size_t i = 0; i < 14; i++) o += (i ? ", " : "") + u(rec.in.ss[i]);
        o += "], \"tex\": " + b(rec.in.tex) + ", \"levels\": " + u(rec.in.levels) + ", \"cube\": " + b(rec.in.cube) +
             ", \"depth\": " + b(rec.in.depth) + ", \"fmt\": " + u(rec.in.tex_fmt) +
             ", \"fallback_border\": " + b(rec.fallback_border) + ", \"border_colour\": " + std::to_string(rec.border_colour) +
             ", \"unmapped\": " + u(rec.unmapped.size()) + ", \"stages\": " + u(rec.stages) + ", \"draws\": " + u(rec.draws) +
             ", \"first\": [" + u(rec.first_frame) + ", " + u(rec.first_draw) + "]}";
    }
    // each shader's bytecode by creation id, which the state check links to the recorded
    // reflection (test/shader_record.py): the ids alone name the run's order, not the shader; and
    // each shader's own reflection by creation id, which the state check holds to that record
    auto shas = [](const std::vector<std::string>& v) {
        std::string s = "[";
        for (size_t i = 0; i < v.size(); i++) s += (i ? ", \"" : "\"") + v[i] + "\"";
        return s + "]";
    };
    auto items = [](const std::vector<std::string>& v) {
        std::string s = "[";
        for (size_t i = 0; i < v.size(); i++) s += (i ? ", " : "") + (v[i].empty() ? std::string("null") : v[i]);
        return s + "]";
    };
    o += "\n  ],\n  \"shaders\": {\"vs\": " + shas(vs_sha256_) + ", \"ps\": " + shas(ps_sha256_) +
         ", \"vs_input_signature\": " + items(vs_reflection_) + ", \"ps_masks\": " + items(ps_reflection_) + "},";
    o += "\n  \"counts\": {\"inputs\": " + u(inputs_.size()) + ", \"keys\": " + u(key_ids_.size()) +
         ", \"state_keys\": " + u(state_key_ids_.size()) + ", \"legacy\": " + u(legacy_ids_.size()) +
         ", \"samplers\": " + u(samplers_.size()) + ", \"sampler_sets\": " + u(sampler_sets_.size()) +
         ", \"draws\": " + u(P.draws_mapped) + "}\n}\n";
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) { P.keys_refused = "cannot write " + tmp; return; }
    const bool ok = std::fwrite(o.data(), 1, o.size(), f) == o.size();
#ifdef _WIN32
    if (std::fclose(f) != 0 || !ok || sa2_replace_rename(tmp.c_str(), path.c_str()) != 0) { P.keys_refused = "cannot write " + path; return; }
#else
    if (std::fclose(f) != 0 || !ok || std::rename(tmp.c_str(), path.c_str()) != 0) { P.keys_refused = "cannot write " + path; return; }
#endif
    P.keys_file = path;
    Sha256 h;
    h.update(o.data(), o.size());
    P.keys_sha256 = h.hex();
}

}  // namespace d9g
