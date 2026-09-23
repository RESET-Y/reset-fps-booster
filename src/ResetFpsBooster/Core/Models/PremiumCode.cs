using System.Text.Json.Serialization;

namespace ResetFpsBooster.Core.Models;

/// One code as a manager sees it in the list. Only ever filled from the
/// server's list_premium_codes(), which refuses anyone who is not a manager.
public sealed class PremiumCode
{
    [JsonPropertyName("code")]          public string Code { get; set; } = "";
    [JsonPropertyName("created_at")]    public DateTimeOffset CreatedAt { get; set; }
    [JsonPropertyName("duration_days")] public int? DurationDays { get; set; }
    [JsonPropertyName("max_uses")]      public int MaxUses { get; set; }
    [JsonPropertyName("uses")]          public int Uses { get; set; }
    [JsonPropertyName("active")]        public bool Active { get; set; }
    [JsonPropertyName("note")]          public string? Note { get; set; }
}
