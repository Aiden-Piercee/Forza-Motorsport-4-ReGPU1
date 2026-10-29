namespace FM4Launcher;

/// <summary>Photo Mode pictures: JPEGs anywhere in a profile's saves, Documents\fm4\profiles\&lt;profile&gt;\content. FM2
/// kept them (without an extension) in SavedPhotos\photos; where FM4's save package puts them isn't pinned down yet,
/// so they're found by their bytes. The page shows them from the launcher's host (Web.PhotoUrl).</summary>
internal static class Photos
{
    public static readonly string Root = Path.Combine(Paths.User, "profiles");

    public record Photo(string Id, string Name, string Profile, long Bytes, DateTime Taken, string Url);

    public static List<Photo> List()
    {
        var list = new List<Photo>();
        if (!Directory.Exists(Root)) return list;
        foreach (var profileDir in new DirectoryInfo(Root).EnumerateDirectories())
        {
            var content = Path.Combine(profileDir.FullName, "content");
            if (!Directory.Exists(content)) continue;
            foreach (var f in new DirectoryInfo(content).EnumerateFiles("*", SearchOption.AllDirectories).Where(IsPhoto))
            {
                var rel = Path.GetRelativePath(Root, f.FullName);
                var profile = rel.Split(Path.DirectorySeparatorChar)[0];
                list.Add(new Photo(rel, f.Name, profile, f.Length, f.LastWriteTime, Web.PhotoUrl(rel, f.LastWriteTimeUtc)));
            }
        }
        return list.OrderByDescending(p => p.Taken).ToList();
    }

    /// <summary>A JPEG (starts FF D8 FF), not a package's .title or .license.</summary>
    public static bool IsPhoto(FileInfo f)
    {
        if (f.Length < 1024 || f.Extension is ".title" or ".license") return false;
        try
        {
            using var s = f.OpenRead();
            Span<byte> head = stackalloc byte[3];
            return s.Read(head) == 3 && head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF;
        }
        catch { return false; }
    }

    /// <summary>The file for an id from List(), refusing anything outside the profiles folder.</summary>
    public static string PathOf(string id)
    {
        var full = Path.GetFullPath(Path.Combine(Root, id));
        if (!full.StartsWith(Path.GetFullPath(Root) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) || !File.Exists(full))
            throw new FileNotFoundException("That photo is gone.");
        return full;
    }

    /// <summary>Where Save as starts: Pictures (on Linux, if the desktop has none, home).</summary>
    public static string SaveFolder
    {
        get
        {
            var pictures = Environment.GetFolderPath(Environment.SpecialFolder.MyPictures);
            return Directory.Exists(pictures) ? pictures : Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        }
    }

    /// <summary>Save as: the JPEG as it is, or the PNG the page drew from it.</summary>
    public static void Write(string id, string file, byte[]? png)
    {
        var source = PathOf(id);
        if (png is not null) File.WriteAllBytes(file, png);
        else File.Copy(source, file, true);
    }

    public static void Reveal(string? id)
    {
        if (id is not null) Platform.Reveal(PathOf(id));
        else Platform.OpenFolder(Directory.CreateDirectory(Root).FullName);
    }
}
