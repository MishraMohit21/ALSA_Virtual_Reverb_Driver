#!/usr/bin/env python3
"""
ALSA Reverb Loopback Tester

Usage:
    python3 test_echo.py                  # Use default test beeps
    python3 test_echo.py --input song.wav # Process a real WAV/audio file
"""
import sounddevice as sd
import numpy as np
import threading
import time
import wave
import sys
import argparse
import os
import json

APP_DIR = os.path.dirname(os.path.abspath(__file__))
UPLOAD_DIR = os.path.join(APP_DIR, 'uploads')
OUTPUT_DIR = os.path.join(APP_DIR, 'output')
os.makedirs(UPLOAD_DIR, exist_ok=True)
os.makedirs(OUTPUT_DIR, exist_ok=True)

# --- ARGUMENT PARSING ---
parser = argparse.ArgumentParser(description='ALSA Reverb Loopback Tester')
parser.add_argument('--input', '-i', type=str, default=None,
                    help='Path to input WAV file (16-bit, mono or stereo). '
                         'If omitted, generates test beeps.')
args = parser.parse_args()

# --- CONFIGURATION (defaults, may be overridden by input file) ---
FS = 44100          # Sample Rate (will be set from input WAV if provided)
CHANNELS = 2        # Channels (will be set from input WAV if provided)
SUPPORTED_RATES = {8000, 16000, 22050, 32000, 44100, 48000, 96000, 192000}

print(f"--- ALSA Reverb Loopback Tester ---")

# ===========================
# 1. FIND DEVICES
# ===========================
print("\n1. Scanning audio devices...")
devices = sd.query_devices()

echo_play_idx = -1
echo_cap_idx = -1
real_play_idx = None  # Will use system default if not found

for i, dev in enumerate(devices):
    name = dev['name']
    if "Echo" in name:
        if dev['max_output_channels'] > 0:
            echo_play_idx = i
            print(f"   [OK] EchoDriver PLAYBACK at index {i}: {name}")
        if dev['max_input_channels'] > 0:
            echo_cap_idx = i
            print(f"   [OK] EchoDriver CAPTURE  at index {i}: {name}")

if echo_play_idx == -1 or echo_cap_idx == -1:
    print("\n[ERROR] Echo Driver not fully found!")
    print("   -> Did you run 'sudo insmod my_audio.ko'?")
    print("   -> Check 'cat /proc/asound/cards'")
    sys.exit(1)

# Find the real/default sound card for final playback
for i, dev in enumerate(devices):
    name = dev['name']
    if "Echo" not in name and dev['max_output_channels'] >= 2:
        real_play_idx = i
        print(f"   [OK] Real sound card at index {i}: {name}")
        break

if real_play_idx is None:
    print("   [WARN] No separate real sound card found, using system default")

# ===========================
# 2. PREPARE AUDIO
# ===========================
if args.input:
    # --- Load from WAV file ---
    input_path = args.input
    if not os.path.isfile(input_path):
        print(f"\n[ERROR] Input file not found: {input_path}")
        sys.exit(1)

    print(f"\n2. Loading input file: {input_path}")

    with wave.open(input_path, 'r') as wf:
        src_channels = wf.getnchannels()
        src_sampwidth = wf.getsampwidth()
        src_rate = wf.getframerate()
        src_frames = wf.getnframes()
        raw_data = wf.readframes(src_frames)

    print(f"   Source: {src_rate}Hz, {src_channels}ch, {src_sampwidth * 8}-bit, "
          f"{src_frames} frames ({src_frames / src_rate:.1f}s)")

    # Convert to numpy array (always to int16)
    if src_sampwidth == 2:
        samples = np.frombuffer(raw_data, dtype=np.int16)
    elif src_sampwidth == 1:
        samples = (np.frombuffer(raw_data, dtype=np.uint8).astype(np.int16) - 128) * 256
    elif src_sampwidth == 3:
        raw = np.frombuffer(raw_data, dtype=np.uint8)
        n_samples = len(raw) // 3
        samples = np.zeros(n_samples, dtype=np.int16)
        for idx in range(n_samples):
            b = raw[idx * 3: idx * 3 + 3]
            val = int.from_bytes(b, byteorder='little', signed=True)
            samples[idx] = np.int16(val >> 8)
    elif src_sampwidth == 4:
        samples = (np.frombuffer(raw_data, dtype=np.int32) >> 16).astype(np.int16)
    else:
        print(f"[ERROR] Unsupported sample width: {src_sampwidth} bytes")
        sys.exit(1)

    # Reshape to (frames, channels)
    samples = samples.reshape(-1, src_channels)

    # Use native channel count (1 or 2), cap at 2
    if src_channels > 2:
        samples = samples[:, :2]
        CHANNELS = 2
        print(f"   Downmixed {src_channels}ch -> stereo")
    else:
        CHANNELS = src_channels
        print(f"   Using native {CHANNELS}ch")

    # Use native sample rate if supported, otherwise resample to nearest supported rate
    if src_rate in SUPPORTED_RATES:
        FS = src_rate
        print(f"   Using native sample rate: {FS}Hz")
    else:
        # Find nearest supported rate
        FS = min(SUPPORTED_RATES, key=lambda r: abs(r - src_rate))
        print(f"   Resampling {src_rate}Hz -> {FS}Hz (nearest supported rate)...")
        src_len = len(samples)
        dst_len = int(src_len * FS / src_rate)
        indices = np.linspace(0, src_len - 1, dst_len)
        resampled_channels = []
        for ch in range(CHANNELS):
            resampled = np.interp(indices, np.arange(src_len), samples[:, ch].astype(float))
            resampled_channels.append(resampled.astype(np.int16))
        samples = np.column_stack(resampled_channels) if CHANNELS > 1 else resampled_channels[0].reshape(-1, 1)

    audio_data = samples.astype(np.int16)
    total_frames = len(audio_data)
    print(f"   Ready: {total_frames} frames ({total_frames / FS:.1f}s) @ {FS}Hz {CHANNELS}ch")

    # Save the (possibly converted) input
    orig_path = os.path.join(OUTPUT_DIR, 'original_input.wav')
    with wave.open(orig_path, 'w') as wf:
        wf.setnchannels(CHANNELS)
        wf.setsampwidth(2)
        wf.setframerate(FS)
        wf.writeframes(audio_data.tobytes())
    print(f"   Saved normalized input to {orig_path}")

else:
    # --- Generate test beeps ---
    FREQ = 440.0
    BEEP_DURATION = 0.2
    SILENCE_DURATION = 1.0
    NUM_BEEPS = 3

    print(f"\n2. Generating test signal: {NUM_BEEPS} short beeps with silence gaps...")

    segments = []
    for b in range(NUM_BEEPS):
        beep_samples = int(FS * BEEP_DURATION)
        t_beep = np.linspace(0, BEEP_DURATION, beep_samples, endpoint=False)
        beep = (0.5 * np.sin(2 * np.pi * FREQ * t_beep) * 32767).astype(np.int16)
        segments.append(np.column_stack([beep, beep]))

        if b < NUM_BEEPS - 1:
            silence_samples = int(FS * SILENCE_DURATION)
        else:
            silence_samples = int(FS * 2.0)
        silence = np.zeros((silence_samples, CHANNELS), dtype=np.int16)
        segments.append(silence)

    audio_data = np.concatenate(segments, axis=0)
    total_frames = len(audio_data)
    print(f"   Generated {total_frames} frames ({total_frames / FS:.1f}s)")
    print(f"   Pattern: BEEP(200ms) - silence(1s) × {NUM_BEEPS}")

    # Save input reference
    orig_path = os.path.join(OUTPUT_DIR, 'original_input.wav')
    with wave.open(orig_path, 'w') as wf:
        wf.setnchannels(CHANNELS)
        wf.setsampwidth(2)
        wf.setframerate(FS)
        wf.writeframes(audio_data.tobytes())
    print(f"   Saved input reference to {orig_path}")

# ===========================
# 3. LOOPBACK: WRITE -> READ
# ===========================
CHUNK_FRAMES = max(1024, FS // 10)  # ~100ms chunks, dynamic based on FS
print(f"\n3. Running reverb loopback ({FS}Hz, {CHANNELS}ch, chunk={CHUNK_FRAMES} frames)...")

captured_chunks = []
capture_done = threading.Event()
playback_started = threading.Event()


def playback_thread_fn():
    """Write audio into EchoDriver's playback stream."""
    try:
        with sd.OutputStream(device=echo_play_idx, channels=CHANNELS,
                             samplerate=FS, dtype='int16',
                             blocksize=CHUNK_FRAMES) as pb_stream:
            playback_started.set()
            offset = 0
            while offset < total_frames:
                end = min(offset + CHUNK_FRAMES, total_frames)
                pb_stream.write(audio_data[offset:end])
                offset = end
    except Exception as e:
        print(f"   [ERROR] Playback thread: {e}")
    finally:
        playback_started.set()


def capture_thread_fn():
    """Read reverb-processed audio back from EchoDriver's capture stream."""
    frames_captured = 0
    target_frames = total_frames

    try:
        with sd.InputStream(device=echo_cap_idx, channels=CHANNELS,
                            samplerate=FS, dtype='int16',
                            blocksize=CHUNK_FRAMES) as cap_stream:
            while frames_captured < target_frames:
                remaining = target_frames - frames_captured
                to_read = min(CHUNK_FRAMES, remaining)
                data, overflowed = cap_stream.read(to_read)
                if overflowed:
                    print("   [WARN] Capture overflow!")
                captured_chunks.append(data.copy())
                frames_captured += len(data)

    except Exception as e:
        print(f"   [ERROR] Capture thread: {e}")
    finally:
        capture_done.set()


# Start PLAYBACK first so the driver's internal buffer gets data
print("   Starting playback...")
pb_thread = threading.Thread(target=playback_thread_fn, daemon=True)
pb_thread.start()
playback_started.wait(timeout=2)

# Small delay so driver buffer has some data before capture starts reading
time.sleep(0.1)

# Now start capture
print("   Starting capture...")
cap_thread = threading.Thread(target=capture_thread_fn, daemon=True)
cap_thread.start()

start_time = time.time()

# Wait for playback to finish
pb_thread.join()
elapsed = time.time() - start_time
print(f"   Playback done in {elapsed:.2f}s")

# Wait for capture to finish
print("   Waiting for capture to finish...")
capture_done.wait(timeout=max(10, total_frames / FS + 5))
cap_thread.join(timeout=2)

if not captured_chunks:
    print("\n[ERROR] No audio was captured! Check driver logs: sudo dmesg | tail")
    sys.exit(1)

# Combine captured chunks
reverb_audio = np.concatenate(captured_chunks, axis=0)
print(f"   Captured {len(reverb_audio)} frames of reverb audio")

# ===========================
# 4. SAVE REVERB AS WAV FILE
# ===========================
output_path = os.path.join(OUTPUT_DIR, 'reverb_output.wav')
print(f"\n4. Saving reverb audio to {output_path}...")

try:
    with wave.open(output_path, 'w') as wf:
        wf.setnchannels(CHANNELS)
        wf.setsampwidth(2)
        wf.setframerate(FS)
        wf.writeframes(reverb_audio.tobytes())
    print(f"   [SUCCESS] Saved {len(reverb_audio)} frames to {output_path}")
    print(f"   Download and listen: scp user@host:{output_path} .")
except Exception as e:
    print(f"   [ERROR] Failed to save WAV: {e}")

# Write metadata JSON for the visualizer
meta = {
    'sample_rate': FS,
    'channels': CHANNELS,
    'bit_depth': 16,
    'input_frames': total_frames,
    'input_duration': round(total_frames / FS, 2),
    'output_frames': len(reverb_audio),
    'output_duration': round(len(reverb_audio) / FS, 2),
    'source_file': os.path.basename(args.input) if args.input else 'generated_beeps',
}
try:
    with open(os.path.join(OUTPUT_DIR, 'metadata.json'), 'w') as f:
        json.dump(meta, f, indent=2)
    print(f"   Saved metadata to output/metadata.json")
except Exception as e:
    print(f"   [WARN] Could not write metadata: {e}")

print("\nDone! Check kernel logs: sudo dmesg | tail -20")
