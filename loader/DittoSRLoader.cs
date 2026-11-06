// DittoSR Loader: a small window that installs DittoSR into a game - install.ps1's job without PowerShell - in place of
// its FSR or of its XeSS (libxess.dll, for games whose FSR can't be reached).
//   Pick the game's folder (or one of the Steam games found), and it looks below it for the FidelityFX DLLs games load:
// amd_fidelityfx_dx12.dll (FidelityFX SDK 1.1, FSR 3.1), amd_fidelityfx_loader_dx12.dll (SDK 2.x), amd_fidelityfx_vk.dll
// (FSR 3.1 on Vulkan) and amd_fidelityfx_upscaler_dx12.dll where the folder has neither of the first two (an SDK 2.x game
// with the loader built into its executable: it loads the upscaler by name). Installing does what install.ps1 does:
// AMD's file (checked for AMD's signature) backed up to <folder>\dittosr-backup\ with its SHA-256, copied to
// <name>.orig.dll (what DittoSR hands frame generation and the FSR toggle to) and replaced by DittoSR; dittosr.ini next
// to it (kept if already there). Restoring does what uninstall.ps1 does. The two are interchangeable with the scripts.
//   XeSS: the game's libxess.dll (checked for Intel's signature, and for exports DittoSR's proxy forwards all of) backed
// up the same way and kept as libxess_orig.dll, which the proxy creates XeSS's contexts with - only its D3D12 execute
// runs DittoSR. Not in the scripts.
//   DittoSR's DLLs and the default dittosr.ini: next to this executable, in build\ (the release's layout) or alongside it
// (the repository's build\), else in ..\build\ and ..\.
// Built by loader\build.bat with the csc.exe of .NET Framework 4.x (part of Windows): C# 5, no other dependency.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

[assembly: AssemblyTitle("DittoSR Loader")]
[assembly: AssemblyProduct("DittoSR")]
[assembly: AssemblyVersion("0.4.0.0")]
[assembly: AssemblyFileVersion("0.4.0.0")]

namespace DittoSR {

// A FidelityFX or XeSS DLL the scan found, and what it is now.
class Candidate {
    public string Folder, Name, State;
    public bool Installable, Recommended;
    public bool Custom;  // (an OptiScaler custom provider's folder: created on install, removed on restore)
    public string Path { get { return System.IO.Path.Combine(Folder, Name); } }
    public bool Xess { get { return Name == Files.Xess; } }
}

static class Files {
    public const string Dx12 = "amd_fidelityfx_dx12.dll", Loader = "amd_fidelityfx_loader_dx12.dll",
                        Vk = "amd_fidelityfx_vk.dll", Upscaler = "amd_fidelityfx_upscaler_dx12.dll";
    public static readonly string[] All = { Dx12, Loader, Vk, Upscaler };
    public const string Xess = "libxess.dll", XessOrig = "libxess_orig.dll";
    public static readonly string[] Restorable = { Dx12, Loader, Vk, Upscaler, Xess };
    public const string Backup = "dittosr-backup", Ini = "dittosr.ini", Log = "dittosr.log", XessLog = "dittosr-xess.log";

    public static string Orig(string folder, string name) {
        // (the XeSS proxy's forwarders name libxess_orig.dll)
        return System.IO.Path.Combine(folder, name == Xess ? XessOrig : System.IO.Path.GetFileNameWithoutExtension(name) + ".orig.dll");
    }

    // the vendor a DLL of that name is signed by
    public static string Vendor(string name) {
        return name == Xess ? "Intel Corporation" : "Advanced Micro Devices";
    }

    // ---- the vendor's signature (Authenticode, as install.ps1's Get-AuthenticodeSignature)
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    class TrustFileInfo {
        public uint cbStruct = (uint)Marshal.SizeOf(typeof(TrustFileInfo));
        public string path;
        public IntPtr file = IntPtr.Zero, knownSubject = IntPtr.Zero;
    }
    [StructLayout(LayoutKind.Sequential)]
    class TrustData {
        public uint cbStruct = (uint)Marshal.SizeOf(typeof(TrustData));
        public IntPtr policyCallbackData = IntPtr.Zero, sipClientData = IntPtr.Zero;
        public uint uiChoice = 2;               // WTD_UI_NONE
        public uint revocationChecks = 0;       // WTD_REVOKE_NONE
        public uint unionChoice = 1;            // WTD_CHOICE_FILE
        public IntPtr file;
        public uint stateAction = 0;            // WTD_STATEACTION_IGNORE
        public IntPtr stateData = IntPtr.Zero, urlReference = IntPtr.Zero;
        public uint provFlags = 0x10;           // WTD_REVOCATION_CHECK_NONE (no network)
        public uint uiContext = 0;
        public IntPtr signatureSettings = IntPtr.Zero;
    }
    [DllImport("wintrust.dll", CharSet = CharSet.Unicode)]
    static extern int WinVerifyTrust(IntPtr hwnd, [MarshalAs(UnmanagedType.LPStruct)] Guid action, [In] TrustData data);
    static readonly Guid GenericVerifyV2 = new Guid("00AAC56B-CD44-11d0-8CC2-00C04FC295EE");

    public static bool SignedBy(string path, string vendor) {
        var info = new TrustFileInfo { path = path };
        IntPtr p = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(TrustFileInfo)));
        try {
            Marshal.StructureToPtr(info, p, false);
            if (WinVerifyTrust(IntPtr.Zero, GenericVerifyV2, new TrustData { file = p }) != 0) return false;
            return X509Certificate.CreateFromSignedFile(path).Subject.Contains(vendor);
        } catch (Exception) {
            return false;
        } finally {
            Marshal.DestroyStructure(p, typeof(TrustFileInfo));
            Marshal.FreeHGlobal(p);
        }
    }

    // DittoSR's version name in a DLL ("DittoSR 0.4.0"), or null: not DittoSR's (AMD's DLLs are tens of MB: not read)
    public static string DittoVersion(string path) {
        try {
            var fi = new FileInfo(path);
            if (fi.Length > 16 << 20) return null;
            var m = Regex.Match(Encoding.ASCII.GetString(File.ReadAllBytes(path)), @"DittoSR \d+\.\d+\.\d+");
            return m.Success ? m.Value : null;
        } catch (Exception) {
            return null;
        }
    }

    public static string Sha256(string path) {
        using (var sha = SHA256.Create())
        using (var f = File.OpenRead(path))
            return BitConverter.ToString(sha.ComputeHash(f)).Replace("-", "");
    }

    // a PE DLL's exported names
    public static HashSet<string> Exports(string path) {
        var names = new HashSet<string>(StringComparer.Ordinal);
        byte[] d = File.ReadAllBytes(path);
        int pe = BitConverter.ToInt32(d, 0x3c), sections = BitConverter.ToUInt16(d, pe + 6), opt = pe + 24;
        int dirs = opt + (BitConverter.ToUInt16(d, opt) == 0x20b ? 112 : 96), sec = opt + BitConverter.ToUInt16(d, pe + 20);
        Func<uint, int> at = rva => {
            for (int i = 0; i < sections; i++) {
                int h = sec + 40 * i;
                uint vs = BitConverter.ToUInt32(d, h + 8), va = BitConverter.ToUInt32(d, h + 12), rs = BitConverter.ToUInt32(d, h + 16);
                if (rva >= va && rva < va + Math.Max(vs, rs)) return (int)(rva - va + BitConverter.ToUInt32(d, h + 20));
            }
            throw new InvalidDataException("not a DLL: " + path);
        };
        uint exportRva = BitConverter.ToUInt32(d, dirs);
        if (exportRva == 0) return names;
        int e = at(exportRva), count = BitConverter.ToInt32(d, e + 24), table = at(BitConverter.ToUInt32(d, e + 32));
        for (int i = 0; i < count; i++) {
            int p = at(BitConverter.ToUInt32(d, table + 4 * i)), end = Array.IndexOf(d, (byte)0, p);
            names.Add(Encoding.ASCII.GetString(d, p, end - p));
        }
        return names;
    }

    public static string VendorVersion(string path) {
        string name = System.IO.Path.GetFileName(path), v = FileVersionInfo.GetVersionInfo(path).ProductVersion;
        string what = name == Xess ? "Intel's XeSS"
                    : string.Equals(name, Loader, StringComparison.OrdinalIgnoreCase) ? "AMD's FidelityFX loader"
                    : string.Equals(name, Upscaler, StringComparison.OrdinalIgnoreCase) ? "AMD's FSR upscaler" : "AMD's FSR";
        return string.IsNullOrEmpty(v) ? what : what + " " + v;
    }
}

// An OptiScaler install: its folder (with OptiScaler.ini) and its version - its DLL's, whatever name it goes by
// (dxgi.dll, winmm.dll, OptiScaler.asi...): the one whose version resource names OptiScaler.
class OptiScaler {
    public string Folder, Main, Version;  // (Folder: OptiScaler.ini's; Main: its main folder, where custom\ goes)
    public int Major;
    // v10 loads FidelityFX providers from custom\<name>\ffx\ (DirectX 12) and custom\<name>\ffx_vk\ (Vulkan), in its
    // main folder
    public bool Custom { get { return Major >= 10; } }
    public string CustomFolder(bool vk) { return Path.Combine(Main, "custom", "DittoSR", vk ? "ffx_vk" : "ffx"); }

    public static OptiScaler At(string dir) {
        var o = new OptiScaler { Folder = dir, Main = MainFolder(dir) };
        try {
            foreach (string d in new[] { dir, o.Main }.Distinct(StringComparer.OrdinalIgnoreCase))
                foreach (string f in Directory.GetFiles(d, "*.dll").Concat(Directory.GetFiles(d, "*.asi"))) {
                    var v = FileVersionInfo.GetVersionInfo(f);
                    if (!string.Equals(v.ProductName, "OptiScaler", StringComparison.OrdinalIgnoreCase) || v.FileMajorPart < o.Major) continue;
                    o.Major = v.FileMajorPart;
                    o.Version = string.IsNullOrEmpty(v.ProductVersion) ? v.FileVersion : v.ProductVersion;
                }
        } catch (Exception) { }
        return o;
    }

    // OptiScaler's own rule: [Libraries] OptiDllPath (by default "OptiScaler", relative to the game's folder), else -
    // not a folder there - the game's folder itself
    static string MainFolder(string dir) {
        string main = "OptiScaler";
        try {
            string section = "";
            foreach (string raw in File.ReadAllLines(Path.Combine(dir, "OptiScaler.ini"))) {
                string line = raw.Trim();
                if (line.StartsWith("[")) { section = line.Trim('[', ']').Trim(); continue; }
                int eq = line.IndexOf('=');
                if (eq < 0 || !string.Equals(section, "Libraries", StringComparison.OrdinalIgnoreCase)) continue;
                if (!string.Equals(line.Substring(0, eq).Trim(), "OptiDllPath", StringComparison.OrdinalIgnoreCase)) continue;
                string v = line.Substring(eq + 1).Trim().Trim('"');
                if (v.Length > 0 && !string.Equals(v, "auto", StringComparison.OrdinalIgnoreCase)) main = v;
            }
        } catch (Exception) { }
        try {
            string p = Path.IsPathRooted(main) ? main : Path.Combine(dir, main);
            if (Directory.Exists(p)) return Path.GetFullPath(p);
        } catch (Exception) { }
        return dir;
    }
}

// The scan: every folder at or below the one picked that holds a FidelityFX or XeSS DLL, and the ones OptiScaler is in
// (its OptiScaler.ini: it loads amd_fidelityfx_dx12.dll from its own folder for its FSR 3.1 option; v10 also custom
// providers from its custom folder, which the scan leaves out - DittoSR there is not a game's FSR).
static class Scanner {
    // the exports DittoSR's libxess.dll has (null: not found next to the loader) - a game's XeSS with others would not load
    public static HashSet<string> XessProxyExports;

    public static List<Candidate> Scan(string root, Func<bool> cancelled, List<string> optiScaler) {
        var found = new List<Candidate>();
        var stack = new Stack<string>();
        stack.Push(root);
        while (stack.Count > 0 && !cancelled()) {
            string dir = stack.Pop();
            Look(dir, found);
            bool opti = File.Exists(System.IO.Path.Combine(dir, "OptiScaler.ini"));
            if (opti) optiScaler.Add(dir);
            string[] subs;
            try { subs = Directory.GetDirectories(dir); } catch (Exception) { continue; }
            foreach (string s in subs) {
                if (string.Equals(System.IO.Path.GetFileName(s), Files.Backup, StringComparison.OrdinalIgnoreCase)) continue;
                // (OptiScaler's custom providers - beside its ini, or in its own folder: DittoSR there is not a game's FSR)
                if (string.Equals(System.IO.Path.GetFileName(s), "custom", StringComparison.OrdinalIgnoreCase) &&
                    (opti || Directory.Exists(System.IO.Path.Combine(s, "DittoSR")))) continue;
                try {
                    if ((File.GetAttributes(s) & FileAttributes.ReparsePoint) != 0) continue;  // (links: not followed)
                } catch (Exception) { continue; }
                stack.Push(s);
            }
        }
        found.Sort((a, b) => {
            int c = string.Compare(a.Folder, b.Folder, StringComparison.OrdinalIgnoreCase);
            return c != 0 ? c : string.Compare(a.Name, b.Name, StringComparison.OrdinalIgnoreCase);
        });
        return found;
    }

    static void Look(string dir, List<Candidate> found) {
        // (XeSS: the game's libxess.dll - not OptiScaler's own, its XeSS backend)
        if (File.Exists(System.IO.Path.Combine(dir, Files.Xess)) && !File.Exists(System.IO.Path.Combine(dir, "OptiScaler.ini")))
            found.Add(DescribeXess(dir));
        var present = Files.All.Where(n => File.Exists(System.IO.Path.Combine(dir, n))).ToList();
        if (present.Count == 0) return;
        // (the upscaler alone where the game has no loader DLL: its executable carries it. A DittoSR installed alone as
        // amd_fidelityfx_dx12.dll - an earlier install there - doesn't count as one)
        bool dx12 = present.Contains(Files.Dx12) &&
                    (File.Exists(Files.Orig(dir, Files.Dx12)) || Files.DittoVersion(System.IO.Path.Combine(dir, Files.Dx12)) == null);
        bool upscalerTarget = !present.Contains(Files.Loader) && !dx12;
        foreach (string n in present) {
            if (n == Files.Upscaler && !upscalerTarget) continue;
            found.Add(Describe(dir, n));
        }
    }

    public static Candidate Describe(string dir, string name) {
        var c = new Candidate { Folder = dir, Name = name };
        string path = c.Path;
        string ditto = Files.DittoVersion(path);
        bool orig = File.Exists(Files.Orig(dir, name));
        if (ditto != null) {
            c.State = ditto + (orig ? " installed (AMD's kept)" : " installed alone");
            c.Installable = true;
            c.Recommended = true;
        } else if (Files.SignedBy(path, Files.Vendor(name))) {
            c.State = Files.VendorVersion(path) + " (the game's)";
            c.Installable = c.Recommended = true;
        } else {
            c.State = "not AMD's - verify the game's files first";
        }
        return c;
    }

    static Candidate DescribeXess(string dir) {
        var c = new Candidate { Folder = dir, Name = Files.Xess };
        string path = c.Path, ditto = Files.DittoVersion(path);
        bool orig = File.Exists(Files.Orig(dir, Files.Xess));
        if (ditto != null) {
            c.State = orig ? ditto + " installed (XeSS kept)" : ditto + ", but " + Files.XessOrig + " is missing - verify the game's files";
            c.Installable = c.Recommended = orig;
        } else if (Files.SignedBy(path, Files.Vendor(Files.Xess))) {
            c.State = Files.VendorVersion(path) + " (the game's)";
            c.Installable = c.Recommended = true;
            if (XessProxyExports != null) {
                int missing;
                try { missing = Files.Exports(path).Count(n => !XessProxyExports.Contains(n)); } catch (Exception) { missing = -1; }
                if (missing != 0) {
                    c.State = Files.VendorVersion(path) + " - newer than DittoSR's proxy knows (" + (missing > 0 ? missing + " new exports" : "unreadable") + ")";
                    c.Installable = c.Recommended = false;
                }
            }
        } else {
            c.State = "not Intel's - verify the game's files first";
        }
        return c;
    }
}

// Install and restore: install.ps1 and uninstall.ps1.
static class Installer {
    public static void Install(Candidate c, string dll, string ini, Action<string> log) {
        string target = c.Path, orig = Files.Orig(c.Folder, c.Name), backup = Path.Combine(c.Folder, Files.Backup);
        if (dll == null) throw new InvalidOperationException("DittoSR's " + (c.Xess ? Files.Xess : Files.Dx12) + " was not found next to the loader");
        if (c.Custom) Directory.CreateDirectory(c.Folder);
        if (File.Exists(target) && !File.Exists(orig) && Files.DittoVersion(target) == null) {
            // the first install: the vendor's file, before anything replaces it
            string vendor = Files.Vendor(c.Name);
            if (!Files.SignedBy(target, vendor)) throw new InvalidOperationException(target + " is not signed by " + vendor + ": restore the game's file first");
            Directory.CreateDirectory(backup);
            File.Copy(target, Path.Combine(backup, c.Name), true);
            File.WriteAllText(Path.Combine(backup, c.Name + ".sha256"), Files.Sha256(target) + "\r\n", Encoding.ASCII);
            File.Copy(target, orig, true);
            log("backed up " + c.Name + " to " + backup + ", the game's copy as " + Path.GetFileName(orig));
        }
        File.Copy(dll, target, true);
        log("installed DittoSR as " + target);
        string iniTarget = Path.Combine(c.Folder, Files.Ini);
        if (!File.Exists(iniTarget) && ini != null) {
            File.Copy(ini, iniTarget);
            log("settings: " + iniTarget);
        } else if (File.Exists(iniTarget)) {
            Migrate(iniTarget, log);
        }
        // (the version the settings are now for: the next update migrates from it)
        string v = Files.DittoVersion(dll);
        if (v != null && File.Exists(iniTarget)) IniKeys.SetText(iniTarget, "version", v.Substring("DittoSR ".Length));
    }

    // An ini kept from an earlier install: the defaults that changed since its version moved to the new ones; a value
    // the user set stays. Each step runs for an ini older than the version that changed that default (no version key:
    // before 0.3.0). Before 0.2.2: sharpness 0.2, a soft blur at rest, to 0.6, the image as reconstructed.
    static void Migrate(string ini, Action<string> log) {
        Version from = new Version(0, 0, 0);
        try { string t = IniKeys.GetText(ini, "version"); if (t != null) from = new Version(t); } catch (Exception) { }
        if (from < new Version(0, 2, 2)) {
            float? sh = IniKeys.Get(ini, "sharpness");
            if (sh.HasValue && Math.Abs(sh.Value - 0.2f) < 0.001f) {
                IniKeys.Set(ini, "sharpness", 0.6f);
                log("settings: sharpness 0.2 (the old default, a soft blur at rest) set to 0.6 (the new default) in " + ini);
            }
        }
    }

    public static void Restore(string folder, Action<string> log) {
        string backup = Path.Combine(folder, Files.Backup);
        foreach (string n in Files.Restorable) {
            string target = Path.Combine(folder, n), saved = Path.Combine(backup, n), orig = Files.Orig(folder, n);
            if (File.Exists(saved)) {
                string want = File.Exists(saved + ".sha256") ? File.ReadAllText(saved + ".sha256").Trim() : "";
                if (!string.Equals(Files.Sha256(saved), want, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidOperationException("the backup of " + n + " in " + folder + " does not match its hash: verify the game's files in its launcher instead");
                File.Copy(saved, target, true);
                if (File.Exists(orig)) File.Delete(orig);
                log("restored the game's " + target);
            } else if (File.Exists(target) && Files.DittoVersion(target) != null) {
                if (File.Exists(orig)) {
                    // (no backup - an install from before the loader kept none: the game's copy)
                    File.Copy(orig, target, true);
                    File.Delete(orig);
                    log("restored the game's " + target + " from " + Path.GetFileName(orig));
                } else {
                    // (installed alone - no game file was there)
                    File.Delete(target);
                    log("removed DittoSR's " + target);
                }
            }
        }
        foreach (string f in new[] { Files.Ini, Files.Log, Files.XessLog }) {
            string p = Path.Combine(folder, f);
            if (File.Exists(p)) File.Delete(p);
        }
        // (an OptiScaler custom provider's folders, custom\DittoSR\ffx: removed once empty)
        string kind = Path.GetFileName(folder), name = Path.GetFileName(Path.GetDirectoryName(folder));
        if ((kind == "ffx" || kind == "ffx_vk") && name == "DittoSR") {
            foreach (string d in new[] { folder, Path.GetDirectoryName(folder) })
                if (Directory.Exists(d) && !Directory.EnumerateFileSystemEntries(d).Any()) Directory.Delete(d);
            log("done");
            return;
        }
        log("done (the backups, if any, stay in " + backup + ")");
    }
}

// Steam's installed games: its libraries (libraryfolders.vdf) and their app manifests.
static class Steam {
    public class Game {
        public string Name, Folder;
        public override string ToString() { return Name; }
    }

    // (Steam's tools and a game's extras: no FidelityFX DLL there - a soundtrack would also stand in for its game's folder)
    static readonly Regex NotAGame = new Regex(@"Soundtrack|Redistributable|^Proton|Steam Linux Runtime|^SteamVR|Dedicated Server|Playtest$", RegexOptions.IgnoreCase);

    public static List<Game> Games() {
        var games = new List<Game>();
        try {
            string steam = Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam", "SteamPath", null) as string;
            if (steam == null) return games;
            var libs = new List<string> { steam.Replace('/', '\\') };
            string vdf = Path.Combine(libs[0], @"steamapps\libraryfolders.vdf");
            if (File.Exists(vdf))
                foreach (Match m in Regex.Matches(File.ReadAllText(vdf), "\"path\"\\s+\"([^\"]+)\""))
                    libs.Add(m.Groups[1].Value.Replace(@"\\", @"\"));
            foreach (string lib in libs.Distinct(StringComparer.OrdinalIgnoreCase)) {
                string apps = Path.Combine(lib, "steamapps");
                if (!Directory.Exists(apps)) continue;
                foreach (string acf in Directory.GetFiles(apps, "appmanifest_*.acf")) {
                    string text = File.ReadAllText(acf);
                    Match name = Regex.Match(text, "\"name\"\\s+\"([^\"]+)\""), dir = Regex.Match(text, "\"installdir\"\\s+\"([^\"]+)\"");
                    if (!name.Success || !dir.Success || NotAGame.IsMatch(name.Groups[1].Value)) continue;
                    string folder = Path.Combine(apps, "common", dir.Groups[1].Value);
                    if (Directory.Exists(folder) && !games.Any(g => string.Equals(g.Folder, folder, StringComparison.OrdinalIgnoreCase)))
                        games.Add(new Game { Name = name.Groups[1].Value, Folder = folder });
                }
            }
        } catch (Exception) {
            // (no Steam, or a library it can't read: browse instead)
        }
        games.Sort((a, b) => string.Compare(a.Name, b.Name, StringComparison.OrdinalIgnoreCase));
        return games;
    }
}

// The look: dark, the actions as rounded blue buttons; a dark title bar and scroll bars where Windows has them.
static class Theme {
    public static readonly Color Back = Color.FromArgb(24, 25, 28), Surface = Color.FromArgb(32, 34, 38),
                                 Header = Color.FromArgb(40, 42, 47), Border = Color.FromArgb(56, 59, 66),
                                 Text = Color.FromArgb(232, 234, 237), Muted = Color.FromArgb(148, 154, 164),
                                 Faint = Color.FromArgb(100, 105, 114), Selection = Color.FromArgb(36, 54, 88),
                                 Accent = Color.FromArgb(37, 99, 235), AccentHover = Color.FromArgb(59, 122, 246),
                                 AccentDown = Color.FromArgb(29, 78, 216), AccentOff = Color.FromArgb(42, 52, 74);

    [DllImport("dwmapi.dll")]
    static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int size);
    [DllImport("uxtheme.dll", CharSet = CharSet.Unicode)]
    static extern int SetWindowTheme(IntPtr hwnd, string app, string idList);

    public static void DarkTitleBar(IntPtr hwnd) {
        int on = 1;
        // (DWMWA_USE_IMMERSIVE_DARK_MODE: 20, 19 before Windows 10 20H1)
        try { if (DwmSetWindowAttribute(hwnd, 20, ref on, 4) != 0) DwmSetWindowAttribute(hwnd, 19, ref on, 4); } catch (Exception) { }
    }

    public static void Dark(Control c, string theme) {
        c.HandleCreated += (s, e) => { try { SetWindowTheme(c.Handle, theme, null); } catch (Exception) { } };
    }

    public static GraphicsPath Round(RectangleF r, float radius) {
        float d = Math.Min(radius * 2, Math.Min(r.Width, r.Height));
        var p = new GraphicsPath();
        p.AddArc(r.X, r.Y, d, d, 180, 90);
        p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
        p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
        p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
        p.CloseFigure();
        return p;
    }

    // a control in a rounded, bordered frame (a text box, the list, the log)
    public static Panel Frame(Control inner, Color back, int pad, float k) {
        var p = new Panel { BackColor = Back, Padding = new Padding(pad), Dock = DockStyle.Fill, Margin = new Padding(0) };
        inner.Dock = DockStyle.Fill;
        inner.BackColor = back;
        p.Controls.Add(inner);
        p.Paint += (s, e) => {
            e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
            using (var path = Round(new RectangleF(0.5f, 0.5f, p.Width - 1.5f, p.Height - 1.5f), 6 * k))
            using (var b = new SolidBrush(back))
            using (var pen = new Pen(Border)) {
                e.Graphics.FillPath(b, path);
                e.Graphics.DrawPath(pen, path);
            }
        };
        p.Resize += (s, e) => p.Invalidate();
        return p;
    }
}

class RoundButton : Button {
    bool hover, down;
    // a toggle (FSR / XeSS): blue when selected, else dark with a border
    public bool Toggle;
    bool selected;
    public bool Selected { get { return selected; } set { selected = value; Invalidate(); } }

    public RoundButton(string text, float k) {
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
        Text = text;
        FlatStyle = FlatStyle.Flat;
        FlatAppearance.BorderSize = 0;
        ForeColor = Color.White;
        Cursor = Cursors.Hand;
        AutoSize = true;
        Padding = new Padding((int)(12 * k), (int)(5 * k), (int)(12 * k), (int)(5 * k));
        MinimumSize = new Size((int)(84 * k), (int)(34 * k));
        Margin = new Padding((int)(3 * k), (int)(3 * k), (int)(5 * k), (int)(3 * k));
    }

    protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { hover = down = false; Invalidate(); base.OnMouseLeave(e); }
    protected override void OnMouseDown(MouseEventArgs e) { down = true; Invalidate(); base.OnMouseDown(e); }
    protected override void OnMouseUp(MouseEventArgs e) { down = false; Invalidate(); base.OnMouseUp(e); }
    protected override void OnEnabledChanged(EventArgs e) { Invalidate(); base.OnEnabledChanged(e); }

    protected override void OnPaint(PaintEventArgs e) {
        var g = e.Graphics;
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.Clear(Parent != null ? Parent.BackColor : Theme.Back);
        bool off = Toggle && !selected;
        Color c = !Enabled ? Theme.AccentOff : off ? (hover ? Theme.Border : Theme.Header)
                : down ? Theme.AccentDown : hover ? Theme.AccentHover : Theme.Accent;
        using (var path = Theme.Round(new RectangleF(0.5f, 0.5f, Width - 1.5f, Height - 1.5f), Height * 0.3f))
        using (var b = new SolidBrush(c)) {
            g.FillPath(b, path);
            if (off)
                using (var pen = new Pen(Theme.Border)) g.DrawPath(pen, path);
            if (Focused && ShowFocusCues)
                using (var pen = new Pen(Color.FromArgb(150, 255, 255, 255))) g.DrawPath(pen, path);
        }
        TextRenderer.DrawText(g, Text, Font, ClientRectangle, !Enabled ? Theme.Faint : off ? Theme.Muted : Color.White,
                              TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine);
    }
}

// A slider of a few fixed steps: a track, a dot per step, a round thumb; a click or a drag snaps to the nearest step,
// the arrow keys move it one.
class StepSlider : Control {
    readonly int steps;
    readonly float k;
    int index;
    public event Action Changed;
    public int Index { get { return index; } set { value = Math.Max(0, Math.Min(steps - 1, value)); if (value != index) { index = value; Invalidate(); if (Changed != null) Changed(); } } }

    public StepSlider(int steps, int index, float k) {
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer |
                 ControlStyles.ResizeRedraw | ControlStyles.Selectable, true);
        this.steps = steps;
        this.index = index;
        this.k = k;
        Height = (int)(30 * k);
        TabStop = true;
        Cursor = Cursors.Hand;
    }

    float Pad { get { return 11 * k; } }
    float X(int i) { return Pad + (Width - 2 * Pad) * i / (steps - 1); }

    protected override void OnPaint(PaintEventArgs e) {
        var g = e.Graphics;
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.Clear(Parent != null ? Parent.BackColor : Theme.Back);
        float y = Height / 2f, x = X(index);
        using (var pen = new Pen(Theme.Border, 4 * k) { StartCap = LineCap.Round, EndCap = LineCap.Round }) g.DrawLine(pen, Pad, y, Width - Pad, y);
        if (index > 0)
            using (var pen = new Pen(Theme.Accent, 4 * k) { StartCap = LineCap.Round, EndCap = LineCap.Round }) g.DrawLine(pen, Pad, y, x, y);
        for (int i = 0; i < steps; i++) {
            float r = 3.5f * k;
            using (var b = new SolidBrush(i <= index ? Theme.AccentHover : Theme.Faint)) g.FillEllipse(b, X(i) - r, y - r, 2 * r, 2 * r);
        }
        float t = 9 * k;
        using (var b = new SolidBrush(Color.White)) g.FillEllipse(b, x - t, y - t, 2 * t, 2 * t);
        using (var pen = new Pen(Theme.Accent, 3 * k)) g.DrawEllipse(pen, x - t + 1.5f * k, y - t + 1.5f * k, 2 * t - 3 * k, 2 * t - 3 * k);
        if (Focused && ShowFocusCues)
            using (var pen = new Pen(Color.FromArgb(120, 255, 255, 255))) g.DrawEllipse(pen, x - t - 3 * k, y - t - 3 * k, 2 * t + 6 * k, 2 * t + 6 * k);
    }

    void Snap(int px) { Index = (int)Math.Round((px - Pad) / (Width - 2 * Pad) * (steps - 1)); }
    protected override void OnMouseDown(MouseEventArgs e) { Focus(); if (e.Button == MouseButtons.Left) Snap(e.X); base.OnMouseDown(e); }
    protected override void OnMouseMove(MouseEventArgs e) { if (e.Button == MouseButtons.Left) Snap(e.X); base.OnMouseMove(e); }
    protected override bool IsInputKey(Keys key) { return key == Keys.Left || key == Keys.Right || base.IsInputKey(key); }
    protected override void OnKeyDown(KeyEventArgs e) {
        if (e.KeyCode == Keys.Left) Index--;
        else if (e.KeyCode == Keys.Right) Index++;
        base.OnKeyDown(e);
    }
    protected override void OnGotFocus(EventArgs e) { Invalidate(); base.OnGotFocus(e); }
    protected override void OnLostFocus(EventArgs e) { Invalidate(); base.OnLostFocus(e); }
}

// dittosr.ini's [dittosr] keys: read, and written in place (the line replaced, or added under the section), every other
// line kept as it was.
static class IniKeys {
    const string Section = "dittosr";

    public static float? Get(string ini, string key) {
        bool inside = false;
        foreach (string raw in File.ReadAllLines(ini)) {
            string line = raw.Trim();
            if (line.StartsWith("[")) { if (inside) break; inside = string.Equals(line, "[" + Section + "]", StringComparison.OrdinalIgnoreCase); continue; }
            if (!inside) continue;
            int eq = line.IndexOf('=');
            float v;
            if (eq > 0 && string.Equals(line.Substring(0, eq).Trim(), key, StringComparison.OrdinalIgnoreCase) &&
                float.TryParse(line.Substring(eq + 1).Trim(), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out v))
                return v;
        }
        return null;
    }

    public static string GetText(string ini, string key) {
        bool inside = false;
        foreach (string raw in File.ReadAllLines(ini)) {
            string line = raw.Trim();
            if (line.StartsWith("[")) { if (inside) break; inside = string.Equals(line, "[" + Section + "]", StringComparison.OrdinalIgnoreCase); continue; }
            int eq = line.IndexOf('=');
            if (inside && eq > 0 && string.Equals(line.Substring(0, eq).Trim(), key, StringComparison.OrdinalIgnoreCase)) return line.Substring(eq + 1).Trim();
        }
        return null;
    }

    public static void Set(string ini, string key, float value) {
        SetText(ini, key, value.ToString("0.0##", System.Globalization.CultureInfo.InvariantCulture));
    }

    public static void SetText(string ini, string key, string value) {
        var lines = File.ReadAllLines(ini).ToList();
        string text = key + "=" + value;
        int section = -1, last = -1;
        for (int i = 0; i < lines.Count; i++) {
            string line = lines[i].Trim();
            if (line.StartsWith("[")) {
                if (section >= 0) break;
                if (string.Equals(line, "[" + Section + "]", StringComparison.OrdinalIgnoreCase)) section = last = i;
                continue;
            }
            if (section < 0) continue;
            if (line.Length > 0 && !line.StartsWith(";")) last = i;
            int eq = line.IndexOf('=');
            if (eq > 0 && string.Equals(line.Substring(0, eq).Trim(), key, StringComparison.OrdinalIgnoreCase)) {
                lines[i] = text;
                File.WriteAllLines(ini, lines);
                return;
            }
        }
        if (section < 0) { lines.Insert(0, ""); lines.Insert(0, text); lines.Insert(0, "[" + Section + "]"); }
        else lines.Insert(last + 1, text);
        File.WriteAllLines(ini, lines);
    }
}

// Settings: dittosr.ini's sharpness and frame_weight as two sliders of five steps (the middle one the default); a value
// set by hand between the steps is kept unless its slider is moved.
class SettingsForm : Form {
    public static readonly float[] Sharpness = { 0.2f, 0.4f, 0.6f, 0.8f, 1.0f }, FrameWeight = { 1.0f, 1.5f, 2.0f, 2.5f, 3.0f };
    readonly float k;
    readonly string ini;
    readonly StepSlider sharp, weight;
    bool sharpMoved, weightMoved;

    public SettingsForm(string ini, float k) {
        this.ini = ini;
        this.k = k;
        Text = "DittoSR settings";
        Font = new Font("Segoe UI", 9.75F);
        BackColor = Theme.Back;
        ForeColor = Theme.Text;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = MinimizeBox = ShowInTaskbar = false;
        StartPosition = FormStartPosition.CenterParent;
        try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch (Exception) { }
        int pad = (int)(20 * k), w = (int)(440 * k), y = (int)(16 * k);

        sharp = Setting(ref y, pad, w, "Sharpness", "How crisp the image looks: Soft smooths fine detail, Sharp adds contrast to it - more at high " +
                        "upscaling ratios and on small screens.", "Soft", "Sharp", Sharpness, IniKeys.Get(ini, "sharpness"), () => sharpMoved = true);
        y += (int)(10 * k);
        weight = Setting(ref y, pad, w, "Frame weight", "How much each new frame counts: Stable is steadier and smoother, with a little more " +
                         "trailing in motion; Responsive is crisper in motion, with more shimmer.", "Stable", "Responsive", FrameWeight,
                         IniKeys.Get(ini, "frame_weight"), () => weightMoved = true);
        y += (int)(10 * k);

        y += (int)(6 * k);
        Controls.Add(new Label { Text = "In the game, F9 applies the change live.", ForeColor = Theme.Faint, AutoSize = true, Location = new Point(pad, y) });
        y += (int)(30 * k);
        var save = new RoundButton("Save", k);
        var cancel = new RoundButton("Cancel", k) { Toggle = true };
        var raw = new RoundButton("Open dittosr.ini", k) { Toggle = true };
        foreach (var b in new[] { save, cancel, raw }) { b.AutoSize = false; b.Size = new Size(b.PreferredSize.Width, (int)(34 * k)); }
        save.Location = new Point(pad + w - save.Width, y);
        cancel.Location = new Point(save.Left - cancel.Width - (int)(8 * k), y);
        raw.Location = new Point(pad, y);
        Controls.AddRange(new Control[] { save, cancel, raw });
        AcceptButton = save;
        CancelButton = cancel;
        save.Click += (s, e) => { Save(); DialogResult = DialogResult.OK; };
        cancel.Click += (s, e) => DialogResult = DialogResult.Cancel;
        raw.Click += (s, e) => { Process.Start("notepad.exe", "\"" + ini + "\""); DialogResult = DialogResult.Cancel; };
        ClientSize = new Size(w + 2 * pad, y + save.Height + pad);
    }

    protected override void OnHandleCreated(EventArgs e) { base.OnHandleCreated(e); Theme.DarkTitleBar(Handle); }

    StepSlider Setting(ref int y, int pad, int w, string title, string about, string left, string right, float[] steps, float? now, Action moved) {
        int at = 2;  // (the default: the middle step)
        bool custom = false;
        if (now.HasValue) {
            at = Enumerable.Range(0, steps.Length).OrderBy(i => Math.Abs(steps[i] - now.Value)).First();
            custom = Math.Abs(steps[at] - now.Value) > 0.001f;
        }
        var head = new Label { Text = title, Font = new Font("Segoe UI Semibold", 10.5F), ForeColor = Theme.Text, AutoSize = true, Location = new Point(pad, y) };
        var value = new Label { ForeColor = Theme.Muted, AutoSize = false, TextAlign = ContentAlignment.MiddleRight, Size = new Size((int)(220 * k), head.PreferredHeight),
                                Location = new Point(pad + w - (int)(220 * k), y) };
        y += head.PreferredHeight + (int)(4 * k);
        var text = new Label { Text = about, ForeColor = Theme.Muted, AutoSize = false, Location = new Point(pad, y) };
        text.Size = new Size(w, TextRenderer.MeasureText(about, Font, new Size(w, 0), TextFormatFlags.WordBreak).Height);
        y += text.Height + (int)(6 * k);
        var slider = new StepSlider(steps.Length, at, k) { Location = new Point(pad, y), Width = w };
        y += slider.Height;
        var l = new Label { Text = left, ForeColor = Theme.Muted, AutoSize = true, Location = new Point(pad, y) };
        var r = new Label { Text = right, ForeColor = Theme.Muted, AutoSize = false, TextAlign = ContentAlignment.TopRight, Size = new Size((int)(160 * k), l.PreferredHeight),
                            Location = new Point(pad + w - (int)(160 * k), y) };
        y += l.PreferredHeight + (int)(8 * k);
        Action show = () => {
            value.Text = custom ? now.Value.ToString("0.0##", System.Globalization.CultureInfo.InvariantCulture) + " (set by hand)"
                                : steps[slider.Index].ToString("0.0", System.Globalization.CultureInfo.InvariantCulture) + (slider.Index == 2 ? " - default" : "");
        };
        slider.Changed += () => { custom = false; moved(); show(); };
        show();
        Controls.AddRange(new Control[] { head, value, text, slider, l, r });
        return slider;
    }

    void Save() {
        if (sharpMoved) IniKeys.Set(ini, "sharpness", Sharpness[sharp.Index]);
        if (weightMoved) IniKeys.Set(ini, "frame_weight", FrameWeight[weight.Index]);
    }
}

class LoaderForm : Form {
    readonly string dll, xessDll, ini, version;
    readonly float k;
    // where DittoSR goes: in place of the game's FSR (0) or XeSS (1), or as an OptiScaler v10 custom provider (2)
    int mode;
    bool xessMode { get { return mode == 1; } }
    bool optiMode { get { return mode == 2; } }
    readonly RoundButton optiButton, fsrButton, xessButton;
    List<OptiScaler> lastOptiInfo = new List<OptiScaler>();
    string lastAutoOpti;  // (the folder the OptiScaler mode was last picked for by itself)
    readonly Label modeHint = new Label { AutoSize = true, ForeColor = Theme.Muted, Anchor = AnchorStyles.Left };
    // the last scan: shown by mode
    string lastRoot;
    List<Candidate> lastFound = new List<Candidate>();
    List<string> lastOpti = new List<string>();
    readonly ComboBox games = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, DrawMode = DrawMode.OwnerDrawFixed, Dock = DockStyle.Fill };
    readonly TextBox folder = new TextBox { BorderStyle = BorderStyle.None };
    readonly ListView list = new ListView { View = View.Details, CheckBoxes = true, FullRowSelect = true, HideSelection = false, OwnerDraw = true, BorderStyle = BorderStyle.None };
    readonly TextBox output = new TextBox { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, BorderStyle = BorderStyle.None };
    readonly Button browse, scan, install, restore, settings, logButton, open;
    Thread scanning;
    volatile bool stopScan;

    public LoaderForm(string startFolder) {
        dll = Find(@"build\" + Files.Dx12, Files.Dx12, @"..\build\" + Files.Dx12);
        xessDll = Find(@"build\" + Files.Xess, Files.Xess, @"..\build\" + Files.Xess);
        if (xessDll != null && Files.DittoVersion(xessDll) == null) xessDll = null;  // (Intel's, next to a build of the loader)
        try { Scanner.XessProxyExports = xessDll != null ? Files.Exports(xessDll) : null; } catch (Exception) { }
        ini = Find(Files.Ini, @"..\" + Files.Ini);
        version = dll != null ? Files.DittoVersion(dll) : null;
        Text = "DittoSR Loader" + (version != null ? " - " + version : "");
        // (DPI aware by its manifest: the sizes below are at 96 DPI, scaled to the screen's)
        using (var g = CreateGraphics()) k = g.DpiX / 96F;
        Font = new Font("Segoe UI", 9.75F);
        BackColor = Theme.Back;
        ForeColor = Theme.Text;
        ClientSize = new Size((int)(880 * k), (int)(600 * k));
        MinimumSize = new Size((int)(620 * k), (int)(440 * k));
        StartPosition = FormStartPosition.CenterScreen;
        Padding = new Padding((int)(14 * k), (int)(10 * k), (int)(14 * k), (int)(14 * k));
        try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch (Exception) { }
        browse = new RoundButton("Browse...", k);
        scan = new RoundButton("Scan", k);
        install = new RoundButton("Install DittoSR", k);
        restore = new RoundButton("Restore game's files", k);
        optiButton = new RoundButton("OptiScaler", k) { Toggle = true, Visible = false };
        fsrButton = new RoundButton("FSR", k) { Toggle = true, Selected = true };
        xessButton = new RoundButton("XeSS", k) { Toggle = true };
        settings = new RoundButton("Settings", k);
        logButton = new RoundButton("Log", k);
        open = new RoundButton("Open folder", k);

        // the header: the name, what it does
        var head = new Panel { Dock = DockStyle.Top, Height = (int)(58 * k) };
        head.Controls.Add(new Label { Text = "Installs DittoSR in place of a game's FSR or XeSS" + (version != null ? " - " + version : ""),
                                      ForeColor = Theme.Muted, AutoSize = true, Location = new Point(0, (int)(34 * k)) });
        head.Controls.Add(new Label { Text = "DittoSR Loader", Font = new Font("Segoe UI Semibold", 15F), ForeColor = Theme.Text,
                                      AutoSize = true, Location = new Point(-(int)(2 * k), 0) });

        // the game: Steam's list, or a folder
        int field = (int)(34 * k);
        var top = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, ColumnCount = 4, Padding = new Padding(0, 0, 0, (int)(8 * k)) };
        top.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        top.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        top.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        top.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        games.ItemHeight = (int)(24 * k);
        games.BackColor = Theme.Surface;
        games.ForeColor = Theme.Text;
        games.DrawItem += DrawGame;
        Theme.Dark(games, "DarkMode_CFD");
        folder.ForeColor = Theme.Text;
        var folderBox = Theme.Frame(folder, Theme.Surface, (int)(8 * k), k);
        folderBox.Height = field;
        folderBox.Margin = games.Margin = new Padding(0, (int)(4 * k), (int)(5 * k), (int)(4 * k));
        top.Controls.Add(Caption("Steam game"), 0, 0);
        top.Controls.Add(games, 1, 0);
        top.SetColumnSpan(games, 3);
        top.Controls.Add(Caption("Game folder"), 0, 1);
        top.Controls.Add(folderBox, 1, 1);
        top.Controls.Add(browse, 2, 1);
        top.Controls.Add(scan, 3, 1);
        // the upscaler to replace
        var modes = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(0, (int)(2 * k), 0, 0) };
        optiButton.MinimumSize = fsrButton.MinimumSize = xessButton.MinimumSize = new Size((int)(70 * k), (int)(30 * k));
        modeHint.Margin = new Padding((int)(8 * k), 0, 0, 0);
        modes.Controls.AddRange(new Control[] { optiButton, fsrButton, xessButton, modeHint });
        top.Controls.Add(Caption("Replace"), 0, 2);
        top.Controls.Add(modes, 1, 2);
        top.SetColumnSpan(modes, 3);

        // the DLLs found
        list.Columns.Add("File");
        list.Columns.Add("Folder");
        list.Columns.Add("Now");
        list.ShowItemToolTips = true;
        list.ForeColor = Theme.Text;
        list.SmallImageList = new ImageList { ImageSize = new Size(1, (int)(28 * k)) };  // (the rows' height)
        list.ClientSizeChanged += (s, e) => {  // (the columns share the width: 30 / 33 / 37 %)
            int w = list.ClientSize.Width;
            list.Columns[0].Width = w * 30 / 100;
            list.Columns[1].Width = w * 33 / 100;
            list.Columns[2].Width = w - list.Columns[0].Width - list.Columns[1].Width;
        };
        list.DrawColumnHeader += DrawHeader;
        list.DrawItem += (s, e) => { };
        list.DrawSubItem += DrawCell;
        list.MouseClick += (s, e) => {  // (a click anywhere on a row's first cell checks it, not only on the box)
            var hit = list.HitTest(e.Location);
            if (hit.Item != null && hit.SubItem == hit.Item.SubItems[0] && e.X > (int)(26 * k)) hit.Item.Checked = !hit.Item.Checked;
        };

        var buttons = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, Padding = new Padding(0, (int)(8 * k), 0, (int)(4 * k)), WrapContents = true };
        buttons.Controls.AddRange(new Control[] { install, restore, settings, logButton, open });

        output.ForeColor = Theme.Muted;
        output.Font = new Font("Segoe UI", 9F);
        Theme.Dark(output, "DarkMode_Explorer");

        var split = new SplitContainer { Dock = DockStyle.Fill, Orientation = Orientation.Horizontal, Panel1MinSize = (int)(140 * k),
                                         Panel2MinSize = (int)(80 * k), SplitterWidth = (int)(10 * k), BackColor = Theme.Back };
        var upper = new TableLayoutPanel { Dock = DockStyle.Fill, RowCount = 2, ColumnCount = 1, Margin = new Padding(0) };
        upper.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        upper.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        upper.Controls.Add(Theme.Frame(list, Theme.Surface, (int)(1 * k) + 1, k), 0, 0);
        upper.Controls.Add(buttons, 0, 1);
        split.Panel1.Controls.Add(upper);
        split.Panel2.Controls.Add(Theme.Frame(output, Color.FromArgb(20, 21, 24), (int)(8 * k), k));
        Controls.Add(split);
        Controls.Add(top);
        Controls.Add(head);
        Load += (s, e) => { split.SplitterDistance = (int)(split.Height * 0.64); };
        Theme.DarkTitleBar(Handle);  // (the handle exists already: CreateGraphics above)

        games.Items.Add("(pick a game, or browse to its folder)");
        foreach (var g in Steam.Games()) games.Items.Add(g);
        games.SelectedIndex = 0;
        games.SelectedIndexChanged += (s, e) => {
            var g = games.SelectedItem as Steam.Game;
            if (g != null) { folder.Text = g.Folder; StartScan(); }
        };
        browse.Click += (s, e) => Browse();
        scan.Click += (s, e) => StartScan();
        folder.KeyDown += (s, e) => { if (e.KeyCode == Keys.Enter) { e.SuppressKeyPress = true; StartScan(); } };
        install.Click += (s, e) => Install();
        restore.Click += (s, e) => Restore();
        settings.Click += (s, e) => ShowSettings();
        logButton.Click += (s, e) => OpenFile(xessMode ? Files.XessLog : Files.Log, "no log yet: it appears once the game loads DittoSR");
        optiButton.Click += (s, e) => SetMode(2);
        fsrButton.Click += (s, e) => SetMode(0);
        xessButton.Click += (s, e) => SetMode(1);
        open.Click += (s, e) => { string f = SelectedFolder(); if (f != null) Process.Start("explorer.exe", "\"" + f + "\""); };
        list.ItemChecked += (s, e) => UpdateButtons();
        list.SelectedIndexChanged += (s, e) => UpdateButtons();
        FormClosing += (s, e) => { stopScan = true; };

        if (dll == null) Log("DittoSR's DLL was not found next to the loader (build\\" + Files.Dx12 + "): extract the whole DittoSR.zip");
        else Log("DittoSR: " + dll + (version != null ? " (" + version + ")" : ""));
        Log("Pick the game's folder (or a Steam game) - the FidelityFX and XeSS DLLs are looked for in every folder below it.");
        SetMode(0);
        if (!string.IsNullOrEmpty(startFolder)) { folder.Text = startFolder; Shown += (s, e) => StartScan(); }
    }

    Label Caption(string text) {
        return new Label { Text = text, ForeColor = Theme.Muted, AutoSize = true, Anchor = AnchorStyles.Left, Margin = new Padding(0, 0, (int)(10 * k), 0) };
    }

    void DrawGame(object sender, DrawItemEventArgs e) {
        if (e.Index < 0) return;
        bool sel = (e.State & DrawItemState.Selected) != 0 && (e.State & DrawItemState.ComboBoxEdit) == 0;
        using (var b = new SolidBrush(sel ? Theme.Selection : Theme.Surface)) e.Graphics.FillRectangle(b, e.Bounds);
        var r = new Rectangle(e.Bounds.X + (int)(6 * k), e.Bounds.Y, e.Bounds.Width - (int)(6 * k), e.Bounds.Height);
        TextRenderer.DrawText(e.Graphics, games.Items[e.Index].ToString(), games.Font, r, e.Index == 0 ? Theme.Muted : Theme.Text,
                              TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPrefix);
    }

    void DrawHeader(object sender, DrawListViewColumnHeaderEventArgs e) {
        using (var b = new SolidBrush(Theme.Header)) e.Graphics.FillRectangle(b, e.Bounds);
        var r = new Rectangle(e.Bounds.X + (int)(10 * k), e.Bounds.Y, e.Bounds.Width - (int)(10 * k), e.Bounds.Height);
        if (e.ColumnIndex == 0) r.X += (int)(22 * k);
        TextRenderer.DrawText(e.Graphics, e.Header.Text, list.Font, r, Theme.Muted, TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.EndEllipsis);
        using (var pen = new Pen(Theme.Border)) e.Graphics.DrawLine(pen, e.Bounds.Left, e.Bounds.Bottom - 1, e.Bounds.Right, e.Bounds.Bottom - 1);
    }

    void DrawCell(object sender, DrawListViewSubItemEventArgs e) {
        var c = (Candidate)e.Item.Tag;
        var g = e.Graphics;
        using (var b = new SolidBrush(e.Item.Selected ? Theme.Selection : Theme.Surface)) g.FillRectangle(b, e.Bounds);
        var r = new Rectangle(e.Bounds.X + (int)(10 * k), e.Bounds.Y, e.Bounds.Width - (int)(14 * k), e.Bounds.Height);
        if (e.ColumnIndex == 0) {
            // the check box, drawn where the list's own sits (it still takes the clicks)
            int sz = (int)(16 * k);
            var box = new RectangleF(e.Bounds.X + (int)(5 * k) + 0.5f, e.Bounds.Y + (e.Bounds.Height - sz) / 2 + 0.5f, sz - 1, sz - 1);
            g.SmoothingMode = SmoothingMode.AntiAlias;
            using (var path = Theme.Round(box, 4 * k)) {
                if (e.Item.Checked) {
                    using (var b = new SolidBrush(c.Installable ? Theme.Accent : Theme.AccentOff)) g.FillPath(b, path);
                    using (var pen = new Pen(Color.White, 2 * k)) {
                        pen.StartCap = pen.EndCap = LineCap.Round;
                        g.DrawLines(pen, new[] { new PointF(box.X + box.Width * 0.24f, box.Y + box.Height * 0.52f),
                                                 new PointF(box.X + box.Width * 0.43f, box.Y + box.Height * 0.70f),
                                                 new PointF(box.X + box.Width * 0.77f, box.Y + box.Height * 0.32f) });
                    }
                } else {
                    using (var pen = new Pen(Theme.Faint, 1.5f * k)) g.DrawPath(pen, path);
                }
            }
            g.SmoothingMode = SmoothingMode.None;
            r.X += (int)(22 * k);
            r.Width -= (int)(22 * k);
        }
        Color fore = !c.Installable ? Theme.Faint : e.ColumnIndex == 0 ? Theme.Text : Theme.Muted;
        if (e.ColumnIndex == 2 && c.Installable && c.State.StartsWith("DittoSR")) fore = Color.FromArgb(96, 165, 250);
        TextRenderer.DrawText(g, e.SubItem.Text, list.Font, r, fore,
                              TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPrefix);
    }

    static string Find(params string[] relative) {
        string dir = Path.GetDirectoryName(Application.ExecutablePath);
        foreach (string r in relative) {
            string p = Path.GetFullPath(Path.Combine(dir, r));
            if (File.Exists(p)) return p;
        }
        return null;
    }

    void Log(string line) {
        output.AppendText(line + Environment.NewLine);
    }

    void Browse() {
        using (var d = new FolderBrowserDialog { Description = "The game's folder (its install folder is enough: DittoSR's DLLs are looked for below it)", ShowNewFolderButton = false }) {
            if (Directory.Exists(folder.Text)) d.SelectedPath = folder.Text;
            if (d.ShowDialog(this) == DialogResult.OK) { folder.Text = d.SelectedPath; StartScan(); }
        }
    }

    void StartScan() {
        string root = folder.Text.Trim().Trim('"');
        if (!Directory.Exists(root)) { Log("not a folder: " + root); return; }
        if (scanning != null && scanning.IsAlive) { stopScan = true; scanning.Join(); }
        stopScan = false;
        list.Items.Clear();
        SetBusy(true);
        Log("scanning " + root + " ...");
        scanning = new Thread(() => {
            List<Candidate> found;
            var opti = new List<string>();
            try { found = Scanner.Scan(root, () => stopScan, opti); } catch (Exception ex) { found = new List<Candidate>(); BeginInvoke((Action)(() => Log("scan failed: " + ex.Message))); }
            var info = opti.Select(OptiScaler.At).ToList();
            if (stopScan) return;
            BeginInvoke((Action)(() => Show(root, found, opti, info)));
        }) { IsBackground = true };
        scanning.Start();
    }

    void Show(string root, List<Candidate> found, List<string> optiScaler, List<OptiScaler> info) {
        SetBusy(false);
        lastRoot = root;
        lastFound = found;
        lastOpti = optiScaler;
        lastOptiInfo = info;
        foreach (var o in info)
            Log("OptiScaler " + (o.Version ?? "(version unknown)") + " in " + o.Folder + (o.Custom ? ": DittoSR can go in as its custom provider, in " + Path.Combine(o.Main, "custom") : ""));
        // OptiScaler v10: its button, picked by itself once for this folder (DittoSR in its list, the game's files untouched)
        bool custom = info.Any(o => o.Custom);
        optiButton.Visible = custom;
        if (custom && lastAutoOpti != root) { lastAutoOpti = root; mode = 2; }
        else if (!custom && optiMode) mode = 0;
        SetMode(mode, false);
        int fsr = found.Count(c => !c.Xess), xess = found.Count(c => c.Xess);
        if (fsr > 0 && xess > 0)
            Log("This game has both FSR and XeSS. If DittoSR's pink face doesn't show with FSR (some games, as Cyberpunk 2077, " +
                "build their FSR in), install it on XeSS and pick XeSS in the game.");
        else if (xess > 0 && !xessMode && optiScaler.Count == 0)
            Log("No FidelityFX DLL, but XeSS: switch to XeSS above.");
        Render();
    }

    void SetMode(int m, bool render = true) {
        mode = m;
        optiButton.Selected = m == 2;
        fsrButton.Selected = m == 0;
        xessButton.Selected = m == 1;
        var o = lastOptiInfo.FirstOrDefault(x => x.Custom);
        modeHint.Text = m == 2 ? "a custom provider of OptiScaler " + (o != null && o.Version != null ? o.Version + " " : "") + "- then pick DittoSR in OptiScaler's upscalers"
                      : m == 1 ? "the game's libxess.dll (XeSS on DirectX 12) - then pick XeSS in the game"
                               : "the game's FidelityFX DLLs (FSR 3.1 or later) - then pick FSR in the game";
        if (!render) return;
        if (lastRoot != null) Render();
        else UpdateButtons();
    }

    // OptiScaler v10's custom provider folders: DirectX 12 and Vulkan, DittoSR's one DLL in both
    void RenderOpti() {
        string root = lastRoot;
        list.BeginUpdate();
        list.Items.Clear();
        foreach (var o in lastOptiInfo.Where(x => x.Custom))
            foreach (bool vk in new[] { false, true }) {
                var c = new Candidate { Folder = o.CustomFolder(vk), Name = vk ? Files.Vk : Files.Dx12, Installable = true, Recommended = true, Custom = true };
                string v = File.Exists(c.Path) ? Files.DittoVersion(c.Path) : null;
                c.State = (v != null ? v + " installed" : "not there") + " - OptiScaler's custom provider (" + (vk ? "Vulkan" : "DirectX 12") + ")";
                list.Items.Add(Row(root, c, true));
            }
        list.EndUpdate();
        UpdateButtons();
    }

    void Render() {
        if (optiMode) { RenderOpti(); return; }
        string root = lastRoot;
        var found = lastFound.Where(c => c.Xess == xessMode).ToList();
        var optiScaler = xessMode ? new List<string>() : lastOpti;
        list.BeginUpdate();
        list.Items.Clear();
        foreach (var c in found) list.Items.Add(Row(root, c, c.Recommended));
        // OptiScaler's folder without amd_fidelityfx_dx12.dll: DittoSR alone there, checked
        foreach (string f in optiScaler) {
            if (found.Any(c => string.Equals(c.Folder, f, StringComparison.OrdinalIgnoreCase) && c.Name == Files.Dx12)) continue;
            list.Items.Add(Row(root, new Candidate { Folder = f, Name = Files.Dx12, State = "not there - DittoSR alone, for OptiScaler", Installable = true }, true));
        }
        foreach (string f in optiScaler)
            Log("OptiScaler in " + f + ": DittoSR goes there as " + Files.Dx12 + ". Then pick FSR 3.X as OptiScaler's upscaler (its FSR version list shows DittoSR first).");
        if (xessMode) {
            if (found.Count == 0) Log("no libxess.dll below " + root + ": the game doesn't offer XeSS on DirectX 12, or keeps it elsewhere.");
            else if (xessDll == null) Log("DittoSR's " + Files.Xess + " was not found next to the loader: extract the whole DittoSR.zip");
            else Log("found " + found.Count + " XeSS DLL" + (found.Count > 1 ? "s" : ""));
        } else if (found.Count == 0 && optiScaler.Count == 0) {
            // (no FidelityFX DLL: DittoSR alone, for OptiScaler or a game that loads it optionally)
            var c = new Candidate { Folder = root, Name = Files.Dx12, State = "not there - DittoSR alone (OptiScaler)", Installable = true };
            list.Items.Add(Row(root, c, false));
            Log("no FidelityFX DLL below " + root + ": the game may not use FSR 3.1 or later, or keeps it elsewhere.");
            Log("  For OptiScaler: check the row and install DittoSR alone as " + Files.Dx12 + " in this folder.");
        } else if (found.Count > 0) {
            int folders = found.Select(c => c.Folder).Distinct(StringComparer.OrdinalIgnoreCase).Count();
            Log("found " + found.Count + " FidelityFX DLL" + (found.Count > 1 ? "s" : "") + (folders > 1 ? " in " + folders + " folders (installing in a folder the game doesn't load does no harm)" : ""));
        }
        list.EndUpdate();
        UpdateButtons();
    }

    static ListViewItem Row(string root, Candidate c, bool check) {
        string rel = c.Folder.Length > root.Length && c.Folder.StartsWith(root, StringComparison.OrdinalIgnoreCase) ? c.Folder.Substring(root.Length).TrimStart('\\') : ".";
        var item = new ListViewItem(new[] { c.Name, rel, c.State }) { Tag = c, Checked = check && c.Installable, ToolTipText = c.Path };
        if (!c.Installable) item.ForeColor = SystemColors.GrayText;
        return item;
    }

    void SetBusy(bool busy) {
        scan.Enabled = browse.Enabled = games.Enabled = !busy;
        UseWaitCursor = busy;
        UpdateButtons();
    }

    List<Candidate> Checked() {
        return list.CheckedItems.Cast<ListViewItem>().Select(i => (Candidate)i.Tag).ToList();
    }

    string SelectedFolder() {
        var item = list.SelectedItems.Count > 0 ? list.SelectedItems[0] : list.CheckedItems.Count > 0 ? list.CheckedItems[0] : list.Items.Count > 0 ? list.Items[0] : null;
        return item != null ? ((Candidate)item.Tag).Folder : Directory.Exists(folder.Text) ? folder.Text : null;
    }

    void UpdateButtons() {
        bool idle = scanning == null || !scanning.IsAlive;
        var chk = Checked();
        install.Enabled = idle && (xessMode ? xessDll : dll) != null && chk.Count > 0 && chk.All(c => c.Installable);
        restore.Enabled = idle && chk.Count > 0;
        settings.Enabled = logButton.Enabled = open.Enabled = idle && list.Items.Count > 0;
    }

    void Install() {
        Run("install", c => Installer.Install(c, c.Xess ? xessDll : dll, ini, Log), Checked());
        if (optiMode)
            Log("Now, in the game, open OptiScaler's menu (Insert) and pick DittoSR as the upscaler. Its Sharpness (Override) and FSR 3's Velocity Factor set DittoSR's sharpness and frame weight.");
        else
            Log("Now choose " + (xessMode ? "XeSS" : "FSR (FSR 3.1 or later)") + " in the game's settings. With [debug] indicator=1, a small pink face shows top-left when DittoSR runs.");
    }

    void Restore() {
        var folders = Checked().Select(c => c.Folder).Distinct(StringComparer.OrdinalIgnoreCase)
                               .Select(f => new Candidate { Folder = f }).ToList();
        Run("restore", c => Installer.Restore(c.Folder, Log), folders);
    }

    void Run(string what, Action<Candidate> act, List<Candidate> items) {
        foreach (var c in items) {
            try {
                act(c);
            } catch (UnauthorizedAccessException) {
                Log(what + ": no permission to write in " + c.Folder);
                if (!IsAdmin() && MessageBox.Show(this, "Windows doesn't let the loader write in\n" + c.Folder + "\n\nRestart the loader as administrator?",
                                                  "DittoSR Loader", MessageBoxButtons.YesNo, MessageBoxIcon.Question) == DialogResult.Yes) {
                    Elevate();
                    return;
                }
                break;
            } catch (IOException ex) {
                Log(what + " failed: " + ex.Message + (ex.Message.Contains("being used") ? " (close the game first)" : ""));
                break;
            } catch (Exception ex) {
                Log(what + " failed: " + ex.Message);
                break;
            }
        }
        StartScan();
    }

    static bool IsAdmin() {
        return new System.Security.Principal.WindowsPrincipal(System.Security.Principal.WindowsIdentity.GetCurrent())
            .IsInRole(System.Security.Principal.WindowsBuiltInRole.Administrator);
    }

    void Elevate() {
        try {
            Process.Start(new ProcessStartInfo(Application.ExecutablePath, "\"" + folder.Text.Trim().Trim('"') + "\"") { Verb = "runas", UseShellExecute = true });
            Close();
        } catch (Exception) {
            Log("not restarted as administrator");
        }
    }

    void ShowSettings() {
        string f = SelectedFolder();
        if (f == null) return;
        string p = Path.Combine(f, Files.Ini);
        if (!File.Exists(p)) { Log("no dittosr.ini there yet: install DittoSR first (" + p + ")"); return; }
        try {
            using (var d = new SettingsForm(p, k))
                if (d.ShowDialog(this) == DialogResult.OK) {
                    float? sh = IniKeys.Get(p, "sharpness"), fw = IniKeys.Get(p, "frame_weight");
                    Log("settings saved in " + p + ": sharpness " + (sh.HasValue ? sh.Value.ToString(System.Globalization.CultureInfo.InvariantCulture) : "-") +
                        ", frame_weight " + (fw.HasValue ? fw.Value.ToString(System.Globalization.CultureInfo.InvariantCulture) : "-") + " (F9 in the game applies them live)");
                }
        } catch (UnauthorizedAccessException) {
            Log("settings: no permission to write " + p + " - restart the loader as administrator");
        } catch (IOException ex) {
            Log("settings failed: " + ex.Message);
        }
    }

    void OpenFile(string name, string missing) {
        string f = SelectedFolder();
        if (f == null) return;
        string p = Path.Combine(f, name);
        if (!File.Exists(p)) { Log(missing + " (" + p + ")"); return; }
        Process.Start("notepad.exe", "\"" + p + "\"");
    }
}

static class Program {
    [STAThread]
    static void Main(string[] args) {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new LoaderForm(args.Length > 0 ? args[0] : null));
    }
}

}
