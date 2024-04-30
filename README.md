# obs-nvfbc-v30
OBS Studio (v30+) plugin for NVIDIA'S Frame Buffer Capture (NvFBC) API for Linux.

NvFBC is an API provided by NVIDIA to capture the frame buffer without using x11grab or similar methods. Since it captures the frame buffer directly, it is much faster and more efficient than other methods. Unlike with XSHM or XComposite, you will not encounter any performance impacts and odd behavior (such as applications suddenly running extremely slow) when capturing the frame buffer with NvFBC.

## Requirements
Patched NVIDIA driver with NvFBC enabled. You can find the patch [here](https://github.com/keylase/nvidia-patch).

## Installation
1. Clone the repository
2. Run `make`
3. Copy the resulting `obs-nvfbc.so` to the OBS Studio plugin directory (usually `~/.config/obs-studio/plugins/bin/64bit/`)
3. Or, optionally, run `make link` to symlink the library to the OBS Studio plugin directory
4. Restart OBS Studio

## Usage
1. Add a new source to your scene
2. Select `NvFBC Source` from the list of available sources
3. Configure the source
4. Click `Update settings` to apply the changes and relaunch the NvFBC capture.

## How it works
Since OBS Studio switched from GLX to EGL, NvFBC became non-functional, as it does not support EGL. This is a fundamental issue with NvFBC and can only be fixed by NVIDIA. At first my idea was to spawn a subprocess with a shared memory area. The subprocess would then capture the frame buffer copy it to system memory, then into the shm and finally into an obs texture. While this did work, it was extremely slow and inefficient (to the point where I'm not sure if it was even faster than XSHM). Here's what we came up with instead (HUGE CREDIT to [0xNULLderef](https://github.com/0xNULLderef) for figuring out all the hacks):

NvFBC internally is a mess. A total mess. It creates a vulkan device and allocates a buffer on the gpu using it. It then passes the raw memory descriptor from the internal Vulkan buffer to the X driver, which will run an internal timer and fill the buffer with the framebuffer. NvFBC itself then internally uses some interop extension to create a GL texture that shares the same memory as the Vulkan image does. Then it post processes that data to convert it's format to whichever one the user selects. Finally it copies it to another GL texture, system buffer or cuda, depending on what you specify.

Since NvFBC internally uses GLX, the first idea was to replace the few GLX calls to the OBS EGL alternative. While it didn't crash, it also didn't work.

The actual solution was far more sophisticated (not really). We hooked all GLX/GL calls and nullified them. This way, NvFBC does not do anything except fill the Vulkan buffer with the framebuffer. Then we do some manual memory mapping and figure out the Vulkan buffer's memory descriptor. Then we use the same interop but to a gs_texture_2d provided by OBS. This way we can directly copy the Vulkan buffer to the OBS texture. This is the fastest and most efficient way to capture the framebuffer with NvFBC.

## Known issues
- Changing any kind of setting will immediately result in the source turning black, until you click `Update settings`. This is because the NvFBC capture has to be restarted in order to apply the changes.
