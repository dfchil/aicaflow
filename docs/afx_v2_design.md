# .afx v2 Format Design

## Core Philosophy

**The ARM7 driver is a dumb stream player.** It never has to reason about instruments, samples, DSP, or voice allocation. Its only job is:

1. Advance virtual clock
2. When `virtual_clock >= opcode.timestamp`, write `opcode.value` to `AICA_BASE + slot*0x80 + reg*4`
3. Handle IPC signals from SH4 (PLAY / STOP / PAUSE / VOLUME)

**All intelligence belongs in `midi2afx` (the host-side precompiler).** Because the compiler runs on a PC with full knowledge of:
- Every sample's byte offset within the `.afx` sample blob
- The full AICA register encoding for each voice parameter
- The exact channel/voice state at every point in the timeline

...it can bake 100% complete, final register writes into the stream ahead of time. The driver never needs to decode, dispatch, or interpret anything beyond "write this value to this register at this time".

---

## .afx v2 Binary Layout

```
[afx_header_v2_t]           - fixed-size header with all section offsets
[sample data blob]          - raw ADPCM/PCM16 sample bytes, back-to-back
[sample descriptor table]   - one afx_sample_desc_t per sample
[stream opcodes]            - array of afx_opcode_t (unchanged format)
[DSP microprogram]          - MPRO words (optional, may be zero-length)
[DSP coefficients]          - COEF words (optional, may be zero-length)
```

All offsets in the header are relative to the start of the file.

---

## C Struct Definitions

### Header

```c
#define AFX_MAGIC_V2    0xA1CAF200
#define AFX_VERSION_V2  2

typedef struct {
    uint32_t magic;              // 0xA1CAF200
    uint32_t version;            // 2
    uint32_t sample_data_off;    // byte offset to raw sample blob
    uint32_t sample_data_size;   // total size of sample blob in bytes
    uint32_t sample_desc_off;    // byte offset to sample descriptor table
    uint32_t sample_desc_count;  // number of afx_sample_desc_t entries
    uint32_t stream_data_off;    // byte offset to opcode array
    uint32_t stream_data_size;   // total size of opcode array in bytes
    uint32_t dsp_mpro_off;       // byte offset to DSP microprogram (0 = none)
    uint32_t dsp_mpro_size;      // size of DSP microprogram in bytes
    uint32_t dsp_coef_off;       // byte offset to DSP coefficients (0 = none)
    uint32_t dsp_coef_size;      // size of DSP coefficients in bytes
    uint32_t total_ticks;        // total song duration in ms
} afx_header_v2_t;
```

### Sample Descriptor

Replaces the old `afx_source_entry_t`. Carries everything the compiler needs to emit
correct SA_HI/SA_LO/LSA_HI/LSA_LO/LEA_HI/LEA_LO opcodes and CTL format bits.

```c
#define AFX_FMT_ADPCM   0   // Yamaha 4-bit ADPCM
#define AFX_FMT_PCM16   1   // 16-bit signed PCM
#define AFX_FMT_PCM8    2   // 8-bit unsigned PCM

#define AFX_LOOP_NONE   0   // no loop
#define AFX_LOOP_FWD    1   // normal forward loop
#define AFX_LOOP_BIDIR  2   // bidirectional loop

typedef struct {
    uint32_t source_id;      // hash of originating WAV (for dedup / info tools)
    uint8_t  gm_program;     // GM program number this was sourced from
    uint8_t  format;         // AFX_FMT_*
    uint8_t  loop_mode;      // AFX_LOOP_*
    uint8_t  root_note;      // MIDI note of the recorded pitch (default 60 = C4)
    int8_t   fine_tune;      // cents offset from root_note (-100..+100)
    uint8_t  reserved[3];
    uint32_t sample_off;     // byte offset into sample data blob
    uint32_t sample_size;    // size of sample data in bytes
    uint32_t loop_start;     // loop start in samples
    uint32_t loop_end;       // loop end in samples (0 = end of sample)
    uint32_t sample_rate;    // original sample rate in Hz
} afx_sample_desc_t;
```

### Opcode (unchanged)

The `afx_opcode_t` format is unchanged. The driver never needs a new opcode type
because the compiler emits fully-resolved register values.

```c
typedef struct {
    uint32_t timestamp;  // absolute time in ms
    uint8_t  slot;       // AICA voice slot 0-63
    uint8_t  reg;        // AICA register (AICA_REG_*)
    uint16_t pad;
    uint32_t value;      // pre-encoded register value, ready to write
} afx_opcode_t;
```

---

## Opcode Register Reference

| Constant        | Value | AICA Register                        |
|-----------------|-------|--------------------------------------|
| AICA_REG_CTL    | 0x00  | Control / KeyOn / KeyOff / Format    |
| AICA_REG_SA_HI  | 0x01  | Sample address [19:16]               |
| AICA_REG_SA_LO  | 0x02  | Sample address [15:0]                |
| AICA_REG_LSA_HI | 0x03  | Loop start address [19:16]           |
| AICA_REG_LSA_LO | 0x04  | Loop start address [15:0]            |
| AICA_REG_LEA_HI | 0x05  | Loop end address [19:16]             |
| AICA_REG_LEA_LO | 0x06  | Loop end address [15:0]              |
| AICA_REG_D2R_D1R| 0x07  | Decay 2 rate / Decay 1 rate          |
| AICA_REG_EGH_RR | 0x08  | EG hold / Release rate               |
| AICA_REG_AR_SR  | 0x09  | Attack rate / Sustain rate           |
| AICA_REG_LNK_DL | 0x0A  | Loop link / Decay level              |
| AICA_REG_FNS_OCT| 0x0C  | Fine frequency step / Octave         |
| AICA_REG_TOT_LVL| 0x0D  | Total level (volume)                 |
| AICA_REG_PAN_VOL| 0x0E  | Panning / Volume                     |

---

## How the Compiler Resolves Everything

### Sample Addresses

At compile time, `midi2afx` knows:
- `sample_desc[i].sample_off` — offset of each sample within the sample blob
- The ARM7 driver places the sample blob at `WAVE_RAM_BASE + sample_data_off`

So the compiler emits pre-adjusted absolute Wave RAM addresses directly into
SA_HI/SA_LO/LSA_*/LEA_* opcodes. The driver applies no fixup at all — it writes
verbatim values. *(This is a change from v1 where the driver added the base offset.)*

### Loop Points

If the sample has `loop_mode != AFX_LOOP_NONE`, the compiler emits:
1. `LSA_HI` / `LSA_LO` — loop start address (absolute Wave RAM)
2. `LEA_HI` / `LEA_LO` — loop end address (absolute Wave RAM)
3. CTL with the loop bit set in addition to KYON

### Pitch / Frequency

For a given MIDI note, the compiler computes the FNS_OCT value using:
- `sample_desc[i].root_note` and `fine_tune`
- The target MIDI note
- Standard AICA frequency formula: `FNS = (f_target / f_root) * 1024`, clipped to [0,1023], OCT computed from octave distance

All of this is arithmetic the ARM7 should not waste cycles on.

### Envelope

Default AR/D1R/D2R/RR/DL for each program are baked into the opcode stream.
MIDI CC 73 (Attack) and CC 72 (Release) adjustments are resolved into absolute
AICA envelope register values at compile time as they are encountered in the event
stream.

### Volume / Pan

MIDI velocity → AICA total level conversion and MIDI pan CC → AICA PAN_VOL
encoding are computed by the compiler and written directly into the opcode stream.

### DSP

If the song uses reverb/chorus, the compiler embeds the DSP microprogram and
coefficient tables directly in the `.afx` file. The driver uploads these to AICA
DSP memory on song start (the one non-trivial initialization step) and then
never touches DSP again.

---

## ARM7 Driver Changes (minimal)

The v1 driver applied a sample address fixup in `execute_opcode()`:

```c
// v1 (to be removed)
if (reg == AICA_REG_SA_LO || reg == AICA_REG_SA_HI) {
    val += (IPC_STATUS->cmd_arg + SONG_HEADER->sample_data_off);
}
```

In v2, the compiler pre-bakes absolute addresses, so this fixup is **removed**.

The only new driver responsibility is uploading any embedded DSP data at song start:

```c
// pseudo-code for PLAY handler
if (header->dsp_mpro_size > 0)
    memcpy(AICA_DSP_MPRO, file_base + header->dsp_mpro_off, header->dsp_mpro_size);
if (header->dsp_coef_size > 0)
    memcpy(AICA_DSP_COEF, file_base + header->dsp_coef_off, header->dsp_coef_size);
// then start the stream interpreter loop as before
```

Everything else in the driver is unchanged.

---

## Implementation Plan

### Step 1 — `include/afx/afx.h`
- Add `afx_header_v2_t`
- Add `afx_sample_desc_t` with format/loop/root note/fine tune/loop points
- Keep `afx_opcode_t` unchanged
- Keep old v1 structs with a deprecation comment for backward compat during transition

### Step 2 — `src/tools/midi2afx.c`
- Replace source map write with sample descriptor table write
- Pre-resolve all sample addresses (base + sample_off) at write time; no runtime fixup
- Emit LSA_*/LEA_* opcodes when sample has loop points
- Compute FNS_OCT from MIDI note + root_note + fine_tune
- Encode velocity → TL, pan → PAN_VOL at emit time
- Write v2 header with correct section offsets

### Step 3 — `src/tools/afx_info.c`
- Decode `afx_header_v2_t` / `afx_sample_desc_t`
- Show loop points, root note, format per sample
- Validate pre-baked sample addresses against sample blob bounds

### Step 4 — `src/driver/aica_driver.c`
- Remove SA_LO/SA_HI fixup from `execute_opcode()`
- Add DSP memory upload in PLAY handler
- No other changes required

### Step 5 — Python emulator (`src/tools/afx_emulator.py`)
- Update header parsing for v2 layout
- Remove sample address fixup (addresses are now absolute)
- Apply loop points from descriptor

### Step 6 — Music volume control (`src/driver/aica_driver.c`)
The driver is permitted one runtime operation on the opcode stream: scaling playback volume in response to a music volume setting from the SH4.

- Add a `music_volume` field (0–255) to the IPC status struct, settable via a new `SET_MUSIC_VOL` IPC command from the SH4
- In `execute_opcode()`, when writing `AICA_REG_TOT_LVL` (total level), scale the pre-baked value by `music_volume / 255` before writing to hardware
- All other register writes remain verbatim — only TL is affected
- Default `music_volume = 255` (full volume, no scaling)

This keeps the driver dumb: it does not interpret the music, it only applies a single global scalar to voice levels.

---

## Non-Goals (by design)

- The ARM7 driver will **not** decode instrument banks, resolve GM program numbers, or perform voice allocation at runtime. All of this happens in the compiler.
- The ARM7 driver will **not** compute pitch ratios, envelope curves, or volume curves at runtime. All register values are pre-encoded.
- There is **no** scripting, no bytecode, no expression evaluation in the driver. It is purely a timestamped register write sequencer.
