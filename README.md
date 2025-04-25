# obs-nvfbc-v30
OBS Studio (v30+) plugin for NVIDIA'S Frame Buffer Capture (NvFBC) API for Linux.

NvFBC is an API provided by NVIDIA to capture the frame buffer without using x11grab or similar methods. Since it captures the frame buffer directly, it is much faster and more efficient than other methods. Unlike with XSHM or XComposite, you will not encounter any performance impacts and odd behavior (such as applications suddenly running extremely slow) when capturing the frame buffer with NvFBC.

## Requirements
Patched NVIDIA driver with NvFBC enabled. You can find the patch [here](https://github.com/keylase/nvidia-patch).

> [!CAUTION]
> NvFBC on driver versions 560.X and 565.X does not work due to the NvFBC patch being incorrect (https://github.com/keylase/nvidia-patch/issues/889).
> Stick to either 555.58.02 (or older) or 570.86.15 (or newer) versions if you want to use this plugin.

## Installation
Unfortunately installing this plugin is not as straight forward as any other plugin, as NvFBC is loaded _before_ the plugin, making patches effectively useless. Follow this guide closely:
1. Clone the repository and open a terminal inside of it
2. Build the project using `cmake -B build && make -C build`.
This will create two files `build/libobs-nvfbc.so` and `build/libobs-nvfbc-pre.so`.
3. Copy the first file (`libobs-nvfbc.so`) into your OBS Studio plugin directory, which is usually located at `~/.config/obs-studio/plugins/bin/64bit`
4. Copy the second file `libobs-nvfbc-pre.so` to wherever you like and note down the path (e.g. `/home/pancake/.nvfbc/libobs-nvfbc-pre.so`).

With that the installation is done, but in order for the plugin to actually work you have to **preload** the `libobs-nvfbc-pre.so` file. This is done by setting `LD_PRELOAD=""` to the path of the `.so` file before running obs (e.g. `LD_PRELOAD="/home/pancake/.nvfbc/libobs-nvfbc-pre.so" obs`).

> [!NOTE]
> You might have to adjust your `obs.desktop` file if you plan on launching OBS Studio with any graphical tool.
> On Arch Linux, open `/usr/share/applications/com.obsproject.Studio.desktop` and adjust the `Exec=` property accordingly.

## Usage
Before you start using the NvFBC Source, there's some properties that need further explanation:
1. Allow direct capture: This option is supposed to attach directly your game and take that framebuffer as opposed to the entire monitor. This makes it ignore notifications, overlays, etc. On my system however, this option seems to just target the *first* app on my PC, which makes it practically useless as it keeps capturing OBS itself.
2. Track Interval (ms): This option tells NvFBC how often to fetch your screen for updates. For some unknown reason NVIDIA decided to only allow for whole numbers here, which is annoying considering 60 FPS is something like 16.6667 milliseconds. Setting this option to 0 will capture the screen as soon as it updates, meaning, if your compositor has an ingame FPS limiter, Vsync or similar, then that might be the best option for you.

After changing your options make sure to click "Update settings", or else the Source will just be black. The reason it doesn't automatically update is because NvFBC seems to lag the system for a second while it initialized, which would be very annoying :P

## Common Issues
Before you open an issue, please make sure you aren't running into any of these issues:

### `Failed to bind NvFBC context: 5` is shown all over the console!
Scroll up a bit, this error is most likely caused by some previous stuff failing.

### `Failed to create NvFBC session: 7` in the console.
This means NvFBC is not supported on your system. Did you apply the patch properly? Is your driver version supported (555 or older, 570 or newer)?

If all of this is the case, check if [NVIDIA's Capture SDK](https://developer.nvidia.com/capture-sdk) samples are working for you (run `make` in any of the samples and then execute the file). If those work, then what the heck is wrong with your system (=> Open an issue). If those don't work, idk bother NVIDIA support or something (=> Don't, just open an issue).

### The plugin doesn't show up. The console says `error: os_dlopen(\<path to so>): \<patch to so>: undefined symbol: gstate`.
You likely forgot to run obs with the `LD_PRELOAD` variable set. Make sure you set this variable to the **full path** to the `libobs-nvfbc-pre.so` file.

## How it works
Since OBS Studio switched from GLX to EGL, NvFBC became non-functional, as it does not support EGL. This is a fundamental issue with NvFBC and can only be fixed by NVIDIA. At first my idea was to spawn a subprocess with a shared memory area. The subprocess would then capture the frame buffer copy it to system memory, then into the shm and finally into an obs texture. While this did work, it was extremely slow and inefficient (to the point where I'm not sure if it was even faster than XSHM). Here's what we came up with instead (HUGE CREDIT to [0xNULLderef](https://github.com/0xNULLderef) for figuring out all the hacks):

NvFBC internally is a mess. A total mess. It creates a vulkan device and allocates a buffer on the gpu using it. It then passes the raw memory descriptor from the internal Vulkan buffer to the X driver, which will run an internal timer and fill the buffer with the framebuffer. NvFBC itself then internally uses some interop extension to create a GL texture that shares the same memory as the Vulkan image does. Then it post processes that data to convert it's format to whichever one the user selects. Finally it copies it to another GL texture, system buffer or cuda, depending on what you specify.

Since NvFBC internally uses GLX, the first idea was to replace the few GLX calls to the OBS EGL alternative. While it didn't crash, it also didn't work.

The actual solution was far more sophisticated (not really). We hooked all GLX/GL calls and nullified them. This way, NvFBC does not do anything except fill the Vulkan buffer with the framebuffer. Then we do some manual memory mapping and figure out the Vulkan buffer's memory descriptor. Then we use the same interop but into a gs_texture_2d provided by OBS. This way we can directly copy the Vulkan buffer to the OBS texture. This is the fastest and most efficient way to capture the framebuffer with NvFBC. It's not "zero-copy", but I doubt we'll get closer than this on X11 or even Wayland, ever.
