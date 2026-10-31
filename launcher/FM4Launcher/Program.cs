namespace FM4Launcher;

internal static class Program
{
    [STAThread]
    static int Main(string[] args)
    {
        // --play: start the game with the saved settings, without the window (shortcuts, tests)
        if (args.Contains("--play"))
        {
            try { Game.Launch(); return 0; }
            catch (Exception ex) { Platform.Error("Forza Motorsport 4 ReGPU", ex.Message); return 1; }
        }
        // one window: starting the launcher again brings the open one forward
        using var single = new Mutex(true, "FM4ReGPU.Launcher", out bool first);
        if (!first)
        {
            if (OperatingSystem.IsWindows())
                foreach (var p in System.Diagnostics.Process.GetProcessesByName("FM4Launcher"))
                    if (p.Id != Environment.ProcessId && p.MainWindowHandle != IntPtr.Zero) SetForegroundWindow(p.MainWindowHandle);
            return 0;
        }
        // WebKitGTK's DMA-BUF renderer fails on some Wayland setups (Gdk "Error 71 (Protocol error)", issue #16).
        // setenv, not Environment.SetEnvironmentVariable: .NET keeps its own copy that native code never sees.
        if (Platform.Linux && Environment.GetEnvironmentVariable("WEBKIT_DISABLE_DMABUF_RENDERER") == null)
            setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", 0);
        if (Platform.Linux) Platform.DesktopEntry();
        Photino.NET.PhotinoWindow window;
        try { window = Web.Create("Forza Motorsport 4 ReGPU", "index.html", 1680, 960, 1000, 700); }
        catch (Exception ex)
        {
            Platform.Error("Forza Motorsport 4 ReGPU", (Platform.Linux
                ? "The launcher needs WebKitGTK (libwebkit2gtk-4.1) and GTK 3."
                : "The launcher needs Microsoft Edge WebView2 (part of Windows 11; a free download for Windows 10).") + "\n\n" + ex.Message);
            return 1;
        }
        _ = new Bridge(window);
        window.WaitForClose();
        return 0;
    }

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    static extern bool SetForegroundWindow(IntPtr hWnd);

    [System.Runtime.InteropServices.DllImport("libc")]
    static extern int setenv(string name, string value, int overwrite);
}
