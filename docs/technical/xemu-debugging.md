# xemu GDB Debugging for Xbox Recompilation

Using xemu's built-in GDB stub as a reference debugger to discover runtime
game state for static recompilation projects.

## Overview

xemu (Xbox emulator) supports the GDB Remote Serial Protocol via its `-s` flag,
exposing the emulated CPU's registers and memory over TCP port 1234. This lets
us halt the guest, read/write memory, set breakpoints, and single-step — all
while the game runs at full speed between halts.

This is the most powerful technique for understanding Xbox game internals:
instead of guessing what memory addresses mean from static disassembly, we
observe them **live** during gameplay and correlate changes with game events.

## Setup

### Launch xemu with GDB Stub

```bash
# Windows (from Git Bash or cmd)
cd "C:/emu/xemu-win-release"
./xemu.exe -s

# Or with pause-at-startup (connect GDB before boot):
./xemu.exe -s -S
```

The `-s` flag opens a GDB stub on `localhost:1234`.

### Connect with Python

```python
import socket, time

def gdb_connect(host='localhost', port=1234, timeout=5.0):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    sock.connect((host, port))
    return sock

def gdb_send(sock, packet_data):
    """Send a GDB RSP packet: $<data>#<checksum>"""
    checksum = sum(ord(c) for c in packet_data) & 0xFF
    raw = f'${packet_data}#{checksum:02x}'.encode()
    sock.sendall(raw)
    return gdb_recv(sock)

def gdb_recv(sock):
    """Receive a GDB RSP response."""
    data = b''
    while b'#' not in data or len(data) < data.index(b'#') + 3:
        chunk = sock.recv(4096)
        if not chunk:
            break
        data += chunk
    # Strip $...#xx framing
    start = data.index(b'$') + 1
    end = data.index(b'#')
    return data[start:end].decode()
```

### Key GDB RSP Commands

| Command | Description |
|---------|-------------|
| `g` | Read all registers (returns hex string) |
| `m<addr>,<len>` | Read memory (hex addr/len, returns hex bytes) |
| `M<addr>,<len>:<data>` | Write memory |
| `Z0,<addr>,1` | Set software breakpoint |
| `z0,<addr>,1` | Remove software breakpoint |
| `c` | Continue execution |
| `s` | Single step |
| `\x03` (raw byte) | Halt/interrupt guest |

## Critical Safety Rules

### 1. HALT BEFORE SETTING BREAKPOINTS

The GDB stub times out if you try to set breakpoints while the guest is running.
Always halt first:

```python
sock.sendall(b'\x03')  # Interrupt
time.sleep(0.3)        # Wait for halt
gdb_recv(sock)         # Consume stop reply
# NOW safe to set breakpoints
gdb_send(sock, 'Z0,110e0,1')
```

### 2. REMOVE BREAKPOINTS BEFORE CONTINUING

Leftover breakpoints (0xCC / int3) crash the game. Always clean up:

```python
try:
    gdb_send(sock, 'Z0,110e0,1')  # Set BP
    gdb_send(sock, 'c')            # Continue
    # ... wait for hit ...
finally:
    gdb_send(sock, 'z0,110e0,1')  # ALWAYS remove
```

### 3. GAME FREEZES DURING HALT

The entire emulated system stops when halted — audio, video, everything.
Keep halt durations short (< 2 seconds) to avoid the user thinking it crashed.

## Techniques

### Page-Level Memory Diffing

The most powerful discovery technique. Instead of guessing which addresses
contain game state, hash every 4KB page of user-mode memory, wait a few
seconds of gameplay, then diff to find which pages changed.

```python
import hashlib

def hash_pages(sock, start=0x10000, end=0x560000, page_size=4096):
    """Hash all 4KB pages in the address range."""
    hashes = {}
    for addr in range(start, end, page_size):
        data = read_memory(sock, addr, page_size)
        hashes[addr] = hashlib.md5(data).hexdigest()
    return hashes

# Take two samples with gameplay between them
halt()
hashes1 = hash_pages(sock)
resume()
time.sleep(3)  # Let the player drive around
halt()
hashes2 = hash_pages(sock)
resume()

# Find changed pages
changed = [addr for addr in hashes1 if hashes1[addr] != hashes2[addr]]
print(f"{len(changed)} pages changed out of {len(hashes1)}")
```

**Typical results for Burnout 3:**
- 47 of 1520 pages change during 3 seconds of driving
- Most active region: 0x4DB000-0x4E0000 (physics simulation)
- Also active: 0x4D9000 (camera), 0x549000 (part transforms), 0x40F000 (vehicle state)

### Float Value Search

Once you know which pages change, search for float values that match observed
gameplay (e.g., if the car is going ~50 units/second, search for floats near 50):

```python
import struct

def find_floats(data, target, tolerance=1.0, base_addr=0):
    """Find all float values near target in a byte buffer."""
    results = []
    for off in range(0, len(data) - 3, 4):
        val = struct.unpack_from('<f', data, off)[0]
        if abs(val - target) < tolerance:
            results.append((base_addr + off, val))
    return results
```

### Transform Matrix Detection

4x4 world transform matrices have distinctive signatures:
- Row 3 column 3 = 1.0 (homogeneous w)
- Column 3 rows 0-2 = 0.0 (no projection)
- Rotation rows have unit length (|row| ≈ 1.0)
- Row 3 columns 0-2 = world position (translation)

```python
def is_transform_matrix(data, offset):
    """Check if 64 bytes at offset look like a 4x4 world matrix."""
    vals = struct.unpack_from('<16f', data, offset)
    # Check w column: [3]=0, [7]=0, [11]=0, [15]=1.0
    if abs(vals[3]) > 0.01 or abs(vals[7]) > 0.01:
        return False
    if abs(vals[11]) > 0.01 or abs(vals[15] - 1.0) > 0.01:
        return False
    # Check rotation row lengths
    for row in range(3):
        i = row * 4
        length = (vals[i]**2 + vals[i+1]**2 + vals[i+2]**2) ** 0.5
        if abs(length - 1.0) > 0.1:
            return False
    return True
```

### Velocity from Position Deltas

To find the speed address, compute velocity from consecutive position samples,
then search memory for a float matching that computed speed:

```
Sample 1: position = (1200, -50, 100) at t=0
Sample 2: position = (1230, -52, 115) at t=3s

velocity = sqrt(30² + 2² + 15²) / 3 ≈ 11.2 units/s

→ Search changed pages for float ≈ 11.2
```

## Xbox Memory Layout Reference

| Range | Contents |
|-------|----------|
| 0x00010000 - 0x03FFFFFF | User-mode code + data (XBE loads here) |
| 0x10000000 - 0x13FFFFFF | Contiguous GPU/physical memory |
| 0x80000000+ | Kernel code (BIOS/kernel) |
| 0xD0000000+ | Tiled memory (GPU surfaces) |
| 0xFD000000 | NV2A GPU registers |
| 0xFE000000+ | Kernel thunk table (import addresses) |

For recompilation, the interesting range is **0x10000-0x3FFFFFF** (user-mode).
Page-diff this range to find active game state.

## RenderDoc Integration

xemu supports RenderDoc frame capture for GPU-level analysis:

1. Install RenderDoc from https://renderdoc.org
2. Launch xemu through RenderDoc (File > Launch Application)
3. Press F12 during gameplay to capture a frame
4. Analyze draw calls, textures, shaders, render state, matrices

This complements GDB debugging by showing the GPU side — what the game
actually renders and how its graphics pipeline is configured.

## Workflow Summary

1. **Launch** xemu with `-s` flag
2. **Boot** the game to the state you want to analyze
3. **Page-diff** to find active memory regions
4. **Narrow down** with float searches and matrix detection
5. **Correlate** changes with gameplay events (accelerating, turning, crashing)
6. **Verify** by writing values and observing effects (optional)
7. **Document** addresses and their meanings
8. **Implement** discovered addresses in the recompilation

This reference-based approach is far more reliable than pure static analysis,
as it gives ground truth about what the game actually does at runtime.
