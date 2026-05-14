# OBS Audio Router

Send audio tracks from your **Gaming PC** to your **Streaming PC** over your local network — no virtual audio cables, no extra software.

---

## How it works

| PC | What you do | What it does |
|---|---|---|
| Gaming PC | Add **"Audio Router: Send Track"** filter to any source | Streams that audio over UDP to your Streaming PC |
| Streaming PC | Add **"Audio Router: Receive Track"** as a new source | Receives the audio and makes it available in OBS |

You can send up to **2 independent stereo tracks** simultaneously (Track 1 on UDP port 9001, Track 2 on UDP port 9002).

---

## Installation

### Step 1 — Get the plugin DLL

1. Go to the **[Actions tab](../../actions)** of this repository on GitHub
2. Click the most recent successful build
3. Download **`obs-audio-router-windows-x64`** from the Artifacts section
4. Unzip it — you'll have `obs-audio-router-windows-x64.zip` inside

### Step 2 — Install on both PCs

1. Copy `obs-audio-router-windows-x64.zip` and `installer/install.ps1` to the same folder on each PC
2. Right-click `install.ps1` → **Run with PowerShell**
   - If prompted about execution policy, choose "Run anyway"
3. The script will:
   - Find your OBS installation automatically
   - Copy the plugin DLL to the right place
   - Add Windows Firewall rules for UDP ports 9001 and 9002
4. **Restart OBS** on both PCs

---

## Usage

### Gaming PC — Sending audio

1. In OBS, right-click any audio source (e.g. "Game Capture", "Mic/Aux")
2. Click **Filters**
3. Click **+** (Add Filter) → **Audio Router: Send Track**
4. Set:
   - **Streaming PC IP address** — the local IP of your streaming PC (e.g. `192.168.1.100`)
   - **Send as track** — Track 1 or Track 2
5. Click **Close**

Repeat for each source you want to send. You can send multiple sources on the same track (they'll be mixed together).

> **Finding your Streaming PC's IP:** On the Streaming PC, open a Command Prompt and type `ipconfig`. Look for the IPv4 address under your network adapter (usually starts with `192.168.` or `10.0.`).

### Streaming PC — Receiving audio

1. In OBS, click **+** in the Sources panel → **Audio Router: Receive Track**
2. Give it a name (e.g. "Game Audio from Gaming PC")
3. Set:
   - **Receive track** — must match what the Gaming PC is sending (Track 1 or Track 2)
4. Click **OK**

The source will appear in your Audio Mixer. You can adjust volume, add filters, and assign it to OBS recording tracks just like any other audio source.

---

## Network requirements

- Both PCs must be on the **same local network** (same router)
- UDP ports **9001** (Track 1) and **9002** (Track 2) must be open on the Streaming PC
- The installer handles Windows Firewall automatically
- If you use a third-party firewall or router firewall, you may need to manually allow these ports

---

## Troubleshooting

| Problem | Fix |
|---|---|
| No audio on Streaming PC | Check the IP address in the sender filter. Run `ipconfig` on the Streaming PC to confirm. |
| Audio cuts out | Make sure both PCs are on wired ethernet, not Wi-Fi |
| Plugin doesn't appear in OBS | Make sure the DLL is in `obs-studio\obs-plugins\64bit\` and you restarted OBS |
| Firewall blocking | Run the installer as Administrator, or manually add UDP inbound rules for ports 9001-9002 |
| Audio is delayed | This is normal — there is a small jitter buffer (~20-40ms). This is intentional to prevent dropouts. |

---

## Technical details

- **Transport:** UDP (fire-and-forget, lowest latency)
- **Format:** 32-bit float PCM, stereo, 48kHz (matches OBS native format — no resampling)
- **Packet size:** ~8KB per chunk (~1024 samples)
- **Jitter buffer:** up to 200ms, adaptive drop when full
- **Latency:** typically 5-30ms on a wired LAN

---

## Building from source

If you want to build it yourself:

```bash
git clone https://github.com/YOUR_USERNAME/obs-audio-router
cd obs-audio-router
```

Then push to GitHub — the Actions workflow builds automatically.

Or locally with CMake (requires OBS Studio headers):

```bash
cmake -B build -S . -Dlibobs_DIR=/path/to/obs/cmake
cmake --build build --config Release
```
