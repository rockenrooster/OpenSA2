# Local patches to SDL3

**These patches are not from the SDL project.** They belong to SA2 Recompiled, which applies
them to its own build of SDL 3.4.16 and to nothing else. Nothing here is submitted to SDL or
prepared for submission: the user's rulings of 2026-10-04
(`Documentation/decisions/2026-10-04-S5-user-rulings.md`, ruling 3, as amended by
`Documentation/decisions/2026-10-04-S5-user-rulings-2.md`) keep the patch in this repository.

| Patch | What it changes |
|---|---|
| `0001-gpu-clamp-to-border-and-depth-clamp-with-clip.patch` | Border addressing (`SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_BORDER` and a border colour) and depth clamp beside depth clip, on Vulkan, D3D12 and Metal, with two device properties that report support |
| `0002-gpu-vulkan-pipeline-cache.patch` | A Vulkan pipeline cache kept in a file (`SDL_PROP_GPU_DEVICE_CREATE_SA2RECOMP_PIPELINE_CACHE_PATH_STRING`): seeded from it at device creation, passed to every pipeline, written back after every 16 new graphics pipelines and at device destruction. Without the property, pipelines are made without a cache, as before. |

Each patch's header names what it changes, the SDL commit it targets
(`fa2c02bb6e21974a89ea9824bc53c9932abe5f9c`, the commit the release tarball records in its
`.git-hash`) and that it is not from the SDL project.

The commit is held by the tarball's own word: its `.git-hash` and `REVISION.txt` must name it,
and the tarball must hash to its pin. Nothing compares the tarball's files with the commit's
tree in SDL's repository (the amendment's "if the commit's tree is what it holds"); the pinned
tarball, not a checkout of the commit, is the source the build uses.

## Marked as altered

SDL's zlib licence, clause 2: "Altered source versions must be plainly marked as such, and
must not be misrepresented as being the original software." The patched build is marked in
two ways:

- **Its revision string.** `tools/s5/build_host_deps.sh` configures SDL with
  `-DSDL_VENDOR_INFO="sa2-recomp sdl3-patches <id>"`, so `SDL_GetRevision()` reads
  `SDL-release-3.4.16-0-gfa2c02bb6 (sa2-recomp sdl3-patches <id>)`. `<id>` is the first 12
  hex digits of the sha256 over each patch's sha256 (64 lowercase hex digits and a newline),
  in file-name order. `tools/pins.json` (`tools.sdl3.vendor_info`) pins it, and
  `Documentation/scripts/check_pins.py` refuses a library that does not carry it.
- **A startup log line.** Both players print, before they draw anything,
  `SDL3: <revision>: patched by sa2-recomp (patches/sdl3), not the SDL project's release`.
  A stock build prints `SDL3: <revision>: stock` instead.

## How the build uses them

`tools/s5/build_host_deps.sh` extracts the pinned tarball, checks its hash and the commit in
its `.git-hash`, and applies every patch here in order through `tools/s5/sdl3_patches.py`: the
pinned GNU patch (`tools.patch`) with `--fuzz=0`, in the C locale and none of the caller's
environment, a dry run first. A hunk that does not apply exactly, a hunk GNU patch would place
at an offset, or an apply that leaves a file it names unchanged stops the build before SDL is
configured, with the install untouched. `git apply` is not an equal way to apply these: it
places a hunk at an offset without saying so. The patched library is installed privately
under `private/s5-tools/install`, and the players load it by `DT_RPATH`, so it never replaces
or shadows a system SDL3, nor a system SDL3 it. SDL's dynamic API would get past that:
`SDL3_DYNAMIC_API` naming another `libSDL3.so.0` routes every SDL call there while the file the
players verify, and the revision they read, are still the private one's. So both players
refuse to start with `SDL3_DYNAMIC_API` set, before any SDL call, and
`host/player/player_env.sh` unsets it.

`--sdl3=stock` builds the unpatched tarball into its own prefix,
`private/s5-tools/install-sdl3-stock`; its SDL reports neither of the two device properties.
Host 2 detects that at run time and reports the shader-emulation fallback as owed (its report's
`sdl_gpu.fallback_owed`). Since phase B the device maps by what it detected: border addressing
to the patch's `CLAMP_TO_BORDER` and depth clamp to the patch's property where the device reports
them, and the shader route's fallback obligations where it does not (`fallback_border`,
`R_DEPTH_CLAMP`). R1 built the border fallback's shader variants (U11, `host/d3d9gpu/d9g_shader.h`,
`Variant::border_u` and `border_v`); the depth-clamp emulation is not built (the translator
refuses it until the depth-clamp measurement finds that a pixel clamp changes). Replays, host
comparisons and test runs on such a device still refuse each draw that owes the clamp, so the
stock build's tripwire still holds. A live session (`--interactive`, every desktop launcher and
Android session), or any run with `SA2_D3D9GPU_NO_DEPTH_CLAMP=1`, draws those clipped at the near
and far planes instead (`GpuInfo::depth_clamp_clip_only`): with that forced, none of the 23,778
frames of the four recorded streams (boot, menu, story, Metal Harbor) differs from the
reference.
`--sdl3=system` builds DXVK against a system SDL3, which no pin holds: no player is built on it
(`host/player/generate_expectations.py` refuses an install whose SDL no pin names), and an SDL3
this project built, from any checkout, is refused as a system one.

## What the gates hold, and what they do not yet

The user's second rulings ask the gates to exercise both configurations: the patched build
held to exact frame identity with host 1, the stock build to running the replays with only the
named border and depth-clamp residues. Host 2 does not draw yet, so the stock half can bite
only once it does (`Documentation/decisions/2026-10-04-S5-user-rulings-2.md`). Until then the
amendments to those rulings ask the stock gate to hold "the build, the fallback chosen by
detection, and the residue classes named". The user's ruling of 2026-10-05
(`Documentation/decisions/2026-10-05-S5-user-rulings-3.md`, item 3) settles the order: "Add the
detection test to the stock-SDL gate now, with both capabilities forced off. The named residues
come with the stock configuration once host 2 draws." So the stock gate holds the build, the
fallback chosen by detection, and the residue classes the state check names on one census
stream, on the stock prefix itself and on the patched install's forced-off leg. The residues in
the stock configuration's own replays are owed, listed below, and come once host 2 draws.

Held at every push today:

- both libraries, each to its own hash and mark (`check_pins.py`, `sdl3` and `sdl3-stock`): a
  patched library carries its vendor string, a stock one the bare revision and no byte of the
  patch's names;
- the patched library's behaviour on lavapipe, with literal pixels, and the stock library
  reporting neither capability (`Documentation/scripts/tests/test_s5_sdl3_patch.py`);
- the patch set, its strict application and the players' contracts on both configurations
  (`Documentation/scripts/tests/test_s5_sdl3_build.py`);
- host 2's own detection, on lavapipe (`Documentation/scripts/tests/test_s5_sdl3_detect.py`).
  Host 2 is built against each prefix and replays the census's story stream, which holds a
  `BORDER` sampler. Every replay's report, and DXVK's log, must name the lavapipe device:
  - on the patched install with both capabilities forced off by a test run
    (`SA2_D3D9GPU_CAPS_OFF=border,depth-clamp`), its report reads both detected, neither used,
    both fallbacks owed (`sdl_gpu.fallback_owed`) and the override named;
  - on the stock prefix, without forcing, it loads the stock library and reads it unmarked, with
    neither capability detected and both fallbacks owed. Its keys file is the forced-off run's
    but for the override and the binary, so the forced-off leg maps every draw as the stock
    prefix does;
  - the state check on the stock prefix's own keys file, under ruling 3's
    `Documentation/decisions/2026-10-05-S5-depth-clamp-ruling.json`, reads
    `STATE_CHECK_FALLBACK_OWED BORDER DEPTH_CLAMP` with no test leg, and counts the same tuples
    in each class as on the forced-off leg;
  - the device is one build on both prefixes: each object of `libsa2_d3d9gpu.a` is
    byte-identical between the two builds once debug information and local symbols are
    stripped, so the device cannot choose by the patch's property macros, which only the
    patched install's headers define. The detection is at run time;
  - the state check on the forced-off run's keys file reads
    `STATE_CHECK_FALLBACK_OWED BORDER DEPTH_CLAMP` as a test leg (`TEST_LEG=border,depth-clamp`),
    never OK;
  - the control, the patched install without forcing, owes nothing, in the report and in the
    state check.

  Host 2 does not draw yet. The test's own tripwire,
  `test_host_2_draws_nothing_yet_the_stock_residues_are_not_due`, fails on the first stock
  replay that draws, naming the residues the ruling then makes due; the detection clauses go on
  reporting meanwhile.

Not held by any gate yet, owed by the stock-configuration work (`sdl-stock-build`):

- host 2's refusal to start when the mark and the device properties disagree, which has no
  negative control yet (a patched build without `SDL_VENDOR_INFO`);
- detection by the device properties rather than by the revision mark. Lavapipe supports both
  capabilities, so on it the properties read what the mark says: host 2 taking `detected` from
  the mark would pass the detection test. Telling them apart needs a device that lacks a
  capability the patched SDL implements;
- the stock build running the replays with only the named `BORDER` and `DEPTH_CLAMP` residues,
  which needs host 2 to draw (the user's ruling of 2026-10-05, item 3);
- the residue classes across the census on the stock prefix. Since phase B the state check names
  them (`fallback BORDER`, the shader route's obligation; `residue DEPTH_CLAMP`, owed under ruling
  3's `Documentation/decisions/2026-10-05-S5-depth-clamp-ruling.json` on a device without depth
  clip and clamp, `STATE_CHECK_FALLBACK_OWED`).
  `Documentation/scripts/tests/test_s5_d3d9gpu_state.py` holds them on the CPU model with neither
  capability, and `test_s5_sdl3_detect.py` on one census stream through host 2, on the stock
  prefix and on the forced-off leg.
  `host/d3d9gpu/state_census.sh` with `CAPS_OFF` runs the forced-off leg over the four census
  streams on this machine's GPU. A census of the replays on the stock prefix itself is still
  owed;
- a load check that accepts a system SDL3 under the system option alone.

Not held yet, owed by host 2's frame equality with host 1:

- the binding of a frame-equality run to the patched configuration. The ruling holds the
  patched build to exact frame identity with host 1, but the players can now be built on either
  prefix, and nothing that compares hosts (`tools/s5/compare_hosts.py`, `check_s5_exit.py`,
  `play_report.py`) reads the build's `sdl3_configuration` or holds its `files.libSDL3.sha256`
  to `tools.sdl3.library_sha256`: `compare_hosts.py`'s provenance holds the contract's keys, so a
  stock-built player would pass there as the patched build. Today only this patch's receipt
  (`Documentation/receipts/s5-sdl3-patch-v1.json`, A4) holds the DXVK player on the patched
  library to the registered references.

Phase B reaches the branches the patch adds to the state check's SDL side
(`host/d3d9gpu/test/ref_sdl.c`). On a device that reports the capabilities, d9g's descriptions
ask for the depth clamp and a border colour, and d9g's one maker of their SDL properties
(`host/d3d9gpu/d9g_props.h`, `CreateProps`) sets them through SDL's property functions.
`ref_sdl.c` models those functions and the device's capabilities, so SDL's own spans build the
clamp-with-clip chain and the border colour from the set d9g made. The state check's
`map-depth-clamp-off` and `map-border-colour` controls fail on the fields they break, so a mutant
of either branch, or of the maker, fails a check.

d9g creates no pipeline or sampler yet. R3's device creates them, and must pass
`CreateProps`' set as the create info's props (`PipelineDesc::create_info` and
`SamplerDesc::create_info` take it, with no default); a create call without it would build
stock SDL's clip-only state and SDL's default border colour, which no check would see.
