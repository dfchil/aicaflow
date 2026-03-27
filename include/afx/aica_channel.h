#ifndef AFX_CHANNEL_REGS_H
#define AFX_CHANNEL_REGS_H

#include <stdint.h>

typedef struct {
  union {
    uint16_t raw;
    struct {
      /* sa_high is only 7 bits because the AICA's wave memory is 23 bits, and
       * the lower 16 bits are stored in sa_low. */
      uint16_t sa_high : 7;

      /* PCM Format
       * - 0=16-bit PCM
       * - 1=8-bit PCM
       * - 2=ADPCM
       *
       * @note ADPCM samples are stored in the same sample data format as 16-bit
       * PCM, but the AICA decodes them differently based on this field. */
      uint16_t pcms : 2;

      /** Loop Control (0=No Loop, 1=Forward Loop).
       * The AICA does not support bidirectional looping, so the loop mode is
       * either disabled (0) or enabled for forward looping (1). When loop
       * control is enabled, the AICA will automatically loop the sample between
       * the Loop Start Address (lsa) and Loop End Address (lea) when it reaches
       * the end of the sample data. Properly setting the loop points and
       * enabling loop control allows for seamless looping of samples, which is
       * essential for sustaining notes or creating rhythmic patterns without
       * gaps.
       * @note when using loop control, the AICA will ignore the
       * key_off command until the sample is released (key_on goes low), so the
       * note will continue to play and loop until it is explicitly released.
       */
      uint16_t lpctl : 1;
      /** Source Sample Control (Wait for zero crossings on looping).
       * When enabled, the AICA will wait for a zero crossing in the audio
       * waveform before looping back to the Loop Start Address (lsa). This can
       * help reduce clicks and pops that may occur when looping samples,
       * especially if the loop points are not perfectly aligned with zero
       * crossings. Enabling this feature can improve the sound quality of
       * loops, but it may introduce a slight delay when the AICA is waiting for
       * the next zero crossing to loop. It's generally recommended to enable
       * this when using loop control (lpctl=1) for smoother looping of audio
       * samples.
       */
      uint16_t ssctl : 1;
      uint16_t reserved : 3;
      /** Key On Action.
       * Controls note triggering and release:
       * - key_on=1, key_on_ex=0: Start or continue note playback.
       * - key_on=0, key_on_ex=0: No change; note continues as is.
       * - key_on=0, key_on_ex=1: Release note immediately (enter envelope
       * release).
       * - key_on=1, key_on_ex=1: Retrigger note from start.
       * key_on_ex takes precedence when set. */
      uint16_t key_on : 1;
      /** Key State flag. Must be 1 to process key_on/key_on_ex.
       * key_on_ex takes priority:
       * - key_on=0, key_on_ex=1: Release note (enter envelope release).
       * - key_on=1, key_on_ex=1: Retrigger note from start.
       *
       * @note Use with care, as abrupt changes may occur if envelope settings
       * are not suitable.
       */
      uint16_t key_on_ex : 1;
    } bits;
  } play_ctrl; // 0x00 - Play Control Register (Format, Address High,
               // Triggering)

  /** Sample Address Low (0x04)
   * Lower 16 bits of the sample start address in AICA wave memory.
   * Full address: (sa_high << 16) | sa_low (23 bits, up to 8 MB).
   * AICA wave memory is 2 MB; addresses must be within this range.
   * Used to fetch sample data for playback and looping.
   */
  uint16_t sa_low;
  /** Loop Start Address (0x08)
   * Offset (in samples) from sample start address.
   * Actual address: sa + (lsa * sample size in bytes).
   * Used for seamless looping when lpctl=1.
   */
  uint16_t lsa;
  /** Loop End Address (0x0C)
   * Offset (in samples) from sample start address.
   * Actual address: sa + (lea * sample size in bytes).
   * @note If looping (lpctl=1), playback jumps back to lsa at this point.
   */
  uint16_t lea;
  union {
    uint16_t raw;
    struct {
      /**
       * Attack Rate (0-31). Higher values cause the note to reach full volume
       * faster. A value of 0 means the note will never reach full volume (it
       * will stay at the initial level, which is typically silence), while 31
       * means it will reach full volume very quickly.
       *
       * @note The actual time for attack depends on the key rate scaling (KRS)
       * and the note's frequency, with higher notes attacking faster when KRS
       * is applied.
       */
      uint16_t ar : 5;
      uint16_t reserved1 : 1;

      /* Decay 1 Rate (0-31). Higher values cause the note to drop from full
       * volume to the sustain level faster. A value of 0 means the note will
       * never decay (it will stay at full volume), while 31 means it will decay
       * very quickly.
       *
       * @note The actual time for decay depends on the key rate scaling (KRS)
       * and the note's frequency, with higher notes decaying faster when KRS
       * is applied.
       */
      uint16_t d1r : 5;

      /* Decay 2 Rate (0-31). Higher values cause the note to drop from the
       * sustain level to silence faster. A value of 0 means the note will
       * never decay from the sustain level (it will stay at the sustain level),
       * while 31 means it will decay to silence very quickly.
       *
       * @note The actual time for decay depends on the key rate scaling (KRS)
       * and the note's frequency, with higher notes decaying faster when KRS
       * is applied.
       */
      uint16_t d2r : 5;
    } bits;
  } env_ad; // 0x10 - Envelope Attack/Decay Rate Register

  union {
    uint16_t raw;
    struct {
      /**
       * Release Rate (0-31). Triggered when key_on goes low. Higher values
       * cause the note to fade out faster. A value of 0 means the note will
       * sustain indefinitely until released, while 31 means it will release
       * very quickly.
       *
       * @note The actual time for release depends on the key rate
       * scaling (KRS) and the note's frequency, with higher notes releasing
       * faster when KRS is applied.
       */
      uint16_t rr : 5;
      /**
       * Sustain Level (0-31). Target level for D1R phase. Higher KRS values
       * cause the effective sustain level to be lower for higher notes, so
       * this is often set to maximum (31) and then modulated by KRS to achieve
       * the desired effect.
       */
      uint16_t dl : 5;

      /** Key Rate Scaling.
       * 
       * - 0 -> Minimum scaling
       * - E -> Maximum scaling
       * - F -> Scaling OFF.
       * 
       * Scaling is applied as a multiplier to the EG rates (AR, D1R, D2R, RR)
       * based on the note's frequency relative to the base frequency (root
       * note). 
       * 
       * @note Higher notes have faster rates, lower notes have slower rates.
       * This allows for natural-sounding decay across the keyboard.
       */
      uint16_t krs : 4;

      /**
       * Loop Sustain Level Note Lock. If set, the sustain level is locked to
       * the note's base frequency, so higher notes have a higher sustain level.
       * This can help maintain consistent timbre across the keyboard when using
       * high KRS values, which would otherwise cause higher notes to have very
       * low sustain levels.
       */
      uint16_t lpslnk : 1;
      uint16_t reserved : 1;
    } bits;
  } env_dr; // 0x14 - Envelope Release Rate and Sustain Level Register

  union {
    uint16_t raw;
    struct {
      /* Frequency Number (0-1023). The AICA uses this value, along with the
       * octave shift (OCT), to determine the pitch of the note. The actual
       * frequency is calculated as:
       * Freq = Base_Freq * 2^((OCT - 4) + (FNS / 1024))
       * where Base_Freq is typically 440 Hz for MIDI note A4. This allows for
       * fine-tuning of the pitch using FNS, while OCT provides coarse control.
       */
      uint16_t fns : 10;
      uint16_t reserved1 : 1;
      /* Octave Shift (Signed integer determining the base pitch).
       * The AICA uses this value, along with the frequency number (FNS), to
       * determine the pitch of the note. The actual frequency is calculated as:
       * Freq = Base_Freq * 2^((OCT - 4) + (FNS / 1024))
       * where Base_Freq is typically 440 Hz for MIDI note A4. OCT provides
       * coarse control over the pitch, allowing for octave shifts up or down.
       * The value is signed, so it can represent shifts both above and below
       * the base octave (OCT=4). For example, OCT=5 would shift up one octave,
       * while OCT=3 would shift down one octave.
       */
      uint16_t oct : 4;
      uint16_t reserved2 : 1;
    } bits;
  } pitch; // 0x18 - Pitch/Frequency Register

  union {
    uint16_t raw;
    struct {
      /* Amplitude LFO (ALFO) and Pitch LFO (PLFO) settings.
       * The AICA has a single LFO that can modulate either amplitude (tremolo)
       * or pitch (vibrato) based on the settings below. The depth and waveform
       * of the LFO can be configured independently for amplitude and pitch
       * modulation. The LFO frequency determines how fast the modulation
       * occurs, and the reset flag allows for synchronizing the LFO phase with
       * note triggering for consistent modulation effects.
       */
      uint16_t alfos : 3;

      /* Amplitude LFO Waveform Shape (0=Saw, 1=Square, 2=Triangle, 3=Random).
       * The AICA's LFO can produce different waveforms for modulation. The
       * selected waveform affects the character of the tremolo or vibrato
       * effect.
       * - Saw: Creates a ramp-up or ramp-down effect, depending on the phase.
       * - Square: Creates an on/off effect, where the modulation is either at
       *   full depth or no depth, creating a choppy tremolo or vibrato.
       * - Triangle: Creates a smooth up-and-down modulation effect, which is
       *   often used for natural-sounding tremolo or vibrato.
       * - Random: Creates a random modulation effect, which can add a sense of
       *   unpredictability or "humanization" to the sound.
       */
      uint16_t alfows : 2;

      /* Pitch LFO Depth (Sensitivity/Vibrato amount).
       * Controls the intensity of the pitch modulation when PLFO is active.
       * Higher values result in a more pronounced vibrato effect, while lower
       * values produce a subtler modulation. The actual pitch deviation in
       * cents can be calculated based on this value and the LFO frequency.
       */
      uint16_t plfos : 3;

      /* Pitch LFO Waveform Shape (0=Saw, 1=Square, 2=Triangle, 3=Random).
       * The AICA's LFO can produce different waveforms for pitch modulation.
       * The selected waveform affects the character of the vibrato effect.
       * - Saw: Creates a ramp-up or ramp-down effect, depending on the phase.
       * - Square: Creates an on/off effect, where the modulation is either at
       *   full depth or no depth, creating a choppy vibrato.
       * - Triangle: Creates a smooth up-and-down modulation effect, which is
       *   often used for natural-sounding vibrato.
       * - Random: Creates a random modulation effect, which can add a sense of
       *   unpredictability or "humanization" to the sound.
       */
      uint16_t plfows : 2;

      /* LFO Frequency (Speed of oscillation).
       * Determines how fast the LFO oscillates, affecting the rate of
       * modulation for both amplitude and pitch. Higher values result in faster
       * modulation, while lower values produce slower modulation.
       */
      uint16_t lfof : 5;

      /* LFO Reset. When set, the LFO phase resets to 0 on each key_on event.
       * This allows for consistent modulation effects that start at the same
       * point in the LFO cycle for each note, which can be important for
       * achieving a desired sound. For example, resetting the LFO can ensure
       * that the vibrato starts at the same pitch deviation for each note, or
       * that the tremolo starts at the same volume level for each note.
       */
      uint16_t lfore : 1;
    } bits;
  } lfo; // 0x1C - Low Frequency Oscillator Control

  union {
    uint16_t raw;
    struct {
      uint16_t reserved1 : 2;
      /* Specifies the mix register address for each slot when direct data is
         output to DAC. The AICA can route the output of each slot directly to
         the DAC for monitoring or special effects, bypassing the DSP's mix
         register (MIXS). When this bit is set, the slot's output is sent
         directly to the DAC instead of MIXS. This allows for unique routing
         possibilities, such as sending a slot's output to the DAC while still
         sending it to MIXS for processing by the DSP, or isolating a slot's
         output for direct monitoring. Care must be taken when using this
         feature, as it can affect the overall mix and may require adjustments
         to levels and panning to achieve the desired sound.
       */
      uint16_t isel : 4;

      /* Total Level (0-255). Controls the overall volume of the slot, with 0
       * being maximum volume and 255 being muted. The actual amount of
       * attenuation is determined by placing this value in the EG value, which
       * is used by the AICA's envelope generator to calculate the final output
       * level of the slot. Properly setting the Total Level is crucial for
       * balancing the mix and achieving the desired dynamics in the sound.
       */
      uint16_t tl : 8;
      uint16_t reserved2 : 2;
    } bits;
  } env_fm; // 0x20 - Envelope FM Control. Can modulate the EG rates and levels
            // of other channels.

  union {
    uint16_t raw;
    struct {
      /* Direct Pan. 0-15 Left to Center, 16-31 Center to Right.
       * Controls the stereo panning of the slot's output when sent directly to
       * the DAC. A value of 0 means the sound is fully panned to the left,
       * 15 means it is panned slightly left of center, 16 means it is
       * centered, 17 means it is panned slightly right of center, and 31
       * means it is fully panned to the right. Properly setting the direct pan
       * can help position sounds in the stereo field for a more immersive mix.
       */
      uint16_t dipan : 5;
      uint16_t reserved : 3;

      /* Direct Level and Mix Send Level. These control the levels of the slot's
       * output when sent to the DAC and the DSP mix register (MIXS),
       * respectively.
       * - disdl: Specifies the send level for each slot when direct data is
       * output to the DAC. The actual output level is determined by multiplying
       * this value by the Total Level (TL) value of env_fm. If disdl is set to
       * 0, the output is not sent to the DAC, but it still contributes to the
       *   MIXS sum for the DSP.
       * - imxl: Specifies the send level for each slot when sound slot output
       *   data is input to the DSP's mix register (MIXS). The actual send level
       *   is determined by multiplying this value by the Total Level (TL) value
       *   of env_fm. If imxl is set to 0, the output is not sent to MIXS, but
       * it still contributes to the MIXS sum for the DSP.
       */
      uint16_t disdl : 4;

      /* imxl: Specifies the send level for each slot when sound slot output
         data is input to the DSP's mix register (MIXS). The actual send level
         is determined by multiplying this value by the Total Level (TL) value
         of env_fm. If imxl is set to 0, the output is not sent to MIXS, but it
         still contributes to the MIXS sum for the DSP. This allows for
         flexible routing of the slot's output, enabling it to be sent to the
         DAC, MIXS, both, or neither, depending on the desired effect and mix
         balance.
       */
      uint16_t imxl : 4;
    } bits;
  } pan; // 0x24 - Channel Panning and Direct Level Control

  union {
    uint16_t raw;
    struct {
      /**
       * Resonance data Sets Q for the FEG filter. Values from
       * -3.00 through 20.25 dB can be set. The relationships
       * between bits and gain are as follows:
       *
       * +-------+----------+-------+----------+
       * |  DATA | GAIN (DB)|  DATA | GAIN (DB)|
       * +-------+----------+-------+----------+
       * | 11111 |    20.25 | 00100 |     0.00 |
       * +-------+----------+-------+----------+
       * | 11100 |    18.00 | 00011 |    -0.75 |
       * +-------+----------+-------+----------+
       * | 11000 |    15.00 | 00010 |    -1.50 |
       * +-------+----------+-------+----------+
       * | 10000 |     9.00 | 00001 |    -2.25 |
       * +-------+----------+-------+----------+
       * | 01100 |     6.00 | 00000 |    -3.00 |
       * +-------+----------+-------+----------+
       * | 01000 |     3.00 |       |          |
       * +-------+----------+-------+----------+
       * | 00110 |     1.50 |       |          |
       * +-------+----------+-------+----------+
       */
      uint16_t q : 5;
      uint16_t reserved : 11;
    } bits;
  } resonance; // 0x28 - Resonance data

  /* FEG (Filter Envelope Generator) Levels. These can modulate the cutoff
   * frequency of the FEG filter over time, allowing for dynamic filtering
   * effects that evolve with the note. The levels correspond to key points in
   * the envelope:
   * - flv0: Cutoff frequency at the time of attack start.
   * - flv1: Cutoff frequency at the time of attack end (decay start time).
   * - flv2: Cutoff frequency at the time of decay end (sustain start time).
   * - flv3: Cutoff frequency at the time of key off (release start).
   * - flv4: Cutoff frequency after release.
   *
   * Each level is a 14-bit value that sets the cutoff frequency of the FEG
   * filter at that point in the envelope. By setting these levels
   * appropriately, you can create a wide range of dynamic filtering effects,
   * such as a bright attack that quickly dulls down to a mellow sustain, or a
   * filter that opens up on release for a sweeping effect.
   */
  uint16_t flv0;
  /** @see flv0 */
  uint16_t flv1;
  /** @see flv0 */
  uint16_t flv2;
  /** @see flv0 */
  uint16_t flv3;
  /** @see flv0 */
  uint16_t flv4;

  union {
    uint16_t raw;
    struct {
      uint16_t reserved1 : 3;
      /* FEG Attack Rate (0-31). Higher values cause the FEG cutoff frequency to
       * reach the target level faster during the attack phase. A value of 0
       * means the cutoff frequency will never reach the target level (it will
       * stay at the initial level), while 31 means it will reach the target
       * level very quickly. The actual time for the FEG to reach the target
       * level depends on the key rate scaling (KRS) and the note's frequency,
       * with higher notes reaching the target level faster when KRS is applied.
       */
      uint16_t far : 5;
      uint16_t reserved2 : 3;

      /* FEG Decay 1 Rate (0-31). Higher values cause the FEG cutoff frequency
       * to transition from the attack level to the decay 1 level faster. A
       * value of 0 means the cutoff frequency will never transition (it will
       * stay at the attack level), while 31 means it will transition very
       * quickly. The actual time for the FEG to transition depends on the key
       * rate scaling (KRS) and the note's frequency, with higher notes
       * transitioning faster when KRS is applied.
       */
      uint16_t fd1r : 5;
    } bits;
  } env_feg; // 0x40 - Envelope FEG Control. Can modulate the FEG cutoff
             // frequency of other channels.
  union {
    uint16_t raw;
    struct {
      uint16_t reserved1 : 3;
      /* FEG Decay 2 Rate (0-31). Higher values cause the FEG cutoff frequency
       * to transition from the decay 1 level to the decay 2 level faster. A
       * value of 0 means the cutoff frequency will never transition (it will
       * stay at the decay 1 level), while 31 means it will transition very
       * quickly. The actual time for the FEG to transition depends on the key
       * rate scaling (KRS) and the note's frequency, with higher notes
       * transitioning faster when KRS is applied.
       */
      uint16_t fd2r : 5;
      uint16_t reserved2 : 3;

      /* FEG Release Rate (0-31). Higher values cause the FEG cutoff frequency
       * to transition from the decay 2 level to the release level faster when
       * the note is released (key_on goes low). A value of 0 means the cutoff
       * frequency will never transition (it will stay at the decay 2 level),
       * while 31 means it will transition very quickly. The actual time for the
       * FEG to transition depends on the key rate scaling (KRS) and the note's
       * frequency, with higher notes transitioning faster when KRS is applied.
       */
      uint16_t frr : 5;
    } bits;
  } env_feg2; // 0x44 - Additional Envelope FEG Control. Can modulate the FEG
              // cutoff frequency of other channels during decay 2 and release
              // phases.
} aica_chnl_packed_t;

#endif /* AFX_CHANNEL_REGS_H */