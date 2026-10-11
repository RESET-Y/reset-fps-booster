namespace ResetFpsBooster.Core.Sensitivity;

public enum SensConfidence { Low, Medium, High }

/// <summary>What the test recommends, and how far to trust it.</summary>
public sealed class SensRecommendation
{
    /// <summary>The best sensitivity, relative to the player's own (1.0 = keep it, 1.2 = 20 % faster).</summary>
    public double Multiplier { get; init; }
    /// <summary>The stretch around it that tested about as well: anything in here is fine.</summary>
    public double LowMultiplier { get; init; }
    public double HighMultiplier { get; init; }
    public SensConfidence Confidence { get; init; }
    /// <summary>The best value sits at the edge of what was tested: the real optimum may lie beyond it.</summary>
    public bool AtEdge { get; init; }
    /// <summary>The rounds did not differ enough to prefer any sensitivity: keep the current one.</summary>
    public bool NoClearDifference { get; init; }
    /// <summary>How well one smooth curve explains the rounds, 0..1.</summary>
    public double RSquared { get; init; }
    /// <summary>The composite score of each round (a z-score: 0 is average), for the table.</summary>
    public IReadOnlyList<(double Multiplier, double Score)> Points { get; init; } = Array.Empty<(double, double)>();
}

/// <summary>
/// FROM THE ROUNDS TO ONE RECOMMENDATION.
///
/// There is no "perfect" sensitivity to read off; there is a curve. Too slow
/// and a flick runs out of mouse and arrives late; too fast and it overshoots
/// and the hand fights the target. In between sits a peak. This takes each
/// round's results, turns them into one score, lays a smooth curve over the
/// scores of all sensitivities tried, and returns the top of that curve -
/// together with the range that is nearly as good, and an honest word on how
/// much the scatter of a short test allows it to claim.
/// </summary>
public static class SensAnalyzer
{
    private const double FlickWeight = 0.65;
    private const double TrackWeight = 0.35;

    /// <summary>Needs at least four scored rounds to say anything about a curve.</summary>
    public static SensRecommendation? Analyze(IReadOnlyList<AimRoundResult> rounds)
    {
        var scored = rounds.Where(r => r.Scored).OrderBy(r => r.Multiplier).ToList();
        if (scored.Count < 4) return null;

        // 0. If the rounds hardly differ, there is nothing to find. A z-score would blow a few
        //    percent of noise up to a full scale, so this is judged on the raw numbers first.
        var rawFlick = scored.Select(r => r.HitsPerSecond * r.Accuracy).ToArray();
        var rawTrack = scored.Select(r => r.TrackFraction).ToArray();
        var meanFlick = rawFlick.Average();
        var flickSpread = meanFlick <= 1e-9 ? 0 : (rawFlick.Max() - rawFlick.Min()) / meanFlick;
        if (flickSpread < 0.08 && rawTrack.Max() - rawTrack.Min() < 0.05)
        {
            return new SensRecommendation
            {
                Multiplier = 1.0, LowMultiplier = 0.85, HighMultiplier = 1.18,
                Confidence = SensConfidence.Low, NoClearDifference = true,
                Points = scored.Select(r => (r.Multiplier, 0.0)).ToList()
            };
        }

        // 1. One number per round. Flick: hits per second, times accuracy (fast AND clean).
        //    Tracking: the share of time on the target. Each is made comparable by a z-score
        //    over the rounds, so the sum does not depend on how hard this player's day is.
        var flick = ZScores(scored.Select(r => r.HitsPerSecond * r.Accuracy).ToArray());
        var track = ZScores(scored.Select(r => r.TrackFraction).ToArray());
        var score = new double[scored.Count];
        var x = new double[scored.Count];
        for (var i = 0; i < scored.Count; i++)
        {
            score[i] = FlickWeight * flick[i] + TrackWeight * track[i];
            x[i] = Math.Log(scored[i].Multiplier);
        }

        // 2. A parabola over the log of the sensitivity (a ratio of 1.5 up is as far as 1.5 down).
        var (a, b, c) = FitQuadratic(x, score);
        var xMin = x.Min();
        var xMax = x.Max();
        var bestObserved = Array.IndexOf(score, score.Max());

        double xStar;
        if (a < -1e-6) xStar = -b / (2 * a);       // a real peak
        else xStar = x[bestObserved];              // no peak (rising or falling): the best one tried

        // 3. Never claim beyond what was tried (a little margin, no more).
        const double margin = 0.2;
        var clamped = Math.Clamp(xStar, xMin - margin, xMax + margin);
        var atEdge = a >= -1e-6 && (bestObserved == 0 || bestObserved == scored.Count - 1)
                     || Math.Abs(clamped - xMin) < 0.1 || Math.Abs(clamped - xMax) < 0.1;
        xStar = clamped;

        // 4. How well the curve fits, and the stretch within one scatter of the top.
        var fit = x.Select(v => a * v * v + b * v + c).ToArray();
        var ssRes = score.Zip(fit, (s, f) => (s - f) * (s - f)).Sum();
        var mean = score.Average();
        var ssTot = score.Sum(s => (s - mean) * (s - mean));
        var r2 = ssTot < 1e-9 ? 0 : Math.Clamp(1 - ssRes / ssTot, 0, 1);
        var sigma = Math.Max(Math.Sqrt(ssRes / Math.Max(1, scored.Count - 3)), 0.15);

        double halfWidth;
        if (a < -1e-6) halfWidth = Math.Sqrt(sigma / -a);
        else halfWidth = (xMax - xMin) / 2;
        // A band wider than about a third either way says nothing; the confidence label
        // already carries "the rounds scatter a lot".
        halfWidth = Math.Clamp(halfWidth, 0.08, 0.33);

        var confidence =
            a < -1e-6 && !atEdge && r2 >= 0.65 && scored.Count >= 6 ? SensConfidence.High :
            a < -1e-6 && r2 >= 0.35 ? SensConfidence.Medium :
            SensConfidence.Low;

        return new SensRecommendation
        {
            Multiplier = Math.Exp(xStar),
            LowMultiplier = Math.Exp(xStar - halfWidth),
            HighMultiplier = Math.Exp(xStar + halfWidth),
            Confidence = confidence,
            AtEdge = atEdge,
            RSquared = r2,
            Points = scored.Select((r, i) => (r.Multiplier, score[i])).ToList()
        };
    }

    private static double[] ZScores(double[] values)
    {
        var mean = values.Average();
        var sd = Math.Sqrt(values.Sum(v => (v - mean) * (v - mean)) / values.Length);
        return values.Select(v => sd < 1e-9 ? 0.0 : (v - mean) / sd).ToArray();
    }

    /// <summary>Least squares y = a x^2 + b x + c, by the normal equations.</summary>
    private static (double A, double B, double C) FitQuadratic(double[] x, double[] y)
    {
        double s0 = x.Length, s1 = 0, s2 = 0, s3 = 0, s4 = 0, t0 = 0, t1 = 0, t2 = 0;
        for (var i = 0; i < x.Length; i++)
        {
            var xi = x[i]; var x2 = xi * xi;
            s1 += xi; s2 += x2; s3 += x2 * xi; s4 += x2 * x2;
            t0 += y[i]; t1 += y[i] * xi; t2 += y[i] * x2;
        }
        // | s4 s3 s2 | |a|   |t2|
        // | s3 s2 s1 | |b| = |t1|
        // | s2 s1 s0 | |c|   |t0|
        var det = s4 * (s2 * s0 - s1 * s1) - s3 * (s3 * s0 - s1 * s2) + s2 * (s3 * s1 - s2 * s2);
        if (Math.Abs(det) < 1e-12) return (0, 0, t0 / s0);
        var da = t2 * (s2 * s0 - s1 * s1) - s3 * (t1 * s0 - s1 * t0) + s2 * (t1 * s1 - s2 * t0);
        var db = s4 * (t1 * s0 - s1 * t0) - t2 * (s3 * s0 - s1 * s2) + s2 * (s3 * t0 - t1 * s2);
        var dc = s4 * (s2 * t0 - t1 * s1) - s3 * (s3 * t0 - t1 * s2) + t2 * (s3 * s1 - s2 * s2);
        return (da / det, db / det, dc / det);
    }
}
