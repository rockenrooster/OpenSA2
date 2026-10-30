"""The host keyboard: tools/s5/play_live.py's session and host/player's key sender.

Design 3.5 feeds input "from the host". The runtime's half, the DirectInput keyboard's
script and FIFO sources (runtime/sa2_dinput.h, "The host keyboard"), is held by
harness/dinput_probe.c's input-* and refuse-input-* cases, which drive the device the way
lifted code does and need the linked probe. Here, without the game, a display or the probe:

  * play_live's commands, from its own --dry-run: each scene's presentation build under its
    own name with the recorder, no bound on the run, the keyboard FIFO and its log, Metal
    Harbor without the recording's controller, the player live on two inherited
    descriptors with --input-path, and the disk's limits; the names, paths and limits it
    refuses; that a dry run runs and makes nothing; that each session gets a directory of
    its own and leaves every earlier one's files as they were;
  * the session's sound: the recorder's (SA2_REC_AUDIO=1, "audio" in the meta) and the
    player's --audio by default, --audio-wav into the session's player/, --sound-driver as
    SDL_AUDIO_DRIVER, an inherited driver taken as SDL takes it (SDL_AUDIO_DRIVER, else
    SDL2's SDL_AUDIODRIVER), kept and shown unless --sound-driver replaces it, and refused
    outside alsa and dummy ("disk" writes into the checkout), --no-sound as the session
    before sound, the options it refuses, and the receipt's and the closing line's account
    of what the player's report says it did;
  * the session's pace: the player's --pace and --pace-log by default and the relay held to
    one whole frame; --no-pace as the session before pacing; the held relay over a real
    pipe, between a writer ahead of a slow reader (held from the first frame, every byte
    still delivered and copied), one behind it (rarely held), one behind and then ahead
    (held again), a reader that goes away (nothing held after) and a writer that goes
    away while held (nothing held after either); the pipe's size for the frames, and how
    the relay takes the writer as waiting or not; host/player/pace.h's schedule, compiled
    into a driver here and fed literal times (a file's that never moves, a live one that
    moves past its slack, the lateness it counts); and the frames ahead and the closing
    line from literal logs;
  * G5's play receipt (--receipt, Metal Harbor's alone, at the level the clause reads): the
    per-tick table's observer in the tick probe's place and its one run setting, nothing
    else of the command changed; the observer's source in the session's directory before
    the game, its objects forgotten with the hooks'; at the end the session's record and,
    from the same bytes, the receipt (written under the test's own directory), and the
    closing line; a receipt step that fails kept in the session's receipt and said, not
    raised, and the session's receipt on disk before the step runs; a session without
    --receipt writes none of these, and no test writes the checkout's receipt
    (tools/s5/play_receipt.py's own tests hold the observer and the writer);
  * how a session's game is found and stopped: the binary among loop_run's children, then
    SIGTERM, a second SIGTERM to the game as found again, SIGKILL, each against a stand-in
    that answers it; and the disk watchdog stopping a session whose stream copy passes its
    cap, or whose disk falls below its floor;
  * host/player/input.cpp's KeySender, compiled into a driver here and fed synthetic SDL
    events: the records it writes into a real FIFO, byte for byte; repeats, other windows,
    unmapped keys and a clear with nothing held changing nothing; nothing at all before
    start(); ENXIO, a full pipe and a reader that goes away; its report; its scancode table
    against zig's dinput.h; and every key the game's bindings name (Config/Keyboard.cfg,
    through the image's own tables) in that table;
  * the record's magic and size, the same in the runtime's header and the player's.

Every signal a test sends goes through a guard that lets it reach only this process's own
descendants: a stand-in found wrongly fails its test instead of signalling a stranger.
"""

from __future__ import annotations

import fcntl
import hashlib
import json
import os
import re
import shlex
import shutil
import signal
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "s5"))
import play_live as PLV  # noqa: E402
sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_s5_play_live_evidence as E  # noqa: E402  a session whose latency evidence joins

TAG = PLV.PR.tree_tag()
SDL_INCLUDE = ROOT / "private" / "s5-tools" / "install" / "include"
DINPUT_H = (ROOT / "private" / "predecessor-tools" / "zig" / "zig-x86_64-linux-0.15.2" / "lib"
            / "libc" / "include" / "any-windows-any" / "dinput.h")
EXE = ROOT / "private" / "exe" / "sonic2app-unpacked.exe"
KEYBOARD_CFG = ROOT / "private" / "install" / "Config" / "Keyboard.cfg"
GXX = shutil.which(os.environ.get("CXX", "g++"))
REAL_KILL = os.kill


def descends_from_this_process(pid: int) -> bool:
    me = os.getpid()
    while pid > 1:
        try:
            pid = int(Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[1])
        except (OSError, ValueError, IndexError):
            return False
        if pid == me:
            return True
    return False


@pytest.fixture(autouse=True)
def the_checkouts_play_receipt_is_left_alone():
    """No test writes the checkout's play receipts: a session's main() with --receipt names
    its evidence below the checkout, so a writer that ignored the test's dest_root would
    write a tracked receipt there."""
    paths = [ROOT / PLV.PRC.receipt_path(host) for host in PLV.PRC.HOSTS]

    def state():
        return [path.stat().st_mtime_ns if path.exists() else None for path in paths]
    before = state()
    yield
    assert state() == before, f"a test wrote one of {paths} in the checkout"


@pytest.fixture(autouse=True)
def signals_reach_only_our_children(monkeypatch):
    """os.kill for this module's tests: a pid that is not this process's descendant is an
    error, not a signal (a game_pid gone wrong once signalled another agent's python)."""
    def guarded(pid: int, sig: int) -> None:
        if not descends_from_this_process(pid):
            raise AssertionError(f"a test signalled pid {pid}, which is not its own child")
        REAL_KILL(pid, sig)
    monkeypatch.setattr(os, "kill", guarded)


# ---------------------------------------------------------------- the commands

def dry_run(capsys, *argv: str) -> dict[str, str]:
    """play_live --dry-run's lines, by their label."""
    assert PLV.main(["--dry-run", *argv]) == 0
    out = {}
    for line in capsys.readouterr().out.splitlines():
        label, _, rest = line.partition(": ")
        out[label] = rest
    return out


def game_parts(line: str) -> tuple[list[str], dict[str, str], list[str], list[str]]:
    """loop_run's argv, its --run settings, and its hooks and wraps in order."""
    argv = shlex.split(line)
    spec = argv[argv.index("--run") + 1].split("|")
    assert spec[0] == "play"
    env = dict(kv.split("=", 1) for kv in spec[1:])
    hooks = [argv[i + 1] for i, a in enumerate(argv) if a == "--hook"]
    wraps = [argv[i + 1] for i, a in enumerate(argv) if a == "--wrap"]
    return argv, env, hooks, wraps


RECORDER_WRAPS = ["sa2_ft_add", "sa2_d3d9_make_d3d9", "sa2_d3d9_d3dx_texture_from_memory",
                  "sa2_d3d9_d3dx_create_texture", "sa2_d3d9_d3dx_filter_texture",
                  "sa2_d3d9_d3dx_load_surface_from_surface",
                  "sa2_d3d9_d3dx_get_shader_constant_table", "sa2_d3d9_d3dx_cube_from_memory",
                  "sa2_d3d9_d3dx_volume_from_memory", "sa2_d3d9_d3dx_create_cube_texture",
                  "sa2_d3d9_d3dx_create_volume_texture", "sa2_d3d9_d3dx_save_surface_to_file",
                  "sa2_d3d9_d3dx_save_texture_to_file"]
BOOT_DIR = "private/generated/s5/input/boot/dry-run"       # a dry run's run id is "dry-run"


def boot_env(sound: bool = True) -> dict[str, str]:
    """The boot session's settings; with `sound` (the default) the recorder's audio and
    "audio" in the meta, as present_run --audio has them."""
    d = BOOT_DIR
    meta = {"build": f"present-play-boot.{TAG}", "host": "dxvk-native", "interactive": True,
            "run": "dry-run", "runner": "tools/s5/play_live.py", "scene": "boot",
            "transport": "fifo"}
    env = {"SA2_D3D9_DEVICE": "1", "SA2_DSOUND_DEVICE": "1", "SA2_IMPORT_BUDGET": "0",
           "SA2_PUMP_POLLS": "0", "SA2_DINPUT_DEVICE": "1", "SA2_XAUDIO2_DEVICE": "1",
           "SA2_DDRAW_DEVICE": "1", "SA2_D3DX_CONSTANT_TABLE": "1",
           "SA2_VIRTUAL_BUDGET_MS": "0", "SA2_VFS_WRITE_ROOT": f"{d}/vfsroot",
           "SA2_REC_TRACE": f"{d}/game.fifo", "SA2_REC_COUNTS": f"{d}/counts.json",
           "SA2_INPUT_DI_SOURCE": f"{d}/input.fifo", "SA2_INPUT_DI_LOG": f"{d}/input.log"}
    if sound:
        meta["audio"] = True
        env["SA2_REC_AUDIO"] = "1"
    env["SA2_REC_META"] = json.dumps(meta, separators=(",", ":"), sort_keys=True)
    return env


def test_the_boot_session_is_the_presentation_build_unbounded_with_the_keyboard_fifo(capsys):
    lines = dry_run(capsys)
    argv, env, hooks, wraps = game_parts(lines["game"])
    build = f"present-play-boot.{TAG}"
    assert argv[:2] == [str(ROOT / ".venv" / "bin" / "python"), str(ROOT / "tools" / "s4" / "loop_run.py")]
    assert argv[2:10] == ["--tag", "next+allarms", "--main", "harness/crt_probe.c", "--name", build,
                          "--out-prefix", build]
    assert hooks == ["harness/replay/wrap_tick_probe.c", "host/sa2_present_rec.c"]
    assert wraps == ["sub_433ee0"] + RECORDER_WRAPS
    assert env == boot_env()                  # every setting, and nothing else
    assert argv[argv.index("--timeout") + 1] == "86400"
    assert argv[-2:] == ["--probe-arg=--timeout", "--probe-arg=0"]
    assert lines["player build"] == "OUT=private/generated/s5/input/bin bash host/player/build.sh"


def test_the_metalharbor_session_is_the_test_spawn_without_the_recordings_controller(capsys):
    lines = dry_run(capsys, "--scene", "metalharbor")
    argv, env, hooks, wraps = game_parts(lines["game"])
    assert argv[argv.index("--name") + 1] == f"present-play-mh.{TAG}"
    assert hooks == ["harness/replay/wrap_tick_probe.c", "harness/replay/wrap_test_spawn.c",
                     "harness/stage_goal_probe.c", "host/sa2_present_rec.c"]
    assert wraps == ["sub_433ee0", "sub_7a8948"] + RECORDER_WRAPS
    d = "private/generated/s5/input/metalharbor/dry-run"
    assert {k: env[k] for k in ("SA2_TEST_SPAWN_LEVEL", "SA2_TEST_SPAWN_CHARACTER",
                                "SA2_GUEST_CMDLINE", "SA2_IMPORT_BUDGET", "SA2_PUMP_POLLS",
                                "SA2_VIRTUAL_BUDGET_MS", "SA2_INPUT_DI_SOURCE",
                                "SA2_INPUT_DI_LOG", "SA2_REC_AUDIO")} == {
        "SA2_TEST_SPAWN_LEVEL": "10", "SA2_TEST_SPAWN_CHARACTER": "0",
        "SA2_GUEST_CMDLINE": "--level metalharbor --character sonic", "SA2_IMPORT_BUDGET": "0",
        "SA2_PUMP_POLLS": "0", "SA2_VIRTUAL_BUDGET_MS": "0",
        "SA2_INPUT_DI_SOURCE": f"{d}/input.fifo", "SA2_INPUT_DI_LOG": f"{d}/input.log",
        "SA2_REC_AUDIO": "1"}
    assert json.loads(env["SA2_REC_META"]) == {
        "audio": True, "build": f"present-play-mh.{TAG}", "host": "dxvk-native",
        "interactive": True, "run": "dry-run", "runner": "tools/s5/play_live.py",
        "scene": "metalharbor", "transport": "fifo"}
    # The recorder's counts beside the run record: the session joins M7's recorded runs.
    assert env["SA2_REC_COUNTS"] == "private/generated/s5/input/metalharbor/dry-run/counts.json"
    assert not {"SA2_INPUT_SCRIPT", "SA2_INPUT_APPLIED", "SA2_INPUT_DI_SCRIPT",
                "SA2_REPLAY_TICKS", "SA2_REPLAY_WORDS", "SA2_GOAL_LATCH", "SA2_STAGE_TRACE",
                "SA2_DEVICE_PROFILE", "SA2_PLAY_TICKS"} & set(env)
    assert lines["receipt"] == ("none (no --receipt): the game writes no per-tick table and the "
                                "session no play receipt")


MH_DIR = "private/generated/s5/input/metalharbor/dry-run"


def test_a_receipt_session_puts_the_tables_observer_in_the_tick_probes_place(capsys):
    """--receipt changes two things in the game's command: the observer the session's
    directory will hold in the tick probe's place among the hooks, and one run setting, where
    it writes the per-tick table. The wraps, every other setting and every other line stay;
    the receipt line says where the receipt or the session's own record goes."""
    plain = dry_run(capsys, "--scene", "metalharbor")
    lines = dry_run(capsys, "--scene", "metalharbor", "--receipt")
    argv, env, hooks, wraps = game_parts(lines["game"])
    argv0, env0, hooks0, wraps0 = game_parts(plain["game"])
    assert hooks == [f"{MH_DIR}/sa2_play_ticks.c", "harness/replay/wrap_test_spawn.c",
                     "harness/stage_goal_probe.c", "host/sa2_present_rec.c"]
    assert hooks0 == ["harness/replay/wrap_tick_probe.c"] + hooks[1:]
    assert wraps == wraps0
    assert env == env0 | {"SA2_PLAY_TICKS": f"{MH_DIR}/play-ticks.tsv",
                          "SA2_PLAY_POLLS": f"{MH_DIR}/play-polls.tsv",
                          "SA2_PLAY_POLLS_REPORT": f"{MH_DIR}/play-polls-report.json"}
    spec = argv.index("--run") + 1
    swapped = [hooks0[0] if a == hooks[0] else a for a in argv]
    swapped[spec] = argv0[spec]
    assert swapped == argv0                    # the rest of loop_run's command as it was
    # The player adds its latency logs; the session ends at the goal too.
    assert lines["player"] == plain["player"].replace(" --audio", (
        f" --key-log {MH_DIR}/player-keys.tsv --present-log {MH_DIR}/player-presents.tsv "
        "--audio"))
    assert lines["end"] == ("the goal: once play-ticks.tsv shows the goal latch risen in the "
                            "stage and 300 ticks past it (any other end sooner than 60 ticks "
                            "past it waits for them, at most 10 s); " + plain["end"])
    assert {k: v for k, v in lines.items() if k not in ("game", "receipt", "player", "end")} == {
        k: v for k, v in plain.items() if k not in ("game", "receipt", "player", "end")}
    assert lines["receipt"] == (
        f"the game writes its per-tick table to {MH_DIR}/play-ticks.tsv (SA2_PLAY_TICKS, by the "
        f"observer {MH_DIR}/sa2_play_ticks.c in the tick probe's place); at the end, the "
        f"session's document to {MH_DIR}/play-record.json, and the same bytes to G5's play "
        "receipt Documentation/receipts/s5-play-dxvk-native-v1.json if the table shows the goal "
        "ring reached after level 10 was entered and the evidence is fit, a receipt in place "
        f"kept first at {MH_DIR}/play-receipt-replaced.json; the latency logs "
        f"{MH_DIR}/player-keys.tsv, {MH_DIR}/player-presents.tsv, {MH_DIR}/play-polls.tsv and "
        f"{MH_DIR}/relay-frames.tsv")


def test_the_play_receipt_is_metal_harbors_alone():
    with pytest.raises(SystemExit, match=re.escape(
            "--receipt: G5's play receipt is Metal Harbor's; add --scene metalharbor")):
        PLV.main(["--dry-run", "--receipt"])


def test_a_receipt_session_spawns_at_the_level_the_clause_reads_or_does_not_start(monkeypatch):
    """The clause reads Metal Harbor's level from the replay registry; the session spawns at
    stage_run's. A session that cannot be the receipt's is refused before it starts."""
    import stage_run as SR
    monkeypatch.setitem(SR.STAGES, "metalharbor", (11,) + SR.STAGES["metalharbor"][1:])
    with pytest.raises(SystemExit) as refused:
        PLV.main(["--dry-run", "--scene", "metalharbor", "--receipt"])
    assert str(refused.value) == (
        "--receipt: the session spawns level 11 (tools/s4/stage_run.py); the play clause reads "
        "Metal Harbor at level 10 (Documentation/replays.json)")


def test_a_receipt_session_with_no_level_to_read_does_not_start(monkeypatch):
    def none(root):
        raise PLV.PRC.X.Refused("Documentation/replays.json registers no replay named metalharbor")
    monkeypatch.setattr(PLV.PRC.X, "stage_level", none)
    with pytest.raises(SystemExit) as refused:
        PLV.main(["--dry-run", "--scene", "metalharbor", "--receipt"])
    assert str(refused.value) == ("--receipt: the play clause reads no level for Metal Harbor: "
                                  "Documentation/replays.json registers no replay named "
                                  "metalharbor")


def test_the_observer_takes_the_tick_probes_one_place_or_is_refused(monkeypatch):
    import stage_run as SR
    assert PLV.scene_hooks("boot", "obs.c") == (["obs.c"], ["sub_433ee0"])
    monkeypatch.setattr(SR, "HOOKS", SR.HOOKS + ["harness/replay/wrap_tick_probe.c"])
    with pytest.raises(SystemExit, match="links harness/replay/wrap_tick_probe.c 2 times"):
        PLV.scene_hooks("metalharbor", "obs.c")


PLAYER_HEAD = (f"SA2_PLAY_OUT={BOOT_DIR} OUT=private/generated/s5/input/bin "
               f"SA2_DXVK_LOG_PATH={BOOT_DIR}/dxvk-log DXVK_LOG_LEVEL=info")
PLAYER_ARGS = (f"bash host/player/run_player.sh --interactive --video-fd 40 --cancel-fd 41 "
               f"--end-fd 42 --input-path {BOOT_DIR}/input.fifo --stage 3 "
               f"--report {BOOT_DIR}/player/report.json")
PACE_ARGS = f"--pace --pace-log {BOOT_DIR}/player/pace.tsv"


@pytest.fixture(autouse=True)
def no_audio_driver(monkeypatch):
    """No SDL_AUDIO_DRIVER or SDL_AUDIODRIVER (SDL2's name, which SDL3 reads where the
    other is unset) in this process's environment, whatever the shell left (a test that
    wants one sets it): play_live keeps an inherited driver and refuses most values."""
    for name in ("SDL_AUDIO_DRIVER", "SDL_AUDIODRIVER"):
        monkeypatch.delenv(name, raising=False)


def test_the_player_runs_live_on_three_inherited_descriptors_with_the_keyboard_fifo(capsys):
    lines = dry_run(capsys, "--no-player-build")
    assert "player build" not in lines
    d = BOOT_DIR
    assert lines["player"] == f"{PLAYER_HEAD} {PLAYER_ARGS} {PACE_ARGS} --audio"
    assert lines["fifos"].startswith(f"keyboard {d}/input.fifo (made and held open O_RDWR")


# ---------------------------------------------------------------- the sound

def plan_of(*argv: str) -> dict:
    """play_live's plan for a dry run with `argv`, through its own parser."""
    return PLV.plan(PLV.parser().parse_args(["--dry-run", *argv]))


def test_a_session_plays_the_games_sound_on_sdls_default_device_by_default(capsys):
    """The recorder's audio and the player's --audio; with no SDL_AUDIO_DRIVER inherited
    and none given, SDL chooses, and the dry run says that means the speakers."""
    lines = dry_run(capsys, "--no-player-build")
    assert game_parts(lines["game"])[1] == boot_env()
    assert lines["sound"] == (
        "the game records it (SA2_REC_AUDIO=1, a version 2 stream) and the player plays it on "
        "SDL's default playback device (--audio), driver SDL's own choice (on this desktop, "
        "the speakers)")
    assert "SDL_AUDIO_DRIVER" not in lines["player"]
    pl = plan_of()
    assert "SDL_AUDIO_DRIVER" not in pl["player_env"] and pl["player_cmd"][-1] == "--audio"
    assert pl["sound"] == {"on": True, "driver": None, "driver_from": None, "wav": None}


# The two names SDL3 takes its audio driver from (SDL_hints.c, GetHintEnvironmentVariable),
# and how the dry run says a driver came from each.
INHERITED_AS = {"SDL_AUDIO_DRIVER": "inherited",
                "SDL_AUDIODRIVER": "inherited as SDL_AUDIODRIVER, SDL2's name"}


@pytest.mark.parametrize("name", sorted(INHERITED_AS))
@pytest.mark.parametrize("driver", ["dummy", "alsa"])
def test_an_inherited_audio_driver_is_kept_and_shown(capsys, monkeypatch, driver, name):
    """SDL_AUDIO_DRIVER=dummy in the shell silences a session, and so does SDL2's
    SDL_AUDIODRIVER=dummy, which SDL3 takes where the other is unset: the player is given
    it as SDL_AUDIO_DRIVER, and the dry run shows it and where it came from."""
    monkeypatch.setenv(name, driver)
    lines = dry_run(capsys, "--no-player-build")
    assert lines["player"] == (f"{PLAYER_HEAD} SDL_AUDIO_DRIVER={driver} {PLAYER_ARGS} "
                               f"{PACE_ARGS} --audio")
    assert lines["sound"].endswith(f"driver SDL_AUDIO_DRIVER={driver} ({INHERITED_AS[name]})")
    pl = plan_of()
    assert pl["player_env"]["SDL_AUDIO_DRIVER"] == driver
    assert "SDL_AUDIODRIVER" not in pl["player_env"]
    assert pl["sound"] == {"on": True, "driver": driver, "driver_from": name, "wav": None}


@pytest.mark.parametrize("name", sorted(INHERITED_AS))
@pytest.mark.parametrize("value", ["disk", "pipewire", "DUMMY", "alsa,dummy"])
def test_an_inherited_audio_driver_outside_alsa_and_dummy_is_refused(monkeypatch, value, name):
    """SDL's "disk" would write the sound into the checkout, under either name; any other
    spelling, even one SDL would take (another case, a list), is refused the same way: a
    session takes the two names exactly."""
    monkeypatch.setenv(name, value)
    with pytest.raises(SystemExit, match=re.escape(
            f"{name}={value!r} is in the environment: a session takes only alsa, "
            "dummy (SDL's 'disk' would write the sound into the checkout); unset it, or give "
            "--sound-driver")):
        PLV.main(["--dry-run"])


@pytest.mark.parametrize("sdl3, driver, shown", [
    ("dummy", "dummy", "driver SDL_AUDIO_DRIVER=dummy (inherited)"),
    ("", None, "driver SDL's own choice (on this desktop, the speakers)"),
], ids=["dummy", "empty"])
def test_sdl3s_name_hides_sdl2s_as_in_sdl_even_when_empty(capsys, monkeypatch, sdl3, driver,
                                                          shown):
    """SDL reads SDL2's SDL_AUDIODRIVER only where SDL_AUDIO_DRIVER is unset: set, even
    empty (SDL's own choice), it is the one, and an SDL_AUDIODRIVER=disk beside it is
    neither refused nor passed on."""
    monkeypatch.setenv("SDL_AUDIO_DRIVER", sdl3)
    monkeypatch.setenv("SDL_AUDIODRIVER", "disk")
    lines = dry_run(capsys, "--no-player-build")
    assert lines["sound"].endswith(shown) and "disk" not in lines["player"]
    pl = plan_of()
    assert pl["sound"]["driver"] == driver
    assert pl["player_env"].get("SDL_AUDIO_DRIVER") == driver
    assert "SDL_AUDIODRIVER" not in pl["player_env"]


@pytest.mark.parametrize("name", sorted(INHERITED_AS))
def test_the_sound_driver_option_replaces_an_inherited_one(capsys, monkeypatch, name):
    monkeypatch.setenv(name, "disk")
    lines = dry_run(capsys, "--no-player-build", "--sound-driver", "dummy")
    assert "SDL_AUDIO_DRIVER=dummy " in lines["player"] and "disk" not in lines["player"]
    assert lines["sound"].endswith("driver SDL_AUDIO_DRIVER=dummy (--sound-driver)")
    pl = plan_of("--sound-driver", "dummy")
    assert pl["player_env"]["SDL_AUDIO_DRIVER"] == "dummy"
    assert "SDL_AUDIODRIVER" not in pl["player_env"]
    assert pl["sound"]["driver_from"] == "--sound-driver"


def test_the_sound_driver_and_the_wav_are_the_options(capsys):
    d = BOOT_DIR
    lines = dry_run(capsys, "--no-player-build", "--sound-driver", "dummy", "--audio-wav")
    assert lines["player"] == (
        f"{PLAYER_HEAD} SDL_AUDIO_DRIVER=dummy {PLAYER_ARGS} {PACE_ARGS} --audio "
        f"--audio-wav {d}/player/sound.wav")
    assert lines["sound"] == (
        "the game records it (SA2_REC_AUDIO=1, a version 2 stream) and the player plays it on "
        "SDL's default playback device (--audio), driver SDL_AUDIO_DRIVER=dummy "
        f"(--sound-driver); written to {d}/player/sound.wav and its .json (--audio-wav)")
    assert game_parts(lines["game"])[1] == boot_env()
    assert "SDL_AUDIO_DRIVER=alsa " in dry_run(capsys, "--sound-driver", "alsa")["player"]


@pytest.mark.parametrize("inherited", [None, ("SDL_AUDIO_DRIVER", "dummy"),
                                       ("SDL_AUDIO_DRIVER", "disk"), ("SDL_AUDIODRIVER", "disk")])
def test_no_sound_is_the_session_as_it_was_before_sound(capsys, monkeypatch, inherited):
    """A version 1 stream and a player with neither --audio nor --audio-wav: the commands
    of the session before sound, word for word, with no audio driver for a player that
    plays nothing (an inherited one, even "disk", under either name, is left out, not
    refused)."""
    if inherited:
        monkeypatch.setenv(*inherited)
    lines = dry_run(capsys, "--no-player-build", "--no-sound")
    assert game_parts(lines["game"])[1] == boot_env(sound=False)
    assert "SA2_REC_AUDIO" not in lines["game"]
    assert lines["player"] == f"{PLAYER_HEAD} {PLAYER_ARGS} {PACE_ARGS}"
    assert lines["sound"] == ("none (--no-sound): the game records none (a version 1 stream), "
                              "and the player plays and writes none")
    assert not {"SDL_AUDIO_DRIVER", "SDL_AUDIODRIVER"} & set(plan_of("--no-sound")["player_env"])


# ---------------------------------------------------------------- the pace

def test_a_session_is_paced_by_default_and_no_pace_is_the_session_before_pacing(capsys):
    d = BOOT_DIR
    lines = dry_run(capsys, "--no-player-build")
    assert lines["pace"] == (
        f"the player presents each frame at its virtual time (--pace; per frame in "
        f"{d}/player/pace.tsv), and the relay takes nothing more from the game while 1 whole "
        "frame(s) wait for the player (2 while the game is not found waiting), so a game ahead "
        "of real time waits in its write")
    assert plan_of()["pace"] == {"on": True, "lead_frames": 1}
    lines = dry_run(capsys, "--no-player-build", "--no-pace", "--no-sound")
    assert lines["player"] == f"{PLAYER_HEAD} {PLAYER_ARGS}"      # the player before pacing
    assert lines["pace"] == ("none (--no-pace): the game runs as fast as it can and the player "
                             "shows each frame as it comes")
    assert plan_of("--no-pace")["pace"] == {"on": False, "lead_frames": None}
    assert game_parts(lines["game"])[1] == boot_env(sound=False)   # the game is the same


@pytest.mark.parametrize("argv", [["--no-sound", "--audio-wav"],
                                  ["--no-sound", "--sound-driver", "dummy"]])
def test_no_sound_refuses_the_options_that_need_sound(argv):
    with pytest.raises(SystemExit, match=re.escape(
            "--no-sound records and plays no sound: --audio-wav and --sound-driver cannot apply")):
        PLV.main(["--dry-run", *argv])


def test_a_sound_driver_that_writes_into_the_checkout_is_not_offered(capsys):
    """SDL's "disk" driver writes the sound into the working directory, the checkout."""
    with pytest.raises(SystemExit) as e:
        PLV.main(["--dry-run", "--sound-driver", "disk"])
    assert e.value.code == 2
    assert "argument --sound-driver: invalid choice: 'disk' (choose from alsa, dummy)" in \
        capsys.readouterr().err


def test_the_disks_limits_are_the_defaults_or_the_options(capsys):
    assert dry_run(capsys)["limits"] == (
        f"the game is asked to stop when {BOOT_DIR}/game.live.sa2s passes 200000000000 bytes "
        "or the free space under private/generated/s5/input/boot falls below 50000000000 bytes")
    assert dry_run(capsys, "--stream-cap-gb", "1.5", "--disk-floor-gb", "0")["limits"] == (
        f"the game is asked to stop when {BOOT_DIR}/game.live.sa2s passes 1500000000 bytes "
        "or the free space under private/generated/s5/input/boot falls below 0 bytes")


@pytest.mark.parametrize("argv, why", [
    (["--build", "replay-boot"], "replay-boot"),                     # a gate's
    (["--build", "present-boot.x"], "a presentation run's own build name"),   # present_run's
    (["--build", "present-mh.x"], "present-mh.x"),                   # the other replay's
    (["--scene", "metalharbor", "--build", "present-boot-probe"], "present-boot-probe"),
    (["--build", "present-live.x"], "a presentation run's own build name"),   # present_live's
    (["--build", "play-boot"], "play-boot"),                         # not a presentation build
    (["--out", "/tmp/sa2-play-session"], "--out /tmp/sa2-play-session: a session's outputs"),
    (["--player-bin", "/tmp/sa2-play-bin"], "--player-bin /tmp/sa2-play-bin: the player"),
    (["--stream-cap-gb", "0"], "--stream-cap-gb must be above 0"),
    (["--disk-floor-gb", "-1"], "--disk-floor-gb not below 0"),
])
def test_names_places_and_limits_a_session_must_not_take_are_refused(argv, why):
    with pytest.raises(SystemExit, match=re.escape(why)):
        PLV.main(["--dry-run", *argv])


def test_a_dry_run_runs_and_makes_nothing(capsys, monkeypatch):
    def refuse(*a, **k):
        raise AssertionError("a dry run started something")
    for mod, name in ((subprocess, "Popen"), (subprocess, "run"), (os, "mkfifo"), (os, "pipe")):
        monkeypatch.setattr(mod, name, refuse)
    name = f"test-dry-run-makes-nothing-{os.getpid()}"
    out = ROOT / "private" / "generated" / "s5" / "input" / name
    assert not out.exists()
    try:
        lines = dry_run(capsys, "--out", str(out.relative_to(ROOT)))
        assert f"{name}/dry-run/input.fifo" in lines["game"]
        assert not out.exists()
    finally:
        shutil.rmtree(out, ignore_errors=True)


def test_a_stage_run_without_the_input_replay_to_take_out_is_refused(monkeypatch):
    import stage_run as SR
    monkeypatch.setattr(SR, "HOOKS", [h for h in SR.HOOKS if h != PLV.INPUT_REPLAY_HOOK])
    with pytest.raises(SystemExit, match="no longer links harness/replay/wrap_input_replay.c"):
        PLV.scene_hooks("metalharbor")


def test_the_receipt_counts_the_logs_key_lines_and_keeps_its_closing_line(tmp_path):
    log = tmp_path / "input.log"
    log.write_text("# SA2_INPUT_DI_LOG of a FIFO source: ...\n1 1 0x1C\n2 3 0x2C,0x2D\n"
                   "# closed at exit: 8 keyboard polls, 4 holding keys, 2 lines; 6 records\n")
    got = PLV.log_summary(log)
    assert (got["key_lines"], got["closed"]) == (
        2, "# closed at exit: 8 keyboard polls, 4 holding keys, 2 lines; 6 records")
    assert PLV.log_summary(tmp_path / "none.log")["present"] is False


@pytest.fixture
def sessions_dir():
    """A directory for a test's sessions under private/generated/s5/input/ (play_live keeps
    its outputs under private/), removed afterwards whatever the test did."""
    base = ROOT / "private" / "generated" / "s5" / "input" / f"test-sessions-{os.getpid()}"
    shutil.rmtree(base, ignore_errors=True)
    yield base
    shutil.rmtree(base, ignore_errors=True)


def test_each_session_has_a_directory_of_its_own_and_leaves_the_earlier_ones(monkeypatch, capsys,
                                                                              sessions_dir):
    """main() twice, with the host checks and the session itself stood in: two directories
    named by run id, each with its own log and receipt, and an earlier session's files as
    they were (a session used to delete the previous one's input.log, receipt and run.json,
    around the runtime's refusal of a log that holds anything)."""
    base = sessions_dir
    earlier = base / "20260101T000000-1-00000000"
    earlier.mkdir(parents=True)
    for f, text in (("input.log", "# an earlier session\n5 9 0x1C\n"), ("play-live.json", "{}\n"),
                    ("run.json", "{}\n")):
        (earlier / f).write_text(text)
    monkeypatch.setattr(PLV.PL, "gate_passes", lambda: [])
    monkeypatch.setattr(PLV.PL, "processes", lambda match: [])
    monkeypatch.setattr(PLV.PL, "mem_available_gb", lambda: 1e6)
    monkeypatch.setattr(PLV.PL, "session", lambda: {"logind": "a test"})
    monkeypatch.setattr(PLV.PL, "session_holds", lambda s: True)
    monkeypatch.setattr(PLV.PR, "forget_hook_objects", lambda build, hooks: [])
    monkeypatch.setattr(PLV.SS, "reset_lifted", lambda cmd, root: {"left": []})
    monkeypatch.setattr(PLV, "free_bytes", lambda path: 10 ** 15)
    made = []

    def session(pl: dict) -> dict:
        p = pl["paths"]
        assert p["out"].is_dir() and not any(p["out"].glob("*.log"))   # new, and empty of logs
        p["input_log"].write_text(f"# {pl['meta']['run']}\n{len(made)} {len(made)} 0x1C\n")
        made.append(p["out"])
        return {"interrupted": False, "game_rc": 0, "player_rc": 0, "player_error": None,
                "player_cancelled": False, "stop": None, "limit": None, "seconds": {},
                "relay": {}}
    monkeypatch.setattr(PLV, "run_session", session)
    for _ in range(2):
        assert PLV.main(["--no-player-build", "--out", str(base.relative_to(ROOT))]) == 0
    capsys.readouterr()
    assert len(made) == 2 and made[0] != made[1]
    for i, d in enumerate(made):
        assert d.parent == base and re.fullmatch(r"\d{8}T\d{6}-\d+-[0-9a-f]{8}", d.name)
        assert (d / "input.log").read_text() == f"# {d.name}\n{i} {i} 0x1C\n"
        receipt = json.loads((d / "play-live.json").read_text())
        assert receipt["input"]["key_lines"] == 1 and receipt["limits"] == {
            "stream_cap_bytes": 200_000_000_000, "disk_floor_bytes": 50_000_000_000}
    assert [(earlier / f).read_text() for f in ("input.log", "play-live.json", "run.json")] == [
        "# an earlier session\n5 9 0x1C\n", "{}\n", "{}\n"]


def test_a_session_refuses_to_start_below_the_disks_floor(monkeypatch, capsys, sessions_dir):
    base = sessions_dir
    for mod, attr, value in ((PLV.PL, "gate_passes", lambda: []),
                             (PLV.PL, "processes", lambda match: []),
                             (PLV.PL, "mem_available_gb", lambda: 1e6),
                             (PLV.PL, "session", lambda: {"logind": "a test"}),
                             (PLV.PL, "session_holds", lambda s: True),
                             (PLV, "free_bytes", lambda path: 49_999_999_999)):
        monkeypatch.setattr(mod, attr, value)
    monkeypatch.setattr(PLV, "run_session", lambda pl: pytest.fail("a session started"))
    assert PLV.main(["--no-player-build", "--out", str(base.relative_to(ROOT))]) == 2
    assert (f"49999999999 bytes free under {base.relative_to(ROOT)}, below the floor of "
            "50000000000 (--disk-floor-gb)") in capsys.readouterr().err
    assert not base.exists()                             # a refused session makes nothing


HOST_FINE = ((PLV.PL, "gate_passes", lambda: []), (PLV.PL, "processes", lambda match: []),
             (PLV.PL, "mem_available_gb", lambda: 1e6),
             (PLV.PL, "session", lambda: {"logind": "a test"}),
             (PLV.PL, "session_holds", lambda s: True),
             (PLV.PR, "forget_hook_objects", lambda build, hooks: []),
             (PLV.SS, "reset_lifted", lambda cmd, root: {"left": []}),
             (PLV, "free_bytes", lambda path: 10 ** 15))


def test_a_session_refuses_to_start_beside_an_audio_scenes_run(monkeypatch, capsys,
                                                               sessions_dir):
    """play_live's guard, PL.processes(PL.is_presentation_run), sees an audio-scenes session:
    the machine's processes stood in by one argv, matched by the real predicate."""
    argv = ["/x/.venv/bin/python", "/x/tools/s5/audio_scenes.py", "all", "--replace"]
    for mod, attr, value in HOST_FINE:
        monkeypatch.setattr(mod, attr, value)
    monkeypatch.setattr(PLV.PL, "processes",
                        lambda match: [f"4242: {' '.join(argv)}"] if match(argv) else [])
    monkeypatch.setattr(PLV, "run_session", lambda pl: pytest.fail("a session started"))
    assert PLV.main(["--no-player-build", "--out", str(sessions_dir.relative_to(ROOT))]) == 2
    assert ("presentation run 4242: /x/.venv/bin/python /x/tools/s5/audio_scenes.py all "
            "--replace") in capsys.readouterr().err
    assert not sessions_dir.exists()                     # a refused session makes nothing


def stand_in_receipt_session(monkeypatch, goal: int | None, seen: dict) -> None:
    """run_session for a --receipt session, stood in: it leaves what the game, the relay and
    the player would (the game's table and poll log, the relay's arrivals, the player's
    report naming its run and the DXVK library it loaded and its key and Present logs, and a
    run record), and notes what the game would have been given."""
    def session(pl: dict) -> dict:
        p = pl["paths"]
        seen["env"] = dict(kv.split("=", 1) for kv in pl["spec"].split("|")[1:])
        seen["observer"] = p["ticks_observer"].read_text()       # there before the game
        return E.play_live_session(p, ROOT, pl["meta"]["run"], goal)
    monkeypatch.setattr(PLV, "run_session", session)


def receipt_elsewhere(monkeypatch, sessions_dir: Path, tmp_path: Path) -> Path:
    """The receipt written under tmp_path, and named below the sessions' own directory,
    which the fixture removes: where a writer that ignored dest_root would put it, rather
    than in the checkout's Documentation/. Where the receipt goes."""
    write = PLV.PRC.write
    monkeypatch.setattr(PLV.PRC, "write",
                        lambda session, root=ROOT, dest_root=None: write(session, root, tmp_path))
    named = str((sessions_dir / "receipts" / "s5-play-dxvk-native-v1.json").relative_to(ROOT))
    monkeypatch.setattr(PLV.PRC, "receipt_path", lambda host: named)
    return tmp_path / named


def step_of(d: Path) -> dict:
    """What the session's receipt step did, as play-receipt-step.json keeps it."""
    return json.loads((d / "play-receipt-step.json").read_text())


def main_receipt(sessions_dir: Path) -> int:
    return PLV.main(["--scene", "metalharbor", "--receipt", "--no-player-build",
                     "--out", str(sessions_dir.relative_to(ROOT))])


LATENCY_LINE = (f"input latency, press to presented frame: median 20.0 ms, p90 20.0, max 20.0 "
                f"over {E.PRESSES} presses in the stage (0 lost, 0 unsent, 0 unpresented; from "
                "dequeue: median 18.0 ms, p90 18.0, max 18.0)")


@pytest.mark.parametrize("goal", [300, None])
def test_a_receipt_session_ends_by_writing_the_play_receipt_or_its_own_record(
        monkeypatch, capsys, sessions_dir, tmp_path, goal):
    """main() --receipt with the host checks and the session stood in: the session's record
    names this session's files and the observer that wrote its table, and the receipt
    (written under tmp_path, not the checkout) is the same bytes, or the record says why it
    is not the receipt; the step's record and the closing lines say which, and the latency."""
    for mod, attr, value in HOST_FINE:
        monkeypatch.setattr(mod, attr, value)
    forgot = []
    monkeypatch.setattr(PLV.PR, "forget_hook_objects",
                        lambda build, hooks: forgot.append(hooks) or [])
    tracked = receipt_elsewhere(monkeypatch, sessions_dir, tmp_path)
    monkeypatch.setattr(PLV.PRC, "reading",
                        lambda host, root=ROOT: f"the clause's reading of {host}, stood in")
    seen = {}
    stand_in_receipt_session(monkeypatch, goal, seen)
    assert main_receipt(sessions_dir) == 0
    out = capsys.readouterr().out.splitlines()
    (d,) = sessions_dir.iterdir()
    rd = d.relative_to(ROOT)
    assert seen["env"]["SA2_PLAY_TICKS"] == f"{rd}/play-ticks.tsv"
    assert seen["env"]["SA2_PLAY_POLLS"] == f"{rd}/play-polls.tsv"
    assert seen["observer"] == PLV.PRC.ticks_observer()
    assert forgot == [(f"{rd}/sa2_play_ticks.c", "harness/replay/wrap_test_spawn.c",
                       "harness/stage_goal_probe.c")]
    live = json.loads((d / "play-live.json").read_text())
    observer = {"path": f"{rd}/sa2_play_ticks.c",
                "sha256": hashlib.sha256(seen["observer"].encode()).hexdigest()}
    assert live["ticks_observer"] == observer
    assert "play_receipt" not in live          # the receipt binds play-live.json as written
    played = step_of(d)
    receipt = str(tracked.relative_to(tmp_path))
    record = json.loads((d / "play-record.json").read_text())
    assert (record["run_id"], record["evidence"]["ticks"]["path"], record["renderer"]["path"],
            record["inputs"]["observer"]) == (d.name, f"{rd}/play-ticks.tsv",
                                              f"{rd}/libdxvk_d3d9.so.0", observer)
    assert record["evidence"]["session"] == {
        "path": f"{rd}/play-live.json", "sha256": hashlib.sha256(
            (d / "play-live.json").read_bytes()).hexdigest()}
    assert not (d / "play-receipt-replaced.json").exists()
    if goal:
        assert played == {"receipt": receipt, "record": f"{rd}/play-record.json",
                          "replaced": None, "entered": 101, "goal": 300,
                          "latency": E.summary(E.PRESSES),
                          "clause_reading": "the clause's reading of dxvk-native, stood in"}
        assert out[-3:] == [
            LATENCY_LINE, f"play receipt {receipt}: level entered at tick 101, the goal at tick "
            "300; the clause's reading of dxvk-native, stood in", f"receipt {rd}/play-live.json"]
        assert tracked.read_bytes() == (d / "play-record.json").read_bytes()
        assert "not_tracked" not in record and record["input_latency"] == E.summary(E.PRESSES)
    else:
        why = "the goal latch never rises: the session did not reach the goal ring"
        diagnostic = E.summary(E.PRESSES) | {"diagnostic": True, "stage_ticks": [101, 400]}
        assert played == {"receipt": None, "record": f"{rd}/play-record.json",
                          "not_tracked": [why], "latency": diagnostic}
        assert out[-3:] == [LATENCY_LINE.replace("input latency,", "input latency (diagnostic),"),
                            f"play record {rd}/play-record.json (untracked; not the play "
                            f"receipt): {why}", f"receipt {rd}/play-live.json"]
        assert record["not_tracked"] == [why] and record["input_latency"] is None
        assert record["input_latency_diagnostic"] == diagnostic
        assert not tracked.exists()


def test_a_receipt_session_that_replaces_a_receipt_says_where_it_is_kept(
        monkeypatch, capsys, sessions_dir, tmp_path):
    for mod, attr, value in HOST_FINE:
        monkeypatch.setattr(mod, attr, value)
    tracked = receipt_elsewhere(monkeypatch, sessions_dir, tmp_path)
    tracked.parent.mkdir(parents=True)
    tracked.write_text(json.dumps({"run_id": "20261003T120000-1-00000000"}))
    monkeypatch.setattr(PLV.PRC, "reading",
                        lambda host, root=ROOT: "the clause's reading, stood in")
    stand_in_receipt_session(monkeypatch, 300, {})
    assert main_receipt(sessions_dir) == 0
    (d,) = sessions_dir.iterdir()
    rd = d.relative_to(ROOT)
    assert step_of(d)["replaced"] == {
        "run_id": "20261003T120000-1-00000000", "kept": f"{rd}/play-receipt-replaced.json"}
    assert capsys.readouterr().out.splitlines()[-2] == (
        f"play receipt {tracked.relative_to(tmp_path)}: level entered at tick 101, the goal at "
        f"tick 300; it replaces run 20261003T120000-1-00000000's, kept at "
        f"{rd}/play-receipt-replaced.json; the clause's reading, stood in")
    assert json.loads((d / "play-receipt-replaced.json").read_text()) == {
        "run_id": "20261003T120000-1-00000000"}


def full(*args, **kwargs):
    raise OSError(28, "No space left on device")


def misread(*args, **kwargs):
    raise RuntimeError("the clause could not be read")


@pytest.mark.parametrize("step, fails, played, said", [
    ("write", full, {"receipt": None, "error": "OSError: [Errno 28] No space left on device"},
     "no play receipt: the receipt step failed: OSError: [Errno 28] No space left on device"),
    ("reading", misread, "written",
     "play receipt {receipt} written, then the receipt step failed: RuntimeError: the clause "
     "could not be read"),
])
def test_a_receipt_step_that_fails_is_kept_in_the_sessions_receipt_and_said(
        monkeypatch, capsys, sessions_dir, tmp_path, step, fails, played, said):
    """An error writing the record or the receipt, or reading the clause, does not take the
    session's own receipt with it: main() ends as the game did, with the error kept in the
    step's record and said."""
    for mod, attr, value in HOST_FINE:
        monkeypatch.setattr(mod, attr, value)
    tracked = receipt_elsewhere(monkeypatch, sessions_dir, tmp_path)
    monkeypatch.setattr(PLV.PRC, "reading",
                        lambda host, root=ROOT: "the clause's reading, stood in")
    monkeypatch.setattr(PLV.PRC, step, fails)
    stand_in_receipt_session(monkeypatch, 300, {})
    assert main_receipt(sessions_dir) == 0
    (d,) = sessions_dir.iterdir()
    receipt = str(tracked.relative_to(tmp_path))
    if played == "written":
        assert step_of(d) == {
            "receipt": receipt, "record": f"{d.relative_to(ROOT)}/play-record.json",
            "replaced": None, "entered": 101, "goal": 300, "latency": E.summary(E.PRESSES),
            "error": "RuntimeError: the clause could not be read"}
    else:
        assert step_of(d) == played
    assert capsys.readouterr().out.splitlines()[-2] == said.format(receipt=receipt)


def test_the_sessions_receipt_is_on_disk_before_the_receipt_step(monkeypatch, sessions_dir,
                                                                  tmp_path):
    """Even a step that does not return, as Ctrl-C there would not, leaves the session's
    own receipt."""
    for mod, attr, value in HOST_FINE:
        monkeypatch.setattr(mod, attr, value)
    receipt_elsewhere(monkeypatch, sessions_dir, tmp_path)

    def interrupted(*args, **kwargs):
        raise KeyboardInterrupt
    monkeypatch.setattr(PLV.PRC, "write", interrupted)
    stand_in_receipt_session(monkeypatch, 300, {})
    with pytest.raises(KeyboardInterrupt):
        main_receipt(sessions_dir)
    (d,) = sessions_dir.iterdir()
    live = json.loads((d / "play-live.json").read_text())
    assert live["ticks_observer"]["path"] == f"{d.relative_to(ROOT)}/sa2_play_ticks.c"
    assert "play_receipt" not in live


def test_a_session_without_receipt_writes_neither(monkeypatch, capsys, sessions_dir):
    for mod, attr, value in HOST_FINE:
        monkeypatch.setattr(mod, attr, value)
    monkeypatch.setattr(PLV.PRC, "write", lambda *a, **k: pytest.fail("a receipt was written"))

    def session(pl: dict) -> dict:
        assert "SA2_PLAY_TICKS" not in pl["spec"]
        assert not pl["paths"]["ticks_observer"].exists()
        return {"interrupted": False, "game_rc": 0, "player_rc": 0, "player_error": None,
                "player_cancelled": False, "stop": None, "limit": None, "seconds": {},
                "relay": {}}
    monkeypatch.setattr(PLV, "run_session", session)
    assert PLV.main(["--scene", "metalharbor", "--no-player-build",
                     "--out", str(sessions_dir.relative_to(ROOT))]) == 0
    (d,) = sessions_dir.iterdir()
    live = json.loads((d / "play-live.json").read_text())
    assert "play_receipt" not in live and "ticks_observer" not in live
    assert not (d / "play-record.json").exists() and not (d / "sa2_play_ticks.c").exists()
    assert not capsys.readouterr().out.splitlines()[-2].startswith("play ")


def player_audio(wav: str | None) -> dict:
    """A player report's "audio", as host/player/player.cpp audio_json writes it (the
    members play_live reads, and a few it does not)."""
    return {"asked": {"device": True, "wav": wav}, "stream_records": 41,
            "device": {"open": True, "driver": "dummy", "failure": None, "lead_ms": 60,
                       "blocks_queued": 1330, "blocks_dropped": 3, "shortfalls": 2},
            "wav": {"path": wav, "frames": 1068000} if wav else None,
            "problems": {"count": 0, "first": []}}


COUNTS_SAID = ("sound: 41 audio records; SDL dummy driver; 1330 blocks queued, 3 dropped on a "
               "full queue, 2 device shortfalls; 0 mixer problems")


PLAYER_PACE = {"live": True, "frames": 3, "virtual_s": 0.033332, "wall_s": 0.034,
               "virtual_per_wall": 0.980353, "late": {"over_frame": 0, "max_ms": 0.25},
               "moves": 0, "moved_s": 0.0}
# The player's pace log and the relay's arrivals, both on CLOCK_MONOTONIC: at Present 1
# the relay has frames 1 and 2 whole (one ahead), at 2 and 3 none beyond.
PACE_TSV = ("frame\tvirtual_us\tready_s\tdue_s\tpresent_s\tmoved_ms\tqueue_ms\n"
            "1\t83330\t9.999000\t10.000000\t10.000000\t0.000\t60.000\n"
            "2\t99996\t10.010000\t10.016666\t10.017000\t0.000\t65.000\n"
            "3\t116662\t10.030000\t10.033332\t10.034000\t0.000\t62.000\n")
RELAY_TSV = "frame\tarrived_s\n1\t9.990000\n2\t9.995000\n3\t10.020000\n"
PACE_SAID = ("pace: 0.980353 virtual s per wall s over 3 frames (0.033332 s in 0.034 s); 0 frames "
             "presented over a frame late, the latest by 0.25 ms; the schedule moved 0 times, "
             "0.0 s in all; the relay held the game 2 times, 0.021 s in all; frames whole at the "
             "relay beyond the one presented: median 0, at most 1")


@pytest.mark.parametrize("argv, on, driver, wav, said, paced", [
    ([], True, None, False, COUNTS_SAID, True),
    (["--sound-driver", "dummy", "--audio-wav"], True, "dummy", True,
     COUNTS_SAID + "; WAV {wav}, 1068000 samples", True),
    (["--no-sound"], False, None, False, "sound: none (--no-sound)", True),
    (["--no-pace"], True, None, False, COUNTS_SAID, False),
])
def test_the_receipt_keeps_the_sound_and_pace_asked_for_and_the_players_account_of_them(
        monkeypatch, capsys, sessions_dir, argv, on, driver, wav, said, paced):
    """main() with the host checks and the session stood in, the session leaving a player
    report, its pace log and the relay's arrivals: the receipt's "sound" and "pace" are what
    was asked, its player.audio and player.pace the report's, its pace.ahead counted from
    the two logs, and the closing lines read them."""
    base = sessions_dir
    for mod, attr, value in ((PLV.PL, "gate_passes", lambda: []),
                             (PLV.PL, "processes", lambda match: []),
                             (PLV.PL, "mem_available_gb", lambda: 1e6),
                             (PLV.PL, "session", lambda: {"logind": "a test"}),
                             (PLV.PL, "session_holds", lambda s: True),
                             (PLV.PR, "forget_hook_objects", lambda build, hooks: []),
                             (PLV.SS, "reset_lifted", lambda cmd, root: {"left": []}),
                             (PLV, "free_bytes", lambda path: 10 ** 15)):
        monkeypatch.setattr(mod, attr, value)
    seen = {}
    held = {"lead_frames": 1, "pauses": 2, "paused_s": 0.021}

    def session(pl: dict) -> dict:
        cmd = seen["player_cmd"] = pl["player_cmd"]
        audio = player_audio(cmd[cmd.index("--audio-wav") + 1] if "--audio-wav" in cmd else None)
        pl["paths"]["player"]["report"].write_text(json.dumps(
            {"pass": False, "exit_status": 4, **({"audio": audio} if "--audio" in cmd else {}),
             **({"pace": PLAYER_PACE} if "--pace" in cmd else {})}))
        if "--pace" in cmd:
            (ROOT / cmd[cmd.index("--pace-log") + 1]).write_text(PACE_TSV)
        pl["paths"]["relay_frames"].write_text(RELAY_TSV)
        return {"interrupted": False, "game_rc": 0, "player_rc": 4, "player_error": None,
                "player_cancelled": False, "stop": None, "limit": None, "seconds": {},
                "relay": {"held": held if pl["pace"]["on"] else None}}
    monkeypatch.setattr(PLV, "run_session", session)
    assert PLV.main(["--no-player-build", "--out", str(base.relative_to(ROOT)), *argv]) == 0
    out = capsys.readouterr().out.splitlines()
    (d,) = base.iterdir()
    receipt = json.loads((d / "play-live.json").read_text())
    rd = d.relative_to(ROOT)
    path, pace_log = f"{rd}/player/sound.wav", f"{rd}/player/pace.tsv"
    assert receipt["sound"] == {"on": on, "driver": driver,
                                "driver_from": "--sound-driver" if driver else None,
                                "wav": path if wav else None}
    assert receipt["player"]["audio"] == (player_audio(path if wav else None) if on else None)
    assert out[-3] == said.format(wav=path)
    assert receipt["commands"]["player"] == seen["player_cmd"]
    report = f"{rd}/player/report.json"
    assert seen["player_cmd"][-3:] == (["--audio", "--audio-wav", path] if wav else
                                       ["--pace-log", pace_log, "--audio"] if on and paced else
                                       ["--report", report, "--audio"] if on else
                                       ["--pace", "--pace-log", pace_log])
    if paced:
        assert receipt["pace"] == {
            "on": True, "lead_frames": 1, "relay_held": held, "pace_log": pace_log,
            "relay_frames": f"{rd}/relay-frames.tsv",
            "ahead": {"presents": 3, "relay_frames": 3, "p50": 0, "p90": 1, "max": 1,
                      "mean": 0.333}}
        assert receipt["player"]["pace"] == PLAYER_PACE
        assert out[-2] == PACE_SAID
    else:
        assert receipt["pace"] == {"on": False, "lead_frames": None, "relay_held": None,
                                   "ahead": None, "pace_log": None,
                                   "relay_frames": f"{rd}/relay-frames.tsv"}
        assert out[-2] == "pace: none (--no-pace)"


def test_the_pace_line_says_when_the_player_reported_no_pace():
    on = {"on": True, "lead_frames": 1}
    assert PLV.pace_said(on, None, None, None) == "pace: the player's report has no pace account"
    assert PLV.pace_said(on, {"pace": PLAYER_PACE}, None, None) == PACE_SAID.split(
        "; the relay held")[0]


def test_frames_ahead_counts_what_the_relay_had_whole_at_each_present(tmp_path):
    """At each Present, the relay's arrivals up to it less the frame's number; nothing when
    either log is missing or empty."""
    pace, relay = tmp_path / "pace.tsv", tmp_path / "relay.tsv"
    pace.write_text(PACE_TSV)
    relay.write_text("frame\tarrived_s\n1\t9.0\n2\t9.1\n3\t9.2\n4\t9.3\n5\t10.0\n6\t10.5\n")
    assert PLV.frames_ahead(pace, relay) == {"presents": 3, "relay_frames": 6, "p50": 3,
                                             "p90": 4, "max": 4, "mean": 3.0}   # 4, 3, 2
    assert PLV.frames_ahead(tmp_path / "none.tsv", relay) is None
    relay.write_text("frame\tarrived_s\n")
    assert PLV.frames_ahead(pace, relay) is None


def test_the_closing_line_says_when_the_player_reported_no_sound(capsys):
    on = {"on": True, "driver": None, "wav": None}
    assert PLV.sound_said(on, None) == "sound: the player's report has no audio account"
    assert PLV.sound_said(on, {"audio": {"stream_records": 0, "device": {
        "open": False, "driver": "alsa", "failure": "SDL_OpenAudioDeviceStream: no device",
        "blocks_queued": 0, "blocks_dropped": 0, "shortfalls": 0},
        "problems": {"count": 1}, "wav": None}}) == (
        "sound: 0 audio records; SDL alsa driver, failed: SDL_OpenAudioDeviceStream: no device; "
        "0 blocks queued, 0 dropped on a full queue, 0 device shortfalls; 1 mixer problems")


# ---------------------------------------------------------------- stopping the game

STAND_IN = r"""
import signal, sys, time
mode = sys.argv[1]
if mode == "quits":          # leaves its loop at the first SIGTERM
    signal.signal(signal.SIGTERM, lambda *a: sys.exit(0))
elif mode == "resets":       # the runtime's SA_RESETHAND: the first only asks
    signal.signal(signal.SIGTERM, lambda *a: signal.signal(signal.SIGTERM, signal.SIG_DFL))
elif mode == "ignores":
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
print("ready", flush=True)
while True:
    time.sleep(0.05)
"""


def stand_in_session(tmp_path: Path, mode: str):
    """A loop_run stand-in whose one child is a 'game' answering SIGTERM as `mode` says;
    loop_run's process, the game's pid, and an event set when loop_run ends."""
    child = tmp_path / "game.py"
    child.write_text(STAND_IN)
    loop = subprocess.Popen([sys.executable, "-c", "import subprocess, sys; "
                             f"subprocess.run([sys.executable, {str(child)!r}, {mode!r}])"],
                            stdout=subprocess.PIPE, text=True, start_new_session=True)
    ready = loop.stdout.readline()
    loop.stdout.close()
    assert ready == "ready\n"                           # its handler is in place
    ended = threading.Event()
    threading.Thread(target=lambda: (loop.wait(), ended.set()), daemon=True).start()
    binary = Path(sys.executable)
    pid = PLV.game_pid(loop.pid, binary)
    assert pid and pid != loop.pid
    return loop, pid, ended, binary


@pytest.mark.parametrize("mode, how, signalled", [
    ("quits", "SIGTERM: WM_QUIT at the game's next keyboard poll", 1),
    ("resets", "a second SIGTERM", 2),
    ("ignores", "SIGKILL to loop_run's group", 2),
])
def test_the_game_is_asked_once_then_ended(tmp_path, monkeypatch, mode, how, signalled):
    monkeypatch.setattr(PLV, "STOP_GRACE", 1.5)
    monkeypatch.setattr(PLV, "KILL_GRACE", 1.5)
    loop, pid, ended, binary = stand_in_session(tmp_path, mode)
    try:
        got = PLV.stop_game(loop, binary, ended)
    finally:
        if loop.poll() is None:
            os.killpg(loop.pid, signal.SIGKILL)
    assert (got["how"], got["pid"], got["signalled"]) == (how, pid, [pid] * signalled)
    assert ended.is_set()


def test_the_second_sigterm_goes_to_the_game_found_again_not_the_first_pid(monkeypatch):
    """After STOP_GRACE the first pid may be gone and another process's: the game is looked
    up again, and a game no longer there is sent nothing (no process is signalled here)."""
    found = iter([4242, None])                    # the game, then no game under loop_run
    monkeypatch.setattr(PLV, "game_pid", lambda loop_pid, binary: next(found))
    sent = []
    monkeypatch.setattr(os, "kill", lambda pid, sig: sent.append((pid, sig)))
    monkeypatch.setattr(PLV, "kill_group", lambda loop: sent.append(("group", loop.pid)))
    monkeypatch.setattr(PLV, "STOP_GRACE", 0.01)
    monkeypatch.setattr(PLV, "KILL_GRACE", 0.01)
    loop = type("Loop", (), {"pid": 77})()
    ended = threading.Event()
    killed = threading.Timer(0.3, ended.set)    # loop_run ends after its group is killed
    killed.start()
    got = PLV.stop_game(loop, Path("/bin/true"), ended)
    killed.cancel()
    assert sent == [(4242, signal.SIGTERM), ("group", 77)]
    assert (got["how"], got["pid"], got["signalled"]) == ("SIGKILL to loop_run's group", 4242, [4242])


def test_the_game_is_found_among_loop_runs_children_by_its_binary(tmp_path):
    loop, pid, ended, binary = stand_in_session(tmp_path, "quits")
    try:
        assert PLV.game_pid(loop.pid, binary) == pid
        assert PLV.game_pid(loop.pid, Path("/bin/true")) is None   # another binary
        assert PLV.game_pid(pid, binary) is None                    # the game has no such child
    finally:
        os.killpg(loop.pid, signal.SIGKILL)


GAME = r"""
import os, signal, sys, time
stream, keys, out = sys.argv[1:4]
stop = []
signal.signal(signal.SIGTERM, lambda *a: stop.append(1))
w = os.open(stream, os.O_WRONLY)                    # the relay's reader is already there
k = os.open(keys, os.O_RDONLY | os.O_NONBLOCK)
os.write(w, b"SA2D3D9S" + bytes(8))
got = b""
deadline = time.monotonic() + 30
while time.monotonic() < deadline:                  # one 'keyboard poll' every 20 ms
    try:
        got += os.read(k, 4096)
    except BlockingIOError:                         # the runner holds a writer: never EOF
        pass
    if stop:                                        # the poll after SIGTERM reads, then quits
        break
    time.sleep(0.02)
open(out, "wb").write(got)
os.close(w)
sys.exit(0 if stop else 3)
"""

PLAYER = r"""
import os, struct, sys
def script(d):                                      # argv[1]: the script a python runs
    try:
        return (open(f"/proc/{d}/cmdline", "rb").read().split(b"\0") + [b"", b""])[1]
    except OSError:
        return b""
game = os.environ["PLAY_TEST_GAME"].encode()
if not any(script(d) == game for d in os.listdir("/proc") if d.isdigit()):
    sys.exit(4)                                     # started before the game was running
a = sys.argv
video, path = int(a[a.index("--video-fd") + 1]), a[a.index("--input-path") + 1]
first = os.read(video, 16)                          # the game's bytes, through the relay
fd = os.open(path, os.O_WRONLY | os.O_NONBLOCK)     # the runner holds a reader: no ENXIO
keys = bytearray(256)
keys[0x1C] = 0x80
os.write(fd, struct.pack("<II", 0x4B324153, 0) + bytes(keys))
open(os.environ["PLAY_TEST_SEEN"], "wb").write(first)
sys.exit(0)                                         # the window closed
"""

# A game that streams until it is asked to stop, and a player that reads to the end.
GAME_STREAMS = r"""
import os, signal, sys, time
stop = []
signal.signal(signal.SIGTERM, lambda *a: stop.append(1))
w = os.open(sys.argv[1], os.O_WRONLY)
os.write(w, b"SA2D3D9S" + bytes(8))
deadline = time.monotonic() + 20
while not stop and time.monotonic() < deadline:
    os.write(w, bytes(4096))
    time.sleep(0.01)
os.close(w)
sys.exit(0 if stop else 3)
"""

PLAYER_READS = r"""
import os, sys
a = sys.argv
video = int(a[a.index("--video-fd") + 1])
n = 0
while True:
    b = os.read(video, 65536)
    if not b:
        break
    n += len(b)
open(os.environ["PLAY_TEST_SEEN"], "w").write(str(n))
"""


def free_descriptors(n: int) -> list[int]:
    """`n` descriptor numbers this process does not use, so run_session's inherited pair
    never lands on one pytest or a plugin holds."""
    got, fd = [], 100
    while len(got) < n:
        if PLV.free_fd(fd):
            got.append(fd)
        fd += 1
    return got


def stand_in_plan(tmp_path: Path, monkeypatch, loop: str, player: str, env: dict,
                  limits: dict | None = None, paced: bool = False,
                  receipt: dict | None = None, synthetic: dict | None = None) -> dict:
    video, cancel, end = free_descriptors(3)
    monkeypatch.setattr(PLV, "VIDEO_FD", video)
    monkeypatch.setattr(PLV, "CANCEL_FD", cancel)
    monkeypatch.setattr(PLV, "END_FD", end)
    p = PLV.paths(tmp_path)
    p["player"]["dir"].mkdir()
    (tmp_path / "player.py").write_text(player)
    return {"paths": p, "build": f"test-play-{os.getpid()}", "binary": Path(sys.executable),
            "cmd": [sys.executable, "-c", loop],
            # No --end-fd: these stand-ins do not answer play_live's end request (SIGUSR1),
            # so the session stops the game itself (test_s5_play_live_session.py's do).
            "player_cmd": [sys.executable, str(tmp_path / "player.py"), "--video-fd", str(video),
                           "--cancel-fd", str(cancel), "--input-path", str(p["input_fifo"])],
            "player_env": dict(os.environ, **env),
            "limits": limits or {"stream_cap_bytes": 10 ** 15, "disk_floor_bytes": 0},
            "pace": {"on": paced, "lead_frames": PLV.LEAD_FRAMES if paced else None},
            "receipt": receipt or {"on": False}, "synthetic": synthetic}


def test_a_session_runs_the_player_with_the_game_and_ends_the_game_when_it_closes(tmp_path,
                                                                                   monkeypatch):
    """run_session with stand-ins for loop_run, the game and the player: the player starts
    once the game's binary runs (not during the stand-in's half-second 'link'), reads the
    stream the relay hands it, writes a key record the game reads through the held FIFO, and
    closes; the game is asked once and leaves."""
    (tmp_path / "game.py").write_text(GAME)
    keys_seen, video_seen = tmp_path / "keys.bin", tmp_path / "video.bin"
    p = PLV.paths(tmp_path)
    loop = (f"import subprocess, sys, time; time.sleep(0.5); "    # the link, before the game
            f"sys.exit(subprocess.run([sys.executable, "
            f"{str(tmp_path / 'game.py')!r}, {str(p['game_fifo'])!r}, {str(p['input_fifo'])!r}, "
            f"{str(keys_seen)!r}]).returncode)")
    pl = stand_in_plan(tmp_path, monkeypatch, loop, PLAYER,
                       {"PLAY_TEST_SEEN": str(video_seen), "PLAY_TEST_GAME": str(tmp_path / "game.py")})
    got = PLV.run_session(pl)
    assert got["stop"]["how"] == "SIGTERM: WM_QUIT at the game's next keyboard poll"
    assert (got["game_rc"], got["player_rc"], got["interrupted"], got["player_cancelled"],
            got["limit"]) == (0, 0, False, False, None)
    assert got["relay"]["end"] == "the writer closed the pipe" and got["relay"]["bytes"] == 16
    assert video_seen.read_bytes() == b"SA2D3D9S" + bytes(8)
    assert keys_seen.read_bytes() == b"SA2K" + bytes(4) + bytes(0x1C) + b"\x80" + bytes(0xE3)
    s = got["seconds"]
    assert s["game_started"] <= s["player_started"] <= s["player_ended"] <= s["game_ended"]


@pytest.mark.parametrize("limits, why", [
    ({"stream_cap_bytes": 65536, "disk_floor_bytes": 0},
     "the stream copy passed 65536 bytes (--stream-cap-gb)"),
    ({"stream_cap_bytes": 10 ** 15, "disk_floor_bytes": 10 ** 15},
     "the free space fell below 1000000000000000 bytes (--disk-floor-gb)"),
])
def test_the_disk_ends_a_session_whose_stream_copy_passes_its_cap_or_whose_disk_runs_low(
        tmp_path, monkeypatch, limits, why):
    """A game that streams until asked to stop and a player that reads to the end: the
    watchdog asks the game, as a closed window does, and the receipt says why."""
    (tmp_path / "game.py").write_text(GAME_STREAMS)
    seen = tmp_path / "seen.txt"
    p = PLV.paths(tmp_path)
    loop = (f"import subprocess, sys; sys.exit(subprocess.run([sys.executable, "
            f"{str(tmp_path / 'game.py')!r}, {str(p['game_fifo'])!r}]).returncode)")
    pl = stand_in_plan(tmp_path, monkeypatch, loop, PLAYER_READS, {"PLAY_TEST_SEEN": str(seen)},
                       limits)
    got = PLV.run_session(pl)
    assert got["limit"]["why"] == why
    assert got["stop"]["how"] == "SIGTERM: WM_QUIT at the game's next keyboard poll"
    assert (got["game_rc"], got["player_rc"], got["interrupted"]) == (0, 0, False)
    assert got["relay"]["end"] == "the writer closed the pipe"
    assert int(seen.read_text()) == got["relay"]["bytes"] == p["copy"].stat().st_size
    if limits["stream_cap_bytes"] == 65536:
        assert got["limit"]["copy_bytes"] > 65536
    assert got["relay"]["bytes"] < 4096 * 2000          # well before the game's own deadline


# ---------------------------------------------------------------- the held relay

def framed_stream(n: int, payload: int) -> tuple[bytes, bytes, bytes]:
    """A stream in host/sa2_stream.h's framing: its header, `n` frames of one CALL record of
    `payload` bytes and its FRAME, and an END."""
    head = json.dumps({"schema": "sa2-d3d9-stream/v1"}).encode()
    hb = 16 + ((len(head) + 3) & ~3)
    header = PLV.PL.MAGIC + struct.pack("<II", 1, hb) + head + b" " * (hb - 16 - len(head))
    frames = b"".join(struct.pack("<III", 1, 12 + payload, 2 * k) + bytes([k % 251]) * payload
                      + struct.pack("<III", PLV.PL.KIND_FRAME, 28, 2 * k + 1)
                      + struct.pack("<QII", 16666 * (k + 1), k + 1, 0) for k in range(n))
    return header, frames, struct.pack("<III", 5, 12, 2 * n)


# The game: the header, then each frame in one write as the recorder writes it at its
# Present, as fast as the pipe takes them (or `gap` seconds apart: a game behind its
# player; for the first argv[8] frames only, when given), noting when each write returned;
# then END.
FRAME_WRITER = r"""
import os, sys, time
fifo, header, frames, n, out = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), sys.argv[5]
gap = float(sys.argv[7])
gap_frames = int(sys.argv[8]) if len(sys.argv) > 8 else n
def write_all(fd, data):
    view = memoryview(data)
    while view:
        view = view[os.write(fd, view):]
h, f = open(header, "rb").read(), open(frames, "rb").read()
size = len(f) // n
fd = os.open(fifo, os.O_WRONLY)
write_all(fd, h)
done = []
for k in range(n):
    if gap and k < gap_frames:
        time.sleep(gap)
    write_all(fd, f[k * size:(k + 1) * size])
    done.append(time.monotonic())
write_all(fd, open(sys.argv[6], "rb").read())
os.close(fd)
open(out, "w").write("\n".join(map(repr, done)))
"""

# The player: reads the stream from stdin, and at each FRAME record notes when and then
# waits `pause` seconds (a frame's time, paced); after `quit_after` frames (if not 0) it
# goes away, as a closed window does.
FRAME_READER = r"""
import hashlib, os, struct, sys, time
out, pause, quit_after = sys.argv[1], float(sys.argv[2]), int(sys.argv[3])
buf, pos, h, n, seen, header = b"", 0, hashlib.sha256(), 0, [], None
while True:
    b = os.read(0, 1 << 16)
    if not b:
        break
    h.update(b); n += len(b); buf += b
    if header is None and len(buf) >= 16:
        header = (struct.unpack_from("<I", buf, 12)[0] + 3) & ~3
        pos = header
    while header is not None and pos + 12 <= len(buf):
        kind, size, _ = struct.unpack_from("<III", buf, pos)
        if pos + size > len(buf):
            break
        pos += size
        if kind == 3:
            seen.append(time.monotonic())
            if quit_after and len(seen) == quit_after:
                open(out, "w").write(f"{n} quit\n" + "\n".join(map(repr, seen)))
                sys.exit(0)
            time.sleep(pause)
open(out, "w").write(f"{n} {h.hexdigest()}\n" + "\n".join(map(repr, seen)))
"""


def held_relay_run(tmp_path: Path, lead: int | None, n: int = 24, payload: int = 60_000,
                   pause: float = 0.03, quit_after: int = 0, gap: float = 0.0,
                   gap_frames: int | None = None):
    """present_live.relay over a real pipe, between FRAME_WRITER and FRAME_READER, held to
    `lead` whole frames (None: not held), with the player's pipe play_live gives a paced
    player. What the relay said, when each frame's write returned, when the reader saw each
    frame, what it read, and the stream."""
    header, frames, end = framed_stream(n, payload)
    for name, data in (("header.bin", header), ("frames.bin", frames), ("end.bin", end)):
        (tmp_path / name).write_bytes(data)
    fifo, copy_ = tmp_path / "s.fifo", tmp_path / "copy.bin"
    wrote, read = tmp_path / "wrote.txt", tmp_path / "read.txt"
    r, sink = os.pipe()
    fcntl.fcntl(sink, PLV.PL.F_SETPIPE_SZ, PLV.PLAYER_PIPE_BYTES)
    reader = subprocess.Popen([sys.executable, "-c", FRAME_READER, str(read), str(pause),
                               str(quit_after)], stdin=r)
    os.close(r)
    box: dict = {}

    def go():
        box["fd"] = PLV.PL.open_pipe(fifo)
        box["st"] = PLV.PL.relay(box["fd"], copy_, sink,
                                 lambda: "writer" not in box or box["writer"].poll() is None,
                                 grace=30, lead_frames=lead)

    th = threading.Thread(target=go, daemon=True)
    th.start()
    t = time.monotonic()
    while not fifo.exists() and time.monotonic() - t < 10:
        time.sleep(0.001)
    writer = subprocess.Popen([sys.executable, "-c", FRAME_WRITER, str(fifo),
                               str(tmp_path / "header.bin"), str(tmp_path / "frames.bin"), str(n),
                               str(wrote), str(tmp_path / "end.bin"), str(gap),
                               str(n if gap_frames is None else gap_frames)])
    box["writer"] = writer
    th.join(timeout=60)
    if th.is_alive():
        writer.kill()
        reader.kill()
        pytest.fail("the relay did not end within 60 s")
    os.close(box["fd"])
    assert writer.wait(timeout=30) == 0 and reader.wait(timeout=30) == 0
    done = [float(x) for x in wrote.read_text().split()]
    head, *seen = read.read_text().splitlines()
    return box["st"], done, [float(x) for x in seen], head.split(), header + frames + end


def frames_written_by(done: list[float], seen: list[float]) -> list[int]:
    """At the moment the reader saw each frame k, how many frames beyond k the writer had
    written in full."""
    return [sum(1 for d in done if d <= s) - k for k, s in enumerate(seen, 1)]


def test_a_held_relay_holds_a_writer_ahead_of_its_reader_and_delivers_every_byte(tmp_path):
    """A writer ahead of a reader that takes 30 ms a frame: held to one whole frame from
    the first, the writer has written at most one frame beyond the one the reader sees
    (the one the relay holds), so its writes take the reader's time; every byte reaches
    the copy and the reader, in order. The writer is taken as waiting until whole frames
    come where a pause could have found it so and none did, so its pipe stays the least
    from the first frame on (the first frame, which no pause can come before, once widened
    the hold and the pipe, and the writer then ran up to three frames ahead at the start)."""
    st, done, seen, (n_read, digest), stream = held_relay_run(tmp_path, lead=1)
    assert (tmp_path / "copy.bin").read_bytes() == stream
    assert (int(n_read), digest) == (len(stream), hashlib.sha256(stream).hexdigest())
    assert st["end"] == "the writer closed the pipe" and st["frames_arrived"] == 24
    ahead = frames_written_by(done, seen)
    assert len(seen) == 24 and max(ahead) <= 1
    assert done[-1] - done[0] >= 21 * 0.03          # the writer went at the reader's pace
    held = st["held"]
    assert held["lead_frames"] == 1 and held["pauses"] >= 20 and held["writer_waited"] >= 10
    assert (held["pipe_max"], held["max_unfed_frames"]) == (16384, 1)   # never widened


def test_an_unheld_relay_lets_the_writer_run_ahead(tmp_path):
    """The same without lead_frames, present_live's relay: the writer is done before the
    reader has seen its second frame, and the relay says nothing of holding."""
    st, done, seen, (n_read, digest), stream = held_relay_run(tmp_path, lead=None)
    assert (int(n_read), digest) == (len(stream), hashlib.sha256(stream).hexdigest())
    assert frames_written_by(done, seen)[1] == 24 - 2
    assert "held" not in st


def test_a_held_relay_does_not_stop_a_writer_behind_its_reader_at_every_frame(tmp_path):
    """A writer 20 ms a frame and a reader 5 ms a frame (a game behind real time): found not
    waiting at its first two whole frames where a pause could find it (present_live's
    QUIET_FRAMES), the relay then holds at two whole frames, which the reader never leaves
    waiting, so it pauses at most a few times and the writer keeps its own pace."""
    st, done, seen, (n_read, digest), stream = held_relay_run(tmp_path, lead=1, pause=0.005,
                                                              gap=0.02)
    assert (int(n_read), digest) == (len(stream), hashlib.sha256(stream).hexdigest())
    assert st["held"]["pauses"] <= 3 and st["held"]["writer_waiting"] is False
    assert done[-1] - done[0] < 23 * 0.02 + 0.25


def test_a_held_relay_holds_a_writer_again_once_it_is_ahead_again(tmp_path):
    """A writer 50 ms a frame for its first four frames, behind a reader that takes 30 ms a
    frame, then as fast as it can: taken as not waiting at its second quiet frame (the hold
    one frame wider, the pipe grown to hold a whole frame), then as waiting again at the
    first pause that finds its pipe full, and held from there: over the last frames at most
    two frames ahead (the one held, and one the grown pipe holds while the shrink the relay
    wants is refused, the waiting writer refilling the pipe as soon as it is read)."""
    st, done, seen, (n_read, digest), stream = held_relay_run(tmp_path, lead=1, gap=0.05,
                                                              gap_frames=4)
    assert (int(n_read), digest) == (len(stream), hashlib.sha256(stream).hexdigest())
    held = st["held"]
    assert held["pipe_max"] == 65536 and held["writer_waiting"] is True
    assert held["writer_waited"] >= 10
    assert max(frames_written_by(done, seen)[10:]) <= 2


def test_a_held_relay_holds_nothing_once_its_reader_goes_away(tmp_path):
    """The player closes after 5 frames: the feeder ends (a broken pipe), and the writer,
    held until then, writes the rest at once into a copy that is still whole."""
    st, done, seen, (n_read, how), stream = held_relay_run(tmp_path, lead=1, quit_after=5)
    assert how == "quit" and len(seen) == 5
    assert (tmp_path / "copy.bin").read_bytes() == stream and st["bytes"] == len(stream)
    assert "Broken pipe" in (st["feed"]["error"] or "")
    assert done[-1] - done[5] < 0.5                  # 18 frames at once, not 18 x 30 ms


def test_a_held_relay_holds_nothing_once_its_writer_goes_away(tmp_path):
    """A player that reads once and then stops reading, alive, holds the relay and the
    writer; the writer then dies in its held write. The relay lets go, takes what was left
    in the pipe into the copy and returns, its feeder still held by the player (a session
    then gives the player its grace and cancels it), instead of holding for good."""
    header, frames, end = framed_stream(24, 60_000)
    stream = header + frames + end
    for name, data in (("header.bin", header), ("frames.bin", frames), ("end.bin", end)):
        (tmp_path / name).write_bytes(data)
    fifo, copy_ = tmp_path / "s.fifo", tmp_path / "copy.bin"
    r, sink = os.pipe()
    fcntl.fcntl(sink, PLV.PL.F_SETPIPE_SZ, PLV.PLAYER_PIPE_BYTES)
    reader = subprocess.Popen([sys.executable, "-c",
                               "import os, time; os.read(0, 4096); time.sleep(60)"], stdin=r)
    os.close(r)
    box: dict = {}

    def go():
        box["fd"] = PLV.PL.open_pipe(fifo)
        box["st"] = PLV.PL.relay(box["fd"], copy_, sink,
                                 lambda: "writer" not in box or box["writer"].poll() is None,
                                 grace=0.5, lead_frames=1)
        box["returned"] = time.monotonic()

    th = threading.Thread(target=go, daemon=True)
    th.start()
    t = time.monotonic()
    while not fifo.exists() and time.monotonic() - t < 10:
        time.sleep(0.001)
    writer = box["writer"] = subprocess.Popen(
        [sys.executable, "-c", FRAME_WRITER, str(fifo), str(tmp_path / "header.bin"),
         str(tmp_path / "frames.bin"), "24", str(tmp_path / "wrote.txt"),
         str(tmp_path / "end.bin"), "0"])
    try:
        time.sleep(1.0)                              # held: the player reads no more
        assert th.is_alive() and writer.poll() is None
        writer.kill()
        killed = time.monotonic()
        th.join(timeout=20)
        assert not th.is_alive(), "the relay still held a writer that was gone"
    finally:
        writer.kill()
        reader.kill()                                # the feeder's write fails, and it ends
        reader.wait()
        th.join(timeout=10)
        if "st" in box:                              # it closes `sink`: not after this test
            box["st"]["feed_thread"].join(timeout=10)
        if "fd" in box:
            os.close(box["fd"])
    st = box["st"]
    assert not st["feed_thread"].is_alive()
    assert box["returned"] - killed < 0.5 + 5       # the feeder's grace, and some slack
    assert st["end"] == "the writer closed the pipe" and st["held"]["pauses"] >= 1
    assert st["feed_ended"] is False                 # the player still held the feeder
    copied = copy_.read_bytes()
    assert len(copied) == st["bytes"] and copied == stream[:len(copied)]
    assert st["frames_arrived"] < 24                 # it was held: the writer never finished


@pytest.mark.parametrize("sizes, waiting, want", [
    ([800_000] * 8, False, 1 << 20),     # Metal Harbor, the game behind: a whole frame
    ([800_000] * 8, True, 256 << 10),    # ... held: at most half the median frame
    ([25_000] * 8, False, 32 << 10),     # the logos' small frames
    ([25_000] * 8, True, 16 << 10),      # ... held: the least
    ([25_000] * 7 + [3_000_000], False, 1 << 20),   # the largest, up to CHUNK
    ([25_000] * 7 + [3_000_000], True, 16 << 10),   # the median, not one large frame
    ([], False, 16 << 10),
])
def test_the_pipe_is_sized_to_the_frames_and_to_whether_the_writer_waits(sizes, waiting, want):
    assert PLV.PL.pipe_size_for(sizes, waiting) == want


def test_the_writer_is_taken_as_not_waiting_only_at_the_second_quiet_frame_a_pause_could_find():
    """fit_pipe's account of the writer at each whole frame (the receipt's relay_held), over
    a real pipe and 60 kB frames: waiting from the start; a frame no pause could come before
    (the first) says nothing; one quiet frame is not enough (a short pause on a busy machine
    finds even a writer that is ahead not yet waiting); a pause finding the pipe full makes
    it waiting at once and starts the count again; at the second quiet frame running it is
    not waiting, and the pipe grows to hold a whole frame."""
    r, w = os.pipe()
    clock = PLV.PL.FrameClock()
    held = {"writer_waiting": True, "waited_this_frame": False, "quiet_frames": 0,
            "pipe_bytes": None, "want_bytes": None, "pipe_min": None, "pipe_max": None,
            "resizes": 0, "resize_refused": 0}

    def frame(could_hold: bool, full_pipe_at_a_pause: bool = False):
        clock.ends.append(60_000 * (len(clock.ends) + 1))
        if full_pipe_at_a_pause:                     # as read_pipe's pause takes it
            held["writer_waiting"] = held["waited_this_frame"] = True
        PLV.PL.fit_pipe(r, clock, held, could_hold)
        return held["writer_waiting"], held["pipe_bytes"]

    try:
        assert frame(could_hold=False) == (True, 16384)
        assert frame(could_hold=True) == (True, 16384)
        assert frame(could_hold=True, full_pipe_at_a_pause=True) == (True, 16384)
        assert frame(could_hold=True) == (True, 16384)
        assert frame(could_hold=False) == (True, 16384)     # not counted, not reset
        assert frame(could_hold=True) == (False, 65536)
        assert frame(could_hold=True, full_pipe_at_a_pause=True)[0] is True
    finally:
        os.close(r)
        os.close(w)


@pytest.mark.parametrize("n, waiting, quiet", [(2, True, 1), (3, False, 2)])
def test_no_pause_before_the_first_whole_frame_says_nothing_of_the_writer(tmp_path, n, waiting,
                                                                          quiet):
    """read_pipe held to one frame, its frames written one at a time into a real pipe, with
    a feeder that has handed everything on, so no pause ever comes: the first whole frame
    came before one could and is not counted, so two frames leave the writer taken as
    waiting (one quiet frame), and only a third makes two quiet frames running."""
    header, frames, end = framed_stream(n, 100)
    size = len(frames) // n
    r, w = os.pipe()
    copy_fd = os.open(tmp_path / "copy.bin", os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
    spool, clock = PLV.PL.Spool(), PLV.PL.FrameClock()
    spool.taken = 1 << 40                            # everything handed on: no pause comes
    box: dict = {}
    th = threading.Thread(target=lambda: box.update(st=PLV.PL.read_pipe(
        r, copy_fd, spool, clock, lambda: True, poll_ms=20, lead_frames=1)), daemon=True)
    th.start()
    try:
        os.write(w, header)
        for k in range(n):                           # each frame whole before the next
            os.write(w, frames[k * size:(k + 1) * size])
            t = time.monotonic()
            while len(clock.ends) <= k and time.monotonic() - t < 10:
                time.sleep(0.001)
        os.write(w, end)
    finally:
        os.close(w)
        th.join(timeout=10)
        os.close(r)
        os.close(copy_fd)
    assert not th.is_alive() and len(clock.ends) == n
    held = box["st"]["held"]
    assert (held["pauses"], held["writer_waiting"], held["quiet_frames"]) == (0, waiting, quiet)


def test_the_frame_clock_keeps_each_whole_frames_end_offset():
    header, frames, end = framed_stream(3, 100)
    clock = PLV.PL.FrameClock()
    data = header + frames + end
    for at in range(0, len(data), 7):
        clock.feed(data[at:at + 7], 0.0)
    first = len(header) + 112 + 28
    assert clock.ends == [first, 2 * first - len(header), 3 * first - 2 * len(header)]
    assert clock.problem is None and clock.records == 7


def test_a_paced_session_holds_the_game_and_still_stops_it(tmp_path, monkeypatch):
    """run_session with stand-ins for loop_run, the game (streaming frames until asked to
    stop) and a player that takes 20 ms a frame and closes after 30: paced, the relay held
    the game and says so; the game is asked once and leaves, and the relay's arrivals are
    written for the receipt."""
    header, frames, end = framed_stream(400, 20_000)
    (tmp_path / "stream.bin").write_bytes(header + frames)
    game = tmp_path / "game.py"
    game.write_text(r"""
import os, signal, sys, time
stop = []
signal.signal(signal.SIGTERM, lambda *a: stop.append(1))
w = os.open(sys.argv[1], os.O_WRONLY)
data = open(sys.argv[2], "rb").read()
hb = (int.from_bytes(data[12:16], "little") + 3) & ~3
os.write(w, data[:hb])
size = (len(data) - hb) // 400
k, deadline = 0, time.monotonic() + 30
while not stop and time.monotonic() < deadline:      # frames until asked to stop
    view = memoryview(data)[hb + (k % 400) * size:hb + (k % 400 + 1) * size]
    while view:
        view = view[os.write(w, view):]
    k += 1
os.close(w)
sys.exit(0 if stop else 3)
""")
    p = PLV.paths(tmp_path)
    loop = (f"import subprocess, sys; sys.exit(subprocess.run([sys.executable, {str(game)!r}, "
            f"{str(p['game_fifo'])!r}, {str(tmp_path / 'stream.bin')!r}]).returncode)")
    player = FRAME_READER.replace("os.read(0,", "os.read(int(sys.argv[sys.argv.index('--video-fd') + 1]),")
    player = player.replace("out, pause, quit_after = sys.argv[1], float(sys.argv[2]), int(sys.argv[3])",
                            "out, pause, quit_after = os.environ['PLAY_TEST_SEEN'], 0.02, 30")
    pl = stand_in_plan(tmp_path, monkeypatch, loop, player,
                       {"PLAY_TEST_SEEN": str(tmp_path / "seen.txt")}, paced=True)
    monkeypatch.setattr(PLV, "STOP_GRACE", 5)
    monkeypatch.setattr(PLV, "KILL_GRACE", 5)
    box: dict = {}
    th = threading.Thread(target=lambda: box.update(got=PLV.run_session(pl)), daemon=True)
    th.start()
    th.join(timeout=90)       # a relay still holding a game whose player is gone hangs here
    if th.is_alive():
        for d in os.listdir("/proc"):
            try:
                if d.isdigit() and descends_from_this_process(int(d)) and str(tmp_path).encode() \
                        in Path(f"/proc/{d}/cmdline").read_bytes():
                    os.kill(int(d), signal.SIGKILL)
            except OSError:
                pass
        pytest.fail("the session did not end within 90 s of its player closing")
    got = box["got"]
    assert got["stop"]["how"] == "SIGTERM: WM_QUIT at the game's next keyboard poll"
    assert (got["game_rc"], got["player_rc"], got["interrupted"]) == (0, 0, False)
    held = got["relay"]["held"]
    assert held["lead_frames"] == 1 and held["pauses"] >= 20 and held["writer_waited"] >= 10
    # The game's pipe stays the least: found waiting while the player runs, and nothing
    # taken as quiet once the player has gone and no pause can come.
    assert held["pipe_max"] == 16384
    # Held to the player, not 30 s of frames: unheld, the stand-in game writes tens of
    # thousands before the player closes. The game runs unheld for the moments between
    # the player's end and the SIGTERM (stop_game looks for it in /proc), several hundred.
    assert got["relay"]["frames_arrived"] < 5000
    assert (tmp_path / "relay-frames.tsv").read_text().startswith("frame\tarrived_s\n1\t")


# ---------------------------------------------------------------- the pace schedule

PACE_DRIVER = r"""
#include "pace.h"
#include <cstdio>
#include <cstdlib>
// Each line: a frame's virtual time (us) and when it is ready (s); the driver presents it
// at its target or when ready, whichever is later, as do_present's pace_until does.
int main(int argc, char** argv) {
    const double slack = argc > 1 ? std::atof(argv[1]) : -1.0;
    Sa2Pace p(1.0, slack);
    unsigned long long vus; double ready;
    while (std::scanf("%llu %lf", &vus, &ready) == 2) {
        const Sa2Pace::Slot s = p.schedule(vus, ready);
        const double at = s.target > ready ? s.target : ready;
        p.presented(vus, s, at);
        std::printf("slot %.6f %.6f %.6f\n", s.target, s.due, s.moved);
    }
    std::printf("json %s\n", p.json().c_str());
    return 0;
}
"""


@pytest.fixture(scope="module")
def pace_driver(tmp_path_factory) -> Path:
    """host/player/pace.h compiled with the driver above (no SDL, no DXVK)."""
    if GXX is None:
        pytest.skip("no C++ compiler")
    d = tmp_path_factory.mktemp("pace")
    (d / "driver.cpp").write_text(PACE_DRIVER)
    exe = d / "driver"
    r = subprocess.run([GXX, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror",
                        f"-I{ROOT / 'host' / 'player'}", str(d / "driver.cpp"), "-o", str(exe)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr[-3000:]
    return exe


def pace_run(driver: Path, slack: float, frames: list[tuple[int, float]]) -> tuple[list, dict]:
    r = subprocess.run([str(driver), str(slack)], input="".join(f"{v} {t}\n" for v, t in frames),
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    lines = r.stdout.splitlines()
    slots = [tuple(float(x) for x in s.split()[1:]) for s in lines if s.startswith("slot ")]
    return slots, json.loads(lines[-1][len("json "):])


# Five 60 Hz frames: the first ready at 100 s, the third 200 ms late, the rest early.
FRAMES = [(0, 100.0), (16667, 100.0), (33333, 100.2333), (50000, 100.24), (66667, 100.25)]


def test_a_files_schedule_never_moves_and_makes_up_a_late_frame(pace_driver):
    slots, js = pace_run(pace_driver, -1, FRAMES)
    assert slots == [(100.0, 100.0, 0.0), (100.016667, 100.016667, 0.0),
                     (100.033333, 100.033333, 0.0), (100.05, 100.05, 0.0),
                     (100.066667, 100.066667, 0.0)]
    assert (js["live"], js["slack_ms"], js["moves"], js["frames"], js["waited"]) == \
           (False, None, 0, 5, 1)
    # Late by 0, 0, 199.967 (the third, 100.2333 - 100.033333), 190 and 183.333 ms; the
    # median's bucket ends at 183.5 ms, and the 90th and 99th are the largest.
    assert js["late"] == {"over_1ms": 3, "over_frame": 3, "over_slack": 0, "p50_ms": 183.5,
                          "p90_ms": 199.967, "p99_ms": 199.967, "max_ms": 199.967}


def test_a_live_schedule_moves_past_its_slack_and_makes_up_only_the_slack(pace_driver):
    """The third frame, 200 ms late, moves the schedule by 140 ms (200 less the 60 ms
    slack), so it is due 60 ms before it was ready; the fourth and fifth, 50 and 43 ms
    late against the moved schedule, move nothing; the pace is the virtual time over the
    wall time between the first Present and the last."""
    slots, js = pace_run(pace_driver, 0.06, FRAMES)
    assert slots == [(100.0, 100.0, 0.0), (100.016667, 100.016667, 0.0),
                     (100.1733, 100.033333, 0.139967), (100.189967, 100.189967, 0.0),
                     (100.206634, 100.206634, 0.0)]
    assert (js["live"], js["slack_ms"], js["moves"], js["moved_s"], js["waited"]) == \
           (True, 60.0, 1, 0.139967, 1)
    # Late by 0, 0, 199.967, then 50.033 and 43.366 ms behind the moved schedule.
    assert js["late"] == {"over_1ms": 3, "over_frame": 3, "over_slack": 1, "p50_ms": 43.5,
                          "p90_ms": 199.967, "p99_ms": 199.967, "max_ms": 199.967}
    assert (js["virtual_s"], js["wall_s"], js["virtual_per_wall"]) == (0.066667, 0.25, 0.266668)


def test_a_live_frame_catching_up_with_real_time_is_due_at_once(pace_driver):
    """A live frame three periods past its predecessor (a game whose clock caught up with
    real time, runtime/sa2_threads.c SA2_CLOCK_FOLLOWS_REAL) that is ready 46.667 ms before
    its time is shown when ready: the schedule moves back by that much, counted with the
    moves, and nothing waits. A file replay's schedule makes the same frame wait."""
    frames = [(0, 100.0), (16667, 100.0167), (66667, 100.02)]
    slots, js = pace_run(pace_driver, 0.06, frames)
    assert slots[2] == (100.02, 100.066667, -0.046667)
    assert (js["moves"], js["moved_s"], js["waited"]) == (1, 0.046667, 0)
    slots, js = pace_run(pace_driver, -1, frames)
    assert slots[2] == (100.066667, 100.066667, 0.0)
    assert (js["moves"], js["waited"]) == (0, 1)


# ---------------------------------------------------------------- the key sender

DRIVER = r"""
#include "input.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <string>

static SDL_Event key(Uint32 type, Uint32 win, SDL_Scancode sc, bool repeat = false) {
    SDL_Event e;
    std::memset(&e, 0, sizeof e);
    e.type = type;
    e.key.windowID = win;
    e.key.scancode = sc;
    e.key.down = type == SDL_EVENT_KEY_DOWN;
    e.key.repeat = repeat;
    return e;
}
static SDL_Event window(Uint32 type, Uint32 win) {
    SDL_Event e;
    std::memset(&e, 0, sizeof e);
    e.type = type;
    e.window.windowID = win;
    return e;
}
// Every whole record waiting: "<tag> <magic> <seq> <dik>=<byte>...".
static void drain(int fd, const char* tag) {
    unsigned char r[264];
    for (;;) {
        const ssize_t n = read(fd, r, sizeof r);
        if (n != (ssize_t)sizeof r) { if (n > 0) std::printf("%s short %zd\n", tag, n); return; }
        const unsigned magic = r[0] | r[1] << 8 | r[2] << 16 | (unsigned)r[3] << 24;
        const unsigned seq = r[4] | r[5] << 8 | r[6] << 16 | (unsigned)r[7] << 24;
        std::printf("%s %08X %u", tag, magic, seq);
        for (int i = 0; i < 256; i++) if (r[8 + i]) std::printf(" %02X=%02X", i, r[8 + i]);
        std::printf("\n");
    }
}

int main(int argc, char** argv) {
    const std::string dir = argv[1];
    if (argc > 2 && !std::strcmp(argv[2], "table")) {
        for (int s = 0; s < SDL_SCANCODE_COUNT; s++)
            if (const unsigned d = sa2_input::dik_of((SDL_Scancode)s)) std::printf("%d %02X\n", s, d);
        return 0;
    }
    const Uint32 W = 7;
    {   // a path that is no FIFO
        const std::string plain = dir + "/plain.txt";
        std::fclose(std::fopen(plain.c_str(), "w"));
        sa2_input::KeySender k(plain);
        const bool started = k.start();
        std::printf("plain %d %s\n", started, k.last_error().c_str());
    }
    {   // made and never started: events and pumps do nothing, and nothing is opened
        const std::string idle = dir + "/idle.fifo";
        mkfifo(idle.c_str(), 0600);
        const int rd0 = open(idle.c_str(), O_RDONLY | O_NONBLOCK);   // a reader is there
        sa2_input::KeySender k(idle);
        k.event(key(SDL_EVENT_KEY_DOWN, W, SDL_SCANCODE_RETURN), W, 1.0, 0);
        k.pump();
        drain(rd0, "idle");
        std::printf("idle-report %s", k.report_json().c_str());
        k.close();
        drain(rd0, "idle");
        close(rd0);
    }
    const std::string path = dir + "/keys.fifo";
    mkfifo(path.c_str(), 0600);
    sa2_input::KeySender k(path);
    std::printf("start %d\n", k.start());                    // no reader yet: ENXIO
    k.event(key(SDL_EVENT_KEY_DOWN, W, SDL_SCANCODE_RETURN), W, 1.0, 0);   // owed
    const int rd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
    drain(rd, "none");
    k.pump();                                                // opens; seq 0
    drain(rd, "a");
    k.event(key(SDL_EVENT_KEY_DOWN, W, SDL_SCANCODE_UP), W, 1.0, 0);              // seq 1
    k.event(window(SDL_EVENT_WINDOW_FOCUS_LOST, 8), W, 1.0, 0);                   // another window
    k.event(key(SDL_EVENT_KEY_DOWN, W, SDL_SCANCODE_RETURN, true), W, 1.0, 0);    // a repeat
    k.event(key(SDL_EVENT_KEY_DOWN, 8, SDL_SCANCODE_X), W, 1.0, 0);               // another window
    k.event(key(SDL_EVENT_KEY_DOWN, W, SDL_SCANCODE_F13), W, 1.0, 0);             // unmapped
    k.event(key(SDL_EVENT_KEY_UP, W, SDL_SCANCODE_RETURN), W, 1.0, 0);            // seq 2
    k.event(key(SDL_EVENT_KEY_UP, W, SDL_SCANCODE_DOWN), W, 1.0, 0);              // already up
    k.event(window(SDL_EVENT_WINDOW_FOCUS_LOST, W), W, 1.0, 0);                   // seq 3, all up
    k.event(window(SDL_EVENT_WINDOW_FOCUS_LOST, W), W, 1.0, 0);                   // nothing held
    drain(rd, "b");
    fcntl(rd, F_SETPIPE_SZ, 4096);                           // fifteen records fit
    for (int i = 0; i < 17; i++)                             // Z down, up, ... down
        k.event(key(i % 2 ? SDL_EVENT_KEY_UP : SDL_EVENT_KEY_DOWN, W, SDL_SCANCODE_Z), W, 1.0, 0);
    drain(rd, "c");
    k.pump();                                                // the latest: seq 19
    drain(rd, "d");
    k.event(key(SDL_EVENT_KEY_DOWN, W, SDL_SCANCODE_A), W, 1.0, 0);  // seq 20
    close(rd);                                               // the reader goes away
    k.event(key(SDL_EVENT_KEY_DOWN, W, SDL_SCANCODE_S), W, 1.0, 0);  // EPIPE: owed, reopened later
    k.pump();                                                // ENXIO: no reader
    const int rd2 = open(path.c_str(), O_RDONLY | O_NONBLOCK);
    k.pump();                                                // seq 21, the latest state
    k.event(window(SDL_EVENT_QUIT, 0), W, 1.0, 0);                   // seq 22, all up
    k.event(key(SDL_EVENT_KEY_DOWN, W, SDL_SCANCODE_Q), W, 1.0, 0);  // seq 23
    k.close();                                               // seq 24, all up
    drain(rd2, "e");
    std::printf("%s", k.report_json().c_str());
    k.event(key(SDL_EVENT_KEY_DOWN, W, SDL_SCANCODE_Q), W, 1.0, 0);  // after close: nothing
    drain(rd2, "f");
    return 0;
}
"""


@pytest.fixture(scope="module")
def driver(tmp_path_factory) -> Path:
    """host/player/input.cpp compiled with the driver above: the player's own code, fed by
    the driver instead of SDL. No SDL function is called, so no SDL library is linked."""
    if GXX is None or not (SDL_INCLUDE / "SDL3" / "SDL_events.h").is_file():
        pytest.skip(f"no C++ compiler, or no SDL3 headers under {SDL_INCLUDE}")
    d = tmp_path_factory.mktemp("keysender")
    (d / "driver.cpp").write_text(DRIVER)
    exe = d / "driver"
    r = subprocess.run([GXX, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", f"-I{SDL_INCLUDE}", f"-I{ROOT / 'host' / 'player'}",
                        str(d / "driver.cpp"), str(ROOT / "host" / "player" / "input.cpp"),
                        "-o", str(exe)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr[-3000:]
    return exe


def run_driver(driver: Path, tmp_path: Path, *args: str) -> list[str]:
    r = subprocess.run([str(driver), str(tmp_path), *args], capture_output=True, text=True,
                       timeout=60)
    assert r.returncode == 0, r.stderr
    return r.stdout.splitlines()


def test_the_key_sender_writes_one_whole_record_per_change_and_nothing_else(driver, tmp_path):
    out = run_driver(driver, tmp_path)
    recs = [x for x in out if re.match(r"^([a-f]|idle) ", x)]
    z_runs = [f"c 4B324153 {4 + i}" + ("" if i % 2 else " 2C=80") for i in range(15)]
    assert recs == ([
        "a 4B324153 0 1C=80",                       # owed while there was no reader
        "b 4B324153 1 1C=80 C8=80",
        "b 4B324153 2 C8=80",
        "b 4B324153 3",                             # focus lost: every key up
    ] + z_runs + [
        "d 4B324153 19 2C=80",                      # the full pipe: the latest state, once
        # seq 20 went with the pipe its reader closed; after EPIPE and a new reader, the
        # state as it is then, Z still held
        "e 4B324153 21 1E=80 1F=80 2C=80",
        "e 4B324153 22",                            # SDL_EVENT_QUIT
        "e 4B324153 23 10=80",
        "e 4B324153 24",                            # close(): every key up
    ])
    assert out[0] == f"plain 0 {tmp_path}/plain.txt is not a FIFO"
    assert out[1].startswith("idle-report ") and out[2] == "start 1"


def test_the_key_sender_reports_what_it_did(driver, tmp_path):
    out = run_driver(driver, tmp_path)
    line = next(x for x in out if x.startswith('  "host_input": '))
    got = json.loads("{" + line.strip().rstrip(",") + "}")["host_input"]
    assert got == {"path": f"{tmp_path}/keys.fifo", "started": True, "open": False,
                   "open_attempts": 4, "enxio": 2, "key_events": 25, "repeats_ignored": 1,
                   "other_window": 1, "unmapped": 1, "changes": 23, "clears": 3,
                   "records_written": 25, "retries": 2, "write_errors": 1, "last_seq": 24,
                   "held_now": 0, "owed": False, "synthetic_key_events": 0, "key_log": None,
                   # kept after the FIFO opened again: what went wrong last, not the state now
                   "last_error": "write: the reader went away (EPIPE)"}


def test_a_key_sender_not_started_opens_nothing_and_says_so(driver, tmp_path):
    """The player makes its sender before the window is up, so every report of a run given
    --input-path carries it; until start() it neither takes events nor opens the FIFO, with
    a reader there to be found."""
    out = run_driver(driver, tmp_path)
    line = next(x for x in out if x.startswith("idle-report "))
    got = json.loads("{" + line[len("idle-report "):].strip().rstrip(",") + "}")["host_input"]
    assert got == {"path": f"{tmp_path}/idle.fifo", "started": False, "open": False,
                   "open_attempts": 0, "enxio": 0, "key_events": 0, "repeats_ignored": 0,
                   "other_window": 0, "unmapped": 0, "changes": 0, "clears": 0,
                   "records_written": 0, "retries": 0, "write_errors": 0, "last_seq": None,
                   "held_now": 0, "owed": False, "synthetic_key_events": 0, "key_log": None, "last_error": None}
    assert not [x for x in out if x.startswith("idle ")]


# SDL scancode names (SDL_scancode.h) whose DIK name (dinput.h) is another word.
DIK_NAME = {"BACKSPACE": "BACK", "LEFTBRACKET": "LBRACKET", "RIGHTBRACKET": "RBRACKET",
            "LCTRL": "LCONTROL", "RCTRL": "RCONTROL", "LALT": "LMENU", "RALT": "RMENU",
            "KP_MULTIPLY": "MULTIPLY", "KP_MINUS": "SUBTRACT", "KP_PLUS": "ADD",
            "KP_PERIOD": "DECIMAL", "KP_DIVIDE": "DIVIDE", "KP_ENTER": "NUMPADENTER",
            "CAPSLOCK": "CAPITAL", "NUMLOCKCLEAR": "NUMLOCK", "SCROLLLOCK": "SCROLL",
            "NONUSBACKSLASH": "OEM_102", "PRINTSCREEN": "SYSRQ", "PAGEUP": "PRIOR",
            "PAGEDOWN": "NEXT", "LGUI": "LWIN", "RGUI": "RWIN", "APPLICATION": "APPS",
            **{f"KP_{i}": f"NUMPAD{i}" for i in range(10)}}


def test_every_mapped_scancode_is_its_keys_dik_code_in_zigs_dinput_h(driver, tmp_path):
    if not DINPUT_H.is_file():
        pytest.skip(f"the pinned zig's dinput.h is not present: {DINPUT_H}")
    names = {int(n): s for s, n in re.findall(r"SDL_SCANCODE_(\w+) = (\d+)",
                                               (SDL_INCLUDE / "SDL3" / "SDL_scancode.h").read_text())}
    dik = {s: int(v, 16) for s, v in re.findall(r"#define DIK_(\w+)\s+0x([0-9A-Fa-f]+)",
                                                  DINPUT_H.read_text())}
    table = {int(s): int(d, 16) for s, d in (x.split() for x in run_driver(driver, tmp_path, "table"))}
    got = {names[s]: d for s, d in table.items()}
    assert len(got) == 105 and len(set(got.values())) == 105   # no key twice
    assert {k: v for k, v in got.items() if dik[DIK_NAME.get(k, k)] != v} == {}
    # Player 1's keys under private/install's Config/Keyboard.cfg (the next test reads them
    # out of the image), literally.
    assert {k: got[k] for k in ("W", "S", "A", "D", "I", "J", "U", "K", "Q", "E", "RETURN")} == {
        "W": 0x11, "S": 0x1F, "A": 0x1E, "D": 0x20, "I": 0x17, "J": 0x24, "U": 0x16, "K": 0x25,
        "Q": 0x10, "E": 0x12, "RETURN": 0x1C}


def test_every_key_the_games_bindings_name_is_one_the_player_sends(driver, tmp_path):
    """Config/Keyboard.cfg as the image reads it (0x004055D0): each <key index> through the
    table at 0x0174F348 to a key code, and through the keyboard wrapper's map at 0x0089E9E8
    to a DIK code. The reader binds each player's slots 1-8, 10, 13 and 16 and reads and
    leaves 9, 11, 12, 14 and 15. Without the file the image's own defaults at 0x008AE040
    hold (36 button and key code pairs, 0xFFFFFFFF none)."""
    if not (EXE.is_file() and KEYBOARD_CFG.is_file()):
        pytest.skip(f"no unpacked executable or staged key configuration: {EXE}, {KEYBOARD_CFG}")
    pefile = pytest.importorskip("pefile")
    pe = pefile.PE(str(EXE), fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase

    def u32(va: int) -> int:
        return int.from_bytes(pe.get_data(va - base, 4), "little")

    def dik(code: int) -> int:
        return u32(0x0089E9E8 + 4 * code)

    players = re.findall(r"<Player [^>]*>(.*?)</Player>", KEYBOARD_CFG.read_text(), re.S)
    slots = [[dik(u32(0x0174F348 + 4 * int(i))) for i in re.findall(r'<key index="(\d+)"', body)]
             for body in players]
    assert slots == [
        [0x11, 0x1F, 0x1E, 0x20, 0x17, 0x24, 0x16, 0x25, 0x2C, 0x10, 0x91, 0x2D, 0x12, 0x91, 0x02, 0x1C],
        [0xC8, 0xD0, 0xCB, 0xCD, 0x4C, 0x4F, 0x4B, 0x50, 0xCF, 0x47, 0x91, 0xD1, 0x48, 0x91, 0x03, 0x4E]]
    bound = [[keys[s - 1] for s in (1, 2, 3, 4, 5, 6, 7, 8, 10, 13, 16)] for keys in slots]
    assert bound[0] == [0x11, 0x1F, 0x1E, 0x20, 0x17, 0x24, 0x16, 0x25, 0x10, 0x12, 0x1C]
    own = [dik(code) for code in (u32(0x008AE040 + 8 * i + 4) for i in range(36))
           if code != 0xFFFFFFFF]
    sent = {int(d, 16) for _, d in (x.split() for x in run_driver(driver, tmp_path, "table"))}
    assert (set(bound[0]) | set(bound[1]) | set(own)) - sent == set()


def test_the_record_is_the_same_in_the_runtime_and_the_player():
    h = (ROOT / "runtime" / "sa2_dinput.h").read_text()
    p = (ROOT / "host" / "player" / "input.h").read_text()
    assert re.search(r"#define SA2_DI_KEYREC_MAGIC (0x[0-9A-F]+)u", h).group(1) == "0x4B324153"
    assert re.search(r"#define SA2_DI_KEYREC_BYTES (\d+)u", h).group(1) == "264"
    assert re.search(r"kMagic = (0x[0-9A-F]+)u;", p).group(1) == "0x4B324153"
    assert re.search(r"kRecordBytes = (\d+);", p).group(1) == "264"
    assert (0x4B324153).to_bytes(4, "little") == b"SA2K"
