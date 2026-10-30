# DittoSR

DittoSR is a temporal upscaler for PC games. It started from Qualcomm's **Snapdragon Game Super Resolution 2** (GSR 2),
a light upscaler made for phones, and was reworked heavily to hold up in demanding PC games (AAA titles at 2–2.3x,
dense foliage, fast camera turns) while staying lighter than the usual alternatives. Thought for handheld PCs while also working pretty great on high-end PCs!

It ships as a drop-in **FidelityFX API** provider for **DirectX 12 and Vulkan**: a game that offers FSR 3.1 (or a
tool that loads FSR 3.1, such as OptiScaler) upscales through DittoSR instead, with no change to the game. One DLL
serves both APIs, under the names games load: `amd_fidelityfx_dx12.dll`, `amd_fidelityfx_loader_dx12.dll` and
`amd_fidelityfx_vk.dll`.

You can expect a perceived quality between FSR3.1 and FSR4.1 while being lighter than both. Also, a little .ini is next to the .dll on every game so you can configure the sharpness and frame blending with hot reloading (F9).

## Install

Build it (below), then run, in PowerShell:

```powershell
scripts\install.ps1 -Dir "<the folder with the game's amd_fidelityfx_*.dll>"
```

- The script looks for `amd_fidelityfx_dx12.dll` (FidelityFX SDK 1.1, FSR 3.1), `amd_fidelityfx_loader_dx12.dll` (SDK 2.x) and `amd_fidelityfx_vk.dll` (FSR 3.1 on Vulkan).
- Each one it finds is checked for AMD's signature, backed up to `dittosr-backup\` with its SHA-256, kept as `<name>.orig.dll`, and replaced by DittoSR.
- `dittosr.ini` goes next to it.
- Then choose **FSR 3.1** (or FSR) in the game's settings.

AMD's DLL, kept as `.orig.dll`, is optional. With it, frame generation keeps working, and with `[debug] enabled=1` **F10** switches between DittoSR and AMD's FSR live. Without it, DittoSR is the only provider, and frame generation is not available.

To restore the game's files:

```powershell
scripts\uninstall.ps1 -Dir "<the same folder>"
```

**OptiScaler** (not yet tested): OptiScaler's FSR 3.1 option loads `amd_fidelityfx_dx12.dll`. Install DittoSR in the folder OptiScaler loads it from and choose FSR 3.1 as OptiScaler's upscaler. With no FidelityFX DLL there, the script installs DittoSR alone, as `amd_fidelityfx_dx12.dll`.

**Linux (Proton)** (not yet tested): the same DLL should work under vkd3d-proton (DirectX 12) and with Vulkan games run through Proton.

**Online games:** don't. Replacing a game's DLLs can trigger anti-cheat systems.

## What it does

Each frame, four compute passes run on the game's command list:

| Pass           | Size          | What it does                                                                                                                                                            |
| -------------- | ------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `convert.hlsl` | render        | GSR 2's convert pass: the motion of the nearest of the 3x3 and its depth clip; DittoSR adds the nearness the disocclusion test reads                                    |
| `change.hlsl`  | 4x4, then 1x1 | how much of the image changed against the history (scene cuts, menus, fades settle at once)                                                                             |
| `upscale.hlsl` | output        | the reconstruction: the jittered render samples through an edge-directed Gaussian, blended with the history moved along the motion vectors, clipped against the samples |
| `sharpen.hlsl` | output        | a light contrast-adaptive sharpen, at rest and moving                                                                                                                   |

### Changes from GSR 2

GSR 2's convert pass is kept almost as it is. The upscale is DittoSR's own. Its main parts:

- **A per-pixel stability level** in the history's alpha. A pixel that has been stable for a few frames takes less of each new frame, so it stops shimmering, but a pixel whose history was just cut converges as fast as before.
- **The new frame's share follows how close its jittered samples landed.** Where a sample landed close to the pixel, the new frame counts more; where none did, its blurred estimate barely touches the history.
- **Disocclusion from the previous frame's depth**, instead of GSR 2's depth clip. A pixel's history is dropped only when, where it came from, the previous frame had a nearer surface moving differently. GSR 2's clip cut the history at every depth edge as soon as the camera moved, which made foliage noisy.
- **The motion-change test compares against the neighbours.** A camera that speeds up or slows down changes every pixel's motion alike, and must not cut the history; an object's edge changes it here and not a little way off.
- **The history is clipped by its low frequencies only.** The clip box comes from the current samples, which alias fine detail. Clipping only the history's local mean keeps the detail it has accumulated, unless the mean falls far outside the box.
- **Motion-aware reconstruction.** While moving, the colour comes through a wider Gaussian and the new frame takes a smaller, more even share. Otherwise the render grid shows as a stipple.
- **An HDR encoding** (Reinhard, then a square root) so blends behave in a perceptual domain, and a **fast settling** pass for scene changes.

## Results

### Cyberpunk 2077 (in game)

RTX 5070 Ti, 3840x2160 output, render 1670x939 (2.3x), the same view: **DittoSR 174 fps, FSR 146–148 fps.**

### Offline harness

A synthetic HDR scene upscaled to 1440x810 through the FidelityFX API, so DittoSR and AMD's FSR 3.1.5 take exactly the
same inputs (`test/harness.cpp`). PSNR is measured against a 4x4-supersampled reference, in a perceptual domain. Shimmer
is the mean difference between consecutive frames once the camera pan is undone (lower is steadier; 0 for a perfect
upscaler). XeSS 2.0.1's figures were measured through its own API with the same scenes.

| Scene                                        | DittoSR               | AMD FSR 3.1.5         | Intel XeSS 2.0.1      |
| -------------------------------------------- | --------------------- | --------------------- | --------------------- |
| Dense grass, still, 2x                       | 34.6 dB, shimmer 1.44 | 32.4 dB, shimmer 1.02 | 36.4 dB, shimmer 1.51 |
| Dense grass, pan 3 px/frame, 2.3x            | 32.0 dB, shimmer 1.24 | 31.4 dB, shimmer 5.08 | 34.4 dB, shimmer 3.25 |
| Dense grass, uneven pan 12 ± 3 px/frame, 2x  | 32.5 dB, shimmer 1.96 | 30.3 dB, shimmer 9.02 | 34.7 dB, shimmer 3.09 |
| Moving object over a detailed background, 2x | 28.4 dB               | 28.7 dB               | 30.7 dB               |

In short: steadier than FSR 3.1.5 and XeSS on foliage in motion, sharper than FSR on foliage, but XeSS still resolves
more fine detail, and objects that move on their own keep a little less contrast than with FSR.

### GPU time

The harness's timestamps either side of the upscale (the median of 400, `scripts\bench.ps1`), RTX 5070 Ti:

| Output    | FSR Quality (1.5x) | FSR Performance (2x) |
| --------- | ------------------ | -------------------- |
| 1920x1080 | 0.20 ms            | 0.19 ms              |
| 2560x1440 | 0.35 ms            | 0.33 ms              |
| 3840x2160 | —                  | 0.75 ms              |

## Integrated GPUs (ROG Ally, Steam Deck-class hardware)

On an integrated GPU, memory bandwidth is shared with the CPU and is the scarce resource. Two advanced keys (below) target it, both on by default:

- **`history_10bit=1`** (the default) keeps the history in RGB10A2 instead of RGBA16F: half the bytes for the texture the upscale reads and writes at output size every frame, and the sharpening reads again.
  - Its stores are dithered by half a step, so the small blends of a stable pixel still converge.
  - In the harness this also measured slightly better than RGBA16F (34.6 against 34.1 dB on still grass).
- **`fp16=-1`** (the default) runs the upscale's and the sharpening's colour work in half precision on AMD GPUs.
  - RDNA's packed 16-bit math runs at twice the FP32 rate.
  - On NVIDIA it measured about 7 % slower, so it stays off there.
  - `fp16=1` forces it on, and `fp16=0` forces it off. It falls back to FP32 by itself on a GPU without native 16-bit math.

FP8 doesn't help here. RDNA3 (the Radeon 780M) has no FP8 math, and an 8-bit float history would band visibly.

To measure on the device, copy `build\DittoSR.zip` (made by `scripts\package.ps1`), extract it, and run:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\bench.ps1
```

- It times every combination of history format and precision, at 1080p and 1440p, at 1.5x and 2x.
- It writes the table to `scripts\bench-results.txt`.
- It needs no game, only D3D12.

## Vulkan

The Vulkan path runs the same passes, compiled from the same HLSL to SPIR-V, on the device the game hands the FidelityFX
API. It assumes nothing beyond Vulkan 1.1 core: no extension, and no optional feature the game might not have enabled.
That costs three things against DirectX 12:

- **The history is always RGBA16F.** `history_10bit` doesn't apply: RGB10A2 as a storage image needs an optional feature.
- **The shaders are always FP32.** `fp16` doesn't apply: 16-bit math needs an extension the game would have had to enable.
- **The game's output must be RGBA16F, RGBA8, RGB10A2, R11G11B10F or RGBA32F.** Every storage image has its format spelled out, so the final pass is compiled once per output format. BGRA8 and sRGB outputs are refused, and the log says so. The debug views 2–4 are approximate.

Tested in Let's Go Recomp, whose FSR 3.1 option runs on Vulkan, with RGBA8 sRGB-encoded colour. Against AMD's FSR 3.1 in
the same scripted walk, DittoSR was steadier from frame to frame and slightly softer, as on DirectX 12.

### Checking that it runs

`dittosr.log`, next to the DLL, records:

- the context's creation (`upscale context ...: DittoSR`);
- the first upscales in full;
- `DittoSR ran (frame 1)`.

`[debug] indicator=1` in `dittosr.ini` draws a small green square in the top-left corner while DittoSR upscales.

## Settings (`dittosr.ini`)

The shipped file has five keys. **F9** reads the file again while playing, so `sharpness` and `frame_weight` can be tuned live.

| Section     | Key            | Meaning                                                                                                                                                                                                           |
| ----------- | -------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `[dittosr]` | `enabled`      | `1` DittoSR; `0` AMD's FSR instead (needs AMD's DLL alongside as `.orig.dll`)                                                                                                                                     |
| `[dittosr]` | `sharpness`    | at rest `0` soft, `0.6` the image as reconstructed, `1` sharpened; moving, sharpened more the higher; 0.2                                                                                                                   |
| `[dittosr]` | `frame_weight` | the weight of each new frame against the accumulated image, 0.25 to 4; 2. Lower: more persistence (steadier, smoother, more motion blur and trailing). Higher: more responsive (crisper in motion, more shimmer). |
| `[debug]`   | `enabled`      | `1`: F10 switches DittoSR ↔ AMD's FSR live, and the log describes the first upscales in full                                                                                                                      |
| `[debug]`   | `indicator`    | `1`: a small green square in the top-left corner while DittoSR upscales                                                                                                                                           |

Above 1, the image at rest takes only a quarter of the excess (3 acts as 1.5 there): more of each new frame at rest only brings in the jittered samples' grain, which looks like oversharpening.

What `frame_weight` does in the harness (dense grass, 2x, `sharpness` 0.5; still: `vel=0`, uneven pan: `vel=4 wobble=4`, moving object: the tiles scene):

| `frame_weight` | Still: PSNR, shimmer | Uneven pan: PSNR, shimmer | Moving object: PSNR |
| -------------- | -------------------- | ------------------------- | ------------------- |
| 0.5            | 32.1 dB, 0.67        | 31.7 dB, 1.91             | 27.6 dB             |
| 1              | 33.7 dB, 1.32        | 33.0 dB, 2.82             | 28.6 dB             |
| 2              | 34.1 dB, 1.67        | 33.3 dB, 4.20             | 29.3 dB             |
| 3              | 34.2 dB, 2.03        | 32.8 dB, 5.61             | 29.8 dB             |

### Advanced keys

These are not in the shipped file. They are read from `[dittosr]` when added there, for tuning and diagnosis:

| Key                                                                                                                                                                                                                                            | Meaning                                                                                                                                                |
| ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `history_10bit`, `fp16`                                                                                                                                                                                                                        | performance: the history in RGB10A2 (`1`, default) or RGBA16F (`0`); the FP16 shaders on AMD only (`-1`, default), always (`1`) or never (`0`)         |
| `toggle_key`, `reload_key`                                                                                                                                                                                                                     | the keys' virtual-key codes (`0x79` = F10, `0x78` = F9; `0` turns one off)                                                                             |
| `debug_view`, `log_dispatches`                                                                                                                                                                                                                 | views instead of the image (`1` motion vectors, `2` the new frame's share, `3` the stability level, `4` the clip); how many upscales the log describes |
| `exposure_scale`, `encode_power`, `reactive_scale`                                                                                                                                                                                             | the HDR encoding; how far the game's reactive mask raises the new frame's share                                                                        |
| `width`, `conf`, `share_far`, `share_near`, `alpha_rest`, `gamma_rest`, `motion_gamma`, `motion_share`, `motion_minmax`, `motion_width`, `motion_near`, `split_clip`, `depth_mode`, `react`, `edge`, `lock_lines`, `change_scale`, `fov_h_deg` | the reconstruction's knobs (`src/dittosr.h` describes each, with its default)                                                                          |
| `jitter_sign_x`, `jitter_sign_y`, `mv_sign`                                                                                                                                                                                                    | convention flips, for diagnosis                                                                                                                        |

## Build

Needs Visual Studio 2022 (C++ tools), the Windows SDK's `dxc` (DirectX 12 shaders), and a `dxc` built with SPIR-V for the Vulkan shaders. The Windows SDK's isn't. Either:
- extract the [DirectXShaderCompiler release](https://github.com/microsoft/DirectXShaderCompiler/releases) into `tools\dxc\` (`tools\dxc\bin\x64\dxc.exe`; `tools\` is ignored by git), or
- install the Vulkan SDK (`%VULKAN_SDK%\Bin\dxc.exe`).

```bat
build.bat
```

The output goes to `build\`:

- `amd_fidelityfx_dx12.dll`, `amd_fidelityfx_loader_dx12.dll` and `amd_fidelityfx_vk.dll`: the same DLL under the three names games load;
- `harness.exe`: the offline test.

`scripts\package.ps1` packs these with the scripts, the ini and the documents into `build\DittoSR.zip`.

## Offline test

```bat
build\harness.exe <folder> <png prefix> [dll=amd_fidelityfx_dx12.dll] [scene=tiles|grass] [frames=64] [out=1440x810] [ratio=1.5] [vel=3] [dense=1] [wobble=0] [blur=0] [sharpen=0] [psnr=1]
```

`<folder>` holds the DLL and a `dittosr.ini`. To compare with AMD's FSR, add AMD's DLL as `<dll>.orig.dll` and, for the SDK 2.x loader, its `amd_fidelityfx_upscaler_dx12.dll`; then set `enabled=0`.

The scenes:

- `tiles`: a detailed background panning and a nearer square moving the other way, for ghosting and disocclusion;
- `grass`: thin blades over a cracked ground, the camera panning.

The grass scene's options:

- `vel`: the pan's speed, in px/frame;
- `wobble`: varies the speed each frame, like a hand on the mouse;
- `dense`: more, thinner blades;
- `blur`: adds motion blur before the upscale, as games do.

The harness prints:

- the providers the game would see;
- PSNR and shimmer;
- the upscale's GPU time, measured by timestamps (the median after 8 warm-up frames).

It also writes PNGs of the output and the reference.

`psnr=0` measures the time alone. It skips the reference and replays 16 rendered frames, so the GPU never idles long enough to drop its clocks.

## Roadmap

- A Vulkan mode in the offline harness (today it drives the DirectX 12 path only).
- Testing with OptiScaler, and under Proton.
- Release builds from CI.

## License

DittoSR is MIT-licensed ([LICENSE](LICENSE)). It derives from Snapdragon GSR 2 (BSD 3-Clause). See
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) and `licenses/`.
