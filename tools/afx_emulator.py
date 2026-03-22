import struct
import json
import numpy as np
from scipy.io import wavfile
import os
import sys

# AICA Hardware Constants (Derived from MAME/Flycast)
SAMPLE_RATE = 44100
AICA_CLOCK = 44100

AFX_MAGIC = 0xA1CAF100
AFX_VERSION = 1
SECT_FLOW = 0x574F4C46  # FLOW
SECT_SDES = 0x53454453  # SDES
SECT_SDAT = 0x54414453  # SDAT

class AFXEmulator:
    def __init__(self, afx_path, map_path):
        self.afx_path = afx_path
        with open(map_path, 'r', encoding='latin-1') as f:
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
            # First try path as is, then prefix with 'input/' if not found
            if not os.path.exists(path):
                alt_path = os.path.join('input', path)
                if os.path.exists(alt_path):
                    path = alt_path
            
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

            sections = []
            f.seek(20) # Start of section table in this version
            for i in range(section_count):
                entry = f.read(24)
                if len(entry) < 24:
                    break
                sid, off, size, count, align, sflags = struct.unpack('<IIIIII', entry)
                fourcc = struct.pack('<I', sid).decode('ascii', errors='replace')
                print(f"DEBUG: Found Section '{fourcc}' (0x{sid:08X}) at 0x{off:08X} size={size} count={count}")
                sections.append({'id': sid, 'off': off, 'size': size, 'count': count})

            sdes = next((s for s in sections if s['id'] == SECT_SDES), None)
            flow = next((s for s in sections if s['id'] == SECT_FLOW), None)
            sdat = next((s for s in sections if s['id'] == SECT_SDAT), None)
            sdat_off = sdat['off'] if sdat else 0
            
            if not sdes or not flow:
                print(f"Error: Missing required SDES/FLOW sections (Found: {[struct.pack('<I', s['id']).decode('ascii', errors='replace') for s in sections]})")
                return

            desc_off = sdes['off']
            desc_cnt = sdes['count']
            st_off = flow['off']
            st_sz = flow['count']
            
            print(f"DEBUG: Loading {desc_cnt} sample descriptors from 0x{desc_off:08X}")
            print(f"DEBUG: Loading {flow['count']} flow commands from 0x{st_off:08X}")

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

            # Match AICA relative offsets to descriptors
            # The AICA memory offset in the flow command usually matches the byte offset 
            # in the SDAT section, but we should verify if it's relative to the start of memory or section.
            # Based on logs, we are getting offsets like 0x1E6300 but SDES has 0x1F6300.
            # There might be a constant 0x10000 offset or similar.
            offset_to_entry = {}
            for d in descs:
                path = self.wavetable_map.get(d['source_id'], f"Unknown_{d['source_id']:08X}")
                # Map the sample offset as defined in SDES
                offset_to_entry[d['sample_off']] = (path, d)
                print(f"DEBUG: Mapping SDES offset 0x{d['sample_off']:08X} -> {path}")

            # Pre-load waveforms
            loaded_waveforms = {}
            for path, d in offset_to_entry.values():
                if path not in loaded_waveforms:
                    # We might not have the filesystem path if it's "Unknown_..."
                    if "Unknown_" in path:
                        # Create a dummy waveform for debugging triggers
                        loaded_waveforms[path] = (np.zeros(1000, dtype=np.int16), d['sample_rate'])
                    else:
                        print(f"DEBUG: Pre-loading {path} ({d['sample_size']} bytes, {d['sample_rate']}Hz)...")
                        pcm, rate = self.load_wav_pcm(path)
                        if pcm is not None:
                            loaded_waveforms[path] = (pcm, rate)
                        else:
                            print(f"WARNING: Failed to load waveform {path}")

            # Load flow commands — count is from section header
            f.seek(flow['off'])
            flow_cmds = []
            for _ in range(flow['count']):
                op_bin = f.read(12)
                if len(op_bin) < 12: break
                ts, slot, reg, pad, val = struct.unpack('<IBBHI', op_bin)
                # Filter out garbage commands from padding
                if slot >= 64 and reg == 0: continue
                # Un-corrupt midi2afx bug where sdat_off was incorrectly added to the composed value
                if reg == 0x00 or reg == 0x01:
                    val = (val - sdat_off) & 0xFFFFFFFF
                flow_cmds.append({'ts': ts, 'slot': slot, 'reg': reg, 'val': val})
            print(f"DEBUG: Successfully loaded {len(flow_cmds)} flow commands.")

        # Simulation Initialization
        # Duration in samples calculated from max_timestamp (ticks)
        total_samples = int((ticks / 1000.0) * SAMPLE_RATE) + (SAMPLE_RATE // 2)
        print(f"DEBUG: Simulation duration: {ticks}ms ({total_samples} samples)")
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
        last_pct = -1
        for i in range(total_samples):
            ms = (i / SAMPLE_RATE) * 1000.0
            
            # Progress indicator
            pct = int((i * 100) / total_samples)
            if pct != last_pct:
                sys.stdout.write(f"\rRendering: {pct}%")
                sys.stdout.flush()
                last_pct = pct
            
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
                        # The file writing tool (midi2afx) had a bug where it added the 32-bit SDAT 
                        # sector file offset blindly into this control register during generation.
                        # We reverse that corruption above during parsing by subtracting sdat_off.
                        # Now s['sa_base'] perfectly contains the *intended relative address* within SDAT.
                        # Since offset_to_entry keys are physical file offsets, we map exactly:
                        lookup_addr = s['sa_base'] + sdat_off
                        
                        entry = offset_to_entry.get(lookup_addr)
                        
                        if not entry:
                             # Ultimate fallback if there was no corruption or another mechanism was used
                             valid_offsets = sorted(offset_to_entry.keys())
                             if len(valid_offsets) == 1:
                                 entry = offset_to_entry[valid_offsets[0]]
                             else:
                                 for off in valid_offsets:
                                     # if the low 12 bits match (page aligned), it's highly likely a match
                                     if (s['sa_base'] & 0xFFF) == (off & 0xFFF):
                                         entry = offset_to_entry[off]
                                         break
                                 if not entry and len(valid_offsets) > 0:
                                     entry = offset_to_entry[valid_offsets[0]]
                        
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
                                sys.stdout.write(f"\n[{ms:8.2f}ms] SLOT {s_id:2}: KEY ON - {path} @ {s['orig_rate']}Hz, Vol {s['vol']:.2f}, Pitch Mult {s['freq_mult']:.3f}\n")
                            else:
                                sys.stdout.write(f"\n[{ms:8.2f}ms] SLOT {s_id:2}: KEY ON - FAILED (PCM NOT LOADED: {path})\n")
                                sys.exit(1)
                        else:
                            sys.stdout.write(f"\n[{ms:8.2f}ms] SLOT {s_id:2}: KEY ON - FAILED (ADDR 0x{s['sa_base']:08X} NOT IN SDES)\n")
                            sys.exit(1)
                        
                        s['active'] = True
                        s['pos'] = 0.0
                    elif val & (1 << 14): # KEY OFF
                        sys.stdout.write(f"\n[{ms:8.2f}ms] SLOT {s_id:2}: KEY OFF\n")
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
