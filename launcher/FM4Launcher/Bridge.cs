using System.Text.Json;
using System.Text.Json.Nodes;
using Photino.NET;

namespace FM4Launcher;

/// <summary>
/// The page's calls: { id, cmd, args } over the window's message channel (wwwroot/host.js), answered with
/// { id, ok, result | error }.
/// </summary>
public sealed class Bridge
{
    static readonly JsonSerializerOptions Json = new(JsonSerializerDefaults.Web);
    readonly PhotinoWindow _window;

    public Bridge(PhotinoWindow window)
    {
        _window = window;
        window.RegisterWebMessageReceivedHandler((_, text) => OnMessage(text));
    }

    async void OnMessage(string text)
    {
        JsonNode? msg;
        try { msg = JsonNode.Parse(text); } catch { return; }
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

    void Post(object o) => _window.SendWebMessage(JsonSerializer.Serialize(o, Json));

    static readonly (string, string[])[] AnyFile = [("Xbox 360 content packages", ["*.*"])];
    static readonly (string, string[])[] ZipFilter = [("Profile zip", ["*.zip"])];

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
                    var files = await _window.ShowOpenFileAsync("Choose DLC packages", null, true, AnyFile);
                    if (files is not { Length: > 0 }) return new { added = 0, errors = Array.Empty<string>(), state = State() };
                    var (added, errors) = Dlc.Add(files);
                    return new { added, errors, state = State() };
                }
            case "photos.open":
                PhotosWindow.Show(_window);
                return null;
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
                    Platform.OpenFolder(path);
                    return null;
                }
            case "profiles.list": return ProfilesState();
            case "profiles.use":
                Profiles.SetActive(Str(a, "name"));
                return ProfilesState();
            case "profiles.create":
                Profiles.Create(Str(a, "name"));
                return ProfilesState();
            case "profiles.duplicate":
                Profiles.Duplicate(Str(a, "name"), Str(a, "newName"));
                return ProfilesState();
            case "profiles.rename":
                Profiles.Rename(Str(a, "name"), Str(a, "newName"));
                return ProfilesState();
            case "profiles.delete":
                {
                    var b = Profiles.Delete(Str(a, "name"));
                    return new { backup = b.File, state = ProfilesState() };
                }
            case "profiles.backup":
                {
                    var b = Profiles.MakeBackup(Str(a, "name"));
                    return new { backup = b.File, state = ProfilesState() };
                }
            case "profiles.restore":
                {
                    var name = Profiles.Restore(Str(a, "file"), a?["asName"]?.GetValue<string>());
                    return new { name, state = ProfilesState() };
                }
            case "profiles.import":
                {
                    var files = await _window.ShowOpenFileAsync("Import a profile", null, false, ZipFilter);
                    if (files is not { Length: > 0 }) return null;
                    var name = Profiles.Restore(files[0], a?["asName"]?.GetValue<string>());
                    return new { name, state = ProfilesState() };
                }
            case "profiles.export":
                {
                    var name = Str(a, "name");
                    var file = await _window.ShowSaveFileAsync("Export profile", Profiles.FolderName(name) + ".zip", ZipFilter);
                    if (string.IsNullOrEmpty(file)) return false;
                    Profiles.Export(name, file);
                    return true;
                }
            case "profiles.mergePhotos":
                return new { copied = Profiles.MergePhotos(Str(a, "from"), Str(a, "into")), state = ProfilesState() };
            case "profiles.openBackups":
                Platform.OpenFolder(Directory.CreateDirectory(Profiles.Backups).FullName);
                return null;
            case "photos.list": return Photos.List();
            case "photos.copy":
                Platform.CopyPng(Convert.FromBase64String(a!.GetValue<string>()));
                return null;
            case "photos.saveAs":
                {
                    var id = a!.GetValue<string>();
                    var file = await _window.ShowSaveFileAsync("Save photo", Path.Combine(Photos.SaveFolder, Path.GetFileName(Photos.PathOf(id)) + ".jpg"),
                        [("JPEG image", ["*.jpg"]), ("PNG image", ["*.png"])]);
                    return string.IsNullOrEmpty(file) ? null : file;
                }
            case "photos.write":
                {
                    var png = a?["png"]?.GetValue<string>();
                    Photos.Write(Str(a, "id"), Str(a, "file"), png is null ? null : Convert.FromBase64String(png));
                    return null;
                }
            case "photos.reveal":
                Photos.Reveal(a?.GetValue<string>());
                return null;
            default:
                throw new ArgumentException($"Unknown command {cmd}.");
        }
    }

    static string Str(JsonNode? a, string key) => a?[key]?.GetValue<string>() ?? throw new ArgumentException($"Missing {key}.");

    static object ProfilesState() => new { profiles = Profiles.List(), backups = Profiles.ListBackups(), running = Game.Running };

    static object State()
    {
        var settings = GameSettings.Load();
        var stringTables = Path.Combine(Paths.GameDir, "Media", "StringTables");
        return new
        {
            settings,
            version = Version(),
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
                languages = Options.InstalledLanguages().Select(l => new { id = l.Id, code = l.Code, name = l.Name }),
                missingLanguages = Directory.Exists(stringTables)
                    ? new[] { "PL", "CZ", "HU" }.Where(c => File.Exists(Path.Combine(stringTables, c + ".zip"))).ToArray() : [],
            },
            shaders = ShaderCache.Get(),
            dlc = Dlc.List(),
        };
    }

    /// <summary>The game's version, from fm4.exe's version resource.</summary>
    static string Version()
    {
        try
        {
            var v = File.Exists(Paths.Exe) ? System.Diagnostics.FileVersionInfo.GetVersionInfo(Paths.Exe).ProductVersion : null;
            return string.IsNullOrWhiteSpace(v) ? "0.0.0" : v.Trim();
        }
        catch { return "0.0.0"; }
    }
}
