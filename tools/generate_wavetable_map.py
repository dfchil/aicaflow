import os
import json
import hashlib

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)

# Standard MIDI 128 Instruments
GM_INSTRUMENTS = {
    0: "Acoustic Grand Piano", 1: "Bright Acoustic Piano", 2: "Electric Grand Piano", 3: "Honky-tonk Piano",
    4: "Electric Piano 1", 5: "Electric Piano 2", 6: "Harpsichord", 7: "Clavinet", 8: "Celesta",
    9: "Glockenspiel", 10: "Music Box", 11: "Vibraphone", 12: "Marimba", 13: "Xylophone",
    14: "Tubular Bells", 15: "Dulcimer", 16: "Drawbar Organ", 17: "Percussive Organ", 18: "Rock Organ",
    19: "Church Organ", 20: "Reed Organ", 21: "Accordion", 22: "Harmonica", 23: "Tango Accordion",
    24: "Acoustic Guitar (nylon)", 25: "Acoustic Guitar (steel)", 26: "Electric Guitar (jazz)",
    27: "Electric Guitar (clean)", 28: "Electric Guitar (muted)", 29: "Overdriven Guitar",
    30: "Distortion Guitar", 31: "Guitar harmonics", 32: "Acoustic Bass", 33: "Electric Bass (finger)",
    34: "Electric Bass (pick)", 35: "Fretless Bass", 36: "Slap Bass 1", 37: "Slap Bass 2",
    38: "Synth Bass 1", 39: "Synth Bass 2", 40: "Violin", 41: "Viola", 42: "Cello", 43: "Contrabass",
    44: "Tremolo Strings", 45: "Pizzicato Strings", 46: "Orchestral Harp", 47: "Timpani",
    48: "String Ensemble 1", 49: "String Ensemble 2", 50: "Synth Strings 1", 51: "Synth Strings 2",
    52: "Choir Aahs", 53: "Voice Oohs", 54: "Synth Voice", 55: "Orchestra Hit", 56: "Trumpet",
    57: "Trombone", 58: "Tuba", 59: "Muted Trumpet", 60: "French Horn", 61: "Brass Section",
    62: "Synth Brass 1", 63: "Synth Brass 2", 64: "Soprano Sax", 65: "Alto Sax", 66: "Tenor Sax",
    67: "Baritone Sax", 68: "Oboe", 69: "English Horn", 70: "Bassoon", 71: "Clarinet", 72: "Piccolo",
    73: "Flute", 74: "Recorder", 75: "Pan Flute", 76: "Blown Bottle", 77: "Shakuhachi", 78: "Whistle",
    79: "Ocarina", 80: "Lead 1 (square)", 81: "Lead 2 (sawtooth)", 82: "Lead 3 (calliope)",
    83: "Lead 4 (chiff)", 84: "Lead 5 (charang)", 85: "Lead 6 (voice)", 86: "Lead 7 (fifths)",
    87: "Lead 8 (bass + lead)", 88: "Pad 1 (new age)", 89: "Pad 2 (warm)", 90: "Pad 3 (polysynth)",
    91: "Pad 4 (choir)", 92: "Pad 5 (bowed)", 93: "Pad 6 (metallic)", 94: "Pad 7 (halo)",
    95: "Pad 8 (sweep)", 96: "FX 1 (rain)", 97: "FX 2 (soundtrack)", 98: "FX 3 (crystal)",
    99: "FX 4 (atmosphere)", 100: "FX 5 (brightness)", 101: "FX 6 (goblins)", 102: "FX 7 (echoes)",
    103: "FX 8 (sci-fi)", 104: "Sitar", 105: "Banjo", 106: "Shamisen", 107: "Koto", 108: "Kalimba",
    109: "Bag pipe", 110: "Fiddle", 111: "Shanai", 112: "Tinkle Bell", 113: "Agogo", 114: "Steel Drums",
    115: "Woodblock", 116: "Taiko Drum", 117: "Melodic Tom", 118: "Synth Drum", 119: "Reverse Cymbal",
    120: "Guitar Fret Noise", 121: "Breath Noise", 122: "Seashore", 123: "Bird Tweet",
    124: "Telephone Ring", 125: "Helicopter", 126: "Applause", 127: "Gunshot"
}

KEYWORDS = {
    0: ["piano", "grand"], 4: ["rhodes"], 5: ["wurlitzer", "wurli"], 10: ["music box"],
    12: ["marimba"], 24: ["nylon"], 25: ["steel", "acoustic guitar"], 32: ["upright", "acoustic bass"],
    38: ["acid", "saw"], 40: ["violin"], 42: ["cello"], 48: ["strings"], 52: ["choir", "vocals"],
    56: ["trumpet"], 60: ["horn"], 61: ["brass"], 66: ["sax"], 71: ["clarinet"], 73: ["flute"],
    80: ["square"], 81: ["sawtooth"], 89: ["warm", "chill"], 104: ["sitar", "tambura"]
}

ROOT_DIR = os.path.join(REPO_ROOT, "input", "wavetable_collections")
MAP_FILE = os.path.join(REPO_ROOT, "input", "wavetables.map")

def get_gm_index(filename, path):
    filename = filename.lower()
    path = path.lower()
    best_idx = None
    max_score = 0
    
    for idx, keys in KEYWORDS.items():
        score = 0
        for key in keys:
            if key in filename or key in path:
                score += len(key)
        if score > max_score:
            max_score = score
            best_idx = idx
            
    if best_idx is None:
        if "lead" in path or "saw" in filename: return 81
        if "pad" in path or "warm" in filename: return 89
        if "bass" in filename: return 38
        if "vocal" in path or "choir" in filename: return 52
        
    return best_idx

def main():
    wavetable_map = []
    
    print(f"Scanning {ROOT_DIR}...")
    for root, dirs, files in os.walk(ROOT_DIR):
        for f in files:
            if f.endswith(".wav"):
                full_path = os.path.join(root, f)
                gm_idx = get_gm_index(f, root)
                
                # Generate a unique 32-bit ID from path hash
                wave_id = int(hashlib.md5(full_path.encode()).hexdigest(), 16) & 0xFFFFFFFF
                
                entry = {
                    "id": wave_id,
                    "gm_idx": gm_idx,
                    "gm_name": "Unknown" if gm_idx is None else GM_INSTRUMENTS.get(gm_idx, "Unknown"),
                    "filename": f,
                    "rel_path": os.path.relpath(full_path, REPO_ROOT),
                    "source": os.path.basename(os.path.dirname(root))
                }
                wavetable_map.append(entry)

    os.makedirs(os.path.dirname(MAP_FILE), exist_ok=True)
    with open(MAP_FILE, "w") as f:
        json.dump(wavetable_map, f, indent=4)
    
    print(f"Generated {MAP_FILE} with {len(wavetable_map)} entries.")

if __name__ == "__main__":
    main()
