# ALSA Virtual Reverb Driver (EchoDriver)

## Project Overview

This project aims to develop a **Linux Kernel Module** that functions as a **virtual ALSA sound card**.  
The primary objective is to create a *loopback-style* audio device that:

- Accepts audio input  
- Applies a **reverb effect** (future implementation)  
- Exposes the processed audio for capture  

---

## Current Architecture

The driver currently implements a functional ALSA architecture with active playback simulation.

### Platform Device
- Registers a virtual platform device (`my_reverb_device`) to host the sound card  
- Complies with modern Linux kernel requirements (5.x+)

### Sound Card Control
- Creates and registers an ALSA card instance named **EchoCard**

### PCM Engine
- Implements a PCM device (`Reverb_PCM`)
- Supports both **playback** and **capture** substreams

### Memory Management
- **DMA Buffer**  
  - Uses `snd_pcm_set_managed_buffer_all`
  - Allocates a 64 KB ring buffer
- **Private Runtime Data**  
  - Uses `struct echo_runtime` for per-stream state
  - Allocated dynamically using `kzalloc()` during device open

### Heartbeat Mechanism (Simulated Hardware)
- Uses `struct timer_list` to simulate hardware interrupts every **10 ms**
- Updates the hardware pointer (`hw_ptr`) with proper ring-buffer wrapping
- Calls `snd_pcm_period_elapsed()` to keep user-space applications synchronized

---

## Current Status

- **Build Status**: Compiles successfully without warnings  
- **Load Status**:  
  - Module loads correctly using `insmod`  
  - Device nodes (e.g. `/dev/snd/pcmC1D0p`) are created  
- **Runtime Status**: Fully functional  
  - User-space applications (`aplay`, Python `sounddevice`) can:
    - Open the device
    - Negotiate parameters (`S16_LE`, `44.1 kHz`)
    - Stream audio without hangs or crashes  
  - Audio is consumed at the correct rate

---

## Implementation Roadmap

- **Phase 1 & 2**: Skeleton & Registration — **COMPLETED**
- **Phase 3**: Heartbeat & Timer Logic — **COMPLETED**
- **Phase 4**: Audio Data Processing — **NEXT**

Planned work for Phase 4:
- Access DMA buffer via `runtime->dma_area`
- Implement circular buffer logic for delayed samples
- Implement reverb algorithm  
  - `output = current_sample + delayed_sample`
- Verify audio modification via kernel logs (“Visualizer”)

---

## Build and Test Instructions

### 1. Compilation
```bash
make
````

---

### 2. Installation

```bash
# Remove old module if present
sudo rmmod my_audio

# Insert new module
sudo insmod my_audio.ko

# Grant permissions for user-space testing (required after every reload)
sudo chmod 777 /dev/snd/*
```

---

### 3. Verification

```bash
# Verify device registration
cat /proc/asound/cards

# Check kernel logs
sudo dmesg | tail
```

---

### 4. Testing (Playback)

#### Option A: Standard ALSA Utility

```bash
# Should play silently and exit gracefully (no hangs)
sudo aplay -D plughw:1,0 /usr/share/sounds/alsa/Front_Center.wav
```

#### Option B: Python Test Suite

Ensure `test_echo.py` is configured for **44,100 Hz**.

```bash
python3 test_echo.py
```

**Expected Output**

```
Success! Playback finished...
```

---

## Notes

This project is intended as a **learning-focused kernel audio driver**, demonstrating:

* ALSA PCM internals
* Timer-based hardware simulation
* Ring-buffer audio processing
* A foundation for real-time DSP effects inside the Linux kernel

