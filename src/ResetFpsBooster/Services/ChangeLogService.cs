using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Utilities;

namespace ResetFpsBooster.Services;

public sealed class ChangeLogService : IChangeLogService
{
    private readonly List<ChangeLogEntry> _entries;

    public ChangeLogService()
    {
        AppPaths.EnsureFoldersExist();
        _entries = JsonStore.Load(AppPaths.ChangeLogFile, () => new List<ChangeLogEntry>());
    }

    public IReadOnlyList<ChangeLogEntry> GetEntries() =>
        _entries.OrderByDescending(e => e.Timestamp).ToList();

    public void Append(IEnumerable<ChangeLogEntry> entries)
    {
        _entries.AddRange(entries);
        Persist();
    }

    public void Clear()
    {
        _entries.Clear();
        Persist();
    }

    private void Persist() => JsonStore.Save(AppPaths.ChangeLogFile, _entries);
}
