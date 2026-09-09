namespace ResetFpsBooster.BottleneckEngine;

/// <summary>
/// Converts an aggregated signal weight (multiple analyzers can contribute to the same kind) into
/// a confidence percentage. Deliberately never reaches 100% — this is a real-time inference from
/// live telemetry, not a certainty, and saying so honestly is the whole point of this engine.
/// </summary>
public static class ConfidenceEngine
{
    private const double MaxConfidencePercent = 97;

    public static double ToConfidencePercent(double aggregatedWeight)
    {
        // Multiple corroborating signals push weight above 1.0 — a soft saturation curve rewards
        // that agreement without ever quite reaching full certainty.
        var normalized = 1 - Math.Exp(-aggregatedWeight * 1.35);
        return Math.Round(Math.Min(MaxConfidencePercent, normalized * 100), 0);
    }
}
