namespace ResetFpsBooster.Core.Models;

public sealed class SystemInfoRow
{
    public string Label { get; set; } = string.Empty;
    public string Value { get; set; } = string.Empty;
}

public sealed class OptimizationCategorySummary
{
    public string Name { get; set; } = string.Empty;
    public string Subtitle { get; set; } = string.Empty;
    public int Count { get; set; }
    public string Glyph { get; set; } = string.Empty;
}
