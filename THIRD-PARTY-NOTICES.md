# Third-party notices

DittoSR includes or derives from the following. Binary releases ship this file and the `licenses/` folder.

| Component | Where | License |
|---|---|---|
| Snapdragon Game Super Resolution 2 (Qualcomm) | `shaders/convert.hlsl`: the depth clip and the convert pass's structure; the upscale grew from GSR 2's | BSD 3-Clause, [licenses/LICENSE-snapdragon-gsr.txt](licenses/LICENSE-snapdragon-gsr.txt) |
| AMD FidelityFX API headers | `src/ffx_api/` | MIT, [src/ffx_api/LICENSE.txt](src/ffx_api/LICENSE.txt) |
| Vulkan headers (The Khronos Group) | `third_party/vulkan/include/` | Apache-2.0 OR MIT (the notice in each file) |
| stb_image_write (Sean Barrett) | `test/stb_image_write.h`, the offline test only | public domain / MIT |

DittoSR is not affiliated with or endorsed by Qualcomm or AMD. "FidelityFX" and "FSR" are AMD's trademarks; they name
the API DittoSR implements. Vulkan is a trademark of The Khronos Group.
