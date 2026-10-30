// stream.cpp: see stream.h.
#include "stream.h"

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cstddef>
#include <new>
#include <fcntl.h>
#include <unistd.h>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
// The UCRT's FILE is opaque; ucrtbase exports this accessor to its read-ahead count (the
// Windows form of glibc's _IO_read_end - _IO_read_ptr, read in wait_readable).
extern "C" errno_t __cdecl _get_stream_buffer_pointers(FILE*, char***, char***, int**);
#else
#include <poll.h>
#endif
#include <sys/stat.h>
#include <time.h>

namespace {
struct LiveParseStopped {};
#ifdef _WIN32
// The live reader on Windows. Its two descriptors are the C runtime's over pipe handles
// (_open_osfhandle: the player opens the session's named pipes as their client, --video-pipe and
// --cancel-pipe in player.cpp), read without waiting: PeekNamedPipe says how many bytes a pipe
// holds, ReadFile takes no more than that, and a pipe whose writer is gone (ERROR_BROKEN_PIPE,
// ERROR_PIPE_NOT_CONNECTED) is at its end. Windows has no poll(2) on a pipe, so a wait for the
// next bytes looks at both pipes once a millisecond for at most the slice (the player sets the
// system timer to 1 ms, so a Sleep(1) is one), the wait poll(2) makes on POSIX.
HANDLE pipe_of(int fd) { return fd < 0 ? INVALID_HANDLE_VALUE : reinterpret_cast<HANDLE>(_get_osfhandle(fd)); }
bool pipe_gone(DWORD e) { return e == ERROR_BROKEN_PIPE || e == ERROR_PIPE_NOT_CONNECTED; }
// Bytes the pipe holds now; -1 when its writer is gone and nothing is left; -2 another error.
long long pipe_avail(HANDLE h, DWORD* error) {
    DWORD avail = 0;
    if (PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) return (long long)avail;
    *error = GetLastError();
    return pipe_gone(*error) ? -1 : -2;
}
double win_now_s() {
    static const double tick = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1.0 / double(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) * tick;
}
#endif
// How often service_live looks at the cancellation descriptor: at its first call, every
// kCancelLookEvery'th after, and the first after a wait for bytes (raw_live). The reader services
// several times a record, about 46,000 times a Metal Harbor frame, and each look is a system call:
// under Wine a pipe's peek is a round trip to its server (about 10 us), and looked at every time the
// peeks held a Wine session to 0.23 of real time; on Linux a poll(2) of the descriptor every time
// was about half of a live session's player time (3.5 of 7 ms a frame). A wait for bytes looks at
// it as well (every millisecond on Windows, in the poll(2) itself on POSIX), so a cancel is taken
// within a millisecond while the reader waits, and within 1024 services (tens of microseconds)
// while it reads.
constexpr uint32_t kCancelLookEvery = 1024;
constexpr uint32_t kMaxRecord = 512u << 20;   // a guard, not a format limit
constexpr uint32_t kMaxArgs = 64;
constexpr uint32_t kMaxBlobs = 256;

uint16_t u16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
uint32_t u32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint64_t u64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }

// ------------------------------------------------------------------ JSON
struct JVal {
    enum T { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
    bool b = false;
    double n = 0;
    std::pmr::string s;
    std::pmr::vector<JVal> a;
    std::pmr::vector<std::pair<std::pmr::string, JVal>> o;
    explicit JVal(std::pmr::memory_resource* r = std::pmr::get_default_resource()) : s(r), a(r), o(r) {}
    const JVal* get(const char* k) const {
        for (const auto& kv : o)
            if (kv.first == k) return &kv.second;
        return nullptr;
    }
};

class JParser {
public:
    JParser(const std::string& s, std::string* err, std::pmr::memory_resource* resource,
            void (*check)(void*), void* arg)
        : p_(s.data()), e_(s.data() + s.size()), err_(err), resource_(resource), check_(check), arg_(arg) {}
    void tick() { if (check_ && (++work_ & 1023u) == 0) check_(arg_); }
    const JVal* get(const JVal& v, const char* key) {
        for (const auto& pair : v.o) { tick(); if (pair.first == key) return &pair.second; }
        return nullptr;
    }
    bool parse(JVal* v) {
        if (!value(v, 0)) return false;
        ws();
        if (p_ != e_) return fail("text after the JSON value");
        return true;
    }

private:
    bool fail(const char* why) {
        if (err_->empty()) *err_ = std::string("header JSON: ") + why;
        return false;
    }
    void ws() { while (p_ < e_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) { tick(); p_++; } }
    bool lit(const char* w) {
        const size_t n = std::strlen(w);
        if (size_t(e_ - p_) < n || std::memcmp(p_, w, n) != 0) return false;
        p_ += n;
        return true;
    }
    static void utf8(std::pmr::string* s, uint32_t cp) {
        if (cp < 0x80) { *s += char(cp); }
        else if (cp < 0x800) { *s += char(0xC0 | cp >> 6); *s += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { *s += char(0xE0 | cp >> 12); *s += char(0x80 | ((cp >> 6) & 0x3F)); *s += char(0x80 | (cp & 0x3F)); }
        else { *s += char(0xF0 | cp >> 18); *s += char(0x80 | ((cp >> 12) & 0x3F)); *s += char(0x80 | ((cp >> 6) & 0x3F)); *s += char(0x80 | (cp & 0x3F)); }
    }
    bool hex4(uint32_t* out) {
        if (e_ - p_ < 4) return false;
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) {
            const char c = p_[i];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
            else return false;
        }
        p_ += 4;
        *out = v;
        return true;
    }
    bool str(std::pmr::string* s) {
        if (p_ >= e_ || *p_ != '"') return fail("expected a string");
        p_++;
        while (p_ < e_ && *p_ != '"') {
            tick();
            const unsigned char c = static_cast<unsigned char>(*p_);
            if (c < 0x20) return fail("control character in a string");
            if (c != '\\') { *s += char(c); p_++; continue; }
            if (++p_ >= e_) return fail("string ends inside an escape");
            const char esc = *p_++;
            switch (esc) {
            case '"': *s += '"'; break;
            case '\\': *s += '\\'; break;
            case '/': *s += '/'; break;
            case 'b': *s += '\b'; break;
            case 'f': *s += '\f'; break;
            case 'n': *s += '\n'; break;
            case 'r': *s += '\r'; break;
            case 't': *s += '\t'; break;
            case 'u': {
                uint32_t cp;
                if (!hex4(&cp)) return fail("bad \\u escape");
                if (cp >= 0xD800 && cp < 0xDC00) {
                    uint32_t lo;
                    if (!lit("\\u") || !hex4(&lo) || lo < 0xDC00 || lo >= 0xE000) return fail("unpaired surrogate");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp < 0xE000) {
                    return fail("unpaired surrogate");
                }
                utf8(s, cp);
                break;
            }
            default: return fail("unknown escape");
            }
        }
        if (p_ >= e_) return fail("unterminated string");
        p_++;
        return true;
    }
    bool num(double* out) {
        const char* s = p_;
        if (p_ < e_ && *p_ == '-') p_++;
        if (p_ >= e_ || !(*p_ >= '0' && *p_ <= '9')) return fail("bad number");
        if (*p_ == '0') p_++;
        else while (p_ < e_ && *p_ >= '0' && *p_ <= '9') { tick(); p_++; }
        if (p_ < e_ && *p_ == '.') {
            p_++;
            if (p_ >= e_ || !(*p_ >= '0' && *p_ <= '9')) return fail("bad fraction");
            while (p_ < e_ && *p_ >= '0' && *p_ <= '9') { tick(); p_++; }
        }
        if (p_ < e_ && (*p_ == 'e' || *p_ == 'E')) {
            p_++;
            if (p_ < e_ && (*p_ == '+' || *p_ == '-')) p_++;
            if (p_ >= e_ || !(*p_ >= '0' && *p_ <= '9')) return fail("bad exponent");
            while (p_ < e_ && *p_ >= '0' && *p_ <= '9') { tick(); p_++; }
        }
        *out = std::strtod(std::pmr::string(s, p_, resource_).c_str(), nullptr);
        return true;
    }
    bool value(JVal* v, int depth) {
        tick();
        if (depth > 64) return fail("nested too deeply");
        ws();
        if (p_ >= e_) return fail("unexpected end");
        const char c = *p_;
        if (c == '{') {
            v->t = JVal::OBJ;
            p_++;
            ws();
            if (p_ < e_ && *p_ == '}') { p_++; return true; }
            for (;;) {
                ws();
                std::pmr::string k(resource_);
                if (!str(&k)) return false;
                ws();
                if (p_ >= e_ || *p_ != ':') return fail("expected ':'");
                p_++;
                v->o.emplace_back(std::move(k), JVal(resource_));
                if (!value(&v->o.back().second, depth + 1)) return false;
                ws();
                if (p_ < e_ && *p_ == ',') { p_++; continue; }
                if (p_ < e_ && *p_ == '}') { p_++; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v->t = JVal::ARR;
            p_++;
            ws();
            if (p_ < e_ && *p_ == ']') { p_++; return true; }
            for (;;) {
                v->a.emplace_back(resource_);
                if (!value(&v->a.back(), depth + 1)) return false;
                ws();
                if (p_ < e_ && *p_ == ',') { p_++; continue; }
                if (p_ < e_ && *p_ == ']') { p_++; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') { v->t = JVal::STR; return str(&v->s); }
        if (lit("true")) { v->t = JVal::BOOL; v->b = true; return true; }
        if (lit("false")) { v->t = JVal::BOOL; v->b = false; return true; }
        if (lit("null")) { v->t = JVal::NUL; return true; }
        v->t = JVal::NUM;
        return num(&v->n);
    }

    const char* p_;
    const char* e_;
    std::string* err_;
    std::pmr::memory_resource* resource_;
    void (*check_)(void*);
    void* arg_;
    size_t work_ = 0;
};

bool whole(const JVal& v, uint32_t* out) {
    if (v.t != JVal::NUM || v.n < 0 || v.n > 4294967295.0 || std::floor(v.n) != v.n) return false;
    *out = uint32_t(v.n);
    return true;
}
}  // namespace

static bool parse_header(const std::string& json, Sa2Header* out, std::string* err,
                         std::pmr::memory_resource* resource, void (*check)(void*), void* arg, size_t* strings = nullptr) {
    *out = Sa2Header{out->method_argc.get_allocator().resource()};
    err->clear();
    JVal root(resource);
    JParser jp(json, err, resource, check, arg);
    if (!jp.parse(&root)) return false;
    auto string_bytes = [](const std::string& text) {
        const auto data = reinterpret_cast<uintptr_t>(text.data());
        const auto object = reinterpret_cast<uintptr_t>(&text);
        return data >= object && data < object + sizeof(text) ? size_t(0) : text.capacity() + 1;
    };
    auto copy_string = [&](std::string* dst, const std::pmr::string& src) {
        if (strings && src.size() + 1 > Sa2StreamReader::live_header_strings_cap - *strings)
            throw Sa2ParserStorageLimit{};
        std::string copy(src.data(), src.size());
        const size_t allocated = string_bytes(copy);
        if (strings && allocated > Sa2StreamReader::live_header_strings_cap - *strings)
            throw Sa2ParserStorageLimit{};
        *dst = std::move(copy);
        if (strings) *strings += allocated;
    };
    if (root.t != JVal::OBJ) { *err = "header JSON is not an object"; return false; }
    const JVal* schema = jp.get(root, "schema");
    if (!schema || schema->t != JVal::STR) { *err = "header JSON has no \"schema\" string"; return false; }
    copy_string(&out->schema, schema->s);
    if (const JVal* w = jp.get(root, "writer"); w && w->t == JVal::STR) copy_string(&out->writer, w->s);
    const JVal* methods = jp.get(root, "methods");
    if (!methods || methods->t != JVal::ARR) { *err = "header JSON has no \"methods\" table"; return false; }
    for (size_t i = 0; i < methods->a.size(); i++) {
        jp.tick();
        const JVal& m = methods->a[i];
        uint32_t type, slot, argc;
        if (m.t != JVal::ARR || m.a.size() != 4 || !whole(m.a[0], &type) || !whole(m.a[1], &slot) ||
            m.a[2].t != JVal::STR || !whole(m.a[3], &argc) || type > 0xFFFF || slot > 0xFFFF) {
            *err = "header JSON \"methods\" row " + std::to_string(i) + " is not [type, slot, name, argc]";
            return false;
        }
        const uint32_t key = type << 16 | slot;
        if (out->method_argc.count(key)) {
            *err = "header JSON \"methods\" names (" + std::to_string(type) + ", " + std::to_string(slot) + ") twice";
            return false;
        }
        out->method_argc[key] = argc;
    }
    // The D3DX entries' table: every stream the folded recorder writes has one;
    // a stream without it is read by the player's own copy of host/sa2_stream.h.
    if (const JVal* entries = jp.get(root, "entries")) {
        if (entries->t != JVal::ARR) { *err = "header JSON \"entries\" is not a table"; return false; }
        out->has_entries = true;
        for (size_t i = 0; i < entries->a.size(); i++) {
            jp.tick();
            const JVal& e = entries->a[i];
            uint32_t entry, argc;
            if (e.t != JVal::ARR || e.a.size() != 4 || !whole(e.a[0], &entry) || e.a[1].t != JVal::STR ||
                !whole(e.a[2], &argc) || e.a[3].t != JVal::STR) {
                *err = "header JSON \"entries\" row " + std::to_string(i) + " is not [entry, name, argc, ops]";
                return false;
            }
            if (out->entries.count(entry)) {
                *err = "header JSON \"entries\" names entry " + std::to_string(entry) + " twice";
                return false;
            }
            Sa2EntryRow row;
            // Charge only retained strings. If map allocation fails, the row's
            // temporary strings are destroyed before the boundary catches it.
            const size_t old_strings = strings ? *strings : 0;
            copy_string(&row.name, e.a[1].s);
            copy_string(&row.ops, e.a[3].s);
            row.argc = argc;
            try { out->entries.emplace(entry, std::move(row)); }
            catch (...) { if (strings) *strings = old_strings; throw; }
        }
    }
    // A version 2 stream's audio op table (host/sa2_stream.h, the `audio` lines).
    if (const JVal* audio = jp.get(root, "audio")) {
        const JVal* ops = audio->t == JVal::OBJ ? jp.get(*audio, "ops") : nullptr;
        if (!ops || ops->t != JVal::ARR) { *err = "header JSON \"audio\" has no \"ops\" table"; return false; }
        // The two STATE structs' sizes, which every STATE blob is read at.
        const JVal* dsb = jp.get(*audio, "ds_state_bytes");
        const JVal* xab = jp.get(*audio, "xa_state_bytes");
        uint32_t ds_bytes = 0, xa_bytes = 0;
        if (!dsb || !whole(*dsb, &ds_bytes) || ds_bytes != sizeof(sa2s_ds_state) || !xab ||
            !whole(*xab, &xa_bytes) || xa_bytes != sizeof(sa2s_xa_state)) {
            *err = "header JSON \"audio\" does not give ds_state_bytes " + std::to_string(sizeof(sa2s_ds_state)) +
                   " and xa_state_bytes " + std::to_string(sizeof(sa2s_xa_state)) +
                   ", the sizes of host/sa2_stream.h's STATE structs";
            return false;
        }
        out->has_audio = true;
        for (size_t i = 0; i < ops->a.size(); i++) {
            jp.tick();
            const JVal& e = ops->a[i];
            uint32_t op, argc;
            if (e.t != JVal::ARR || e.a.size() != 6 || !whole(e.a[0], &op) || e.a[1].t != JVal::STR ||
                e.a[2].t != JVal::STR || e.a[3].t != JVal::STR || !whole(e.a[4], &argc) || e.a[5].t != JVal::STR) {
                *err = "header JSON \"audio\" row " + std::to_string(i) +
                       " is not [op, NAME, interface, method, argc, aops]";
                return false;
            }
            if (out->audio_ops.count(op)) {
                *err = "header JSON \"audio\" names op " + std::to_string(op) + " twice";
                return false;
            }
            Sa2AudioRow row;
            const size_t old_strings = strings ? *strings : 0;
            copy_string(&row.name, e.a[1].s);
            copy_string(&row.iface, e.a[2].s);
            copy_string(&row.method, e.a[3].s);
            copy_string(&row.aops, e.a[5].s);
            row.argc = argc;
            try { out->audio_ops.emplace(op, std::move(row)); }
            catch (...) { if (strings) *strings = old_strings; throw; }
        }
    }
    return true;
}

bool sa2_parse_header_json(const std::string& json, Sa2Header* out, std::string* err) {
    return parse_header(json, out, err, std::pmr::get_default_resource(), nullptr, nullptr);
}

Sa2StreamReader::~Sa2StreamReader() {
    if (zstd_) ZSTD_freeDStream(static_cast<ZSTD_DStream*>(zstd_));
    if (f_ && f_ != stdin) std::fclose(f_);
    if (video_fd_ >= 0) ::close(video_fd_);
    if (cancel_fd_ >= 0 && cancel_fd_ != video_fd_) ::close(cancel_fd_);
}

int Sa2StreamReader::open(const std::string& path, std::string* err) {
    if (live_fd_mode_) { *err = "open requires a finite-file reader"; return OPEN_IO; }
    f_ = path == "-" ? stdin : std::fopen(path.c_str(), "rb");
    if (!f_) { *err = "cannot open " + path; return OPEN_IO; }
    struct stat st;
#ifdef _WIN32
    live_ = fstat(fileno(f_), &st) == 0 && S_ISFIFO(st.st_mode);   // a pipe; Windows has no socket file
#else
    live_ = fstat(fileno(f_), &st) == 0 && (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode));
#endif
    static char iobuf[1 << 22];
    std::setvbuf(f_, iobuf, _IOFBF, sizeof iobuf);
    uint8_t magic4[4];
    const int c0 = std::fgetc(f_);
    if (c0 == EOF) { *err = path + " is empty"; return OPEN_MALFORMED; }
    std::ungetc(c0, f_);
    if (c0 == 0x28) {                  // possibly zstd: 28 B5 2F FD
        size_t got = std::fread(magic4, 1, 4, f_);
        file_read_ += got;
        sha_.update(magic4, got);
        if (got == 4 && magic4[0] == 0x28 && magic4[1] == 0xB5 && magic4[2] == 0x2F && magic4[3] == 0xFD) {
            zstd_ = ZSTD_createDStream();
            ZSTD_initDStream(static_cast<ZSTD_DStream*>(zstd_));
            zin_.resize(ZSTD_DStreamInSize() > (1u << 20) ? ZSTD_DStreamInSize() : (1u << 20));
            zout_.resize(ZSTD_DStreamOutSize() > (1u << 20) ? ZSTD_DStreamOutSize() : (1u << 20));
            std::memcpy(zin_.data(), magic4, 4);
            zin_len_ = 4;
        } else {
            *err = path + " starts with 0x28 but is neither a stream nor zstd";
            return OPEN_MALFORMED;
        }
    }
    uint8_t head[16];
    bool eof = false;
    if (!fill(head, 16, &eof)) { *err = path + ": short file header"; return OPEN_MALFORMED; }
    if (std::memcmp(head, SA2S_MAGIC, 8) != 0) { *err = path + ": magic is not " SA2S_MAGIC; return OPEN_MALFORMED; }
    version_ = u32(head + 8);
    const uint32_t header_bytes = u32(head + 12);
    if (header_bytes < 16 || header_bytes > (64u << 20)) { *err = path + ": header_bytes out of range"; return OPEN_MALFORMED; }
    // header_bytes counts the 16 bytes read; the JSON follows, padded to 4.
    const uint32_t json_len = header_bytes - 16;
    const uint32_t padded = (header_bytes + 3u) & ~3u;
    std::vector<uint8_t> rest(padded - 16);
    if (!rest.empty() && !fill(rest.data(), rest.size(), &eof)) { *err = path + ": short header JSON"; return OPEN_MALFORMED; }
    json_.assign(reinterpret_cast<const char*>(rest.data()), json_len);
    while (!json_.empty() && (json_.back() == '\0' || json_.back() == ' ' || json_.back() == '\n'))
        json_.pop_back();                  // the writer pads the JSON with spaces to header_bytes
    offset_ = padded;
    // A writer never changes a record layout without changing the version
    // (host/sa2_stream.h): this reader vouches for version 1 and for version 2,
    // which is version 1 and the AUDIO kind, and for no other.
    if (version_ != SA2S_VERSION && version_ != SA2S_VERSION_AUDIO) {
        *err = path + ": stream version " + std::to_string(version_) + "; this reader reads versions " +
               std::to_string(SA2S_VERSION) + " and " + std::to_string(SA2S_VERSION_AUDIO) + " only";
        return OPEN_MALFORMED;
    }
    std::string jerr;
    if (!sa2_parse_header_json(json_, &header_, &jerr)) { *err = path + ": " + jerr; return OPEN_MALFORMED; }
    const char* want = version_ == SA2S_VERSION ? SA2S_SCHEMA : SA2S_SCHEMA_AUDIO;
    if (header_.schema != want) {
        *err = path + ": stream version " + std::to_string(version_) + " with schema \"" + header_.schema +
               "\"; that version's schema is \"" + want + "\"";
        return OPEN_MALFORMED;
    }
    if (header_.has_audio != (version_ == SA2S_VERSION_AUDIO)) {
        *err = path + ": a version " + std::to_string(version_) + " header " +
               (header_.has_audio ? "with" : "without") + " an \"audio\" table";
        return OPEN_MALFORMED;
    }
    index_header();
    return OPEN_OK;
}

void Sa2StreamReader::index_header() {
    argc_flat_.assign(size_t(kArgcTypes) * kArgcSlots, -1);
    for (const auto& [key, argc] : header_.method_argc) {
        const uint32_t type = key >> 16, slot = key & 0xFFFFu;
        if (type < kArgcTypes && slot < kArgcSlots) argc_flat_[size_t(type) * kArgcSlots + slot] = int32_t(argc);
    }
}

std::string Sa2StreamReader::hash_rest() {
    if (live_fd_mode_ || !f_) return std::string();
    std::vector<uint8_t> chunk(1u << 22);
    size_t got;
    while ((got = std::fread(chunk.data(), 1, chunk.size(), f_)) > 0) {
        file_read_ += got;
        sha_.update(chunk.data(), got);
    }
    if (std::ferror(f_) || !std::feof(f_)) return std::string();
    eof_ = true;
    return sha_.hex();
}

// Read exactly n bytes; *eof when the stream ended cleanly before the first byte.
bool Sa2StreamReader::fill(void* dst, size_t n, bool* eof) {
    uint8_t* out = static_cast<uint8_t*>(dst);
    size_t done = 0;
    *eof = false;
    if (!zstd_) {
        wait_readable();
        done = std::fread(out, 1, n, f_);
        file_read_ += done;
        sha_.update(out, done);
        if (done < n && std::feof(f_)) eof_ = true;
        if (done == 0) *eof = true;
        return done == n;
    }
    auto* ds = static_cast<ZSTD_DStream*>(zstd_);
    while (done < n) {
        if (zout_pos_ < zout_len_) {
            const size_t take = std::min(n - done, zout_len_ - zout_pos_);
            std::memcpy(out + done, zout_.data() + zout_pos_, take);
            zout_pos_ += take;
            done += take;
            continue;
        }
        if (zin_pos_ == zin_len_) {
            if (zin_eof_) { *eof = done == 0; return false; }
            wait_readable();
            zin_len_ = std::fread(zin_.data(), 1, zin_.size(), f_);
            file_read_ += zin_len_;
            sha_.update(zin_.data(), zin_len_);
            zin_pos_ = 0;
            if (zin_len_ == 0) { zin_eof_ = true; eof_ = std::feof(f_) != 0; continue; }
        }
        ZSTD_inBuffer in{zin_.data(), zin_len_, zin_pos_};
        ZSTD_outBuffer o{zout_.data(), zout_.size(), 0};
        const size_t rc = ZSTD_decompressStream(ds, &o, &in);
        if (ZSTD_isError(rc)) return false;
        zin_pos_ = in.pos;
        zout_pos_ = 0;
        zout_len_ = o.pos;
    }
    return true;
}

int Sa2StreamReader::next(Sa2Record* r, std::string* err) {
    if (live_fd_mode_) { *err = "next requires the finite-file path"; return -1; }
    uint8_t head[12];
    bool eof = false;
    r->arg_missing = false;
    if (!fill(head, 12, &eof)) {
        if (eof) return 0;
        *err = "stream ends inside a record header at offset " + std::to_string(offset_);
        return -1;
    }
    r->kind = u32(head);
    r->bytes = u32(head + 4);
    r->seq = u32(head + 8);
    r->offset = offset_;
    if (r->bytes < 12 || (r->bytes & 3u) || r->bytes > kMaxRecord) {
        *err = "record at offset " + std::to_string(offset_) + " has size " + std::to_string(r->bytes);
        return -1;
    }
    buf_.resize(r->bytes - 12);
    if (!buf_.empty() && !fill(buf_.data(), buf_.size(), &eof)) {
        *err = "stream ends inside record seq " + std::to_string(r->seq);
        return -1;
    }
    offset_ += r->bytes;
    r->args.clear();
    r->blobs.clear();
    r->note.clear();
    return parse_record(r, err) ? 1 : -1;
}

bool Sa2StreamReader::parse_record(Sa2Record* r, std::string* err) {
    const uint8_t* p = record_data();
    const size_t n = record_size();
    switch (r->kind) {
    case SA2S_CALL:
    case SA2S_D3DX:
        return parse_call_or_d3dx(r, err);
    case SA2S_FRAME:
        if (n != 16) { *err = "FRAME record seq " + std::to_string(r->seq) + " is not 16 bytes of payload"; return false; }
        r->virtual_us = u64(p);
        r->frames = u32(p + 8);
        r->polls = u32(p + 12);
        return true;
    case SA2S_NOTE: {
        if (n < 4) { *err = "NOTE record too short"; return false; }
        const uint32_t len = u32(p);
        if (len > n - 4) { *err = "NOTE length past its record"; return false; }
        if (!live_fd_mode_) {
            r->note.assign(reinterpret_cast<const char*>(p + 4), len);
        } else {
            // Reserve once so append cannot hide an unbounded reallocation copy.
            // Allocation itself is an explicit owner gap; payload touches are batched.
            r->note.reserve(len);
            parser_service(this);
            for (size_t copied = 0; copied < len;) {
                const size_t take = std::min(size_t(len) - copied, live_batch_bytes);
                r->note.append(reinterpret_cast<const char*>(p + 4 + copied), take);
                copied += take;
                parser_service(this);
            }
        }
        return true;
    }
    case SA2S_END:
        if (n) { *err = "END record seq " + std::to_string(r->seq) + " has a payload"; return false; }
        return true;
    case SA2S_AUDIO:
        // A version 1 stream has no AUDIO kind: the caller counts the record as one
        // its version does not have, as any other unknown kind.
        if (version_ != SA2S_VERSION_AUDIO) return true;
        return parse_audio(r, err);
    default:
        return true;                       // a kind this reader does not know: the caller counts it
    }
}

bool Sa2StreamReader::parse_call_or_d3dx(Sa2Record* r, std::string* err) {
    const uint8_t* p = record_data();
    const uint8_t* end = p + record_size();
    auto need = [&](size_t k) { return size_t(end - p) >= k; };
    if (r->kind == SA2S_CALL) {
        if (!need(16)) { *err = "CALL record too short"; return false; }
        r->type = u16(p);
        r->slot = u16(p + 2);
        r->self = u32(p + 4);
        r->thread = u32(p + 8);
        p += 12;
    } else {
        if (!need(8)) { *err = "D3DX record too short"; return false; }
        r->entry = u32(p);
        p += 4;
    }
    const uint32_t argc = u32(p);
    p += 4;
    if (argc > kMaxArgs || !need(size_t(argc) * 4 + 8)) {
        *err = "record seq " + std::to_string(r->seq) + " has argc " + std::to_string(argc);
        return false;
    }
    r->args.resize(argc);
    if (argc) std::memcpy(r->args.data(), p, size_t(argc) * 4);
    p += size_t(argc) * 4;
    r->eax = u32(p);
    p += 4;
    return parse_blobs(p, end, r, err);
}

// SA2S_AUDIO: u16 api, u16 op, u32 this, u32 thread, u64 virtual_us, u32 argc,
// args, u32 eax, blobs (host/sa2_stream.h).
bool Sa2StreamReader::parse_audio(Sa2Record* r, std::string* err) {
    const uint8_t* p = record_data();
    const uint8_t* end = p + record_size();
    if (size_t(end - p) < 24) { *err = "AUDIO record seq " + std::to_string(r->seq) + " too short"; return false; }
    r->api = u16(p);
    r->op = u16(p + 2);
    r->self = u32(p + 4);
    r->thread = u32(p + 8);
    r->virtual_us = u64(p + 12);
    const uint32_t argc = u32(p + 20);
    p += 24;
    if (argc > kMaxArgs || size_t(end - p) < size_t(argc) * 4 + 8) {
        *err = "AUDIO record seq " + std::to_string(r->seq) + " has argc " + std::to_string(argc);
        return false;
    }
    r->args.resize(argc);
    if (argc) std::memcpy(r->args.data(), p, size_t(argc) * 4);
    p += size_t(argc) * 4;
    r->eax = u32(p);
    p += 4;
    return parse_blobs(p, end, r, err);
}

bool Sa2StreamReader::parse_blobs(const uint8_t* p, const uint8_t* end, Sa2Record* r, std::string* err) {
    if (end - p < 4) { *err = "record seq " + std::to_string(r->seq) + " has no blob count"; return false; }
    const uint32_t nblobs = u32(p);
    p += 4;
    if (nblobs > kMaxBlobs) { *err = "record seq " + std::to_string(r->seq) + " claims " + std::to_string(nblobs) + " blobs"; return false; }
    r->blobs.resize(nblobs);
    for (uint32_t i = 0; i < nblobs; i++) {
        if (end - p < 8) { *err = "blob header past record seq " + std::to_string(r->seq); return false; }
        Sa2Blob& b = r->blobs[i];
        b.role = u16(p);
        b.arg = u16(p + 2);
        b.len = u32(p + 4);
        p += 8;
        if (size_t(end - p) < b.len) { *err = "blob data past record seq " + std::to_string(r->seq); return false; }
        b.data = p;
        p += (size_t(b.len) + 3u) & ~size_t(3);
        if (p > end) p = end;          // the last blob's padding may be absent
    }
    if (p != end) {
        *err = "record seq " + std::to_string(r->seq) + " has " + std::to_string(end - p) + " bytes after its last blob";
        return false;
    }
    return true;
}

// Before a read that would block on a live stream, wait in slices and call the idle hook
// between them. glibc's FILE keeps what it has read ahead between _IO_read_ptr and
// _IO_read_end, and bytes there need no wait; the player links glibc only (DXVK Native is a
// glibc library), so this is the one C library it meets. poll() returning for POLLHUP
// (the writer is gone) lets the read go on to its end of stream.
#ifdef _WIN32
// Windows: the FILE's read-ahead count from the UCRT, and poll()'s wait on the pipe as
// PeekNamedPipe once a millisecond for the slice; a pipe whose writer is gone (PeekNamedPipe
// fails: ERROR_BROKEN_PIPE) lets the read go on to its end of stream, as POLLHUP does.
void Sa2StreamReader::wait_readable() {
    int* buffered = nullptr;
    if (!live_ || !idle_hook_ || (_get_stream_buffer_pointers(f_, nullptr, nullptr, &buffered) == 0 && buffered &&
                                  *buffered > 0)) return;
    const HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fileno(f_)));
    timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    bool waited = false;
    for (;;) {
        bool ready = false;
        for (int ms = 0; ms < idle_ms_ && !ready; ms++) {
            DWORD avail = 0;
            if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) || avail > 0) ready = true;
            else Sleep(1);
        }
        if (ready) break;
        waited = true;
        idle_slices_++;
        idle_hook_(idle_arg_);
    }
    if (waited) {
        clock_gettime(CLOCK_MONOTONIC, &b);
        idle_waits_++;
        idle_s_ += double(b.tv_sec - a.tv_sec) + double(b.tv_nsec - a.tv_nsec) * 1e-9;
    }
}
#else
void Sa2StreamReader::wait_readable() {
#if defined(__BIONIC__)
    // android_app.py arm_portable: bionic's FILE is opaque (no read-ahead count), so no idle wait.
    if (true) return;
#else
    if (!live_ || !idle_hook_ || f_->_IO_read_ptr < f_->_IO_read_end) return;
#endif
    pollfd p{fileno(f_), POLLIN, 0};
    timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    bool waited = false;
    for (;;) {
        const int r = poll(&p, 1, idle_ms_);
        if (r > 0 || (r < 0 && errno != EINTR)) break;
        if (r == 0) {
            waited = true;
            idle_slices_++;
            idle_hook_(idle_arg_);
        }
    }
    if (waited) {
        clock_gettime(CLOCK_MONOTONIC, &b);
        idle_waits_++;
        idle_s_ += double(b.tv_sec - a.tv_sec) + double(b.tv_nsec - a.tv_nsec) * 1e-9;
    }
}
#endif

namespace {
struct alignas(std::max_align_t) CodecAllocation { size_t bytes; };
}

void* Sa2StreamReader::codec_alloc(void* opaque, size_t size) {
    auto* r = static_cast<Sa2StreamReader*>(opaque);
    if (size > live_codec_cap - sizeof(CodecAllocation) ||
        size + sizeof(CodecAllocation) > live_codec_cap - r->codec_bytes_) {
        r->codec_denied_ = true;
        return nullptr;
    }
    const size_t bytes = size + sizeof(CodecAllocation);
    auto* p = static_cast<CodecAllocation*>(std::malloc(bytes));
    if (!p) { r->codec_failed_ = true; return nullptr; }
    p->bytes = bytes;
    r->codec_bytes_ += bytes;
    r->codec_peak_ = std::max(r->codec_peak_, r->codec_bytes_);
    return p + 1;
}

void Sa2StreamReader::codec_free(void* opaque, void* address) {
    if (!address) return;
    auto* r = static_cast<Sa2StreamReader*>(opaque);
    auto* p = static_cast<CodecAllocation*>(address) - 1;
    r->codec_bytes_ -= p->bytes;
    std::free(p);
}

Sa2ReaderStorage Sa2StreamReader::live_storage() const {
    size_t metadata = 0;
    if (live_pending_) {
        const auto& pending = *live_pending_;
        const auto data = reinterpret_cast<uintptr_t>(pending.note.data());
        const auto object = reinterpret_cast<uintptr_t>(&pending.note);
        const size_t note_heap = data >= object && data < object + sizeof(pending.note) ?
            size_t(0) : pending.note.capacity() + 1;
        metadata = pending.args.capacity() * sizeof(uint32_t) +
            pending.blobs.capacity() * sizeof(Sa2Blob) + note_heap;
    }
    return {raw_.capacity(), zout_.capacity(), buf_.capacity() + live_body_capacity_, json_.capacity(),
            codec_bytes_, codec_peak_, metadata, parser_storage_.bytes, parser_storage_.peak,
            header_strings_bytes_, live_pending_ ? live_pending_->note.size() : 0};
}

const char* sa2_cancel_waiting(int fd) {
    if (fd < 0) return nullptr;
#ifdef _WIN32
    DWORD error = 0;
    const long long held = pipe_avail(pipe_of(fd), &error);
    if (held > 0) return "the caller cancelled";
    if (held == -1) return "the caller's cancellation pipe is closed";
    if (held == -2) return "the caller's cancellation pipe failed";
    return nullptr;
#else
    pollfd p{fd, POLLIN, 0};
    if (::poll(&p, 1, 0) <= 0) return nullptr;
    if (p.revents & POLLIN) return "the caller cancelled";
    if (p.revents & POLLHUP) return "the caller's cancellation pipe is closed";
    if (p.revents & (POLLERR | POLLNVAL)) return "the caller's cancellation pipe failed";
    return nullptr;
#endif
}

Sa2ReadOutcome Sa2StreamReader::finish_live(Sa2ReadStatus status, const std::string& detail) {
    live_status_ = status;
    live_detail_ = detail;
    return {status, detail};
}

Sa2ReadStatus Sa2StreamReader::service_live() {
    if (live_status_ != Sa2ReadStatus::Ready) return live_status_;
#ifdef _WIN32
    // The cancellation pipe as POSIX reads it: byte 1 is the caller's cancel; its writer gone, or
    // any other byte, is a failure.
    DWORD error = 0;
    const HANDLE wake = pipe_of(cancel_fd_);
    const long long held = cancel_services_++ % kCancelLookEvery == 0 ? pipe_avail(wake, &error) : 0;
    if (held == -2)
        return finish_live(Sa2ReadStatus::IoFailure, "cancellation peek: Windows error " + std::to_string(error)).status;
    if (held != 0) {
        uint8_t value = 0;
        DWORD n = 0;
        if (held > 0 && ReadFile(wake, &value, 1, &n, nullptr) && n == 1 && value == 1)
            return finish_live(Sa2ReadStatus::Cancelled, "explicit cancellation wake").status;
        return finish_live(Sa2ReadStatus::IoFailure, "cancellation channel closed, unreadable or invalid").status;
    }
    ++live_services_;
    if (service_ && service_(service_arg_) == Sa2ServiceDecision::Cancel)
        return finish_live(Sa2ReadStatus::Cancelled, "cancelled by service").status;
    return Sa2ReadStatus::Ready;
#else
    if (cancel_services_++ % kCancelLookEvery == 0) {
        pollfd wake{cancel_fd_, POLLIN, 0};
        const int rc = ::poll(&wake, 1, 0);
        if (rc < 0) {
            cancel_services_ = 0;              // not looked at: the next service looks again
            if (errno == EINTR) return Sa2ReadStatus::Ready;
            return finish_live(Sa2ReadStatus::IoFailure, "cancellation poll: " + std::string(std::strerror(errno))).status;
        }
        if (wake.revents & (POLLERR | POLLNVAL))
            return finish_live(Sa2ReadStatus::IoFailure, "cancellation descriptor failed").status;
        if (wake.revents & (POLLIN | POLLHUP)) {
            uint8_t value = 0;
            const ssize_t n = ::read(cancel_fd_, &value, 1);
            if (n == 1 && value == 1)
                return finish_live(Sa2ReadStatus::Cancelled, "explicit cancellation wake").status;
            if (n < 0 && errno == EINTR) { cancel_services_ = 0; return Sa2ReadStatus::Ready; }
            return finish_live(Sa2ReadStatus::IoFailure, "cancellation channel closed, unreadable or invalid").status;
        }
    }
    ++live_services_;
    if (service_ && service_(service_arg_) == Sa2ServiceDecision::Cancel)
        return finish_live(Sa2ReadStatus::Cancelled, "cancelled by service").status;
    return Sa2ReadStatus::Ready;
#endif
}

// Each successful raw read is the single hashing point, including sniff bytes (none with the
// live hashing off, set_live_hashing).
Sa2ReadStatus Sa2StreamReader::raw_live() {
    if (raw_pos_ < raw_len_) return Sa2ReadStatus::Ready;
    if (raw_eof_) return Sa2ReadStatus::End;
#ifdef _WIN32
    const HANDLE video = pipe_of(video_fd_), wake = pipe_of(cancel_fd_);
    for (;;) {
        if (service_live() != Sa2ReadStatus::Ready) return live_status_;
        ++live_read_calls_;
        DWORD error = 0;
        const long long held = pipe_avail(video, &error);
        if (held == -1) { raw_eof_ = true; return Sa2ReadStatus::End; }
        if (held == -2)
            return finish_live(Sa2ReadStatus::IoFailure, "video peek: Windows error " + std::to_string(error)).status;
        if (held > 0) {
            DWORD got = 0;
            const DWORD want = DWORD(std::min<unsigned long long>((unsigned long long)held, live_batch_bytes));
            if (!ReadFile(video, raw_.data(), want, &got, nullptr)) {
                error = GetLastError();
                if (pipe_gone(error)) { raw_eof_ = true; return Sa2ReadStatus::End; }
                if (error != ERROR_MORE_DATA)
                    return finish_live(Sa2ReadStatus::IoFailure, "video read: Windows error " + std::to_string(error)).status;
            }
            if (got == 0) continue;
            raw_pos_ = 0;
            raw_len_ = size_t(got);
            file_read_ += size_t(got);
            if (live_hash_) sha_.update(raw_.data(), size_t(got));
            return Sa2ReadStatus::Ready;
        }
        // Nothing yet: wait for either pipe, at most the slice, as poll(2) does on POSIX.
        const double a = win_now_s();
        bool woke = false;
        while (!woke && win_now_s() - a < idle_ms_ / 1000.0) {
            Sleep(1);
            DWORD e2 = 0;
            if (pipe_avail(wake, &e2) != 0) {
                cancel_services_ = 0;          // the next service looks at it
                woke = true;
            }
            woke = woke || pipe_avail(video, &e2) != 0;
        }
        ++idle_waits_;
        idle_s_ += win_now_s() - a;
        if (!woke) ++idle_slices_;
        // Recheck the independent wake before reading any newly available video.
    }
#else
    for (;;) {
        if (service_live() != Sa2ReadStatus::Ready) return live_status_;
        ++live_read_calls_;
        const ssize_t got = ::read(video_fd_, raw_.data(), live_batch_bytes);
        if (got > 0) {
            raw_pos_ = 0;
            raw_len_ = size_t(got);
            file_read_ += size_t(got);
            if (live_hash_) sha_.update(raw_.data(), size_t(got));
            return Sa2ReadStatus::Ready;
        }
        if (got == 0) { raw_eof_ = true; return Sa2ReadStatus::End; }
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return finish_live(Sa2ReadStatus::IoFailure, "video read: " + std::string(std::strerror(errno))).status;
        pollfd p[2]{{video_fd_, POLLIN, 0}, {cancel_fd_, POLLIN, 0}};
        timespec a{}, b{};
        clock_gettime(CLOCK_MONOTONIC, &a);
        int slice = idle_ms_;
        if (wake_at_ > 0) {                    // set_wake_at: the service runs by then
            const double left = wake_at_ - (double(a.tv_sec) + double(a.tv_nsec) * 1e-9);
            slice = left <= 0 ? 0 : std::min(idle_ms_, int(left * 1000.0) + 1);
        }
        const int rc = ::poll(p, 2, slice);
        clock_gettime(CLOCK_MONOTONIC, &b);
        cancel_services_ = 0;                  // the next service looks at the cancellation
        ++idle_waits_;
        idle_s_ += double(b.tv_sec - a.tv_sec) + double(b.tv_nsec - a.tv_nsec) * 1e-9;
        if (rc == 0) ++idle_slices_;
        if (rc < 0 && errno != EINTR)
            return finish_live(Sa2ReadStatus::IoFailure, "video poll: " + std::string(std::strerror(errno))).status;
        if (p[0].revents & (POLLERR | POLLNVAL))
            return finish_live(Sa2ReadStatus::IoFailure, "video descriptor failed").status;
        // Recheck the independent wake before reading any newly available video.
    }
#endif
}

// The service runs between two batches of a fill and, unless `final_service` is false, after
// it; a fill its caller follows with a service of its own (next_live: the record's header and
// body, then the service before Ready) passes false. A batch that needs a read is preceded by
// raw_live's service. It ran before every batch and after every fill, five times a record (the
// thermal unit, 2026-10-12).
Sa2ReadStatus Sa2StreamReader::fill_live(void* dst, size_t n, bool final_service) {
    auto* out = static_cast<uint8_t*>(dst);
    size_t done = 0;
    while (done < n) {
        if (done && service_live() != Sa2ReadStatus::Ready) return live_status_;
        if (!zstd_) {
            const auto status = raw_live();
            if (status != Sa2ReadStatus::Ready) return status;
            const size_t take = std::min({n - done, raw_len_ - raw_pos_, live_batch_bytes});
            std::memcpy(out + done, raw_.data() + raw_pos_, take);
            raw_pos_ += take;
            done += take;
            live_decoded_read_ += take;
            continue;
        }
        if (zout_pos_ < zout_len_) {
            const size_t take = std::min({n - done, zout_len_ - zout_pos_, live_batch_bytes});
            std::memcpy(out + done, zout_.data() + zout_pos_, take);
            zout_pos_ += take;
            done += take;
            live_decoded_read_ += take;
            continue;
        }
        if (raw_pos_ == raw_len_ && !codec_drain_) {
            const auto status = raw_live();
            if (status == Sa2ReadStatus::End) {
                if (codec_result_ != 0)
                    return finish_live(Sa2ReadStatus::Malformed, "compressed frame ends before completion").status;
                return Sa2ReadStatus::End;
            }
            if (status != Sa2ReadStatus::Ready) return status;
        }
        ZSTD_inBuffer in{raw_.data(), raw_len_, raw_pos_};
        ZSTD_outBuffer output{zout_.data(), zout_.size(), 0};
        const size_t old_pos = raw_pos_;
        codec_result_ = ZSTD_decompressStream(static_cast<ZSTD_DStream*>(zstd_), &output, &in);
        if (ZSTD_isError(codec_result_)) {
            if (codec_failed_) return finish_live(Sa2ReadStatus::IoFailure, "zstd allocation failed").status;
            return finish_live(Sa2ReadStatus::Malformed, codec_denied_ ? "zstd codec storage limit" :
                "zstd: " + std::string(ZSTD_getErrorName(codec_result_))).status;
        }
        raw_pos_ = in.pos;
        zout_pos_ = 0;
        zout_len_ = output.pos;
        codec_drain_ = codec_result_ != 0 && output.pos == zout_.size();
        if (old_pos == in.pos && output.pos == 0 && raw_pos_ < raw_len_)
            return finish_live(Sa2ReadStatus::Malformed, "zstd decoder made no progress").status;
    }
    return final_service ? service_live() : live_status_;
}

Sa2ReadOutcome Sa2StreamReader::open_live(int video_fd, int cancel_fd,
        Sa2ServiceDecision (*service)(void*), void* arg, int slice_ms) {
    if (f_ || live_fd_mode_) {
        if (video_fd >= 0 && video_fd != video_fd_ && video_fd != cancel_fd_) ::close(video_fd);
        if (cancel_fd >= 0 && cancel_fd != video_fd && cancel_fd != video_fd_ && cancel_fd != cancel_fd_) ::close(cancel_fd);
        return {Sa2ReadStatus::IoFailure, "open_live requires a fresh reader"};
    }
    live_fd_mode_ = live_ = true;
    video_fd_ = video_fd;
    cancel_fd_ = cancel_fd;
    service_ = service;
    service_arg_ = arg;
    if (video_fd < 0 || cancel_fd < 0 || video_fd == cancel_fd || slice_ms < 1 || slice_ms > 1000)
        return finish_live(Sa2ReadStatus::IoFailure, "invalid live descriptors or poll slice");
    idle_ms_ = slice_ms;
#ifdef _WIN32
    // Each a pipe (a named pipe's client end, or an anonymous pipe's read end), read through
    // PeekNamedPipe and ReadFile, which never wait (raw_live, service_live).
    for (int fd : {video_fd_, cancel_fd_}) {
        const HANDLE h = pipe_of(fd);
        if (h == INVALID_HANDLE_VALUE || GetFileType(h) != FILE_TYPE_PIPE)
            return finish_live(Sa2ReadStatus::IoFailure, "live descriptor is not a pipe");
    }
#else
    for (int fd : {video_fd_, cancel_fd_}) {
        const int flags = ::fcntl(fd, F_GETFL);
        if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
            return finish_live(Sa2ReadStatus::IoFailure, "cannot make live descriptor nonblocking");
        if ((flags & O_ACCMODE) == O_WRONLY)
            return finish_live(Sa2ReadStatus::IoFailure, "live descriptor is write-only");
    }
#endif
    try {
        raw_.resize(live_batch_bytes + 4);
        uint8_t prefix[4];
        auto status = fill_live(prefix, sizeof(prefix));
        if (status != Sa2ReadStatus::Ready)
            return status == Sa2ReadStatus::End ? finish_live(Sa2ReadStatus::Malformed, "short live prefix") : Sa2ReadOutcome{status, live_detail_};
        // The prefix may cross several raw reads. Prepend its saved bytes to
        // the final unread span, without assuming they remain in that buffer.
        const size_t unread = raw_len_ - raw_pos_;
        std::memmove(raw_.data() + sizeof(prefix), raw_.data() + raw_pos_, unread);
        std::memcpy(raw_.data(), prefix, sizeof(prefix));
        raw_pos_ = 0;
        raw_len_ = unread + sizeof(prefix);
        live_decoded_read_ = 0;
        if (prefix[0] == 0x28) {
            if (std::memcmp(prefix, "\x28\xb5\x2f\xfd", 4))
                return finish_live(Sa2ReadStatus::Malformed, "live prefix is neither a stream nor zstd");
            zstd_ = ZSTD_createDStream_advanced(ZSTD_customMem{codec_alloc, codec_free, this});
            if (!zstd_) return finish_live(Sa2ReadStatus::IoFailure, "cannot allocate zstd decoder");
            size_t rc = ZSTD_initDStream(static_cast<ZSTD_DStream*>(zstd_));
            if (!ZSTD_isError(rc)) rc = ZSTD_DCtx_setParameter(static_cast<ZSTD_DStream*>(zstd_), ZSTD_d_windowLogMax, live_window_log);
            if (ZSTD_isError(rc)) return finish_live(Sa2ReadStatus::IoFailure, "cannot configure live zstd window limit");
            zout_.resize(live_batch_bytes);
        }
        uint8_t head[16];
        status = fill_live(head, sizeof(head));
        if (status != Sa2ReadStatus::Ready)
            return status == Sa2ReadStatus::End ? finish_live(Sa2ReadStatus::Malformed, "short live file header") : Sa2ReadOutcome{status, live_detail_};
        if (std::memcmp(head, SA2S_MAGIC, 8)) return finish_live(Sa2ReadStatus::Malformed, "live magic is not " SA2S_MAGIC);
        version_ = u32(head + 8);
        const uint32_t bytes = u32(head + 12);
        if (bytes < 16 || bytes > live_header_cap) return finish_live(Sa2ReadStatus::Malformed, "live header_bytes out of range");
        const uint32_t padded = (bytes + 3u) & ~3u;
        buf_.resize(padded - 16);
        status = fill_live(buf_.data(), buf_.size());
        if (status != Sa2ReadStatus::Ready)
            return status == Sa2ReadStatus::End ? finish_live(Sa2ReadStatus::Malformed, "short live header JSON") : Sa2ReadOutcome{status, live_detail_};
        json_.assign(reinterpret_cast<const char*>(buf_.data()), bytes - 16);
        size_t trimmed = 0;
        while (!json_.empty() && (json_.back() == '\0' || json_.back() == ' ' || json_.back() == '\n')) {
            json_.pop_back();
            if ((++trimmed & 1023u) == 0) parser_service(this);
        }
        offset_ = padded;
        if (version_ != SA2S_VERSION && version_ != SA2S_VERSION_AUDIO)
            return finish_live(Sa2ReadStatus::Malformed, "unsupported live stream version");
        std::string err;
        header_.~Sa2Header();
        new (&header_) Sa2Header(&parser_storage_);
        if (!parse_header(json_, &header_, &err, &parser_storage_, parser_service, this, &header_strings_bytes_)) return finish_live(Sa2ReadStatus::Malformed, err);
        if (header_.schema != (version_ == SA2S_VERSION ? SA2S_SCHEMA : SA2S_SCHEMA_AUDIO))
            return finish_live(Sa2ReadStatus::Malformed, "unsupported live stream schema");
        if (header_.has_audio != (version_ == SA2S_VERSION_AUDIO))
            return finish_live(Sa2ReadStatus::Malformed, "live stream audio table does not match its version");
        if (service_live() != Sa2ReadStatus::Ready) return {live_status_, live_detail_};
        index_header();
        live_opened_ = true;
        return {Sa2ReadStatus::Ready, {}};
    } catch (const Sa2ParserStorageLimit&) {
        return finish_live(Sa2ReadStatus::Malformed, "live header parser storage limit");
    } catch (const LiveParseStopped&) {
        return {live_status_, live_detail_};
    } catch (const std::bad_alloc&) {
        return finish_live(Sa2ReadStatus::IoFailure, "live reader allocation failed");
    }
}

// A record as a default one, but with its args, blobs and note storage kept for the next.
static void reset_record(Sa2Record& rec) {
    std::vector<uint32_t> args = std::move(rec.args);
    std::vector<Sa2Blob> blobs = std::move(rec.blobs);
    std::string note = std::move(rec.note);
    args.clear();
    blobs.clear();
    note.clear();
    rec = Sa2Record{};
    rec.args = std::move(args);
    rec.blobs = std::move(blobs);
    rec.note = std::move(note);
}

Sa2ReadOutcome Sa2StreamReader::next_live(Sa2Record* r) {
    if (!live_fd_mode_) return {Sa2ReadStatus::IoFailure, "next_live requires open_live"};
    if (live_status_ != Sa2ReadStatus::Ready) return {live_status_, live_detail_};
    if (!live_opened_) return finish_live(Sa2ReadStatus::IoFailure, "live header was not opened");
    try {
        uint8_t head[12];
        const uint64_t before = offset_;
        const uint64_t decoded_before = live_decoded_read_;
        auto status = fill_live(head, sizeof(head), false);
        if (status == Sa2ReadStatus::End) {
            return live_decoded_read_ == decoded_before ? finish_live(Sa2ReadStatus::End, {}) :
                finish_live(Sa2ReadStatus::Malformed, "live stream ends inside record header at offset " + std::to_string(before));
        }
        if (status != Sa2ReadStatus::Ready) return {status, live_detail_};
        Sa2Record& pending = *r;
        reset_record(pending);
        pending.kind = u32(head); pending.bytes = u32(head + 4); pending.seq = u32(head + 8); pending.offset = before;
        if (pending.bytes < 12 || (pending.bytes & 3u) || pending.bytes > kMaxRecord)
            return finish_live(Sa2ReadStatus::Malformed, "live record size out of range at offset " + std::to_string(before));
        live_body_size_ = pending.bytes - 12;
        live_view_ = nullptr;
        if (!zstd_ && raw_len_ - raw_pos_ >= live_body_size_) {
            // The whole body is in the read buffer: its blobs are views of it there, valid until
            // the next read as any record's are. Before the thermal unit (2026-10-12) every body
            // was copied into live_body_ first: the whole stream, 0.64 MB a frame in a session
            // (its game.fifo.status: 4,200,655,984 bytes in 6,593 frames).
            live_view_ = raw_.data() + raw_pos_;
            raw_pos_ += live_body_size_;
            live_decoded_read_ += live_body_size_;
        } else {
            if (live_body_capacity_ < live_body_size_) {
                // Release the prior view before allocating: no hidden overlapping body.
                live_body_.reset();
                live_body_capacity_ = 0;
                live_body_.reset(new uint8_t[live_body_size_]);
                live_body_capacity_ = live_body_size_;
            }
            status = fill_live(live_body_.get(), live_body_size_, false);
            if (status != Sa2ReadStatus::Ready)
                return status == Sa2ReadStatus::End ? finish_live(Sa2ReadStatus::Malformed, "live stream ends inside record seq " + std::to_string(pending.seq)) : Sa2ReadOutcome{status, live_detail_};
        }
        struct PendingScope {
            const Sa2Record*& owner;
            PendingScope(const Sa2Record*& pointer, const Sa2Record& value) : owner(pointer) { owner = &value; }
            ~PendingScope() { owner = nullptr; }
        } pending_scope(live_pending_, pending);
        std::string err;
        if (!parse_record(&pending, &err)) return finish_live(Sa2ReadStatus::Malformed, err);
        if (service_live() != Sa2ReadStatus::Ready) return {live_status_, live_detail_};
        offset_ += pending.bytes;
        return {Sa2ReadStatus::Ready, {}};
    } catch (const LiveParseStopped&) {
        return {live_status_, live_detail_};
    } catch (const std::bad_alloc&) {
        return finish_live(Sa2ReadStatus::IoFailure, "live reader allocation failed");
    }
}

void* Sa2ParserStorage::do_allocate(size_t size, size_t alignment) {
    if (size > cap - bytes) throw Sa2ParserStorageLimit{};
    void* p = std::pmr::get_default_resource()->allocate(size, alignment);
    bytes += size; peak = std::max(peak, bytes);
    return p;
}
void Sa2ParserStorage::do_deallocate(void* p, size_t size, size_t alignment) {
    bytes -= size;
    std::pmr::get_default_resource()->deallocate(p, size, alignment);
}
void Sa2StreamReader::parser_service(void* opaque) {
    auto* r = static_cast<Sa2StreamReader*>(opaque);
    if (r->service_live() != Sa2ReadStatus::Ready) throw LiveParseStopped{};
}
