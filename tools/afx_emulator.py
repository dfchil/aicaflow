import struct
import json
import numpy as np
from scipy.io import wavfile
import os
import sys

# AICA Hardware Constants (Derived from MAME/Flycast)
SAMPLE_RATE = 44100
AICA_CLOCK = 44100

AFX_MAGIC = 0xA1CAF200
AFX_VERSION = 1
SECT_FLOW = 0x574F4C46  # FLOW
SECT_SDES = 0x53454453  # SDES

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
            # Parse lean header (20 bytes): Magic(4), Version(4), SecCount(4), TotalTicks(4), Flags(4)
            hdr_bin = f.read(20)
            if len(hdr_bin) < 20:
                print("Error: Invalid AFX header size")
                return

            magic, ver, section_count, ticks, flags = struct.unpack('<IIIII', hdr_bin)

            if magic != AFX_MAGIC:
                print(f"Error: Invalid Magic 0x{magic:08X}")
                return
            if ver != AFX_VERSION:
                print(f"Error: Unsupported AFX version {ver}")
                return

            # Section table follows immediately after 20-byte header
            sections = []
            for i in range(section_count):
                entry = f.read(24)
                if len(entry) < 24:
                    break
                sid, off, size, count, align, sflags = struct.unpack('<IIIIII', entry)
                sections.append({'id': sid, 'off': off, 'size': size, 'count': count})

            sdes = next((s for s in sections if s['id'] == SECT_SDES), None)
            flow = next((s for s in sections if s['id'] == SECT_FLOW), None)
            if not sdes or not flow:
                print("Error: Missing required SDES/FLOW sections")
                return

            desc_off = sdes['off']
            desc_cnt = sdes['count']
            st_off = flow['off']
            st_sz = flow['count']

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
            # Sample offset in descriptor is still section-relative.
            offset_to_entry = {}
            for d in descs:
                path = self.wavetable_map.get(d['source_id'])
                if path:
                    offset_to_entry[d['sample_off']] = (path, d)

            # Pre-load required waveforms
            loaded_waveforms = {}
            for path, _ in offset_to_entry.values():
                if path not in loaded_waveforms:
                    pcm, rate = self.load_wav_pcm(path)
                    if pcm is not None:
                        loaded_waveforms[path] = (pcm, rate)

            # Load flow commands — count is from section header
            f.seek(flow['off'])
            flow_cmds = []
            for _ in range(flow['count']):
                op_bin = f.read(12)
                if len(op_bin) < 12: break
                ts, slot, reg, pad, val = struct.unpack('<IBBHI', op_bin)
                flow_cmds.append({'ts': ts, 'slot': slot, 'reg': reg, 'val': val})

        # Simulation Initialization
        # Duration in samples calculated from max_timestamp (ticks)
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
                'loop_end_samp': 0,
                'sa_base': 0
            })

        # Render process
        op_idx = 0
        for i in range(total_samples):
            ms = (i / SAMPLE_RATE) * 1000.0
            
            # Process all flow commands due for this timestamp
            while op_idx < len(flow_cmds) and flow_cmds[op_idx]['ts'] <= ms:
                op = flow_cmds[op_idx]
                op_idx += 1
                
                s_id = op['slot']
                if s_id >= 64: continue
                s = slots[s_id]
                
                reg = op['reg']
                val = op['val']
                
                # Register Mappings (New Layout)
                # 0x00: SA_HI & CTL (Bit 15: KYON, Bit 14: KYOFF)
                # 0x01: SA_LO
                # 0x02: LSA
                # 0x03: LEA
                if reg == 0x00: # SA_HI & CTL
                    # Track Sample Address (Bits 0-6 of SA_HI | Bits 0-15 of SA_LO)
                    s['sa_base'] = (s['sa_base'] & 0x00FFFF) | ((val & 0x7F) << 16)

                    if val & (1 << 15): # KEY ON
                        # Resolution of Sample based on combined SA address
                        entry = offset_to_entry.get(s['sa_base'])
                        if entry:
                            path, d = entry
                            s['program'] = d['gm_program']
                            s['orig_rate'] = d['sample_rate']
                            s['loop_mode'] = d['loop_mode']
                            fmt = d['format']
                            bps = 2.0 if fmt == 0 else (1.0 if fmt == 1 else 0.5)
                            s['loop_start_samp'] = int(d['loop_start'] / bps)
                            s['loop_end_samp']   = int(d['loop_end']   / bps) if d['loop_end'] > 0 else 0
                            if path in loaded_waveforms:
                                s['pcm'], _ = loaded_waveforms[path]
                        
                        s['active'] = True
                        s['pos'] = 0.0
                    elif val & (1 << 14): # KEY OFF
                        s['active'] = False
                elif reg == 0x01: # SA_LO
                    s['sa_base'] = (s['sa_base'] & 0x7F0000) | (val & 0xFFFF)
                elif reg == 0x0C: # FNS_OCT
                    s['freq_mult'] = self.aica_pitch_to_freq_mult(val)
                elif reg == 0x0D: # TOT_LVL
                    # AICA Total Level (0=loudest, 255=silent). Invert to 0.0..1.0
                    s['vol'] = (255 - (val & 0xFF)) / 255.0
                elif reg == 0x0E: # PAN_VOL
                    # PAN_VOL top bits [15:13] might be volume? (AICA specific)
                    # For now just use TOT_LVL for gain simulation
                    pass

            # Perform synthesis for this sample
            for s in slots:
                if s['active'] and s['pcm'] is not None:
                    # Calculate playback increment
                    # Hardware frequency = orig_rate * freq_mult
                    # Sample step = Hardware freq / Simulation Sample Rate
                    step = (s['orig_rate'] * s['freq_mult']) / SAMPLE_RATE
                    pcm_len = len(s['pcm'])
                    loop_end = s['loop_end_samp'] if s['loop_end_samp'] > 0 else pcm_len
                    loop_end = min(loop_end, pcm_len)
                    idx = int(s['pos'])
                    
                    if idx >= loop_end:
                        if s['loop_mode'] != 0:  # AFX_LOOP_NONE=0
                            s['pos'] = float(s['loop_start_samp'])
                            idx = int(s['pos'])
                        else:
                            s['active'] = False
                            continue
                            
                    if idx < pcm_len:
                        frac = s['pos'] - idx
                        next_idx = min(idx + 1, pcm_len - 1)
                        sample = s['pcm'][idx] * (1.0 - frac) + s['pcm'][next_idx] * frac
                        mix_buffer[i] += sample * s['vol'] * 0.25
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
