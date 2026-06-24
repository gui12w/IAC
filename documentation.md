# IAC Codec Documentation (Imprecision Audio Codec)

## Overview

IAC is a lossy audio codec that reduces resolution to 15 bits (signed) and applies Rice compression with delta encoding, achieving significant compression ratios. The format supports up to 8 channels, instant seeking (seek table), and is designed for parallel processing.

The 5 files below make up the ecosystem:

- **iac_input_ram.c** – Encoder (input: any audio via ffmpeg or WAV, output: .iac)
- **iac_output_table.c** – Decoder to WAV file (offline)
- **iac_play.c** – Player with Sokol Audio backend (cross-platform)
- **iac_playexclusive.c** – Player with Miniaudio backend in WASAPI exclusive mode (Windows)
- **iac_stream.c** – Streamer for Windows named pipe (for use with other tools)

---

## 1. iac_input_ram.c – IAC Encoder

### Description

Reads an audio file (any format supported by ffmpeg or WAV directly), converts to 32-bit PCM, applies 110 dB reduction, maps to 14 bits signed, writes in 15 bits, and compresses each channel independently using:

- Difference between consecutive samples (delta)
- Rice coding with k optimized per 1024-sample block
- Byte alignment at the start of each 5-second chunk to enable seek table

The result is written to a .iac file with a header, channel offset table, and for each channel: the seek table (absolute offsets of each 5s chunk) followed by the compressed bitstream.

### Compilation

```bash
zig cc iac_input_ram.c -std=gnu99 -pthread -O3 -lm -o iac_encode.exe
```

Or with gcc/clang:

```bash
gcc -std=gnu99 -pthread -O3 -lm iac_input_ram.c -o iac_encode
```

### Usage

```bash
./iac_encode <input_file> <output.iac>
```

Example:

```bash
./iac_encode music.flac music.iac
```

### Main components

- **BitBuffer**: bit-by-bit writing with byte alignment.
- **Channel**: dynamic storage of samples per channel.
- **Audio reading**: uses ffmpeg via pipe for any format, or direct WAV reading.
- **Compression**: `compactar_canal_14bit_thread` function executed in parallel per channel.
- **Seek table**: automatically generated every 5 seconds of audio (chunk).
- **File writing**: header, channel indices, and for each channel: seek table + data.

### Internal flow

1. Opens the input file (via ffmpeg or fopen).
2. Reads the WAV (32-bit or 16-bit) and stores it in separate channels.
3. For each channel, spawns a thread that:
   - Initializes a BitBuffer
   - Processes 1024-sample blocks
   - Calculates the best k for Rice
   - Writes the k (4 bits) and the encoded deltas
   - Every 5s, aligns to byte and records the offset in the seek table
4. Writes the header, indices, and each channel's data to the final file.
5. Displays compression statistics.

---

## 2. iac_output_table.c – Decoder to WAV

### Description

Reads a .iac file and reconstructs the audio as 32-bit integer PCM (s32), saving to a standard WAV file. The process is done in blocks (chunks) with per-channel parallelism to speed up decoding.

### Compilation

```bash
zig cc iac_output_table.c -O3 -lm -lpthread -o iac_output.exe
```

Or:

```bash
gcc -O3 -lm -lpthread iac_output_table.c -o iac_output
```

### Usage

```bash
./iac_output <file.iac> <output.wav>
```

### Main components

- **BitStream**: bit-by-bit reading with cache buffer.
- **Rice decoding**: `decode_next_sample` function that restores the delta and absolute value.
- **Per-channel parallelism**: each thread processes a block of `CHUNK_SECONDS` (10s) for a specific channel.
- **WAV writing**: header with PCM 32-bit format (audio_format = 1).
- **Normalization**: applies the `NORMALIZATION_FACTOR` (316227.766) to reverse the 110 dB reduction.

### Internal flow

1. Opens the .iac and reads the header.
2. Reads the offsets and sizes of each channel.
3. Opens a WAV file for writing and writes the header.
4. For each channel, initializes a BitStream pointing to its block in the file.
5. Processes in 10s blocks:
   - Spawns one thread per channel to decode its block.
   - Waits for all threads.
   - Interleaves the channel samples (L/R/L/R...) and writes to the WAV.
6. Displays progress and finishes.

---

## 3. iac_play.c – Player with Sokol Audio

### Description

Real-time audio player using the Sokol Audio library for cross-platform output. Features:

- Per-channel parallel decoding (workers) coordinated by a master thread.
- Ring buffer to store decoded audio (past + future).
- Controls: play/pause, seek forward/backward 5s.
- Smooth fade-in to avoid pops on seeks and after starvation.
- Infinite loop support (file repetition).

### Compilation

```bash
zig cc iac_play.c -std=gnu99 -pthread -O3 -lm -o iacplay.exe
```

Note: requires the `sokol_audio.h` library in the same directory (or in the include path). For Windows, you may need to link with `-lwinmm` or `-lole32` depending on the implementation.

### Usage

```bash
./iacplay <file.iac>
```

Controls during playback:

- `space` – pause/continue
- `x` – go back 5 seconds
- `c` – go forward 5 seconds
- `q` – quit

### Main components

- **iacplay_desc**: central structure with ring buffer, per-channel streams, mutexes, conditionals.
- **channel_worker_func**: thread that decodes one channel for a 5s chunk.
- **decode_thread_func**: master thread that coordinates workers, detects space in the ring buffer, and dispatches new chunks.
- **audio_cb**: Sokol Audio callback that consumes the ring buffer and sends to the device.
- **Seek**: uses the file's seek table to jump to any position without needing to decode the entire history.

### Internal flow

1. Opens the .iac and initializes structures.
2. Allocates the ring buffer of capacity `(PAST_BUFFER_SECONDS + FUTURE_BUFFER_SECONDS)`.
3. For each channel, creates a worker thread and a BitStream.
4. Starts the master thread (organizer).
5. Initializes Sokol Audio with the callback.
6. Enters the user command loop.
7. On exit, finalizes threads and frees resources.

---

## 4. iac_playexclusive.c – WASAPI Exclusive Player

### Description

Player variant that uses the Miniaudio library with WASAPI exclusive mode support on Windows. This allows low latency and direct hardware access, bypassing the system mixer. Structurally very similar to `iac_play.c`, but:

- Uses Miniaudio instead of Sokol Audio.
- Configures the device with `ma_share_mode_exclusive`.
- Keeps the same thread, ring buffer, and seek mechanisms.

### Compilation

```bash
zig cc iac_playexclusive.c -std=gnu99 -pthread -O3 -lm -o iacplay_exclusive.exe
```

Requires the `miniaudio.h` file in the same directory.

### Usage

```bash
./iacplay_exclusive <file.iac>
```

Controls identical to the normal player.

### Notable differences

- The callback is of type `ma_device` instead of Sokol.
- Explicit configuration `playback.shareMode = ma_share_mode_exclusive`.
- May fail if the sample rate is not natively supported by the hardware.

---

## 5. iac_stream.c – Windows Pipe Streamer

### Description

Utility to stream the contents of a .iac file to a Windows named pipe (`\\.\pipe\iac_stream`). The data is sent as 32-bit PCM WAV, allowing other programs (e.g., FFmpeg, players) to read from the pipe as if it were a WAV file.

Differentiator: support for fast seeking (`start_time_seconds`) using multi-core parallelism to quickly advance to the desired point.

### Compilation

```bash
zig cc iac_stream.c -O3 -pthread -lm -o iac_stream.exe
```

Or with gcc (on Windows with MSYS2 or similar):

```bash
gcc -O3 -pthread -lm iac_stream.c -o iac_stream.exe
```

### Usage

```bash
./iac_stream <file.iac> <start_time_in_seconds>
```

Example:

```bash
./iac_stream music.iac 120.5
```

This creates the pipe `\\.\pipe\iac_stream` and starts transmitting from 120.5 seconds. Another program can read from this pipe.

To consume the pipe with FFmpeg, for example:

```bash
ffmpeg -f wav -i \\.\pipe\iac_stream -c copy output.wav
```

### Main components

- **BitStream**: bit-by-bit reading with support for `_fseeki64` and `_ftelli64` for large files.
- **Parallel fast-forward**: `channel_fast_forward_worker` function that each channel executes in parallel to reach the start position without decoding the entire previous audio.
- **Pipe writing**: `write_wav_header_to_pipe` function that writes the WAV header and then sends the data as 32-bit PCM.
- **Initial silence**: if `start_time_seconds > 0`, sends silence to maintain synchronization.

### Internal flow

1. Opens the .iac and reads the header and offsets.
2. Creates the named pipe and waits for connection.
3. Writes the WAV header.
4. If `start_sample > 0`, sends the corresponding silence.
5. Spawns threads for each channel to fast-forward to `start_sample`.
6. Waits for all threads.
7. Continuously streams samples from the start point to the end of the file.
8. Closes the pipe and frees resources.

---

## Final Notes

- All programs use `pthread` for parallelism, compatible with Windows (via `zig cc` or compilers that support pthreads on Windows, such as GCC from MSYS2).
- The IAC format is optimized for long, multi-channel audio files (up to 8 channels).
- The seek table allows O(1) jumps to any point, without needing prior decoding.
- The 110 dB reduction and mapping to 15 bits signed ensure near-CD quality, with typical compression ratios between 2x and 4x compared to 32-bit PCM.
- The players use a ring buffer to avoid underflow and offer basic playback controls.

For more details, refer to the comments at the top of each file.