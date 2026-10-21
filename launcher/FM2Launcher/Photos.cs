namespace FM2Launcher;

/// <summary>Photo Mode pictures: Documents\fm2\profiles\&lt;profile&gt;\content\&lt;user&gt;\SavedPhotos\photos\&lt;name&gt;
/// (JPEGs without an extension). The page shows them through the photos.fm2 virtual host.</summary>
internal static class Photos
{
    public const string Host = "photos.fm2";
    public static readonly string Root = Path.Combine(Paths.User, "profiles");

    public record Photo(string Id, string Name, string Profile, long Bytes, DateTime Taken, string Url);

    public static List<Photo> List()
    {
        var list = new List<Photo>();
        if (!Directory.Exists(Root)) return list;
        foreach (var dir in Directory.EnumerateDirectories(Root, "photos", SearchOption.AllDirectories))
        {
            if (!string.Equals(Path.GetFileName(Path.GetDirectoryName(dir)), "SavedPhotos", StringComparison.OrdinalIgnoreCase)) continue;
            foreach (var f in new DirectoryInfo(dir).EnumerateFiles())
            {
                var rel = Path.GetRelativePath(Root, f.FullName);
                var profile = rel.Split(Path.DirectorySeparatorChar)[0];
                var url = $"https://{Host}/" + string.Join('/', rel.Split(Path.DirectorySeparatorChar).Select(Uri.EscapeDataString)) + $"?v={f.LastWriteTimeUtc.Ticks}";
                list.Add(new Photo(rel, f.Name, profile, f.Length, f.LastWriteTime, url));
            }
        }
        return list.OrderByDescending(p => p.Taken).ToList();
    }

    /// <summary>The file for an id from List(), refusing anything outside the profiles folder.</summary>
    public static string PathOf(string id)
    {
        var full = Path.GetFullPath(Path.Combine(Root, id));
        if (!full.StartsWith(Path.GetFullPath(Root) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) || !File.Exists(full))
            throw new FileNotFoundException("That photo is gone.");
        return full;
    }

    public static void Copy(string id)
    {
        using var image = Image.FromStream(new MemoryStream(File.ReadAllBytes(PathOf(id))));
        Clipboard.SetImage(image);
    }

    public static bool SaveAs(string id, IWin32Window owner)
    {
        var source = PathOf(id);
        using var dialog = new SaveFileDialog
        {
            Title = "Save photo",
            FileName = Path.GetFileName(source) + ".jpg",
            Filter = "JPEG image|*.jpg|PNG image|*.png",
            InitialDirectory = Environment.GetFolderPath(Environment.SpecialFolder.MyPictures),
        };
        if (dialog.ShowDialog(owner) != DialogResult.OK) return false;
        if (dialog.FilterIndex == 2)
        {
            using var image = Image.FromStream(new MemoryStream(File.ReadAllBytes(source)));
            image.Save(dialog.FileName, System.Drawing.Imaging.ImageFormat.Png);
        }
        else
            File.Copy(source, dialog.FileName, true);
        return true;
    }

    public static void Reveal(string? id)
    {
        if (id is not null)
            System.Diagnostics.Process.Start("explorer.exe", $"/select,\"{PathOf(id)}\"");
        else
            System.Diagnostics.Process.Start("explorer.exe", $"\"{Directory.CreateDirectory(Root).FullName}\"");
    }
}
