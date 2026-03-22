# AICA Flow (AFX) Sequencer

A high-performance, low-overhead MIDI-to-AICA sequencer for the SEGA Dreamcast. This project enables streaming complex musical arrangements to the AICA SPU with minimal ARM7 CPU intervention by pre-computing timestamped register commands on the host side.

The current `.afx` format is stable and single-track for this project. The toolchain emits a self-contained file with a sample blob, per-sample descriptors, a flow-command table, and optional DSP payloads.

## Project Architecture

The system is split into three header-defined domains:

1. **Common (`include/afx/common.h`)**: Shared file format, IPC types, and memory layout constants.
2. **Driver (`include/afx/driver.h`)**: ARM7-specific internal state, AICA register offsets, and performance utilities.
3. **Host (`include/afx/host.h`)**: SH-4 and host tool-side command IDs and section-parsing logic.

Detailed architecture diagrams and runtime memory-flow notes are maintained in `docs/afx_design.md`.

## Key Features

- **Integrated Wavetable Synthesis**: Automatically scans a user-defined directory (e.g., "Echo Sound Works Core Tables") to map MIDI Program Change messages to high-quality PCM samples.
- **Absolute Time Sequencing**: All MIDI events are pre-calculated into 1 ms absolute timestamps, including tempo changes across multiple tracks.
- **Descriptor-Based Sample Packing**: The compiler emits per-sample metadata including format, loop mode, root note, loop points, and sample rate.
- **Seekable Playback**: The ARM7 driver supports binary-search seek by target tick through IPC.
- **Global Music Volume**: Runtime volume scaling is applied only to `TOT_LVL` writes; all other flow commands remain precomputed.
- **Optional DSP Payloads**: `.afx` files can carry DSP coefficient and microprogram sections that are uploaded on song start.
- **Test Coverage**: `make test` runs unit tests plus an end-to-end integration path that generates and inspects a real `.afx` file.

## File Format (.afx)

| Section | Description |
| :--- | :--- |
| **Header (`afx_header_t`)** | 20-byte lean header containing magic, version, section count, and duration. |
| **Section Table (`afx_section_entry_t[]`)** | Directory of typed chunks immediately following the header. |
| **FLOW** | Array of 8-byte `afx_cmd_t` entries: timestamp, slot, register, value. |
| **SDES** | Array of `afx_sample_desc_t` entries with source ID, GM program, format, loop info, root note, sample rate, and offsets. |
| **SDAT** | Raw ADPCM or PCM sample bytes packed back-to-back. |
| **DSPM / DSPC / META** | Optional DSP and metadata payload sections. |

Relevant format properties:

- `AICAF_MAGIC = 0xA1CAF100`
- `AICAF_VERSION = 1`
- The section table is accessed via implicit offset (`sizeof(afx_header_t)`).
- `AFX_ALIGN32` is supported for DMA-safe section alignment.
- Sample addresses in flow commands are file-relative offsets; the ARM7 driver resolves them as `afx_base + relative_offset`.

## Build Instructions

Requirements:
- A modern C23-compliant host compiler (GCC 13+ or Clang 17+).
- `arm-none-eabi-gcc` for the SPU driver.
- KallistiOS (KOS) environment for SH4 components.

To build the entire project:
```bash
make all
```

Individual components:
- `make tools`: Build `midi2afx` and `afx_info`.
- `make driver`: Build the ARM7 binary (`src/driver/aica_driver.bin`).
- `make test`: Run unit and integration tests.

## Usage

Generate or refresh the wavetable map from the default input collection:

```bash
python3 ./tools/generate_wavetable_map.py
```

By default this scans `input/wavetable_collections` and writes `input/wavetables.map`.

Generate a family-driven patch map (keys/pads/bass/leads/percussion/fx) from the scanned wavetable set:

```bash
python3 ./tools/build_family_patches.py
```

This reads `input/family_patch_profiles.json` and writes `input/wavetables.family.map` with one selected patch per GM program.
It also writes coverage reports:

- `input/wavetables.family.coverage.json`: machine-readable per-program assignments + duplicate stats
- `input/wavetables.family.coverage.txt`: human-readable summary and full GM mapping table

When a map entry includes family playback fields (`policy_note_trim_ms`, `policy_min_hold_ms`, `policy_velocity_gamma`, `policy_velocity_gain`, `policy_release_bias`), `midi2afx` applies them while generating flow commands:

- note-off timing trim with a minimum hold guard
- velocity shaping curve before Total-Level conversion
- release-rate bias layered on top of MIDI CC-derived release

Convert a MIDI file to the AFX format:
```bash
./tools/midi2afx input/bwv1007.mid output.afx input/wavetables.map
```

To use the family-driven map, pass `input/wavetables.family.map` instead:

```bash
./tools/midi2afx input/bwv1007.mid output.afx input/wavetables.family.map
```

Useful options:

- `--trim`: trim trailing silence and clamp long samples before packing
- `--16bit`: store PCM16 instead of ADPCM

Inspect an AFX file:
```bash
./tools/afx_info output.afx input/wavetables.map
```

Run the Python emulator against a generated file:

```bash
python3 ./tools/afx_emulator.py output.afx input/wavetables.map render.wav
```

Run the full test suite:

```bash
make test
```

Test artifacts are written under `tests/artifacts/`.

## Runtime Control

The SH4/ARM7 IPC interface currently supports:

- `PLAY`
- `STOP`
- `PAUSE`
- `VOLUME`
- `SEEK`

Control commands are transported over a ring queue in AICA RAM (`AFX_IPC_CMD_QUEUE_ADDR`) with SH4 as producer and ARM7 as consumer.

For bring-up, SH4 can upload and initialize firmware with `afx_upload_and_init_firmware()`. This helper uses the default firmware load address, writes a 32-byte aligned dynamic-upload base marker immediately after the firmware image, and resets allocator state tracked in SH4-side `aica_state_t`.

The ARM7 driver remains intentionally simple: it advances a virtual clock, streams register writes, uploads optional DSP data at song start, and applies only one runtime transform to the flow-command stream: global total-level scaling for music volume (via a cached 256-entry TL lookup table rebuilt only on volume changes).

