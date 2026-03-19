import struct
import json
import numpy as np
from scipy.io import wavfile
import os
import sys

# AICA Hardware Constants (Derived from MAME/Flycast)
SAMPLE_RATE = 44100
AICA_CLOCK = 44100

class AFXEmulator:
    def __init__(self, afx_path, map_path):
        self.afx_path = afx_path
        with open(map_path, 'r') as f:
            data = json.load(f)
            # Map by ID for quick reverse lookup
            self.wavetable_map = {item['id']: item['rel_path'] for item in data}
        
    def aica_pitch_to_freq_mult(self, fns_oct):
        # MAME/Flycast logic: (FNS / 1024 + 1) * 2^(OCT)
        fns = fns_oct & 0x3FF
        octave = (fns_oct >> 10) & 0xF
        if octave >= 8: 
            octave -= 16 # Sign extend 4-bit OCT
        return (fns / 1024.0 + 1.0) * (2.0 ** octave)

    def load_wav_pcm(self, path):
        try:
            sample_rate, data = wavfile.read(path)
            if data.dtype != np.int16:
                data = (data * 32767).astype(np.int16)
            if len(data.shape) > 1:
                data = data[:, 0] # Mono only for now
            return data.astype(np.float32) / 32768.0, sample_rate
        except Exception as e:
            print(f"Error loading {path}: {e}")
            return None, 44100

    def render(self, output_wav):
        print(f"Simulating AFX Hardware Playback: {self.afx_path}")
        with open(self.afx_path, 'rb') as f:
            # Parse v2 header: 13 x uint32 = 52 bytes
            hdr_bin = f.read(52)
            if len(hdr_bin) < 52:
                print("Error: Invalid AFX header size")
                return

            (magic, ver,
             s_data_off, s_data_sz,
             desc_off, desc_cnt,
             st_off, st_sz,
             dsp_m_off, dsp_m_sz,
             dsp_c_off, dsp_c_sz,
             ticks) = struct.unpack('<IIIIIIIIIIIII', hdr_bin)

            if magic != 0xA1CAF200:
                print(f"Error: Invalid Magic 0x{magic:08X}")
                return

            # Load sample descriptors (afx_sample_desc_t = 32 bytes each)
            # source_id(4), gm_program(1), format(1), loop_mode(1), root_note(1),
            # fine_tune(1), reserved[3](3), sample_off(4), sample_size(4),
            # loop_start(4), loop_end(4), sample_rate(4)
            f.seek(desc_off)
            descs = []
            for _ in range(desc_cnt):
                entry_bin = f.read(32)
                if len(entry_bin) < 32: break
                (sid, prog, fmt, loop_mode, root_note,
                 fine_tune, _r0, _r1, _r2,
                 sample_off, sample_size, loop_start, loop_end, sample_rate) = struct.unpack('<IBBBBbBBBIIIII', entry_bin)
                descs.append({
                    'source_id': sid, 'gm_program': prog, 'format': fmt,
                    'loop_mode': loop_mode, 'root_note': root_note, 'fine_tune': fine_tune,
                    'sample_off': sample_off, 'sample_size': sample_size,
                    'loop_start': loop_start, 'loop_end': loop_end, 'sample_rate': sample_rate
                })

            # Map blob-local offset -> (wav_path, descriptor)
            offset_to_entry = {}
            for desc in descs:
                path = self.wavetable_map.get(desc['source_id'])
                if path:
                    offset_to_entry[desc['sample_off']] = (path, desc)

            # Pre-load required waveforms
            loaded_waveforms = {}
            for path, _ in offset_to_entry.values():
                if path not in loaded_waveforms:
                    pcm, rate = self.load_wav_pcm(path)
                    if pcm is not None:
                        loaded_waveforms[path] = (pcm, rate)

            # Load opcodes — st_sz is entry count in v2 (not byte count)
            # afx_opcode_t: timestamp(4), slot(1), reg(1), pad(2), value(4) = 12 bytes
            f.seek(st_off)
            opcodes = []
            for _ in range(st_sz):
                op_bin = f.read(12)
                if len(op_bin) < 12: break
                ts, slot, reg, pad, val = struct.unpack('<IBBHI', op_bin)
                opcodes.append({'ts': ts, 'slot': slot, 'reg': reg, 'val': val})

        # Simulation Initialization
        # Duration in samples based on max_timestamp in header
        total_samples = int((ticks / 1000.0) * SAMPLE_RATE) + (SAMPLE_RATE // 2) 
        mix_buffer = np.zeros(total_samples, dtype=np.float32)
        
        # Hardware Racks (64 Slots)
        slots = []
        for i in range(64):
            slots.append({
                'active': False,
                'pcm': None,
                'orig_rate': 44100,
                'freq_mult': 1.0,
                'pos': 0.0,
                'program': 0,
                'vol': 1.0,
                'loop_mode': 0,
                'loop_start_samp': 0,
                'loop_end_samp': 0
            })

        # Render process
        op_idx = 0
        for i in range(total_samples):
            ms = (i / SAMPLE_RATE) * 1000.0
            
            # Process all opcodes due for this timestamp
            while op_idx < len(opcodes) and opcodes[op_idx]['ts'] <= ms:
                op = opcodes[op_idx]
                op_idx += 1
                
                s_id = op['slot']
                if s_id >= 64: continue
                s = slots[s_id]
                
                reg = op['reg']
                val = op['val']
                
                if reg == 0x00: # CTL / KYON
                    if val & (1 << 15): # KEY ON
                        s['active'] = True
                        s['pos'] = 0.0
                    elif val & (1 << 14): # KEY OFF
                        s['active'] = False
                elif reg == 0x02: # SA_LO — blob-local offset identifies the sample
                    entry = offset_to_entry.get(val)
                    if entry:
                        path, desc = entry
                        s['program'] = desc['gm_program']
                        s['orig_rate'] = desc['sample_rate']
                        s['loop_mode'] = desc['loop_mode']
                        # Convert loop byte offsets to sample indices based on format
                        # AFX_FMT_PCM16=0 (2 bps), AFX_FMT_PCM8=1 (1 bps), AFX_FMT_ADPCM=3 (0.5 bps)
                        fmt = desc['format']
                        bps = 2.0 if fmt == 0 else (1.0 if fmt == 1 else 0.5)
                        s['loop_start_samp'] = int(desc['loop_start'] / bps)
                        s['loop_end_samp']   = int(desc['loop_end']   / bps) if desc['loop_end'] > 0 else 0
                        if path in loaded_waveforms:
                            s['pcm'], _ = loaded_waveforms[path]
                elif reg == 0x0C: # FNS_OCT
                    s['freq_mult'] = self.aica_pitch_to_freq_mult(val)

            # Perform synthesis for this sample
            for s in slots:
                if not (s['active'] and s['pcm'] is not None):
                    continue
                else:
                    # Calculate playback increment
                    # Hardware frequency = orig_rate * freq_mult
                    # Sample step = Hardware freq / Simulation Sample Rate
                    step = (s['orig_rate'] * s['freq_mult']) / SAMPLE_RATE
                    pcm_len = len(s['pcm'])
                    loop_end = s['loop_end_samp'] if s['loop_end_samp'] > 0 else pcm_len
                    loop_end = min(loop_end, pcm_len)
                    idx = int(s['pos'])
                    if idx >= loop_end:
                        if s['loop_mode'] != 0:  # forward or bidir: wrap to loop_start
                            s['pos'] = float(s['loop_start_samp'])
                            idx = s['loop_start_samp']
                        else:
                            s['active'] = False
                            continue
                    frac = s['pos'] - idx
                    next_idx = min(idx + 1, loop_end - 1)
                    sample = s['pcm'][idx] * (1.0 - frac) + s['pcm'][next_idx] * frac
                    mix_buffer[i] += sample * 0.25
                    s['pos'] += step

        # Export result
        max_val = np.max(np.abs(mix_buffer))
        if max_val > 0:
            mix_buffer = mix_buffer / max_val
            
        wav_data = (mix_buffer * 32767).astype(np.int16)
        wavfile.write(output_wav, SAMPLE_RATE, wav_data)
        print(f"Simulation Complete: {output_wav}")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python3 afx_emulator.py <file.afx> <wavetables.map> [output.wav]")
        sys.exit(1)
        
    afx_f = sys.argv[1]
    map_f = sys.argv[2]
    out_f = sys.argv[3] if len(sys.argv) > 3 else "render.wav"
    
    emu = AFXEmulator(afx_f, map_f)
    emu.render(out_f)
