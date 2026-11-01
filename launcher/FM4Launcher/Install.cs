using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace FM4Launcher;

/// <summary>Where things are: the install (fm4.exe, game\) and the user folder (Documents\fm4).</summary>
public static class Paths
{
    /// <summary>The install folder: the first folder up from the launcher that holds build\fm4\fm4.exe (a source
    /// checkout) or fm4.exe itself (an install).</summary>
    public static readonly string Root = Env("FM4_ROOT") ?? FindRoot();
    public static string Exe => Env("FM4_EXE") ?? (File.Exists(Path.Combine(Root, "fm4.exe")) ? Path.Combine(Root, "fm4.exe") : Path.Combine(Root, "build", "fm4", "fm4.exe"));
    /// <summary>Linux: the Proton prefix the game runs in (as linux/release/play.sh picks it).</summary>
    public static readonly string CompatData = Environment.GetEnvironmentVariable("FM4_COMPATDATA") is { Length: > 0 } c ? c
        : Path.Combine(Environment.GetEnvironmentVariable("FM4_LINUX_HOME") is { Length: > 0 } h ? h
            : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".fm4-linux"), "compatdata");
    public static string GameDirDefault => Path.Combine(Root, "game");
    /// <summary>The disc's files: FM4_GAME_DIR, else the Game folder chosen in the launcher, else game next to the exe.</summary>
    public static string GameDir => Env("FM4_GAME_DIR") ?? (GameSettings.Load().GameFolder is { Length: > 0 } g ? g : GameDirDefault);
    /// <summary>Where fm4.exe looks for default.xexp when nothing is chosen (Documents\fm4\update, src/loader.cpp).</summary>
    public static string TitleUpdateDirDefault => Path.Combine(User, "update");
    public static readonly string User = FindUser();
    public static string SettingsFile => Path.Combine(User, "settings.ini");
    public static string Dlc => Path.Combine(User, "dlc");
    public static string DlcDisabled => Path.Combine(User, "dlc-disabled");

    /// <summary>Documents\fm4 in the Documents folder Windows reports (it may be moved off C:), as fm4.exe picks it
    /// (src/user_dir.cpp): if only the old %USERPROFILE%\Documents\fm4 exists, that one.</summary>
    static string? Env(string name) => Environment.GetEnvironmentVariable(name) is { Length: > 0 } v ? v : null;

    static string FindUser()
    {
        if (Env("FM4_USER_DIR") is { } custom) return Path.GetFullPath(custom);
        // Linux: fm4.exe's Documents is the Proton prefix's.
        if (Platform.Linux) return Path.Combine(CompatData, "pfx", "drive_c", "users", "steamuser", "Documents", "fm4");
        var current = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "fm4");
        var old = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Documents", "fm4");
        return !Directory.Exists(current) && Directory.Exists(old) ? old : current;
    }

    static string FindRoot()
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
            if (File.Exists(Path.Combine(dir.FullName, "build", "fm4", "fm4.exe")) || File.Exists(Path.Combine(dir.FullName, "fm4.exe")))
                return dir.FullName;
        return Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, ".."));
    }
}

/// <summary>
/// Documents\fm4\settings.ini, the file fm4.exe reads (src/settings.cpp). Keys the launcher doesn't know are
/// kept as they are.
/// </summary>
public sealed class GameSettings
{
    public string Profile { get; set; } = "Player";
    public string Resolution { get; set; } = "1280x720";
    public string Aspect { get; set; } = "auto";
    public bool Fullscreen { get; set; }
    public bool Vsync { get; set; } = true;
    public int MaxFps { get; set; } = 60;
    public int Msaa { get; set; } = 1;
    public bool Enhance { get; set; } // enhanced lighting post pass (experimental)
    public int Smaa { get; set; }
    public bool Anisotropy { get; set; } = true; // launcher only: off sets REGPU_NO_ANISO at launch (Game.Launch)
    // The 3D scene's upscaler: off, fsr1, fsr3 or fsr4 (settings.ini "upscaler"), at Upscale = the game's
    // upscale_quality (1 quality to 4 ultra performance).
    public string Upscaler { get; set; } = "off";
    public int Upscale { get; set; } = 1;
    // Launcher only (environment at launch, Game.Launch): the background shader optimiser (0 off, else the fxc
    // optimisation level 1-3), the upscaler's motion or depth input shown instead of the picture (off, motion,
    // depth) and REGPU_PERF's timing lines in the log.
    public int OptimizeLevel { get; set; }
    public string DebugView { get; set; } = "off";
    public bool PerfLog { get; set; } = true; // on unless the settings file says otherwise
    // Launcher only: where the disc's files and the title update (the folder holding default.xexp) are; empty = the
    // defaults (Paths.GameDirDefault, Paths.TitleUpdateDirDefault).
    public string GameFolder { get; set; } = "";
    public string TitleUpdateFolder { get; set; } = "";
    public int Sharpening { get; set; } = 50; // percent; settings.ini holds 0-1
    public bool HideShaderOverlay { get; set; } // launcher only: sets REGPU_NO_OVERLAY at launch
    public bool FastMode { get; set; } = true; // launcher only: set on the game's environment at launch
    public int Language { get; set; } = 1;

    // What fm4.exe reads (src/settings.cpp). FM2's later options (SMAA, anisotropy, render scale, vertex arena, the
    // shader pop-up, music, wheel) have no counterpart in FM4's runtime yet, so they aren't offered.
    static readonly string[] Known = ["profile", "resolution", "aspect", "fullscreen", "vsync", "max_fps", "msaa", "smaa", "anisotropy", "fast_mode", "upscaler", "upscale_quality", "sharpness", "optimize_level", "debug_view", "perf_log", "game_folder", "title_update_folder", "enhance", "hide_shader_overlay", "language"];

    public static GameSettings Load()
    {
        var s = new GameSettings();
        foreach (var (k, v) in Read())
            switch (k)
            {
                case "profile": s.Profile = v; break;
                case "resolution": s.Resolution = v; break;
                case "aspect": s.Aspect = v; break;
                case "fullscreen": s.Fullscreen = v == "1"; break;
                case "vsync": s.Vsync = v == "1"; break;
                case "max_fps": s.MaxFps = int.TryParse(v, out var f) ? f : 60; break;
                case "msaa": s.Msaa = int.TryParse(v, out var m) ? m : 1; break;
                case "enhance": s.Enhance = v == "1"; break;
                case "smaa": s.Smaa = int.TryParse(v, out var sm) ? Math.Clamp(sm, 0, 4) : 0; break;
                case "anisotropy": s.Anisotropy = v != "0"; break;
                case "upscaler": s.Upscaler = Options.Upscalers.Any(u => u.Value == v) ? v : "off"; break;
                case "upscale_quality": s.Upscale = int.TryParse(v, out var q) ? Math.Clamp(q, 1, 4) : 1; break;
                case "optimize_level": s.OptimizeLevel = int.TryParse(v, out var ol) ? Math.Clamp(ol, 0, 3) : 0; break;
                case "debug_view": s.DebugView = v is "motion" or "depth" ? v : "off"; break;
                case "perf_log": s.PerfLog = v == "1"; break;
                case "game_folder": s.GameFolder = v; break;
                case "title_update_folder": s.TitleUpdateFolder = v; break;
                case "sharpness":
                    s.Sharpening = double.TryParse(v, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var sh)
                        ? (int)Math.Round(sh * 100) : 50;
                    break;
                case "hide_shader_overlay": s.HideShaderOverlay = v == "1"; break;
                case "fast_mode": s.FastMode = v != "0"; break;
                case "language": s.Language = int.TryParse(v, out var l) && l > 0 ? l : 1; break;
            }
        return s;
    }

    public void Save()
    {
        var keep = Read().Where(kv => !Known.Contains(kv.Key)).ToList();
        var profile = new string(Profile.Trim().Where(c => !char.IsControl(c) && c != '=').Take(15).ToArray());
        var sb = new StringBuilder("# Forza Motorsport 4 (fm4) settings; the launcher edits this file.\n");
        sb.Append($"profile={(profile.Length > 0 ? profile : "Player")}\n");
        sb.Append($"resolution={Resolution}\naspect={Aspect}\nfullscreen={B(Fullscreen)}\nvsync={B(Vsync)}\n");
        sb.Append($"max_fps={Math.Clamp(MaxFps, 30, 360)}\nmsaa={Msaa}\nsmaa={Math.Clamp(Smaa, 0, 4)}\nanisotropy={B(Anisotropy)}\nfast_mode={B(FastMode)}\nenhance={B(Enhance)}\nhide_shader_overlay={B(HideShaderOverlay)}\nlanguage={Language}\n");
        var sharp = (Math.Clamp(Sharpening, 0, 100) / 100.0).ToString(System.Globalization.CultureInfo.InvariantCulture);
        sb.Append($"upscaler={(Options.Upscalers.Any(u => u.Value == Upscaler) ? Upscaler : "off")}\nupscale_quality={Math.Clamp(Upscale, 1, 4)}\nsharpness={sharp}\n");
        sb.Append($"optimize_level={Math.Clamp(OptimizeLevel, 0, 3)}\ndebug_view={(DebugView is "motion" or "depth" ? DebugView : "off")}\nperf_log={B(PerfLog)}\ngame_folder={Clean(GameFolder)}\ntitle_update_folder={Clean(TitleUpdateFolder)}\n");

        foreach (var (k, v) in keep) sb.Append($"{k}={v}\n");
        Directory.CreateDirectory(Paths.User);
        File.WriteAllText(Paths.SettingsFile, sb.ToString());
    }

    static string B(bool b) => b ? "1" : "0";
    static string Clean(string path) => new string(path.Where(c => !char.IsControl(c)).ToArray()).Trim();

    static List<KeyValuePair<string, string>> Read()
    {
        var list = new List<KeyValuePair<string, string>>();
        if (!File.Exists(Paths.SettingsFile)) return list;
        foreach (var raw in File.ReadAllLines(Paths.SettingsFile))
        {
            var line = raw.Trim();
            int eq = line.IndexOf('=');
            if (line.StartsWith('#') || eq <= 0) continue;
            list.Add(new(line[..eq].Trim(), line[(eq + 1)..].Trim()));
        }
        return list;
    }
}

/// <summary>Choices the Play page offers.</summary>
public static class Options
{
    public sealed record Language(int Id, string Code, string Name);

    // The console's language IDs (XGetLanguage) by the string table each should open; not traced in FM4 yet the way
    // FM2's were. The disc also has CZ, NL and HU tables, which no language ID names, and GB and MX variants of
    // English and Spanish.
    public static readonly Language[] Languages =
    [
        new(1, "EN", "English"), new(3, "DE", "Deutsch"), new(4, "FR", "Français"), new(5, "ES", "Español"),
        new(6, "IT", "Italiano"), new(9, "BR", "Português (Brasil)"), new(11, "PL", "Polski"), new(12, "RU", "Русский"),
        new(2, "JP", "日本語"), new(7, "KO", "한국어"), new(8, "CHT", "繁體中文"),
    ];

    /// <summary>The languages whose string table is in this install's game\Media\StringTables.</summary>
    public static IEnumerable<Language> InstalledLanguages()
    {
        var dir = Path.Combine(Paths.GameDir, "Media", "StringTables");
        if (!Directory.Exists(dir)) return Languages;
        var have = Directory.GetFiles(dir, "*.zip").Select(f => Path.GetFileNameWithoutExtension(f).ToUpperInvariant()).ToHashSet();
        return Languages.Where(l => have.Contains(l.Code));
    }

    public static readonly int[] Fps = [30, 60, 120, 144, 165, 240, 360];
    public static readonly (string Value, string Name)[] Aspects = [("auto", "Auto (match resolution)"), ("16:9", "16:9"), ("21:9", "21:9 (experimental)"), ("32:9", "32:9 (experimental)")];
    public static readonly (int Value, string Name)[] Msaa = [(1, "Off"), (2, "2x MSAA"), (4, "4x MSAA")];
    public static readonly (int Value, string Name)[] Smaa = [(0, "Off"), (1, "Low"), (2, "Medium"), (3, "High"), (4, "Ultra")];
    public static readonly (string Value, string Name)[] Upscalers = [("off", "Off (native)"), ("fsr1", "FSR 1 (any graphics card)"), ("fsr3", "FSR 3 (any graphics card)"), ("fsr4", "FSR 4 (AMD RDNA 4, else FSR 3)")];
    public static readonly (int Value, string Name)[] Upscale = [(1, "Quality"), (2, "Balanced"), (3, "Performance"), (4, "Ultra performance")];
    public static readonly (int Value, string Name)[] OptimizeLevels = [(0, "Off (default)"), (1, "Level 1"), (2, "Level 2"), (3, "Level 3")];
    public static readonly (string Value, string Name)[] DebugViews = [("off", "Off (normal picture)"), ("motion", "Motion vectors"), ("depth", "Depth")];
    public static readonly (int Value, string Name)[] Sharpening = [(0, "Off"), (25, "Low"), (50, "Medium"), (75, "High"), (100, "Maximum")];

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct DEVMODE
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
        public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
        public int dmFields, dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
        public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
        public short dmLogPixels;
        public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
        public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
    }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern bool EnumDisplaySettingsW(string? device, int mode, ref DEVMODE dm);

    /// <summary>The primary display's modes from 1280x720 up, smallest first (as the old launcher listed them).</summary>
    public static List<string> Resolutions()
    {
        var set = new HashSet<(int, int)>();
        if (Platform.Linux)
            foreach (var (w, h) in LinuxModes())
            {
                if (w >= 1280 && h >= 720) set.Add((w, h));
            }
        else
        {
            var dm = new DEVMODE { dmSize = (short)Marshal.SizeOf<DEVMODE>() };
            for (int i = 0; EnumDisplaySettingsW(null, i, ref dm); i++)
                if (dm.dmPelsWidth >= 1280 && dm.dmPelsHeight >= 720) set.Add((dm.dmPelsWidth, dm.dmPelsHeight));
        }
        if (set.Count == 0) set.Add((1280, 720));
        return set.OrderBy(m => (long)m.Item1 * m.Item2).Select(m => $"{m.Item1}x{m.Item2}").ToList();
    }

    // Linux: the connected outputs' modes from xrandr (X11 and XWayland list them alike); without it, the common
    // 16:9 sizes. Wine offers the game the same modes.
    static IEnumerable<(int, int)> LinuxModes()
    {
        var modes = new List<(int, int)>();
        try
        {
            var psi = new ProcessStartInfo("xrandr", "--query") { RedirectStandardOutput = true, UseShellExecute = false };
            using var p = Process.Start(psi)!;
            foreach (var line in p.StandardOutput.ReadToEnd().Split('\n'))
            {
                var m = System.Text.RegularExpressions.Regex.Match(line, @"^\s+(\d+)x(\d+)\s");
                if (m.Success) modes.Add((int.Parse(m.Groups[1].Value), int.Parse(m.Groups[2].Value)));
            }
            p.WaitForExit(2000);
        }
        catch { }
        return modes.Count > 0 ? modes : [(1280, 720), (1600, 900), (1920, 1080), (2560, 1440), (3840, 2160)];
    }
}

/// <summary>Starting the game, as play-fm4.cmd does: the DLC next to the install copied in first.</summary>
public static class Game
{
    public static bool Running => Platform.Linux ? RunningUnderWine() : Process.GetProcessesByName("fm4").Any(p =>
    {
        try { return string.Equals(p.MainModule?.FileName, Paths.Exe, StringComparison.OrdinalIgnoreCase); }
        catch { return true; } // can't look inside: assume it is ours
    });

    // Under Proton, fm4.exe is a wine process whose command line names it (as a Unix or a Z:\ path).
    static bool RunningUnderWine()
    {
        try
        {
            foreach (var dir in Directory.EnumerateDirectories("/proc"))
            {
                if (!int.TryParse(Path.GetFileName(dir), out var pid) || pid == Environment.ProcessId) continue;
                string cmdline;
                try { cmdline = File.ReadAllText(Path.Combine(dir, "cmdline")); } catch { continue; }
                var first = cmdline.Split('\0')[0];
                if (first.EndsWith("fm4.exe", StringComparison.OrdinalIgnoreCase)) return true;
            }
        }
        catch { }
        return false;
    }

    /// <summary>Linux: Proton as play.sh finds it: $PROTON, else Proton Experimental, 11 or 10 in Steam's libraries.</summary>
    public static string? Proton()
    {
        if (Environment.GetEnvironmentVariable("PROTON") is { Length: > 0 } set) return File.Exists(set) ? set : null;
        var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        foreach (var dir in new[] { ".local/share/Steam/steamapps/common", ".steam/steam/steamapps/common", ".var/app/com.valvesoftware.Steam/data/Steam/steamapps/common" })
            foreach (var name in new[] { "Proton - Experimental", "Proton 11.0", "Proton 10.0" })
            {
                var proton = Path.Combine(home, dir, name, "proton");
                if (File.Exists(proton)) return proton;
            }
        return null;
    }

    static ProcessStartInfo LinuxStart(string log)
    {
        var proton = Proton() ?? throw new InvalidOperationException("No Proton found: install Proton Experimental in Steam (Library, search \"Proton Experimental\"), or set PROTON to its proton script.");
        if (!File.Exists(Path.Combine(Paths.GameDir, "default.xex"))) throw new InvalidOperationException("No game files yet: extract your disc into the game folder first.");
        Directory.CreateDirectory(Paths.CompatData);
        var psi = new ProcessStartInfo("sh") { WorkingDirectory = Paths.Root, UseShellExecute = false };
        foreach (var a in new[] { "-c", "exec \"$0\" run \"$1\" \"$2\" > \"$3\" 2>&1", proton, Paths.Exe, Paths.GameDir, log }) psi.ArgumentList.Add(a);
        var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        psi.Environment["STEAM_COMPAT_CLIENT_INSTALL_PATH"] = Environment.GetEnvironmentVariable("STEAM_COMPAT_CLIENT_INSTALL_PATH") is { Length: > 0 } s ? s : Path.Combine(home, ".steam", "steam");
        psi.Environment["STEAM_COMPAT_DATA_PATH"] = Paths.CompatData;
        // Wine's own d3dcompiler_47 can't compile the renderer's shaders: the Microsoft DLL beside fm4.exe instead.
        var overrides = Environment.GetEnvironmentVariable("WINEDLLOVERRIDES");
        psi.Environment["WINEDLLOVERRIDES"] = "d3dcompiler_47=n" + (string.IsNullOrEmpty(overrides) ? "" : ";" + overrides);
        return psi;
    }

    public static void Launch()
    {
        if (!File.Exists(Paths.Exe)) throw new InvalidOperationException($"fm4.exe isn't at {Paths.Exe}.");
        CopyMissing(Path.Combine(Paths.Root, "dlc"), Paths.Dlc, Paths.DlcDisabled);
        Directory.CreateDirectory(Path.Combine(Paths.Root, "out"));
        var log = Path.Combine(Paths.Root, "out", "play-fm4.log");
        var psi = Platform.Linux ? LinuxStart(log) : new ProcessStartInfo("cmd.exe", $"/c \"\"{Paths.Exe}\" \"{Paths.GameDir}\" > \"{log}\" 2>&1\"")
        {
            WorkingDirectory = Paths.Root,
            UseShellExecute = false,
            CreateNoWindow = true,
        };
        psi.Environment["FM2_NO_LAUNCHER"] = "1"; // the settings come from settings.ini, saved by this launcher
        // As play-fm4.cmd: fences written when recorded, and resolves up to 1024 pixels written back to memory (the
        // road surface and the photo mode tint read them).
        var settings = GameSettings.Load();
        if (settings.FastMode) psi.Environment["REGPU_EARLY_FENCES"] = "1";
        if (!settings.Anisotropy) psi.Environment["REGPU_NO_ANISO"] = "1";
        if (settings.HideShaderOverlay) psi.Environment["REGPU_NO_OVERLAY"] = "1";
        psi.Environment["REGPU_RESOLVE_WRITEBACK_MAX"] = "1024";
        if (settings.OptimizeLevel > 0)
        {
            psi.Environment["REGPU_OPTIMIZE_SHADERS"] = "1";
            psi.Environment["REGPU_OPTIMIZE_LEVEL"] = settings.OptimizeLevel.ToString();
        }
        if (settings.DebugView == "motion") psi.Environment["REGPU_VIEW_MOTION"] = "1";
        if (settings.DebugView == "depth") psi.Environment["REGPU_VIEW_DEPTH"] = "1";
        if (settings.PerfLog) psi.Environment["REGPU_PERF"] = "1";
        // The live debug server (127.0.0.1 only, tools\regpuctl.py in thirdparty\ReGPU): status, screenshots, live switches.
        psi.Environment["REGPU_DEBUG"] = "1";
        if (settings.TitleUpdateFolder.Length > 0) psi.Environment["FM2_TITLE_UPDATE_DIR"] = settings.TitleUpdateFolder;
        // FSR 3 and FSR 4 do their own anti-aliasing: MSAA and SMAA go to the game off, the saved values stay.
        if (settings.Upscaler is "fsr3" or "fsr4")
        {
            psi.Environment["FM2_MSAA"] = "1";
            psi.Environment["REGPU_SMAA"] = "0";
        }
        var game = Process.Start(psi);
        if (game is null || !Platform.Linux) return;
        // Under Proton the game's stderr never reaches the log (only Proton's own lines do): add its fm4.log.
        game.EnableRaisingEvents = true;
        game.Exited += (_, _) =>
        {
            try { File.AppendAllText(log, File.ReadAllText(Path.Combine(Paths.Root, "fm4.log"))); } catch (IOException) { }
        };
    }

    // robocopy /XC /XN /XO in install-content.cmd: what is already there is left alone. A DLC package the game has
    // unpacked is a folder of the same name; one switched off sits in dlc-disabled.
    static void CopyMissing(string from, string to, string? alsoIn = null)
    {
        if (!Directory.Exists(from)) return;
        foreach (var file in Directory.EnumerateFiles(from, "*", SearchOption.AllDirectories))
        {
            var rel = Path.GetRelativePath(from, file);
            var dest = Path.Combine(to, rel);
            var top = rel.Split(Path.DirectorySeparatorChar)[0]; // the package (or the file itself)
            bool Has(string dir) => File.Exists(Path.Combine(dir, top)) || Directory.Exists(Path.Combine(dir, top));
            if (File.Exists(dest) || Directory.Exists(dest)) continue;
            if (top == rel && Has(to)) continue;                         // a package the game unpacked into a folder
            if (alsoIn is not null && Has(alsoIn)) continue;             // switched off
            Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
            File.Copy(file, dest);
        }
    }
}

/// <summary>The shader and pipeline caches in Documents\fm4 (what play-cold.cmd moves aside).</summary>
public static class ShaderCache
{
    public sealed record Info(bool Exists, int Shaders, long Bytes, DateTime? Built);

    static readonly string[] Dirs = ["shadercache", "shaders"];
    static readonly string[] Files = ["pipelines.bin", "pipelines.manifest", "static_shaders.bin"];

    public static Info Get()
    {
        long bytes = 0;
        int count = 0;
        DateTime? built = null;
        foreach (var d in Dirs.Select(d => Path.Combine(Paths.User, d)).Where(Directory.Exists))
            foreach (var f in new DirectoryInfo(d).EnumerateFiles("*", SearchOption.AllDirectories))
            {
                bytes += f.Length;
                if (f.Extension is ".dxbc" or ".ucode") count++;
            }
        foreach (var f in Files.Select(f => new FileInfo(Path.Combine(Paths.User, f))).Where(f => f.Exists))
        {
            bytes += f.Length;
            if (f.Name == "pipelines.bin") built = f.LastWriteTime;
        }
        return new Info(bytes > 0, count, bytes, built);
    }

    /// <summary>Deletes the caches; the next start rebuilds them (several minutes).</summary>
    public static void Delete()
    {
        if (Game.Running) throw new InvalidOperationException("Close the game first: it is using the shader cache.");
        foreach (var d in Dirs.Select(d => Path.Combine(Paths.User, d)).Where(Directory.Exists)) Directory.Delete(d, true);
        foreach (var f in Files.Select(f => Path.Combine(Paths.User, f)).Where(File.Exists)) File.Delete(f);
    }
}

/// <summary>DLC packages in Documents\fm4\dlc (disabled ones wait in dlc-disabled). A package is a file or an unpacked
/// folder, with a .title (its name, UTF-16) and a .license beside it.</summary>
public static class Dlc
{
    public sealed record Pack(string Id, string Name, bool Enabled, long Bytes);

    // FM2's mods (marked by a .mod file) share this folder there; FM4 has none, so anything marked so is left out.
    public static List<Pack> List() => ListAll(mods: false);

    static List<Pack> ListAll(bool mods)
    {
        var packs = new List<Pack>();
        foreach (var (dir, enabled) in new[] { (Paths.Dlc, true), (Paths.DlcDisabled, false) })
        {
            if (!Directory.Exists(dir)) continue;
            foreach (var entry in new DirectoryInfo(dir).EnumerateFileSystemInfos())
            {
                if (entry.Extension is ".title" or ".license" or ".mod") continue;
                if (File.Exists(Path.Combine(dir, entry.Name + ".mod")) != mods) continue;
                var titleFile = Path.Combine(dir, entry.Name + ".title");
                var name = File.Exists(titleFile) ? Encoding.Unicode.GetString(File.ReadAllBytes(titleFile)).TrimEnd('\0').Trim() : entry.Name;
                long bytes = entry is FileInfo fi ? fi.Length
                    : ((DirectoryInfo)entry).EnumerateFiles("*", SearchOption.AllDirectories).Sum(f => f.Length);
                packs.Add(new Pack(entry.Name, name.Length > 0 ? name : entry.Name, enabled, bytes));
            }
        }
        return packs.OrderBy(p => p.Name, StringComparer.CurrentCultureIgnoreCase).ToList();
    }

    public static void SetEnabled(string id, bool enabled)
    {
        if (id.Contains('/') || id.Contains('\\') || id.Contains("..")) throw new ArgumentException("Not a package name.");
        if (Game.Running) throw new InvalidOperationException("Close the game first: DLC is read while it runs.");
        var (from, to) = enabled ? (Paths.DlcDisabled, Paths.Dlc) : (Paths.Dlc, Paths.DlcDisabled);
        Directory.CreateDirectory(to);
        foreach (var name in new[] { id, id + ".title", id + ".license", id + ".mod" })
        {
            var src = Path.Combine(from, name);
            var dst = Path.Combine(to, name);
            if (Directory.Exists(src)) Directory.Move(src, dst);
            else if (File.Exists(src)) File.Move(src, dst, true);
        }
    }

    /// <summary>Installs packages (as downloaded from Xbox Live): unpacked into the DLC folder the way the game reads
    /// them.</summary>
    public static (int Added, List<string> Errors) Add(IEnumerable<string> files)
    {
        if (Game.Running) throw new InvalidOperationException("Close the game first: DLC is read while it runs.");
        Directory.CreateDirectory(Paths.Dlc);
        int added = 0;
        var errors = new List<string>();
        foreach (var f in files.Where(File.Exists))
        {
            try
            {
                Stfs.Install(f, Paths.Dlc, false);
                added++;
            }
            catch (Exception ex) { errors.Add($"{Path.GetFileName(f)}: {ex.Message}"); }
        }
        return (added, errors);
    }
}
