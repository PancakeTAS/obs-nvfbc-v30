# obs-nvobs
OBS Studio plugin for NVIDIA'S Frame Buffer Capture (NvFBC) API for Linux.

NvFBC is an API provided by NVIDIA to capture the frame buffer without using x11grab or similar methods. Since it captures the frame buffer directly, it is much faster and more efficient than other methods. Unlike with XSHM or XComposite, you will not encounter any performance impacts and odd behavior (such as applications suddenly running extremely slow) when capturing the frame buffer with NvFBC.

## Requirements
Patched NVIDIA driver with NvFBC enabled. You can find the patch [here](https://github.com/keylase/nvidia-patch).

## Installation
1. Clone the repository
2. Run `make`
3. Copy the resulting `obs-nvobs.so` and `obs-nvobs.o` to the OBS Studio plugin directory (usually `~/.config/obs-studio/plugins/bin/64bit/`)
4. Restart OBS Studio

## Usage
1. Add a new source to your scene
2. Select `NvFBC Source` from the list of available sources
3. Configure the source
4. Click `Update settings` to apply the changes and relaunch the NvFBC capture.

## How it works
Since OBS Studio switched from GLX to EGL, NvFBC became non-functional, as it does not support EGL. This is a fundamental issue with NvFBC and can only be fixed by NVIDIA. This plugin spawns a subprocess and uses a shared memory buffer to capture the frame buffer and copy it to OBS Studio. It is not extremely efficient as the buffer is first being copied to the system memory, then to OBS Studio and then again uploaded to the GPU for rendering. However, it is still much much faster than using x11grab or similar methods. The performance impact is basically negligible.

## Known issues
- Dead subprocesses are left behind when relaunching the NvFBC capture. Those processes are not alive and do not consume any resources. They are just there. No clue why.
- Changing any kind of setting will immediately result in the source turning black, until you click `Update settings`. This is because the subprocess is killed and restarted when changing settings.
- If the subprocess crashes, OBS Studio will not notify you.
- Additionally, OBS Studio randomly crashes when clicking `Update settings` every now and then. No clue why.
- Direct capture mode is not supported yet... because it's 2 am and I'm incredibly tired.
