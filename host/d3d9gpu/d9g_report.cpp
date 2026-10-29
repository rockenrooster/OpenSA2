// d9g_report.cpp: the device's report (schema sa2-d3d9gpu-report/v1), written to
// $SA2_D3D9GPU_REPORT at device creation, every 100 Presents and, complete, when the
// device is destroyed; the answer and not-implemented counts; the fault knobs.
//
// The report is a plain global: its destructor touches no SDL (the device is linked into
// the player, never unloaded, and the player calls SDL_Quit before the process exits).
#include "d9g.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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

namespace fs = std::filesystem;

namespace d9g {

namespace {
Report g_report;

std::string strf(const char* fmt, ...) {
    va_list ap, aq;
    va_start(ap, fmt);
    va_copy(aq, ap);
    const int n = std::vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    std::string s(n > 0 ? size_t(n) : 0, '\0');
    if (n > 0) std::vsnprintf(s.data(), size_t(n) + 1, fmt, aq);
    va_end(aq);
    return s;
}
std::string jstr(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
        else if (c == '\n') o += "\\n";
        else if (c < 0x20) o += strf("\\u%04x", c);
        else o += char(c);
    }
    return o + "\"";
}
std::string jnull(const std::string& s) { return s.empty() ? "null" : jstr(s); }
std::string kinds_json(const std::map<std::string, std::map<std::string, uint64_t>>& m) {
    std::string s = "{";
    bool first = true;
    for (const auto& [method, by] : m) {
        s += strf("%s%s: {", first ? "" : ", ", jstr(method).c_str());
        bool f2 = true;
        for (const auto& [prim, n] : by) {
            s += strf("%s%s: %llu", f2 ? "" : ", ", jstr(prim).c_str(), (unsigned long long)n);
            f2 = false;
        }
        s += "}";
        first = false;
    }
    return s + "}";
}
std::string counts_json(const std::map<std::string, uint64_t>& m) {
    std::string s = "{";
    bool first = true;
    for (const auto& [k, n] : m) {
        s += strf("%s%s: %llu", first ? "" : ", ", jstr(k).c_str(), (unsigned long long)n);
        first = false;
    }
    return s + "}";
}
std::string env(const char* k) {
    const char* v = getenv(k);
    return v ? v : "";
}
}  // namespace

Report& report() { return g_report; }

uint16_t x87_cw() {
    uint16_t cw = 0;
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("fnstcw %0" : "=m"(cw));
#endif
    return cw;
}
uint32_t mxcsr() {
    uint32_t v = 0;
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("stmxcsr %0" : "=m"(v));
#elif defined(__aarch64__)
    uint64_t f;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(f));
    v = uint32_t(f);
#endif
    return v;
}

HRESULT answer(int iface, unsigned slot, HRESULT hr) {
    if (iface >= 0 && iface < I_COUNT && slot < kMaxSlots) {
        Report::Ans& a = g_report.ans[iface][slot];
        a.calls++;
        if (FAILED(hr)) {
            if (!a.failed) a.first_failed_hr = uint32_t(hr);
            a.failed++;
        }
    }
    return hr;
}

void not_implemented(int iface, unsigned slot) {
    if (iface >= 0 && iface < I_COUNT && slot < kMaxSlots) {
        if (!g_report.ni_methods[iface][slot]++)
            std::fprintf(stderr, "d3d9gpu: not implemented: %s::%s\n", d9g_gen::kInterfaceName[iface],
                         slot < d9g_gen::kSlotCount[iface] ? d9g_gen::kSlotNames[iface][slot] : "?");
        Report::Ans& a = g_report.ans[iface][slot];
        a.calls++;
    }
}

void not_implemented_value(const std::string& what) {
    g_report.ni_value_count++;
    if (g_report.ni_values.size() < 40) {
        g_report.ni_values.push_back(what);
        std::fprintf(stderr, "d3d9gpu: not implemented: %s\n", what.c_str());
    }
}

std::string refuse_output(const std::string& path) {
    std::error_code ec;
    const fs::path abs = fs::weakly_canonical(fs::absolute(fs::path(path), ec), ec);
    if (ec) return "cannot resolve " + path + ": " + ec.message();
    for (fs::path d = abs.parent_path(); !d.empty(); d = d.parent_path()) {
        if (fs::exists(d / ".git", ec)) {
            const fs::path rel = abs.lexically_relative(d / "private");
            if (!rel.empty() && *rel.begin() != "..") return "";
            return path + " resolves to " + abs.string() + ", inside the working tree " + d.string() +
                   " and outside its private/ (AGENTS.md rule 9)";
        }
        if (d == d.root_path()) break;
    }
    return "";
}

FaultSet parse_faults(const char* s) {
    FaultSet f;
    if (!s || !*s) return f;
    f.names = s;
    std::string cur;
    auto take = [&](const std::string& n) {
        if (n == "clear-colour") f.bits |= F_CLEAR_COLOUR;
        else if (n == "count-draw") f.bits |= F_COUNT_DRAW;
        else if (n == "scene-answer") f.bits |= F_SCENE_ANSWER;
        else if (n == "present-scribble") f.bits |= F_PRESENT_SCRIBBLE;
        else if (n == "clear-noop") f.bits |= F_CLEAR_NOOP;
        else if (n == "geometry-noop") f.bits |= F_GEOMETRY_NOOP;
        else if (n == "surface-lost") f.bits |= F_SURFACE_LOST;
        else if (const uint32_t m = map_fault_bit(n)) f.map_bits |= m;   // the state check's mapping faults
        else if (!n.empty()) f.error += (f.error.empty() ? "unknown fault " : ", ") + n;
    };
    for (const char* p = s;; ++p) {
        if (*p == ',' || !*p) { take(cur); cur.clear(); if (!*p) break; }
        else cur += *p;
    }
    return f;
}

CapsOff parse_caps_off(const char* names, const char* test_run) {
    CapsOff c;
    if (!names || !*names) return c;
    std::string cur, unknown;
    auto take = [&](const std::string& n) {
        if (n == "border") c.border = true;
        else if (n == "depth-clamp") c.depth_clamp = true;
        else if (n == "sub-dword") c.sub_dword = true;
        else unknown += (unknown.empty() ? "" : ", ") + (n.empty() ? std::string("(empty)") : n);
    };
    for (const char* p = names;; ++p) {
        if (*p == ',' || !*p) { take(cur); cur.clear(); if (!*p) break; }
        else cur += *p;
    }
    for (const auto& [on, name] : {std::pair<bool, const char*>{c.border, "border"}, {c.depth_clamp, "depth-clamp"},
                                   {c.sub_dword, "sub-dword"}})
        if (on) c.names += (c.names.empty() ? "" : ",") + std::string(name);
    if (!unknown.empty())
        c.error = "SA2_D3D9GPU_CAPS_OFF names " + unknown + ": the capabilities are border, depth-clamp and sub-dword";
    else if (!test_run || std::string(test_run) != "1")
        c.error = "SA2_D3D9GPU_CAPS_OFF=" + std::string(names) + " is a test run's override, refused outside one "
                  "(SA2_D3D9GPU_TEST_RUN=1): the device is not made";
    return c;
}

uint64_t texture_bytes(const TexDesc& d) {
    const uint32_t block = d.fmt == GFmt::bc1 ? 8u : (d.fmt == GFmt::bc2 || d.fmt == GFmt::bc3) ? 16u : 0u;
    const uint32_t texel = d.fmt == GFmt::d32s8 ? 8u : 4u;
    const uint64_t layers = d.type == TexType::cube ? 6u : 1u;
    uint64_t n = 0;
    for (uint32_t l = 0; l < d.levels; l++) {
        const uint64_t w = level_dim(d.w, l), h = level_dim(d.h, l);
        const uint64_t slice = block ? ((w + 3) / 4) * ((h + 3) / 4) * block : w * h * texel;
        n += slice * (d.type == TexType::volume ? level_dim(d.depth, l) : 1u);
    }
    return n * layers;
}

void apply_caps_off(GpuInfo& g, const CapsOff& off) {
    g.sampler_clamp_to_border = g.sampler_clamp_to_border_detected && !off.border;
    g.depth_clip_and_clamp = g.depth_clip_and_clamp_detected && !off.depth_clamp;
    g.capability_override = off.names;
}

namespace {
// A Vulkan version as "major.minor.patch", null where unread.
std::string vk_version(uint32_t v) {
    if (!v) return "null";
    return strf("\"%u.%u.%u\"", (v >> 22) & 0x7fu, (v >> 12) & 0x3ffu, v & 0xfffu);
}
// VkPhysicalDeviceType by name.
const char* vk_device_type(uint32_t t) {
    static const char* names[] = {"other", "integrated GPU", "discrete GPU", "virtual GPU", "CPU"};
    return t < 5 ? names[t] : "unknown";
}
// sdl_gpu's "vk11" member (GpuInfo::vk11), "" on a device made at 1.3, so its report keeps every line it had.
std::string vk11_json(const GpuInfo& gpu) {
    if (!gpu.vk11) return "";
    return strf(", \"vk11\": {\"api_version\": \"1.1\", \"reason\": %s, \"forced\": %s, \"device_type\": %s, "
                "\"device_api_version\": %s, \"instance_api_version\": %s}",
                jstr(gpu.vk11_reason).c_str(), jnull(gpu.vk11_forced).c_str(),
                gpu.vk_device_api ? jstr(vk_device_type(gpu.vk_device_type)).c_str() : "null",
                vk_version(gpu.vk_device_api).c_str(), vk_version(gpu.vk_instance_api).c_str());
}
// sdl_gpu's "vk13_below_device" member (GpuInfo::vk13_below_device), "" unless set, so a device reporting
// 1.3 or later keeps every line it had.
std::string vk13_below_json(const GpuInfo& gpu) {
    if (!gpu.vk13_below_device) return "";
    return strf(", \"vk13_below_device\": {\"api_version\": \"1.3\", \"device_api_version\": %s, "
                "\"instance_api_version\": %s}",
                vk_version(gpu.vk_device_api).c_str(), vk_version(gpu.vk_instance_api).c_str());
}
}  // namespace

std::string Report::json() const {
    std::string s = "{\n  \"schema\": \"sa2-d3d9gpu-report/v1\",\n  \"tool\": \"host/d3d9gpu libsa2_d3d9gpu.a\",\n";
    s += strf("  \"complete\": %s,\n  \"phase\": %s,\n  \"failure\": %s,\n", complete ? "true" : "false",
              jstr(phase).c_str(), jnull(failure).c_str());
    s += strf("  \"library\": {\"path\": %s, \"sha256\": %s},\n", jnull(library_path).c_str(),
              jnull(library_sha256).c_str());
    s += strf("  \"fault\": %s,\n  \"fault_error\": %s,\n", jnull(fault.names).c_str(), jnull(fault.error).c_str());
    s += "  \"env\": {";
    const char* keys[] = {"SA2_D3D9GPU_REPORT", "SA2_D3D9GPU_FAULT", "SA2_D3D9GPU_PRESENT", "SA2_D3D9GPU_DEBUG",
                          "SA2_D3D9GPU_CAPS_OFF", "SA2_D3D9GPU_NO_CLIPDIST", "SA2_D3D9GPU_NO_BC", "SA2_D3D9GPU_NO_DEPTH_CLAMP", "SA2_D3D9GPU_TEST_RUN", "SA2_D3D9GPU_SHADER_DUMP", "SA2_D3D9GPU_SHADER_FAULT",
                          "SDL_VIDEO_DRIVER", "VK_DRIVER_FILES", "VK_ICD_FILENAMES", "VK_LOADER_LAYERS_DISABLE",
                          "VK_LOADER_LAYERS_ENABLE", "VK_INSTANCE_LAYERS", "SDL_ASSERT"};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
        s += strf("%s\"%s\": %s", i ? ", " : "", keys[i], jstr(env(keys[i])).c_str());
    s += "},\n";
    // the shader route's fallback owed: where the device lacks a capability, and where it has it
    // but a description still needs the fallback (BORDER on a view DXVK swizzles, or a custom
    // colour; the pipeline block's counts): the I6 device review's NIT 4
    const bool owe_border = !gpu.sampler_clamp_to_border || pipeline.fallback_border_samplers > 0;
    const bool owe_clamp = !gpu.depth_clip_and_clamp || pipeline.fallback_depth_clamp_keys > 0;
    s += strf("  \"sdl_gpu\": {\"driver\": %s, \"device_name\": %s, \"driver_name\": %s, \"driver_version\": %s, "
              "\"driver_info\": %s, \"depth_format\": %s, \"debug_mode\": %s, \"vulkan_options\": %s, "
              "\"sub_dword_access\": %s, \"clip_distance\": %s, \"clip_distance_forced_off\": %s, \"vulkan_lacking\": %s, "
              "\"bc_unsupported\": %s, \"bc_forced_off\": %s, "
              "\"shader_formats\": \"spirv\", \"note\": \"SDL_GPUVulkanOptions: the Vulkan version and features the "
              "shader route's modules need (R1), the host comparison rerun with them\", \"vk_query_ok\": %s, "
              "\"vk_query\": %s, \"vk_vendor_id\": %u, \"vk_device_id\": %u, \"vk_point_size_max\": %.9g, "
              "\"sdl_revision\": %s, \"sdl_patched\": %s, \"sdl_capabilities_present\": %s, "
              "\"detected\": {\"sampler_clamp_to_border\": %s, \"depth_clip_and_clamp\": %s}, "
              "\"capability_override\": %s, "
              "\"sampler_clamp_to_border\": %s, \"depth_clip_and_clamp\": %s, "
              "\"fallback_owed\": {\"border\": %s, \"depth_clamp\": %s}, "
              "\"sdl_features_dropped\": %s, \"sampler_anisotropy\": %s, \"anisotropy_dropped_samplers\": %llu, "
              "\"depth_clamp_forced_off\": %s, \"live_session\": %s, \"depth_clamp_clip_only\": %s, "
              "\"depth_clamp_clip_only_draws\": %llu%s%s},\n",
              jstr(gpu.driver).c_str(), jstr(gpu.device_name).c_str(), jstr(gpu.driver_name).c_str(),
              jstr(gpu.driver_version).c_str(), jstr(gpu.driver_info).c_str(), jstr(gpu.depth_format).c_str(),
              gpu.debug_mode ? "true" : "false", gpu.vulkan_options.empty() ? "null" : gpu.vulkan_options.c_str(),
              gpu.sub_dword_access ? "true" : "false", gpu.clip_distance ? "true" : "false",
              gpu.clip_distance_forced_off ? "true" : "false", jnull(gpu.vulkan_lacking).c_str(),
              jnull(gpu.bc_unsupported).c_str(), gpu.bc_forced_off ? "true" : "false",
              gpu.vk_query_ok ? "true" : "false", jstr(gpu.vk_query).c_str(),
              gpu.vk_vendor_id, gpu.vk_device_id, double(gpu.vk_point_size_max), jstr(gpu.sdl_revision).c_str(),
              gpu.sdl_patched ? "true" : "false", gpu.sdl_capabilities_present ? "true" : "false",
              gpu.sampler_clamp_to_border_detected ? "true" : "false", gpu.depth_clip_and_clamp_detected ? "true" : "false",
              jnull(gpu.capability_override).c_str(),
              gpu.sampler_clamp_to_border ? "true" : "false", gpu.depth_clip_and_clamp ? "true" : "false",
              owe_border ? "true" : "false", owe_clamp ? "true" : "false", jnull(gpu.sdl_features_dropped).c_str(),
              gpu.sampler_anisotropy ? "true" : "false", (unsigned long long)anisotropy_dropped_samplers,
              gpu.depth_clamp_forced_off ? "true" : "false", gpu.live_session ? "true" : "false",
              gpu.depth_clamp_clip_only ? "true" : "false", (unsigned long long)depth_clamp_clip_only_draws,
              vk11_json(gpu).c_str(), vk13_below_json(gpu).c_str());
    s += strf("  \"fpu\": {\"measured\": %s, \"x87_cw_before\": \"%04x\", \"x87_cw_after\": \"%04x\", "
              "\"mxcsr_before\": \"%08x\", \"mxcsr_after\": \"%08x\", \"claim_x87_cw_before\": \"%04x\", "
              "\"claim_x87_cw_after\": \"%04x\", \"claim_mxcsr_before\": \"%08x\", \"claim_mxcsr_after\": \"%08x\", "
              "\"unchanged\": %s},\n",
              fpu_measured ? "true" : "false", x87_before, x87_after, mxcsr_before, mxcsr_after, claim_x87_before,
              claim_x87_after, claim_mxcsr_before, claim_mxcsr_after,
              (x87_before == x87_after && mxcsr_before == mxcsr_after && claim_x87_before == claim_x87_after &&
               claim_mxcsr_before == claim_mxcsr_after) ? "true" : "false");
    s += strf("  \"present\": {\"mode\": %s, \"interval\": %s, \"swapchain_mode\": %s, \"presents\": %llu, "
              "\"presented\": %llu, \"frames_without_swapchain_texture\": %llu, \"surface_lost\": %llu, \"failures\": %llu, "
              "\"swapchain_size\": [%u, %u], \"scribbles\": %llu, \"basis\": \"frames are read back (GetRenderTargetData) "
              "before Present; SDL_gpu gives no swapchain texture while the window is hidden, occluded or minimised, "
              "which is counted here and changes no frame; surface_lost counts those of them whose window had no "
              "surface (Android destroys it in the background) or whose acquire failed: drawn and submitted, not shown, "
              "answered D3D_OK as DXVK answers them\"},\n",
              jstr(present_mode).c_str(), jstr(present_interval).c_str(), jstr(swapchain_mode).c_str(),
              (unsigned long long)presents, (unsigned long long)presented, (unsigned long long)no_swapchain_texture,
              (unsigned long long)present_surface_lost, (unsigned long long)present_failures, swapchain_w, swapchain_h,
              (unsigned long long)scribbles);
    s += strf("  \"draws\": {\"drawn\": %llu, \"total\": %llu, \"fault_uncounted\": %llu, "
              "\"accepted\": %llu, \"unmapped_count\": %llu, \"backend_failed_count\": %llu, \"suppressed_count\": %llu, "
              "\"encoded\": %s, \"unmapped\": %s, \"backend_failed\": %s, \"fault_suppressed\": %s, "
              "\"refused\": %s, \"empty\": %s, \"failures\": [",
              (unsigned long long)drawn, (unsigned long long)draws_total, (unsigned long long)fault_uncounted,
              (unsigned long long)accepted_draws, (unsigned long long)unmapped_draws,
              (unsigned long long)backend_failed_draws, (unsigned long long)suppressed_draws,
              kinds_json(draws.encoded).c_str(), kinds_json(draws.unmapped).c_str(),
              kinds_json(draws.backend_failed).c_str(), kinds_json(draws.fault_suppressed).c_str(),
              kinds_json(draws.refused).c_str(), kinds_json(draws.empty).c_str());
    for (size_t i = 0; i < draw_failures.size(); i++) s += (i ? ", " : "") + jstr(draw_failures[i]);
    s += "], \"per_frame\": [";
    for (size_t i = 0; i < per_frame.size(); i++)
        s += strf("%s[%llu, %llu, %llu]", i ? ", " : "", (unsigned long long)per_frame[i][0],
                  (unsigned long long)per_frame[i][1], (unsigned long long)per_frame[i][2]);
    s += strf("], \"after_last_present\": %llu},\n", (unsigned long long)frame_draws);
    s += strf("  \"clears\": {\"calls\": %llu, \"zero_count_with_rects\": %llu, \"refused\": %llu, \"empty_region\": %llu, "
              "\"regions\": %llu, \"full_colour\": %llu, \"full_depth_stencil\": %llu, \"partial_colour\": %llu, "
              "\"partial_depth_stencil\": %llu, \"images_skipped\": %llu, \"past_edge\": %llu, \"fault_noop\": %llu, "
              "\"host_failures\": %llu},\n",
              (unsigned long long)clear_calls, (unsigned long long)clear_zero_count_rects,
              (unsigned long long)clear_refused, (unsigned long long)clear_empty, (unsigned long long)clear_regions,
              (unsigned long long)clear_full_colour, (unsigned long long)clear_full_depth,
              (unsigned long long)clear_partial_colour, (unsigned long long)clear_partial_depth,
              (unsigned long long)clear_skipped_images, (unsigned long long)clear_past_edge,
              (unsigned long long)clear_fault_noop, (unsigned long long)clear_host_failures);
    s += strf("  \"uploads\": {\"texture_levels\": %llu, \"texture_bytes\": %llu, \"zero_init_bytes\": %llu, "
              "\"buffer_bytes\": %llu, \"up_vertex_bytes\": %llu, \"up_index_bytes\": %llu, \"up_frame_peak_bytes\": %llu, "
              "\"up_arena_uploads\": %llu, \"fan_cache_vertices\": %llu, \"fan_cache_uploads\": %llu, "
              "\"up_arena_bytes\": %llu, \"up_pad_bytes\": %llu, \"up_draw_uploaded_bytes\": %llu},\n",
              (unsigned long long)texture_level_uploads, (unsigned long long)texture_upload_bytes,
              (unsigned long long)zero_init_bytes, (unsigned long long)buffer_upload_bytes,
              (unsigned long long)up_vertex_bytes, (unsigned long long)up_index_bytes,
              (unsigned long long)up_frame_peak, (unsigned long long)up_arena_uploads,
              (unsigned long long)fan_cache_vertices, (unsigned long long)fan_cache_uploads,
              (unsigned long long)up_arena_bytes, (unsigned long long)up_pad_bytes, (unsigned long long)up_uploaded_bytes);
    // the GPU textures held (in the formats made) and their peak, and the DXT textures decoded on the CPU
    // where the GPU samples no BC format (d9g_device.cpp make_gpu_image)
    s += strf("  \"textures\": {\"gpu_bytes_held\": %llu, \"gpu_bytes_peak\": %llu, \"decoded\": %s, "
              "\"decoded_levels\": %llu, \"decoded_block_bytes\": %llu, \"decode_ms\": %.3f},\n",
              (unsigned long long)gpu_texture_bytes, (unsigned long long)gpu_texture_bytes_peak,
              counts_json(decoded_textures).c_str(), (unsigned long long)decoded_levels,
              (unsigned long long)decoded_block_bytes, double(decode_ns) / 1e6);
    {
        // R2b: the pipeline and sampler descriptions (d9g_device.cpp "pipeline state"); unmapped
        // and undecided values are counted here, never in not_implemented
        const Pipeline& p = pipeline;
        std::string uv = "[";
        for (size_t i = 0; i < p.unmapped_values.size(); i++) uv += (i ? ", " : "") + jstr(p.unmapped_values[i]);
        uv += "]";
        std::string mf;
        for (uint32_t b = 1; b; b <<= 1)
            if ((fault.map_bits & b) && map_fault_name(b)) mf += (mf.empty() ? "" : ",") + std::string(map_fault_name(b));
        s += strf("  \"pipeline\": {\"draws_mapped\": %llu, \"recomputes\": %llu, \"sampler_recomputes\": %llu, "
                  "\"distinct_inputs\": %llu, \"distinct_keys\": %llu, \"distinct_state_keys\": %llu, \"distinct_legacy\": %llu, "
                  "\"distinct_samplers\": %llu, \"distinct_sampler_sets\": %llu, "
                  "\"undecided\": {\"rt_feedback_keys\": %llu}, "
                  "\"patch\": {\"border_samplers\": %llu, \"depth_clamp_keys\": %llu}, "
                  "\"fallback_owed\": {\"border_samplers\": %llu, \"border_stage_draws\": %llu, \"depth_clamp_keys\": %llu}, "
                  "\"unmapped\": {\"inputs\": %llu, \"samplers\": %llu, \"values\": %s}, "
                  "\"rules\": {\"bgra_keys\": %llu, \"fan_as_list_draws\": %llu}, \"device_params\": %s, "
                  "\"keys_file\": %s, \"keys_sha256\": %s, \"keys_refused\": %s, \"map_faults\": %s},\n",
                  (unsigned long long)p.draws_mapped, (unsigned long long)p.recomputes, (unsigned long long)p.sampler_recomputes,
                  (unsigned long long)p.distinct_inputs, (unsigned long long)p.distinct_keys,
                  (unsigned long long)p.distinct_state_keys, (unsigned long long)p.distinct_legacy,
                  (unsigned long long)p.distinct_samplers, (unsigned long long)p.distinct_sampler_sets,
                  (unsigned long long)p.rt_feedback_keys, (unsigned long long)p.border_mapped_samplers,
                  (unsigned long long)p.depth_clamp_property_keys, (unsigned long long)p.fallback_border_samplers,
                  (unsigned long long)p.fallback_border_stage_draws, (unsigned long long)p.fallback_depth_clamp_keys,
                  (unsigned long long)p.unmapped_inputs, (unsigned long long)p.unmapped_samplers, uv.c_str(),
                  (unsigned long long)p.bgra_keys, (unsigned long long)p.fan_as_list_draws,
                  p.device_params ? "true" : "false", jnull(p.keys_file).c_str(), jnull(p.keys_sha256).c_str(),
                  jnull(p.keys_refused).c_str(), jnull(mf).c_str());
    }
    s += strf("  \"readbacks\": %llu,\n", (unsigned long long)readbacks);
    s += "  \"objects\": {\"created\": " + counts_json(created) + ", \"live_at_destroy\": " +
         counts_json(live_at_destroy) + "},\n";
    {
        // R1: every shader translated at creation (d9g_device.cpp translate_shader), with the profile
        // the device's facts give (its record's sha256 and the facts query), and the dump
        const ShaderTranslation& t = shader_tx;
        std::string reasons = "[";
        for (size_t i = 0; i < t.failure_reasons.size(); i++) reasons += (i ? ", " : "") + jstr(t.failure_reasons[i]);
        reasons += "]";
        std::string record = "[";
        if (gpu.shader_facts_ok) {
            const std::string rec = shader::determine_profile(gpu.shader_facts).record();
            size_t at = 0, n = 0;
            for (size_t nl; (nl = rec.find('\n', at)) != std::string::npos; at = nl + 1)
                record += (n++ ? ", " : "") + jstr(rec.substr(at, nl - at));
        }
        record += "]";
        s += strf("  \"shaders\": {\"vs\": %llu, \"ps\": %llu, \"vs_bytes\": %llu, \"ps_bytes\": %llu, \"translated\": %llu, "
                  "\"translate_failures\": %llu, \"failure_reasons\": %s, \"translator\": %s, "
                  "\"profile\": {\"facts_ok\": %s, \"facts_query\": %s, \"record_sha256\": %s, \"record\": %s}, "
                  "\"dump\": {\"dir\": %s, \"refused\": %s, \"dumped\": %llu, \"failures\": %llu}},\n",
                  (unsigned long long)vs, (unsigned long long)ps, (unsigned long long)vs_bytes,
                  (unsigned long long)ps_bytes, (unsigned long long)t.translated, (unsigned long long)t.failures,
                  reasons.c_str(), jstr(shader::translator_identity()).c_str(), gpu.shader_facts_ok ? "true" : "false",
                  jstr(gpu.shader_facts_query).c_str(), jnull(t.profile_sha256).c_str(), record.c_str(),
                  jnull(t.dump_dir).c_str(), jnull(t.dump_refused).c_str(), (unsigned long long)t.dumped,
                  (unsigned long long)t.dump_failures);
    }
    s += strf("  \"declarations\": {\"created\": %llu, \"elements\": %llu, \"elements_without_sdl_format\": %llu, "
              "\"bgra_inputs\": %llu, \"fvf\": %llu},\n",
              (unsigned long long)decls, (unsigned long long)decl_elements,
              (unsigned long long)decl_elements_without_sdl, (unsigned long long)decl_bgra_inputs,
              (unsigned long long)fvf_decls);
    s += strf("  \"gpu\": {\"command_buffers\": %llu, \"submits\": %llu, \"waits\": %llu, \"ring_cycles\": %llu, "
              "\"dedicated_transfers\": %llu, \"render_passes\": %llu, \"copies\": %llu, \"uploads\": %llu, "
              "\"upload_bytes\": %llu, \"downloads\": %llu, \"blits\": %llu, \"clears_folded\": %llu, "
              "\"clears_dropped\": %llu, \"shaders_made\": %llu, \"pipelines_made\": %llu, \"pipeline_make_ms\": %.1f},\n",
              (unsigned long long)gpu_counters.command_buffers, (unsigned long long)gpu_counters.submits,
              (unsigned long long)gpu_counters.waits, (unsigned long long)gpu_counters.ring_cycles,
              (unsigned long long)gpu_counters.dedicated_transfers, (unsigned long long)gpu_counters.render_passes,
              (unsigned long long)gpu_counters.copies, (unsigned long long)gpu_counters.uploads,
              (unsigned long long)gpu_counters.upload_bytes, (unsigned long long)gpu_counters.downloads,
              (unsigned long long)gpu_counters.blits, (unsigned long long)gpu_counters.clears_folded,
              (unsigned long long)gpu_counters.clears_dropped, (unsigned long long)gpu_counters.shaders_made,
              (unsigned long long)gpu_counters.pipelines_made, double(gpu_counters.pipeline_make_ns) / 1e6);
    // Answers: every method called, by interface and name, with its failures.
    s += "  \"methods\": [";
    bool first = true;
    uint64_t ni_total = 0;
    std::string ni_list;
    for (int i = 0; i < I_COUNT; i++)
        for (unsigned k = 0; k < d9g_gen::kSlotCount[i] && k < kMaxSlots; k++) {
            const Ans& a = ans[i][k];
            if (ni_methods[i][k]) {
                ni_total += ni_methods[i][k];
                ni_list += strf("%s\"%s::%s\": %llu", ni_list.empty() ? "" : ", ", d9g_gen::kInterfaceName[i],
                                d9g_gen::kSlotNames[i][k], (unsigned long long)ni_methods[i][k]);
            }
            if (!a.calls) continue;
            s += strf("%s\n    {\"type\": \"%s\", \"method\": \"%s\", \"slot\": %u, \"calls\": %llu, \"failed\": %llu",
                      first ? "" : ",", d9g_gen::kInterfaceName[i], d9g_gen::kSlotNames[i][k], k,
                      (unsigned long long)a.calls, (unsigned long long)a.failed);
            if (a.failed) s += strf(", \"first_failed_hr\": \"0x%08x\"", a.first_failed_hr);
            s += "}";
            first = false;
        }
    s += "\n  ],\n";
    std::string vals = "[";
    for (size_t i = 0; i < ni_values.size(); i++) vals += (i ? ", " : "") + jstr(ni_values[i]);
    vals += "]";
    s += strf("  \"not_implemented\": {\"count\": %llu, \"methods\": {%s}, \"value_count\": %llu, \"values\": %s}\n}\n",
              (unsigned long long)(ni_total + ni_value_count), ni_list.c_str(), (unsigned long long)ni_value_count,
              vals.c_str());
    return s;
}

bool Report::write() const {
    if (path.empty() || !path_refused.empty()) return false;
    const std::string s = json();
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) return false;
    const bool ok = std::fwrite(s.data(), 1, s.size(), f) == s.size();
#ifdef _WIN32
    if (std::fclose(f) != 0 || !ok || sa2_replace_rename(tmp.c_str(), path.c_str()) != 0) return false;
#else
    if (std::fclose(f) != 0 || !ok || std::rename(tmp.c_str(), path.c_str()) != 0) return false;
#endif
    return true;
}

}  // namespace d9g
