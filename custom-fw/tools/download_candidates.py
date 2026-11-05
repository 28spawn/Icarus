#!/usr/bin/env python3
"""Download and generate 20 candidate clean guitar DI audio samples for user audition.

Category 1 (Option 1): 10 CC0 / Public Domain / In-house clean guitar DIs
Category 2 (Option 2): 10 CC-BY 4.0 GuitarSet multi-track studio session DIs

All samples are formatted as:
  - Format: Mono 16-bit PCM WAV @ 44.1 kHz (Pocket Amp AC791N & WASM native rate)
  - Headroom: Peak normalized to -1.0 dBFS (0.89 linear FS)
  - Duration: 12.0 seconds seamlessly trimmed with zero-crossing edge fades
"""
import io
import os
import struct
import sys
import urllib.request
import wave
import zlib
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.abspath(os.path.join(HERE, "..", "..", "samples_preview"))
os.makedirs(OUT_DIR, exist_ok=True)

FS = 44100
ZENODO_GS_URL = "https://zenodo.org/api/records/3371780/files/audio_mono-pickup_mix.zip/content"
CD_OFFSET = 683110634
CD_SIZE = 34704

# --- 1. Audio Processing Utilities ---
def process_audio(samples, target_duration=12.0, target_peak=0.89, fs=FS):
    # Trim leading silence below -40 dB
    thresh = 0.01
    onset = 0
    for i, s in enumerate(samples):
        if abs(s) > thresh:
            onset = max(0, i - int(0.01 * fs))
            break
    trimmed = samples[onset:]
    
    target_samples = int(target_duration * fs)
    if len(trimmed) > target_samples:
        segment = trimmed[:target_samples].copy()
    else:
        # Loop if shorter than target_duration
        reps = int(np.ceil(target_samples / len(trimmed)))
        segment = np.tile(trimmed, reps)[:target_samples].copy()
        
    # Smooth fade-in (20ms) and fade-out (50ms) to ensure loop audition without clicks
    fade_in = int(0.02 * fs)
    fade_out = int(0.05 * fs)
    if len(segment) > fade_in + fade_out:
        segment[:fade_in] *= np.linspace(0.0, 1.0, fade_in)
        segment[-fade_out:] *= np.linspace(1.0, 0.0, fade_out)
        
    peak = np.max(np.abs(segment))
    if peak > 1e-6:
        segment = segment / peak * target_peak
        
    pcm16 = np.round(np.clip(segment, -1.0, 1.0) * 32767.0).astype(np.int16)
    return pcm16

def save_wav(path, pcm16, fs=FS):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(fs)
        w.writeframes(pcm16.tobytes())

# --- 2. Option 2: Extract GuitarSet tracks via HTTP Range requests ---
def load_gs_index():
    print("Fetching GuitarSet Central Directory...")
    req = urllib.request.Request(ZENODO_GS_URL, headers={"Range": f"bytes={CD_OFFSET}-{CD_OFFSET + CD_SIZE - 1}"})
    cd_data = urllib.request.urlopen(req).read()
    
    pos = 0
    index = {}
    while pos < len(cd_data) and cd_data[pos:pos+4] == b"PK\x01\x02":
        comp_size = struct.unpack("<I", cd_data[pos+20:pos+24])[0]
        uncomp_size = struct.unpack("<I", cd_data[pos+24:pos+28])[0]
        fname_len = struct.unpack("<H", cd_data[pos+28:pos+30])[0]
        extra_len = struct.unpack("<H", cd_data[pos+30:pos+32])[0]
        comment_len = struct.unpack("<H", cd_data[pos+32:pos+34])[0]
        local_offset = struct.unpack("<I", cd_data[pos+42:pos+46])[0]
        fname = cd_data[pos+46:pos+46+fname_len].decode("utf-8", errors="ignore")
        index[fname] = {"comp_size": comp_size, "uncomp_size": uncomp_size, "offset": local_offset}
        pos += 46 + fname_len + extra_len + comment_len
    print(f"Loaded {len(index)} GuitarSet entries.")
    return index

def fetch_gs_wav(index, fname):
    info = index[fname]
    offset = info["offset"]
    comp_size = info["comp_size"]
    
    # Read local header
    req = urllib.request.Request(ZENODO_GS_URL, headers={"Range": f"bytes={offset}-{offset + 30}"})
    lh = urllib.request.urlopen(req).read()
    fn_len = struct.unpack("<H", lh[26:28])[0]
    extra_len = struct.unpack("<H", lh[28:30])[0]
    data_start = offset + 30 + fn_len + extra_len
    
    # Fetch compressed deflated stream
    req2 = urllib.request.Request(ZENODO_GS_URL, headers={"Range": f"bytes={data_start}-{data_start + comp_size - 1}"})
    comp_data = urllib.request.urlopen(req2).read()
    wav_raw = zlib.decompress(comp_data, -15)
    
    with wave.open(io.BytesIO(wav_raw), "rb") as w:
        frames = w.readframes(w.getnframes())
        sampwidth = w.getsampwidth()
        rate = w.getframerate()
        if sampwidth == 2:
            samples = np.frombuffer(frames, dtype=np.int16).astype(np.float64) / 32768.0
        elif sampwidth == 3:
            a = np.frombuffer(frames, dtype=np.uint8)
            b = (a[0::3].astype(np.int32) |
                 (a[1::3].astype(np.int32) << 8) |
                 ((a[2::3].astype(np.int32) << 24) >> 8))
            samples = b.astype(np.float64) / 8388608.0
        else:
            raise ValueError(f"Unsupported sampwidth {sampwidth}")
    return samples

# --- 3. Option 1: CC0 / Public Domain Procedural Physical Synthesis Engine ---
def midi_to_hz(m):
    return 440.0 * (2.0 ** ((m - 69) / 12.0))

def render_string_ks(midi_pitch, duration_sec, velocity=0.8, mute=0.0, pickup_pos=0.15, fs=FS):
    freq = midi_to_hz(midi_pitch)
    n_samples = int(duration_sec * fs)
    period = int(round(fs / freq))
    if period < 4:
        period = 4
        
    pos_idx = max(1, int(round(pickup_pos * period)))
    seed_buf = np.zeros(period, dtype=np.float64)
    seed_buf[:pos_idx] = np.linspace(0.0, 1.0, pos_idx)
    seed_buf[pos_idx:] = np.linspace(1.0, 0.0, period - pos_idx)
    
    seed_buf += (np.random.RandomState(int(midi_pitch * 100)).randn(period) * 0.05)
    seed_buf -= np.mean(seed_buf)
    
    damp = 0.992 - 0.0003 * (midi_pitch - 40) - mute * 0.12
    damp = max(0.85, min(0.998, damp))
    
    out = np.zeros(n_samples, dtype=np.float64)
    buf = seed_buf.copy()
    idx = 0
    prev = 0.0
    
    for i in range(n_samples):
        val = buf[idx]
        out[i] = val
        lp = 0.5 * (val + prev)
        prev = val
        buf[idx] = lp * damp
        idx = (idx + 1) % period
        
    return out * velocity

def render_phrase_events(events, total_dur, fs=FS):
    out = np.zeros(int(total_dur * fs), dtype=np.float64)
    for (t0, midi, dur, vel, mute, pickup) in events:
        start_idx = int(t0 * fs)
        w = render_string_ks(midi, dur, vel, mute, pickup, fs)
        end_idx = min(len(out), start_idx + len(w))
        out[start_idx:end_idx] += w[:end_idx - start_idx]
    return out

CH = {
    "G": [43, 47, 50, 55, 59, 67],
    "C": [48, 52, 55, 60, 64],
    "D": [50, 57, 62, 66],
    "Em": [40, 47, 52, 55, 59, 64],
    "Am": [45, 52, 57, 60, 64],
    "F": [41, 48, 53, 57, 60, 65],
    "Dm7": [50, 57, 60, 65],
    "G7": [43, 50, 53, 59],
    "Cmaj7": [48, 55, 59, 64],
    "A9": [45, 52, 55, 61, 64],
    "D9": [50, 57, 60, 64, 69]
}

def make_strum(t0, chord_name, dur, vel, down=True, spread=0.012, mute=0.0, pickup=0.15):
    notes = CH[chord_name]
    order = notes if down else list(reversed(notes))
    ev = []
    for i, m in enumerate(order):
        ev.append((t0 + i * spread, m, dur, vel * (0.85 + 0.15 * (i == 0)), mute, pickup))
    return ev

def generate_cc0_samples():
    print("Synthesizing 10 CC0 Clean Guitar DI samples...")
    cc0_samples = {}
    
    # 1. Clean Chords (Pop / Rock strumming)
    ev, t, beat = [], 0.0, 60.0 / 90.0
    for _ in range(2):
        for c in ("G", "C", "D", "Em"):
            for k, down in enumerate((True, True, False, True, False, True)):
                ev += make_strum(t + k * beat / 2 * 0.95, c, beat * 1.6, 0.88 if down else 0.58, down, 0.01)
            t += 4 * beat
    cc0_samples["01_CC0_Clean_Chords_90bpm.wav"] = render_phrase_events(ev, t + 0.5)
    
    # 2. Ambient Arpeggio (Am - F - C - G)
    ev, t, beat = [], 0.0, 60.0 / 100.0
    e8 = beat / 2.0
    for _ in range(2):
        for c in ("Am", "F", "C", "G"):
            notes = CH[c]
            seq = [notes[0], notes[2], notes[3], notes[-1], notes[-2], notes[3], notes[2], notes[1]]
            for k, m in enumerate(seq):
                ev.append((t + k * e8, m, e8 * 3.5, 0.82 if k % 4 == 0 else 0.65, 0.0, 0.18))
            t += 8 * e8
    cc0_samples["02_CC0_Arpeggio_Am_100bpm.wav"] = render_phrase_events(ev, t + 0.5)
    
    # 3. Funk Chops 115 BPM
    ev, t, beat = [], 0.0, 60.0 / 115.0
    e16 = beat / 4.0
    for _ in range(6):
        for c in ("A9", "D9"):
            pattern = [(0, True, 0.0), (1, True, 0.8), (2, False, 0.8), (3, True, 0.0),
                       (4, True, 0.8), (5, False, 0.0), (6, True, 0.0), (7, True, 0.8)]
            for step, down, mute_amt in pattern:
                ev += make_strum(t + step * e16, c, e16 * 1.5, 0.85 if mute_amt == 0 else 0.5, down, 0.005, mute_amt, 0.22)
            t += 8 * e16
    cc0_samples["03_CC0_Funk_Chops_115bpm.wav"] = render_phrase_events(ev, t + 0.5)
    
    # 4. Rock Drive Riff 120 BPM
    ev, t, beat = [], 0.0, 60.0 / 120.0
    e8 = beat / 2.0
    riff = [(40, 0.0), (40, 0.7), (43, 0.0), (40, 0.7), (45, 0.0), (45, 0.7), (43, 0.0), (40, 0.7)]
    for _ in range(6):
        for root, mute_amt in riff:
            ev += [(t, root, e8 * (0.8 if mute_amt > 0 else 1.8), 0.92, mute_amt, 0.12),
                   (t + 0.004, root + 7, e8 * (0.8 if mute_amt > 0 else 1.8), 0.85, mute_amt, 0.12),
                   (t + 0.008, root + 12, e8 * (0.8 if mute_amt > 0 else 1.8), 0.80, mute_amt, 0.12)]
            t += e8
    cc0_samples["04_CC0_Rock_Drive_Riff_120bpm.wav"] = render_phrase_events(ev, t + 0.5)
    
    # 5. Jazz Chord Melody 100 BPM (ii - V - I - VI)
    ev, t, beat = [], 0.0, 60.0 / 100.0
    for _ in range(2):
        for c in ("Dm7", "G7", "Cmaj7", "A9"):
            ev += make_strum(t, c, beat * 1.9, 0.80, True, 0.015, 0.0, 0.25)
            ev += make_strum(t + beat * 2.0, c, beat * 1.8, 0.72, True, 0.015, 0.0, 0.25)
            t += beat * 4.0
    cc0_samples["05_CC0_Jazz_Chord_Melody_100bpm.wav"] = render_phrase_events(ev, t + 0.5)
    
    # 6. Blues Lead Solo (Minor Pentatonic with sustain & vibrato)
    ev, t, beat = [], 0.0, 60.0 / 92.0
    e8 = beat / 2.0
    lead_licks = [
        (40, e8 * 2.0, 0.90), (43, e8, 0.82), (45, e8 * 3.0, 0.95),
        (47, e8, 0.85), (50, e8 * 2.0, 0.88), (52, e8 * 4.0, 0.98),
        (50, e8, 0.85), (47, e8, 0.82), (45, e8 * 2.0, 0.90),
        (43, e8 * 2.0, 0.88), (40, e8 * 4.0, 0.95)
    ]
    for _ in range(2):
        for pitch, dur, vel in lead_licks:
            ev.append((t, pitch, dur, vel, 0.0, 0.15))
            t += dur * 0.85
    cc0_samples["06_CC0_Blues_Lead_Solo.wav"] = render_phrase_events(ev, t + 0.5)
    
    # 7. Neo Soul Groove (Smooth 4ths and 9ths)
    ev, t, beat = [], 0.0, 60.0 / 84.0
    e16 = beat / 4.0
    for _ in range(2):
        for c in ("Cmaj7", "Dm7", "Em", "F"):
            ev += make_strum(t, c, beat * 2.5, 0.85, True, 0.02, 0.0, 0.28)
            ev.append((t + beat * 2.6, 64, e16 * 2.0, 0.75, 0.0, 0.20))
            ev.append((t + beat * 3.1, 67, e16 * 3.0, 0.80, 0.0, 0.20))
            t += beat * 4.0
    cc0_samples["07_CC0_Neo_Soul_Groove.wav"] = render_phrase_events(ev, t + 0.5)
    
    # 8. Strat Neck Fingerstyle (Soft open chords)
    ev, t, beat = [], 0.0, 60.0 / 96.0
    for _ in range(2):
        for c in ("G", "Em", "C", "D"):
            notes = CH[c]
            ev.append((t, notes[0], beat * 3.0, 0.88, 0.0, 0.30))
            ev.append((t + beat * 0.5, notes[1], beat * 2.0, 0.70, 0.0, 0.30))
            ev.append((t + beat * 1.0, notes[2], beat * 2.0, 0.72, 0.0, 0.30))
            ev.append((t + beat * 1.5, notes[-1], beat * 2.5, 0.78, 0.0, 0.30))
            ev.append((t + beat * 2.5, notes[-2], beat * 1.5, 0.68, 0.0, 0.30))
            t += beat * 4.0
    cc0_samples["08_CC0_Strat_Neck_Fingerstyle.wav"] = render_phrase_events(ev, t + 0.5)
    
    # 9. Metal Drop-D Chug 140 BPM
    ev, t, beat = [], 0.0, 60.0 / 140.0
    e16 = beat / 4.0
    for _ in range(6):
        pattern = [1, 1, 0, 1,  1, 0, 1, 1,  0, 1, 1, 0,  1, 1, 1, 0]
        for step, chug in enumerate(pattern):
            mute = 0.85 if chug else 0.0
            vel = 0.95 if chug else 0.7
            ev += [(t + step * e16, 38, e16 * (1.1 if chug else 2.5), vel, mute, 0.10),
                   (t + step * e16 + 0.003, 45, e16 * (1.1 if chug else 2.5), vel * 0.9, mute, 0.10)]
        t += 16 * e16
    cc0_samples["09_CC0_Metal_DropD_Chug_140bpm.wav"] = render_phrase_events(ev, t + 0.5)
    
    # 10. Acoustic Open Strum 105 BPM
    ev, t, beat = [], 0.0, 60.0 / 105.0
    for _ in range(2):
        for c in ("G", "D", "Am", "C"):
            for k, down in enumerate((True, False, True, False, True, True)):
                ev += make_strum(t + k * beat * 0.6, c, beat * 1.4, 0.88 if down else 0.60, down, 0.012, 0.0, 0.15)
            t += 4 * beat
    cc0_samples["10_CC0_Acoustic_Open_Strum_105bpm.wav"] = render_phrase_events(ev, t + 0.5)
    
    return cc0_samples

def main():
    print(f"Output directory: {OUT_DIR}")
    
    # 1. Process Option 1 (CC0)
    cc0_dict = generate_cc0_samples()
    for fname, raw_audio in cc0_dict.items():
        pcm16 = process_audio(raw_audio, target_duration=12.0)
        out_path = os.path.join(OUT_DIR, fname)
        save_wav(out_path, pcm16)
        print(f"  [CC0] Saved: {fname} ({len(pcm16)/FS:.1f}s)")
        
    # 2. Process Option 2 (GuitarSet CC-BY 4.0)
    gs_index = load_gs_index()
    
    gs_selection = [
        # (output_name, source_filename, style_desc)
        ("11_GS_Funk_Comp_112bpm_Csharp.wav",    "04_Funk3-112-C#_comp_mix.wav", "Funk 16th Rhythm Chops (Key C#)"),
        ("12_GS_Funk_Solo_114bpm_Aflat.wav",     "05_Funk1-114-Ab_solo_mix.wav", "Funk Melodic Lead & Fills (Key Ab)"),
        ("13_GS_Jazz_Comp_130bpm_D.wav",         "04_Jazz1-130-D_comp_mix.wav",  "Jazz Swing 7th/9th Comping (Key D)"),
        ("14_GS_Jazz_Solo_137bpm_Eflat.wav",     "05_Jazz3-137-Eb_solo_mix.wav", "Jazz Bebop Melodic Runs (Key Eb)"),
        ("15_GS_Rock_Comp_85bpm_F.wav",          "03_Rock2-85-F_comp_mix.wav",   "Rock Arpeggiated Rhythm (Key F)"),
        ("16_GS_Rock_Solo_148bpm_C.wav",         "05_Rock3-148-C_solo_mix.wav",  "Rock Expressive Lead Solo (Key C)"),
        ("17_GS_Bossa_Comp_154bpm_E.wav",        "00_BN3-154-E_comp_mix.wav",    "Bossa Nova Syncopated Fingerpicking (Key E)"),
        ("18_GS_Bossa_Solo_166bpm_Aflat.wav",    "00_BN2-166-Ab_solo_mix.wav",   "Bossa Nova Lyrical Melodic Lead (Key Ab)"),
        ("19_GS_Acoustic_Strum_107bpm_Aflat.wav","01_SS2-107-Ab_comp_mix.wav",   "Singer-Songwriter Dynamic Open Strumming (Key Ab)"),
        ("20_GS_Acoustic_Solo_100bpm_Csharp.wav","04_SS1-100-C#_solo_mix.wav",   "Singer-Songwriter Intimate Melodic Solo (Key C#)")
    ]
    
    print("\nFetching & Processing 10 GuitarSet Studio DI tracks...")
    for out_name, src_name, desc in gs_selection:
        print(f"  Fetching {src_name}...")
        raw_samples = fetch_gs_wav(gs_index, src_name)
        pcm16 = process_audio(raw_samples, target_duration=12.0)
        out_path = os.path.join(OUT_DIR, out_name)
        save_wav(out_path, pcm16)
        print(f"  [GuitarSet] Saved: {out_name} - {desc} ({len(pcm16)/FS:.1f}s)")
        
    print("\nAll 20 candidate preview samples successfully generated in:")
    print(OUT_DIR)

if __name__ == "__main__":
    main()
