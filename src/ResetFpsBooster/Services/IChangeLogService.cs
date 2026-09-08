using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.Services;

public interface IChangeLogService
{
    IReadOnlyList<ChangeLogEntry> GetEntries();
    void Append(IEnumerable<ChangeLogEntry> entries);
    void Clear();
}
