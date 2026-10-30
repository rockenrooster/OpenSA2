// pace.h: when a replayed frame is presented under --pace (player.cpp, do_present).
//
// The schedule puts each frame at t0 + (v - v0) / speed: v is the frame's virtual time
// (its FRAME record's virtual_us), and t0 and v0 are the first paced frame's wall time
// and virtual time. A frame ready before its time waits for it. A frame ready after its
// time is late, and is presented at once.
//
// A file replay keeps its schedule whatever happens. After a late frame, the frames that
// follow are shown as soon as they are ready, until the replay is back on time.
//
// A live stream (a pipe: tools/s5/play_live.py's game) is made by a game that is at times
// behind for seconds, such as a load that takes longer on the host than in virtual time.
// Making all of that up afterwards would show the game fast until it had, and would fill
// the sound's queue past its bound. So a live schedule moves. A frame more than `slack`
// late moves t0 on by the excess, so that it is `slack` late, and only the slack is made
// up. player.cpp gives the sound's lead (kAudioLeadMs) as the slack: a stall that the
// device's queue absorbs moves nothing, and after a longer one the sound keeps its lead
// on the picture.
//
// A live frame whose virtual time is more than one and a half 60 Hz periods past its
// predecessor's is a game catching up with real time (runtime/sa2_threads.c,
// SA2_CLOCK_FOLLOWS_REAL, or several game ticks in one frame): it never waits. Were it to,
// the game would wait on the pace pipe for it, that wait would be real time its clock
// then caught up, and the next frame would be early again, a loop that held a game able to
// draw 57 frames a second to 27 (2026-10-29). Such a frame moves t0 back so that it is due
// at once, counted with the moves.
//
// What it counts, for the report's "pace":
//   frames      paced frames
//   waited      frames ready before their time, and the wait in all
//   late        lateness: how long after its time, as the schedule stood when the frame
//               was ready, each frame was presented (a frame that waited is late only by
//               the sleep's overshoot); frames over 1 ms, over one 60 Hz frame, over the
//               slack; quantiles from a histogram of 0.25 ms buckets to 250 ms (a
//               quantile past that reads as the largest lateness), and the largest
//   moves       how often a live schedule moved, and by how much in all
//   virtual_per_wall  the virtual time from the first paced frame to the last over the
//               wall time between their presents: the session's pace, 1.0 when it kept
//               real time
#pragma once

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>

class Sa2Pace {
public:
    static constexpr int kBuckets = 1000;
    static constexpr double kBucketS = 0.00025;   // kBuckets of these: 250 ms
    static constexpr double kFrameS = 1.0 / 60.0;

    // slack_s < 0: a file replay's schedule, which never moves.
    Sa2Pace(double speed, double slack_s) : speed_(speed), slack_s_(slack_s) {}

    struct Slot {
        double target;    // when to present the frame (monotonic seconds)
        double due;       // its time as the schedule stood when it was ready
        double moved;     // how far the schedule moved for it (live only)
    };

    // The slot of the frame of virtual time `vus`, ready at `now`.
    Slot schedule(uint64_t vus, double now) {
        if (!started_) { started_ = true; t0_ = now; v0_ = vus; }
        const double due = t0_ + (vus > v0_ ? double(vus - v0_) : 0.0) / 1e6 / speed_;
        double moved = 0.0;
        if (slack_s_ >= 0.0 && now - due > slack_s_) {
            moved = now - due - slack_s_;
            t0_ += moved;
            moves_++;
            moved_s_ += moved;
        } else if (slack_s_ >= 0.0 && frames_ > 0 && now < due && vus > last_vus_ &&
                   double(vus - last_vus_) / 1e6 > 1.5 * kFrameS) {
            moved = now - due;                       // a frame catching up: due at once
            t0_ += moved;
            moves_++;
            moved_s_ -= moved;
        }
        if (now < due + moved) { waited_++; wait_s_ += due + moved - now; }
        return {due + moved, due, moved};
    }

    // The frame of `slot` was presented at `at`.
    void presented(uint64_t vus, const Slot& slot, double at) {
        if (!frames_) { first_vus_ = vus; first_at_ = at; }
        frames_++;
        last_vus_ = vus;
        last_at_ = at;
        const double late = std::max(0.0, at - slot.due);
        late_max_ = std::max(late_max_, late);
        hist_[std::min(kBuckets, int(late / kBucketS))]++;
        over_ms_ += late > 0.001;
        over_frame_ += late > kFrameS;
        over_slack_ += slack_s_ >= 0.0 && late > slack_s_;
    }

    uint64_t frames() const { return frames_; }
    uint64_t moves() const { return moves_; }
    double moved_s() const { return moved_s_; }
    double late_max_s() const { return late_max_; }
    uint64_t over_frame() const { return over_frame_; }   // the report's late "over_frame"
    double last_at_s() const { return last_at_; }          // when the last paced frame was presented

    // The q-quantile of the lateness, in seconds: the upper edge of the bucket it falls
    // in, or the largest lateness when it falls past the histogram (or is the largest).
    double late_quantile(double q) const {
        if (!frames_) return 0.0;
        const uint64_t want = std::max<uint64_t>(1, uint64_t(q * double(frames_) + 0.999999));
        uint64_t seen = 0;
        for (int i = 0; i < kBuckets; i++) {
            seen += hist_[i];
            if (seen >= want) return std::min(late_max_, double(i + 1) * kBucketS);
        }
        return late_max_;
    }

    double virtual_s() const { return frames_ > 1 ? double(last_vus_ - first_vus_) / 1e6 : 0.0; }
    double wall_s() const { return frames_ > 1 ? last_at_ - first_at_ : 0.0; }

    std::string json() const {
        char slack[32] = "null", ratio[32] = "null", b[1024];
        if (slack_s_ >= 0.0) std::snprintf(slack, sizeof slack, "%.3f", slack_s_ * 1e3);
        if (wall_s() > 0.0) std::snprintf(ratio, sizeof ratio, "%.6f", virtual_s() / wall_s());
        std::snprintf(b, sizeof b,
                      "{\"live\": %s, \"speed\": %g, \"slack_ms\": %s, \"frames\": %" PRIu64
                      ", \"waited\": %" PRIu64 ", \"wait_s\": %.6f, \"late\": {\"over_1ms\": %" PRIu64
                      ", \"over_frame\": %" PRIu64 ", \"over_slack\": %" PRIu64
                      ", \"p50_ms\": %.3f, \"p90_ms\": %.3f, \"p99_ms\": %.3f, \"max_ms\": %.3f}, "
                      "\"moves\": %" PRIu64 ", \"moved_s\": %.6f, \"virtual_s\": %.6f, \"wall_s\": %.6f, "
                      "\"virtual_per_wall\": %s}",
                      slack_s_ >= 0.0 ? "true" : "false", speed_, slack, frames_, waited_, wait_s_, over_ms_,
                      over_frame_, over_slack_, late_quantile(0.5) * 1e3, late_quantile(0.9) * 1e3,
                      late_quantile(0.99) * 1e3, late_max_ * 1e3, moves_, moved_s_, virtual_s(), wall_s(), ratio);
        return b;
    }

private:
    double speed_, slack_s_;
    bool started_ = false;
    double t0_ = 0.0;
    uint64_t v0_ = 0;
    uint64_t frames_ = 0, waited_ = 0, moves_ = 0, over_ms_ = 0, over_frame_ = 0, over_slack_ = 0;
    double wait_s_ = 0.0, moved_s_ = 0.0, late_max_ = 0.0;
    uint64_t first_vus_ = 0, last_vus_ = 0;
    double first_at_ = 0.0, last_at_ = 0.0;
    uint64_t hist_[kBuckets + 1] = {};
};
