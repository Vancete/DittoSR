@echo off
rem Builds build\DittoSRLoader.exe (the loader: a window that installs DittoSR into a game) with the csc.exe of
rem .NET Framework 4.x, part of Windows - no SDK to install. It runs on any Windows 10 or 11 as it is. Its icon:
rem dittosr.ico, from scripts\make_icon.py.
setlocal
set "ROOT=%~dp0.."
set "OUT=%ROOT%\build"
set "CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if not exist "%CSC%" set "CSC=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe"
if not exist "%CSC%" (echo no .NET Framework 4.x csc.exe in %WINDIR%\Microsoft.NET & exit /b 1)
if not exist "%OUT%" mkdir "%OUT%"
"%CSC%" /nologo /target:winexe /optimize+ /platform:anycpu /out:"%OUT%\DittoSRLoader.exe" ^
   /win32manifest:"%~dp0app.manifest" /win32icon:"%~dp0dittosr.ico" ^
   /r:System.dll /r:System.Core.dll /r:System.Drawing.dll /r:System.Windows.Forms.dll ^
   "%~dp0DittoSRLoader.cs" || exit /b 1
echo built %OUT%\DittoSRLoader.exe
