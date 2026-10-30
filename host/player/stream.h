// stream.h: a reader for the Direct3D 9 call stream a presentation build of
// the lifted game writes (host/sa2_stream.h has the format; the recorder owns
// it). Records are read one at a time into a reused buffer, so a stream of any
// size costs one record of memory. A zstd-compressed stream (magic 28 B5 2F FD)
// is decompressed on the fly. The file's bytes are hashed as they are read, so
// a report can pin the stream it played without a second pass.
//
// open() refuses a stream this reader cannot vouch for: a magic other than
// SA2S_MAGIC, a version other than SA2S_VERSION or SA2S_VERSION_AUDIO, a header
// JSON that does not parse, a schema other than the version's (SA2S_SCHEMA,
// SA2S_SCHEMA_AUDIO), no "methods" table (the table each CALL's argc is checked
// by), an "entries" table (the same for each D3DX record; older streams have
// none) whose rows are not [entry, name, argc, ops], or a version 2 header whose
// "audio" table is missing, has rows that are not [op, NAME, interface, method,
// argc, aops], or gives ds_state_bytes and xa_state_bytes other than the sizes of
// sa2s_ds_state and sa2s_xa_state (a version 1 header with one is refused too).
// Record-level checks (seq, argc, END) are the caller's, because what they mean
// depends on how far it reads.
#pragma once

#include "sa2_stream.h"
#include "sha256.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <memory_resource>
#include <string>
#include <vector>

struct Sa2Blob {
    uint16_t role = 0, arg = 0;
    uint32_t len = 0;
    const uint8_t* data = nullptr;
};

struct Sa2Record {
    uint32_t kind = 0, bytes = 0, seq = 0;
    uint64_t offset = 0;                 // of the record in the (decompressed) stream
    // SA2S_CALL (and SA2S_AUDIO: self and thread)
    uint16_t type = 0, slot = 0;
    uint32_t self = 0, thread = 0;
    // SA2S_AUDIO (virtual_us below is its clock too)
    uint16_t api = 0, op = 0;
    // SA2S_D3DX
    uint32_t entry = 0;
    // both
    std::vector<uint32_t> args;
    uint32_t eax = 0;
    std::vector<Sa2Blob> blobs;
    // SA2S_FRAME
    uint64_t virtual_us = 0;
    uint32_t frames = 0, polls = 0;
    // SA2S_NOTE
    std::string note;
    // Set when arg() was asked for an index the record does not carry: the
    // caller counts it, so a missing argument never reads as a silent 0.
    mutable bool arg_missing = false;

    // The first blob with this role and (unless arg < 0) this argument index.
    const Sa2Blob* blob(uint16_t role, int arg = -1) const {
        for (const Sa2Blob& b : blobs)
            if (b.role == role && (arg < 0 || b.arg == uint16_t(arg))) return &b;
        return nullptr;
    }
    uint32_t arg(size_t i) const {
        if (i < args.size()) return args[i];
        arg_missing = true;
        return 0u;
    }
};

// One row of the header's "entries" table: what the writer records for an
// SA2S_D3DX entry (host/sa2_stream.h, the `entry` lines).
struct Sa2EntryRow {
    std::string name;
    uint32_t argc = 0;
    std::string ops;
};

// One row of a version 2 header's "audio" table: what the writer records for an
// SA2S_AUDIO op (host/sa2_stream.h, the `audio` lines).
struct Sa2AudioRow {
    std::string name, iface, method, aops;
    uint32_t argc = 0;
};

// What the reader takes from the header JSON.
struct Sa2Header {
    std::string schema, writer;
    std::pmr::map<uint32_t, uint32_t> method_argc;   // (type << 16) | slot -> argc
    bool has_entries = false;                   // the header carries an "entries" table
    std::pmr::map<uint32_t, Sa2EntryRow> entries;    // SA2S_D3DX entry -> its row
    bool has_audio = false;                     // the header carries an "audio" table
    std::pmr::map<uint32_t, Sa2AudioRow> audio_ops;  // SA2S_AUDIO op -> its row
    explicit Sa2Header(std::pmr::memory_resource* r = std::pmr::get_default_resource())
        : method_argc(r), entries(r), audio_ops(r) {}
};

// Parse the header JSON (a strict JSON parser: objects, arrays, strings with
// escapes, numbers, true/false/null). false with *err set on anything else.
bool sa2_parse_header_json(const std::string& json, Sa2Header* out, std::string* err);

enum class Sa2ReadStatus { Ready, End, Cancelled, Malformed, IoFailure };
enum class Sa2ServiceDecision { Continue, Cancel };
struct Sa2ReadOutcome {
    Sa2ReadStatus status;
    std::string detail;
};
struct Sa2ReaderStorage {
    size_t raw, decoded, record, header_json, codec, codec_peak, record_metadata;
    size_t parser, parser_peak;
    size_t header_strings;
    size_t pending_note_bytes;
};

struct Sa2ParserStorageLimit {};
class Sa2ParserStorage : public std::pmr::memory_resource {
public:
    static constexpr size_t cap = 8u << 20;
    size_t bytes = 0, peak = 0;
private:
    void* do_allocate(size_t, size_t) override;
    void do_deallocate(void*, size_t, size_t) override;
    bool do_is_equal(const std::pmr::memory_resource& r) const noexcept override { return this == &r; }
};

// The caller's cancellation pipe (open_live's cancel_fd), looked at without taking its byte, so
// the reader still takes it at its next look (service_live) and ends the live read as it says:
// nullptr while the pipe holds nothing (or fd < 0, or the look itself failed: the reader's own look
// says that); otherwise why the read will end: "the caller cancelled" (the pipe holds a byte), "the
// caller's cancellation pipe is closed" or "the caller's cancellation pipe failed". For a wait that
// does not read the stream (host/player/tweaks/pause-unfocused.cpp's hold).
const char* sa2_cancel_waiting(int fd);

class Sa2StreamReader {
public:
    enum { OPEN_OK = 0, OPEN_IO = 1, OPEN_MALFORMED = 2 };
    Sa2StreamReader() = default;
    Sa2StreamReader(const Sa2StreamReader&) = delete;
    Sa2StreamReader& operator=(const Sa2StreamReader&) = delete;
    ~Sa2StreamReader();
    int open(const std::string& path, std::string* err);   // OPEN_*
    // Transfer two distinct, unread descriptors, including on failure. The
    // cancellation pipe carries byte 1 for explicit Cancel; EOF/error or any
    // other byte is IoFailure. Its writer must remain open until teardown.
    // Service runs before the first byte, between bounded read/decode/copy
    // batches, and before Ready. It must itself be bounded and nonblocking.
    // Use a fresh reader. No FILE may read or buffer these descriptors.
    // On Windows both are C runtime descriptors over pipe handles (_open_osfhandle: the
    // session's named pipes, opened as their client), read through PeekNamedPipe and ReadFile
    // without waiting; a wait for bytes looks at both once a millisecond for the slice.
    Sa2ReadOutcome open_live(int video_fd, int cancel_fd,
        Sa2ServiceDecision (*service)(void*), void* arg, int slice_ms = 20);
    // Ready gives the record in *r; any other outcome leaves *r unspecified. The record is
    // parsed into *r itself, whose args, blobs and note keep their storage from the record before
    // (a live session reads 6,550 records a frame, and a fresh record for each cost two
    // allocations and two frees). Blob views remain valid until the next read.
    Sa2ReadOutcome next_live(Sa2Record* r);
    Sa2ReaderStorage live_storage() const;
    uint64_t live_read_calls() const { return live_read_calls_; }
    uint64_t live_services() const { return live_services_; }
    int live_cancel_fd() const { return cancel_fd_; }   // open_live's cancel_fd, -1 before it
    static constexpr size_t live_batch_bytes = 64u << 10;
    static constexpr int live_window_log = 24;
    static constexpr size_t live_codec_cap = 32u << 20;
    static constexpr size_t live_header_cap = 1u << 20;
    static constexpr size_t live_header_strings_cap = 2u << 20;
    uint32_t version() const { return version_; }
    const std::string& header_json() const { return json_; }
    const Sa2Header& header() const { return header_; }
    // The header table's argc for (type, slot); -1 when the table has no row. Asked for every
    // CALL record, so the rows of every type and slot below kArgcTypes and kArgcSlots are kept
    // flat once the header is read (index_header); any other is looked up in the table.
    int method_argc(uint16_t type, uint16_t slot) const {
        if (type < kArgcTypes && slot < kArgcSlots && !argc_flat_.empty())
            return argc_flat_[size_t(type) * kArgcSlots + slot];
        auto it = header_.method_argc.find(uint32_t(type) << 16 | slot);
        return it == header_.method_argc.end() ? -1 : int(it->second);
    }
    // The header's "entries" row for a D3DX entry; nullptr when it has none
    // (or no table: header().has_entries says which).
    const Sa2EntryRow* entry_row(uint32_t entry) const {
        auto it = header_.entries.find(entry);
        return it == header_.entries.end() ? nullptr : &it->second;
    }
    // The header's "audio" row for an SA2S_AUDIO op; nullptr when it has none.
    const Sa2AudioRow* audio_row(uint32_t op) const {
        auto it = header_.audio_ops.find(op);
        return it == header_.audio_ops.end() ? nullptr : &it->second;
    }
    bool compressed() const { return zstd_ != nullptr; }
    // 1: a record was read; 0: clean end of the stream; -1: malformed (err says why).
    int next(Sa2Record* r, std::string* err);
    uint64_t offset() const { return offset_; }
    uint64_t file_bytes_read() const { return file_read_; }
    // A live stream (a pipe or a socket) goes quiet for as long as the game takes to make
    // its next frame. With an idle hook set, the reader waits for more bytes in poll(2)
    // slices of `slice_ms` and calls the hook between them, so the caller can answer its
    // window system meanwhile. A regular file never waits, so the hook is never called for
    // one, and a file replay is unchanged.
    void set_idle(void (*hook)(void*), void* arg, int slice_ms) {
        idle_hook_ = hook; idle_arg_ = arg; idle_ms_ = slice_ms;
    }
    // A live descriptor's wait ends by `mono_s` (CLOCK_MONOTONIC seconds) at the latest, so the
    // service runs then: the player's next interpolated Present (player.cpp, SA2_PLAYER_INTERP).
    // 0: the slice alone, as before.
    void set_wake_at(double mono_s) { wake_at_ = mono_s; }
    bool live() const { return live_; }
    uint64_t idle_waits() const { return idle_waits_; }
    uint64_t idle_slices() const { return idle_slices_; }
    double idle_s() const { return idle_s_; }
    bool whole_file_read() const { return live_fd_mode_ ? live_status_ == Sa2ReadStatus::End : eof_; }
    // The sha256 of the file's bytes as read; "" unless the whole file was read (or, from a
    // live descriptor stream, when its hashing was off).
    // Call once, after the last next(), and not after hash_rest().
    std::string file_sha256() { return whole_file_read() && (!live_fd_mode_ || live_hash_) ? sha_.hex() : std::string(); }
    // A live descriptor stream's hashing (open_live; files are always hashed): off, its bytes are
    // not hashed as they are read, and file_sha256() is "". Set before open_live; on by default.
    void set_live_hashing(bool on) { live_hash_ = on; }
    // For a caller that stopped before the end: reads the rest of the file into
    // the hash without parsing it, and returns the whole file's sha256 ("" when
    // the rest cannot be read). Call once, instead of file_sha256().
    std::string hash_rest();

private:
    static constexpr uint32_t kArgcTypes = 32, kArgcSlots = 256;
    void index_header();
    Sa2ReadOutcome finish_live(Sa2ReadStatus, const std::string&);
    Sa2ReadStatus service_live();
    Sa2ReadStatus raw_live();
    Sa2ReadStatus fill_live(void*, size_t, bool final_service = true);
    bool parse_record(Sa2Record*, std::string*);
    const uint8_t* record_data() const {
        if (!live_fd_mode_) return buf_.data();
        return live_view_ ? live_view_ : live_body_ ? live_body_.get() : buf_.data();
    }
    size_t record_size() const { return live_fd_mode_ ? live_body_size_ : buf_.size(); }
    static void* codec_alloc(void*, size_t);
    static void codec_free(void*, void*);
    static void parser_service(void*);
    void wait_readable();
    bool fill(void* dst, size_t n, bool* eof);
    bool parse_call_or_d3dx(Sa2Record* r, std::string* err);
    bool parse_audio(Sa2Record* r, std::string* err);
    bool parse_blobs(const uint8_t* p, const uint8_t* end, Sa2Record* r, std::string* err);

    FILE* f_ = nullptr;
    void* zstd_ = nullptr;               // ZSTD_DStream*
    std::vector<uint8_t> zin_, zout_;
    size_t zin_pos_ = 0, zin_len_ = 0, zout_pos_ = 0, zout_len_ = 0;
    bool zin_eof_ = false, eof_ = false;
    uint32_t version_ = 0;
    std::string json_;
    Sa2ParserStorage parser_storage_;
    Sa2Header header_;
    std::vector<uint8_t> buf_;
    std::unique_ptr<uint8_t[]> live_body_;
    size_t live_body_capacity_ = 0, live_body_size_ = 0;
    // The record's body where it lies in raw_, when a read brought it in whole (next_live):
    // parsed there, not copied into live_body_ first. nullptr when it was copied.
    const uint8_t* live_view_ = nullptr;
    const Sa2Record* live_pending_ = nullptr;
    std::vector<int32_t> argc_flat_;     // method_argc's rows, index_header
    uint64_t offset_ = 0, file_read_ = 0;
    bool live_ = false;                  // a pipe or a socket, not a file
    void (*idle_hook_)(void*) = nullptr;
    void* idle_arg_ = nullptr;
    int idle_ms_ = 20;
    double wake_at_ = 0;                 // set_wake_at
    uint64_t idle_waits_ = 0, idle_slices_ = 0;
    double idle_s_ = 0;
    Sha256 sha_;
    bool live_fd_mode_ = false, live_opened_ = false, raw_eof_ = false, live_hash_ = true;
    int video_fd_ = -1, cancel_fd_ = -1;
    uint32_t cancel_services_ = 0;       // service_live's calls since it looked at the cancellation (every 1024th)
    Sa2ServiceDecision (*service_)(void*) = nullptr;
    void* service_arg_ = nullptr;
    Sa2ReadStatus live_status_ = Sa2ReadStatus::Ready;
    std::string live_detail_;
    std::vector<uint8_t> raw_;
    size_t raw_pos_ = 0, raw_len_ = 0, codec_result_ = 1;
    size_t codec_bytes_ = 0, codec_peak_ = 0;
    size_t header_strings_bytes_ = 0;
    bool codec_denied_ = false, codec_failed_ = false, codec_drain_ = false;
    uint64_t live_read_calls_ = 0, live_services_ = 0, live_decoded_read_ = 0;
};
