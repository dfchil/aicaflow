# .afx Format Design

## Core Philosophy

The ARM7 driver is a dumb stream player.
It does not do instrument logic, sample lookup, or synthesis decisions.

Driver responsibilities:
1. Advance virtual clock
2. When `virtual_clock >= flow_cmd.timestamp`, write `flow_cmd.value` to the target AICA register
3. Process SH4->ARM7 control commands from IPC queue

Host tool (`midi2afx`) responsibilities:
- Parse MIDI and resolve musical intent
- Select/pack samples and descriptors
- Encode AICA register command stream
- Bake per-note and per-voice decisions into flow commands

The result is a flow-command architecture where the runtime is deterministic and lightweight.

---

## Binary Layout

```
[afx_header_t]             - compact format header (version, section table info)
[afx_section_entry_t[]]    - typed section directory
[FLOW]                     - array of afx_cmd_t
[SDES]                     - array of afx_sample_desc_t
[SDAT]                     - raw ADPCM/PCM bytes, back-to-back
[DSPM]                     - optional DSP microprogram payload
[DSPC]                     - optional DSP coefficient payload
[META]                     - optional metadata payload
```

All section offsets are relative to file start.

---

## Data Structures (Current)

### Header (`afx_header_t`)

```c
#define AICAF_MAGIC    0xA1CAF200
#define AICAF_VERSION  1

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t header_size;
    uint32_t section_count;
    uint32_t section_table_off;
    uint32_t section_table_size;
    uint32_t total_ticks;      // total duration in ms
    uint32_t flags;
} afx_header_t;

typedef struct {
    uint32_t id;               // AFX_SECT_* fourcc
    uint32_t offset;           // file-relative byte offset
    uint32_t size;             // bytes
    uint32_t count;            // entry count for array sections
    uint32_t align;            // alignment (typically 4)
    uint32_t flags;
} afx_section_entry_t;
```

### Sample Descriptor (`afx_sample_desc_t`)

```c
#define AFX_FMT_PCM16  0
#define AFX_FMT_PCM8   1
#define AFX_FMT_ADPCM  3

#define AFX_LOOP_NONE  0
#define AFX_LOOP_FWD   1
#define AFX_LOOP_BIDIR 2

typedef struct {
    uint32_t source_id;
    uint8_t  gm_program;
    uint8_t  format;
    uint8_t  loop_mode;
    uint8_t  root_note;
    int8_t   fine_tune;
    uint8_t  reserved[3];
    uint32_t sample_off;   // byte offset into sample blob
    uint32_t sample_size;  // bytes
    uint32_t loop_start;   // byte offset relative to sample_off
    uint32_t loop_end;     // byte offset relative to sample_off
    uint32_t sample_rate;
} afx_sample_desc_t;
```

### Flow Command Entry (`afx_cmd_t`)

```c
typedef struct {
    uint32_t timestamp;  // absolute ms
    uint8_t  slot;
    uint8_t  reg;
    uint16_t pad;
    uint32_t value;
} afx_cmd_t;
```

---

## Naming: "Opcode" vs "Flow Command"

Is "opcode" misleading here? Slightly, yes.

Reason:
- "Opcode" suggests an interpreted VM instruction set
- This stream is actually direct hardware register commands with timestamps

Preferred term in docs:
- flow command

Compatibility note:
- Use `afx_cmd_t` in code and docs

---

## AICA Register Command Targets

(Previously titled "Opcode Register Reference")

| Constant        | Value | AICA Register                         |
|----------------|-------|---------------------------------------|
| AICA_REG_CTL    | 0x00  | Control / KeyOn / KeyOff / format     |
| AICA_REG_SA_HI  | 0x01  | Sample address [23:16]                |
| AICA_REG_SA_LO  | 0x02  | Sample address [15:0]                 |
| AICA_REG_LSA_HI | 0x03  | Loop start [23:16]                    |
| AICA_REG_LSA_LO | 0x04  | Loop start [15:0]                     |
| AICA_REG_LEA_HI | 0x05  | Loop end [23:16]                      |
| AICA_REG_LEA_LO | 0x06  | Loop end [15:0]                       |
| AICA_REG_D2R_D1R| 0x07  | Decay 2 / Decay 1                     |
| AICA_REG_EGH_RR | 0x08  | EG hold / Release                     |
| AICA_REG_AR_SR  | 0x09  | Attack / Sustain                      |
| AICA_REG_LNK_DL | 0x0A  | Link / Decay level                    |
| AICA_REG_FNS_OCT| 0x0C  | Frequency / Octave                    |
| AICA_REG_TOT_LVL| 0x0D  | Total level                           |
| AICA_REG_PAN_VOL| 0x0E  | Pan / Volume                          |

---

## Runtime Address Model (Current)

The command stream stores blob-local sample offsets in SA command values.
At runtime, ARM7 adds `sample_base` and writes split SA_HI/SA_LO register data.

So current behavior is:
- host emits blob-local offsets
- driver applies one address fixup for SA writes

This is intentional in the current implementation.

---

## SH4/ARM7 IPC Model (Current)

IPC transport is now a ring queue in AICA RAM.

Control/status block layout:
- `afx_ipc_status_t` (head/tail/status/tick/volume)
- command queue (`afx_ipc_cmd_t[]`)
- `afx_player_state_t`

Queue characteristics:
- fixed-size circular buffer
- SH4 is producer (`q_head`)
- ARM7 is consumer (`q_tail`)
- poll interval ~1ms, with drain-until-empty behavior

Supported commands:
- PLAY
- STOP
- PAUSE
- VOLUME
- SEEK

---

## SH4 Dynamic Memory Ownership (Current)

SH4 owns dynamic AICA RAM allocation.

Implemented host helpers:
- `afx_mem_reset`
- `afx_mem_alloc`
- `afx_mem_write`
- `afx_upload_afx`
- `afx_upload_and_init_firmware`

Firmware upload/init behavior:
- upload firmware at requested SPU address
- write 32-byte aligned dynamic-base marker immediately after firmware size
- clear queue/player/status control blocks
- initialize allocator cursor to computed dynamic base

---

## Family Playback Policies (Current)

Family policy metadata is generated into family map entries and consumed by converter.

Applied during conversion:
- note-off trim (`policy_note_trim_ms`) with min hold (`policy_min_hold_ms`)
- velocity shaping (`policy_velocity_gamma`, `policy_velocity_gain`)
- release-rate bias (`policy_release_bias`)

This affects flow-command generation, not driver complexity.

---

## What Has Been Done

1. Stable header/descriptor/stream schema implemented in code
2. DSP payload embedding and driver upload path implemented
3. Global runtime music volume scaling on TOT_LVL implemented
4. SH4-managed dynamic AICA upload helpers implemented
5. Firmware upload/init helper with dynamic-base marker implemented
6. IPC ring queue mechanism implemented (replacing single mailbox fields)
7. Queue resized to a compact practical default (`0x0400`)
8. Family patch workflow and policy-aware conversion implemented
9. Coverage reports for family patch mapping implemented

---

## What Still Needs To Be Done

1. Queue robustness instrumentation:
- optional overflow counter / dropped-command counter
- optional watermark metrics for tuning queue size on hardware

2. Queue API ergonomics:
- expose explicit success/failure return path to callers for enqueue operations
- optional timeout policy hooks on SH4 side

3. Emulator alignment:
- verify Python emulator behavior against latest queue/layout and policy changes
- add regression tests for seek + policy timing interactions

4. Documentation consolidation:
- sync README and this design doc whenever constants/struct fields change
- optionally auto-generate constant tables from headers

---

## Non-Goals (Still True)

- ARM7 does not perform instrument-bank decoding
- ARM7 does not do runtime pitch/envelope synthesis logic
- ARM7 does not run an interpreted script VM
- Runtime stays a timestamped register-command sequencer
