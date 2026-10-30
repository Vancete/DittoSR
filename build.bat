@echo off
rem Builds build\amd_fidelityfx_dx12.dll, build\amd_fidelityfx_loader_dx12.dll and build\amd_fidelityfx_vk.dll (DittoSR:
rem one DLL, D3D12 and Vulkan, under the three names games load) and build\harness.exe (the offline test).
rem Needs Visual Studio's C++ tools, the Windows SDK's dxc (DXIL) and a dxc with SPIR-V (the Vulkan shaders): the
rem DirectXShaderCompiler release in tools\dxc\ (bin\x64\dxc.exe) or the Vulkan SDK's (%VULKAN_SDK%\Bin\dxc.exe).
setlocal
set "ROOT=%~dp0"
set "OUT=%ROOT%build"
if not exist "%OUT%\gen" mkdir "%OUT%\gen"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"

if defined VCToolsInstallDir goto :tools
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:tools
set DXC=dxc.exe
where dxc.exe >nul 2>nul || set DXC="%WindowsSdkVerBinPath%x64\dxc.exe"
set "SPV=%ROOT%tools\dxc\bin\x64\dxc.exe"
if not exist "%SPV%" set "SPV=%VULKAN_SDK%\Bin\dxc.exe"
if not exist "%SPV%" (echo no dxc with SPIR-V: put the DirectXShaderCompiler release in tools\dxc\ or install the Vulkan SDK & exit /b 1)

rem D3D12 (DXIL)
for %%s in (convert change upscale sharpen) do (
  %DXC% -nologo -T cs_6_0 -E main -O3 -Fh "%OUT%\gen\cs_%%s.h" -Vn g_cs_%%s "%ROOT%shaders\%%s.hlsl" || exit /b 1
)
rem the FP16 variants: native 16-bit types (SM 6.2), picked at run time where the GPU supports them
for %%s in (upscale sharpen) do (
  %DXC% -nologo -T cs_6_2 -enable-16bit-types -D DITTO_FP16=1 -E main -O3 -Fh "%OUT%\gen\cs_%%s16.h" -Vn g_cs_%%s16 "%ROOT%shaders\%%s.hlsl" || exit /b 1
)
rem Vulkan (SPIR-V): one descriptor set - b0 the constants at 0, t0-t6 at 10, u0-u1 at 20, s0 at 30 (dittosr_vk.cpp)
set SPVF=-nologo -spirv -fspv-target-env=vulkan1.1 -fvk-b-shift 0 0 -fvk-t-shift 10 0 -fvk-u-shift 20 0 -fvk-s-shift 30 0 -D DITTO_VK=1 -T cs_6_0 -E main -O3
for %%s in (convert change upscale) do (
  "%SPV%" %SPVF% -Fh "%OUT%\gen\spv_%%s.h" -Vn g_spv_%%s "%ROOT%shaders\%%s.hlsl" || exit /b 1
)
rem the sharpening pass writes the game's output: once per output format
for %%f in (rgba16f rgba8 rgb10a2 r11g11b10f rgba32f) do (
  "%SPV%" %SPVF% -DOUTFMT=\"%%f\" -Fh "%OUT%\gen\spv_sharpen_%%f.h" -Vn g_spv_sharpen_%%f "%ROOT%shaders\sharpen.hlsl" || exit /b 1
)

cl /nologo /std:c++17 /O2 /MT /W3 /EHsc /LD /I"%ROOT%src" /I"%OUT%\gen" /I"%ROOT%third_party\vulkan\include" "%ROOT%src\ffx_dll.cpp" ^
   "%ROOT%src\dittosr.cpp" "%ROOT%src\dittosr_vk.cpp" "%ROOT%src\common.cpp" /Fo"%OUT%\obj\\" /Fe"%OUT%\amd_fidelityfx_dx12.dll" ^
   /link d3d12.lib dxgi.lib user32.lib || exit /b 1
copy /y "%OUT%\amd_fidelityfx_dx12.dll" "%OUT%\amd_fidelityfx_loader_dx12.dll" >nul || exit /b 1
copy /y "%OUT%\amd_fidelityfx_dx12.dll" "%OUT%\amd_fidelityfx_vk.dll" >nul || exit /b 1

cl /nologo /std:c++17 /O2 /MT /W3 /EHsc /I"%ROOT%src" /I"%ROOT%test" "%ROOT%test\harness.cpp" ^
   /Fo"%OUT%\obj\\" /Fe"%OUT%\harness.exe" /link d3d12.lib dxgi.lib || exit /b 1
echo built %OUT%\amd_fidelityfx_dx12.dll, amd_fidelityfx_loader_dx12.dll, amd_fidelityfx_vk.dll, harness.exe
