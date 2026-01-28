import sounddevice as sd
import numpy as np
import time

# --- CONFIGURATION ---
FS = 44100          # Sample Rate (48kHz)
DURATION = 5        # How long to play
FREQ = 440.0        # Tone Frequency (A4)

print(f"--- ALSA Echo Driver Tester ({FS}Hz) ---")

# 1. Search for the Driver
print("1. Scanning audio devices...")
devices = sd.query_devices()
echo_idx = -1

for i, dev in enumerate(devices):
    # Search for 'Echo' in the device name (Output devices only)
    if "Echo" in dev['name'] and dev['max_output_channels'] > 0:
        echo_idx = i
        print(f"   [SUCCESS] Found 'EchoDriver' at Index {i}: {dev['name']}")
        break

if echo_idx == -1:
    print("\n[ERROR] Echo Driver NOT found!")
    print("   -> Did you run 'sudo insmod my_audio.ko'?")
    print("   -> Check 'cat /proc/asound/cards'")
    exit()

# 2. Generate the Beep (Sine Wave)
print(f"2. Generating {DURATION} seconds of audio data...")
t = np.linspace(0, DURATION, int(FS * DURATION), endpoint=False)
audio_data = 0.5 * np.sin(2 * np.pi * FREQ * t)
# Convert float (-1.0 to 1.0) to 16-bit PCM integers
audio_data = (audio_data * 32767).astype(np.int16)

# 3. Feed the Driver
print(f"3. Playing audio to Device {echo_idx}...")
try:
    start_time = time.time()
    
    # Open the stream and write the data
    with sd.OutputStream(device=echo_idx, channels=1, 
                         samplerate=FS, dtype='int16') as stream:
        stream.write(audio_data)
        
    end_time = time.time()
    print(f"\n[VICTORY] Playback finished in {end_time - start_time:.2f} seconds.")
    print("Check 'sudo dmesg' in another terminal. If you see no errors, IT WORKS.")

except Exception as e:
    print(f"\n[FAIL] Playback crashed: {e}")
