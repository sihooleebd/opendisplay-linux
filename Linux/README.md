# OpenDisplay for Linux

This target connects the existing, unmodified iOS/iPadOS receiver to a KDE or
Hyprland Wayland session. It discovers `_opensidecar._tcp` services with Avahi
or opens the receiver's port through usbmuxd, captures with PipeWire, and
streams Annex B H.264. On VA-API it encodes in-process through libavcodec and
takes the compositor's frame as a DMA-BUF without copying it; NVENC and
software encoding still run FFmpeg as a subprocess. The command-line client
and an initial Kirigami control application share the same connection engine.

## Arch Linux dependencies

```sh
sudo pacman -S --needed base-devel cmake qt6-base qt6-declarative qt6-wayland \
  kirigami pipewire avahi libusbmuxd usbmuxd ffmpeg libdrm libkscreen wayland \
  xdg-desktop-portal xdg-desktop-portal-kde
```

`ffmpeg` is both a build and a runtime dependency. The VA-API encoder links
`libavcodec`, `libavfilter` and `libavutil`, and `libdrm` supplies the DMA-BUF
format definitions; the `ffmpeg` binary is still invoked for the NVENC and
`libx264` paths, so it must also stay on `PATH`.

For Hyprland, also install `hyprland xdg-desktop-portal-hyprland`. The package
keeps the KDE dependencies so one binary can select either backend.

For hardware encoding, install the driver for the GPU: `libva-mesa-driver` for AMD,
`intel-media-driver` for modern Intel GPUs, or `nvidia-utils` for NVIDIA.
`libva-utils` is useful for checking VA-API with `vainfo`. The process must be
able to open `/dev/dri/renderD128`; normal Arch installations grant this through
the active desktop session.

USB requires the device to be unlocked and paired with the machine. Accept the
device's Trust prompt, then verify the usbmuxd path before starting OpenDisplay:

```sh
idevice_id -l
idevicepair pair       # first connection only
idevicepair validate
```

If `idevice_id` cannot see the device, reconnect the cable and inspect
`journalctl -u usbmuxd.service`; OpenDisplay cannot use a device that usbmuxd
has not finished adding. A journal `lockdown error -8` is a failed mux
preflight: unlock and disconnect the device, restart `usbmuxd.service`, then
reconnect and accept the Trust prompt. Start the service if socket activation
does not do so automatically. Wi-Fi discovery requires a running Avahi daemon;
installing the package does not enable system services:

```sh
sudo systemctl enable --now avahi-daemon.service
systemctl status avahi-daemon.service
```

## Build and test

```sh
cmake -S Linux -B build/linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux -j
ctest --test-dir build/linux --output-on-failure
```

## Build and install the Arch package

The `-git` package follows the pushed `linux-port` branch on the public
[GitHub repository](https://github.com/tixwho/opendisplay-linux). From the
repository root, use a clean package build so an older clone of `main` cannot
remain in makepkg's `src/` directory:

```sh
(cd Linux/packaging/arch && makepkg --cleanbuild --syncdeps --install)
```

The short equivalent is `makepkg -Csi`. Building the package consumes the
pushed branch, not uncommitted files in the current worktree. Push a commit to
`origin/linux-port` before packaging when testing new local changes.

## Run

Open the existing OpenDisplay app on the iOS device, then run:

```sh
./build/linux/opendisplay-gui
./build/linux/opendisplay-linux --transport auto --mode extend
```

The GUI exposes the CLI defaults and common monitor-layout overrides. It uses
the desktop's StatusNotifier tray when available. On a compositor without a
compatible tray host it remains a normal launcher window, and closing the
window exits instead of leaving an invisible background process. On Hyprland,
a StatusNotifier-capable bar such as Waybar enables tray behavior.

The compositor backend is detected automatically; override it with
`--compositor kde` or `--compositor hyprland`. The first run displays the
desktop's capture permission dialog. Other useful forms are:

```sh
./build/linux/opendisplay-linux --list
./build/linux/opendisplay-linux --transport usb --udid DEVICE_UDID
./build/linux/opendisplay-linux --transport wifi --host 192.168.1.40
./build/linux/opendisplay-linux --encoder vaapi --vaapi-device /dev/dri/renderD128
./build/linux/opendisplay-linux --encoder nvenc --mode mirror --no-input
./build/linux/opendisplay-linux --compositor hyprland --reference-monitor eDP-1
./build/linux/opendisplay-linux --no-zero-copy --verbose
```

### Virtual monitor layout

In extend mode, OpenDisplay queries the selected compositor before opening its
portal. One enabled monitor is selected automatically; with two or more, pass
its connector name or current backend id explicitly:

```sh
opendisplay-linux --reference-monitor DP-1
opendisplay-linux --reference-monitor eDP-1 --extend-to right --align-to bottom
opendisplay-linux --extend-to bottom --align-to right
```

The default is bottom-right: extend right and align bottom. Left/right extension
accepts `top`, `bottom`, or `center` alignment; top/bottom extension accepts
`left`, `right`, or `center`. Positions use compositor logical coordinates.

The automatic mode starts with the iPad's native pixels. It uses physical DPI
when both panel sizes are available, otherwise the iOS-reported native scale;
the result is rounded to a 0.05 scale step. Pixel dimensions are nudged to the
nearest values that produce integer logical dimensions; widths are also made
divisible by eight for KDE's CVT-generated custom modes. Hyprland keeps native
pixel dimensions when they already produce an integer logical size. Detection
and any part of the calculation can be overridden:

```sh
opendisplay-linux --virtual-resolution 2420x1668 --display-scale 1.25
opendisplay-linux --virtual-refresh 60 --ipad-size-mm 263x181
opendisplay-linux --reference-resolution 3840x2160 --reference-scale 1.5 \
  --reference-size-mm 597x336
opendisplay-linux --reference-geometry 2560x1440+0+0
```

`--scale` is the video encoder resolution multiplier and does not change
desktop display scaling. It multiplies the resolution the compositor actually
captures, not the receiver's panel size. At the default of `1.0` the encoder
therefore matches the captured frame exactly and the GPU only converts colour,
with no scaling pass: a virtual output nudged to produce integer logical
geometry rarely lands on the receiver's native pixels, and resampling every
frame to cover that handful of pixels costs a filter pass and visibly softens
text. The receiver's own display scaler closes the gap for nothing. Lowering
it (`--scale 0.8`) is the lever for trading resolution against latency, since
it shrinks both the colour conversion and the encode. KDE custom virtual modes require Plasma/libkscreen
6.6 or newer. OpenDisplay uses libkscreen in-process so output identity and
detailed configuration errors remain reliable while KDE adds or renumbers
outputs.

On Hyprland, OpenDisplay creates a named headless output with `hyprctl`, applies
the same resolution, scaling, and placement calculation, and asks the
ScreenCast portal to capture a monitor. Select the displayed `OpenDisplay`
monitor in the share picker; OpenDisplay asks the portal to remember that
choice with a restore token, so the picker only has to be answered once per
captured output. Touch input uses Hyprland's
`zwlr_virtual_pointer_manager_v1` support and does not require the unsupported
RemoteDesktop portal. Current Hyprland Lua monitor rules are used first, with
the pre-0.55 `keyword monitor` command as a compatibility fallback. The
headless output is removed when the session stops. Before XDPH opens its share
picker, OpenDisplay pins it with a Hyprland window rule to the output the
pointer is on — falling back to the reference monitor when the pointer sits on
the virtual display — and focuses that output, so the authorization prompt
never opens on a screen nobody can see. The reference
output is temporarily pinned to its detected geometry while the headless output
exists, preventing Hyprland `auto` rules from reversing their relative order;
the user's configuration is reloaded after teardown.

If Bonjour discovery is unavailable, confirm that the phone app is open and
inspect the advertised service with `avahi-browse -rt _opensidecar._tcp`.
You can bypass Avahi when the receiver's address is known:

```sh
opendisplay-linux --transport wifi --host 192.168.1.40 --port 9000
```

`--encoder auto` prefers VA-API, then NVENC, and falls back to `libx264`.

### Zero-copy capture

With VA-API, OpenDisplay asks PipeWire for the compositor's frame as a DMA-BUF
and imports it directly into a VA-API surface, then converts and encodes it
in-process. Nothing touches system memory on the way: the frame is never
copied out of the GPU, never written through a pipe, and never uploaded back.

The pieces degrade independently, so there is always a working path:

- A compositor that will not share GPU buffers negotiates system memory
  instead, and the in-process encoder uploads each frame.
- A machine where a VA-API device cannot be opened falls back to the FFmpeg
  subprocess, as do `--encoder nvenc` and `--encoder software`, neither of
  which can accept a GPU buffer through a pipe.

`--no-zero-copy` forces frames through system memory. `--verbose` reports
which path was negotiated and breaks the encode down per stage every few
seconds, which is the quickest way to see where latency is going:

```
PipeWire format: 2380x1666 BGRA @ 0/1 fps, GPU buffers (zero copy)
Encoding 2380x1666 to 2380x1666 (convert only, no rescale)
Encoder stages (ms over 41 frames): surface p50 0.0 p95 0.1, convert p50 1.9 \
p95 5.4, encode p50 5.3 p95 9.1
```

`surface` is acquiring the frame -- importing a DMA-BUF, or uploading a copy;
`convert` is the GPU colour conversion to NV12; `encode` is H.264 itself.

Use `Linux/tools/fake_receiver.py` to exercise the TCP framing without an iOS
device; it does not decode video or advertise Bonjour.

## Current MVP limits

KDE Plasma Wayland and Hyprland are supported. Hyprland's portal picker cannot
yet be preselected by output name, so the requested monitor must be chosen in
the dialog. Audio,
clipboard, cursor sprites, encryption, reconnection, and multi-device sessions
remain out of scope. Compositor-neutral layout lives in `display_layout`, while
KScreen and Hyprland output operations remain isolated in their respective
controllers. Later compositor backends need not change transport, protocol,
layout planning, or encoding code.
