using System.Text.Json;
using System.Text.Json.Nodes;
using Microsoft.Web.WebView2.Core;

namespace FM2Launcher;

/// <summary>
/// The page's calls: { id, cmd, args } over WebView2's message channel, answered with { id, ok, result | error }.
/// Progress and other pushes go out as { event, ... }.
/// </summary>
public sealed class Bridge(CoreWebView2 web, Form owner)
{
    static readonly JsonSerializerOptions Json = new(JsonSerializerDefaults.Web);
    Updater.Release? _latest;

    public async void OnMessage(object? sender, CoreWebView2WebMessageReceivedEventArgs e)
    {
        JsonNode? msg;
        try { msg = JsonNode.Parse(e.WebMessageAsJson); } catch { return; }
        var id = msg?["id"]?.GetValue<int>() ?? 0;
        var cmd = msg?["cmd"]?.GetValue<string>() ?? "";
        var args = msg?["args"];
        try
        {
            var result = await Run(cmd, args);
            Post(new { id, ok = true, result });
        }
        catch (Exception ex)
        {
            Post(new { id, ok = false, error = ex.Message });
        }
    }

    void Post(object o) => web.PostWebMessageAsJson(JsonSerializer.Serialize(o, Json));
    void Push(string name, object data) => Post(new { @event = name, data });

    async Task<object?> Run(string cmd, JsonNode? a)
    {
        switch (cmd)
        {
            case "state": return State();
            case "save":
                a.Deserialize<GameSettings>(Json)!.Save();
                return State();
            case "launch":
                a?["settings"].Deserialize<GameSettings>(Json)?.Save();
                if (Game.Running) throw new InvalidOperationException("The game is already running.");
                Game.Launch();
                return null;
            case "shaders.delete":
                ShaderCache.Delete();
                return State();
            case "shaders.rebuild":
                ShaderCache.Delete();
                Game.Launch();
                return State();
            case "dlc.toggle":
                Dlc.SetEnabled(a!["id"]!.GetValue<string>(), a["enabled"]!.GetValue<bool>());
                return State();
            case "dlc.add":
                {
                    using var dialog = new OpenFileDialog
                    {
                        Title = "Choose DLC packages",
                        Filter = "Xbox 360 content packages|*.*",
                        Multiselect = true,
                    };
                    int added = dialog.ShowDialog(owner) == DialogResult.OK ? Dlc.Add(dialog.FileNames) : 0;
                    return new { added, state = State() };
                }
            case "open":
                {
                    var which = a?.GetValue<string>();
                    var path = which switch
                    {
                        "user" => Paths.User,
                        "dlc" => Directory.CreateDirectory(Paths.Dlc).FullName,
                        "shaders" => Paths.User,
                        "install" => Paths.Root,
                        "log" => Path.Combine(Paths.Root, "out"),
                        _ => throw new ArgumentException("Unknown folder."),
                    };
                    System.Diagnostics.Process.Start("explorer.exe", $"\"{path}\"");
                    return null;
                }
            case "photos.list": return Photos.List();
            case "photos.copy":
                Photos.Copy(a!.GetValue<string>());
                return null;
            case "photos.save": return Photos.SaveAs(a!.GetValue<string>(), owner);
            case "photos.reveal":
                Photos.Reveal(a?.GetValue<string>());
                return null;
            case "url":
                {
                    var url = a?.GetValue<string>() ?? "";
                    if (!url.StartsWith("https://github.com/", StringComparison.OrdinalIgnoreCase)) throw new ArgumentException("Only GitHub links open from here.");
                    System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(url) { UseShellExecute = true });
                    return null;
                }
            case "update.check":
                _latest = await Updater.LatestAsync();
                return UpdateInfo();
            case "update.apply":
                {
                    _latest ??= await Updater.LatestAsync();
                    var applied = await Updater.DownloadAndApplyAsync(_latest, (p, text) => Push("progress", new { p, text }));
                    return new { applied.Files, applied.LauncherChanged, update = UpdateInfo(), state = State() };
                }
            case "restart":
                System.Diagnostics.Process.Start(Environment.ProcessPath!);
                owner.BeginInvoke(() => owner.Close());
                return null;
            default:
                throw new ArgumentException($"Unknown command {cmd}.");
        }
    }

    object UpdateInfo()
    {
        var installed = Updater.InstalledVersion();
        return new
        {
            installed,
            latest = _latest?.Version,
            name = _latest?.Name,
            notes = _latest?.Notes,
            page = _latest?.Page,
            published = _latest?.Published,
            patchBytes = _latest?.PatchBytes ?? 0,
            hasPatch = _latest?.PatchUrl is not null,
            available = _latest is not null && Updater.IsNewer(_latest.Version, installed),
        };
    }

    static object State()
    {
        var settings = GameSettings.Load();
        var stringTables = Path.Combine(Paths.GameDir, "Media", "StringTables");
        return new
        {
            settings,
            version = Updater.InstalledVersion(),
            paths = new { root = Paths.Root, user = Paths.User },
            checks = new
            {
                exe = File.Exists(Paths.Exe),
                game = File.Exists(Path.Combine(Paths.GameDir, "default.xex")),
                update = File.Exists(Path.Combine(Paths.Update, "default.xexp")),
                running = Game.Running,
            },
            options = new
            {
                resolutions = Options.Resolutions(),
                aspects = Options.Aspects.Select(x => new { value = x.Value, name = x.Name }),
                fps = Options.Fps,
                msaa = Options.Msaa.Select(x => new { value = x.Value, name = x.Name }),
                languages = Options.InstalledLanguages().Select(l => new { id = l.Id, code = l.Code, name = l.Name }),
                missingLanguages = Directory.Exists(stringTables)
                    ? new[] { "PL", "CZ", "HU" }.Where(c => File.Exists(Path.Combine(stringTables, c + ".zip"))).ToArray() : [],
            },
            shaders = ShaderCache.Get(),
            dlc = Dlc.List(),
        };
    }
}
