namespace ResetFpsBooster.Core.Sensitivity;

public enum AimPhase { Intro, Flick, Track, Break, Done, Aborted }

/// <summary>What one round of the test measured, at one sensitivity.</summary>
public sealed class AimRoundResult
{
    /// <summary>Sensitivity relative to the player's own (1.0 = unchanged).</summary>
    public double Multiplier { get; init; }
    public double Cm360 { get; init; }
    public bool Scored { get; init; }

    public int Hits { get; set; }
    /// <summary>Clicks beside the target plus targets that ran out of time.</summary>
    public int Misses { get; set; }
    public double FlickSeconds { get; set; }
    public double TotalTimeToHit { get; set; }      // seconds, summed over hits
    public double EfficiencySum { get; set; }       // straight line / path travelled, summed over hits
    public double TrackSeconds { get; set; }
    public double TrackOnTarget { get; set; }       // seconds with the crosshair on the target
    public double TrackErrorSum { get; set; }       // degrees x seconds

    public double Accuracy => Hits + Misses == 0 ? 0 : Hits / (double)(Hits + Misses);
    public double HitsPerSecond => FlickSeconds <= 0 ? 0 : Hits / FlickSeconds;
    public double MeanTimeToHitMs => Hits == 0 ? 0 : TotalTimeToHit / Hits * 1000.0;
    public double PathEfficiency => Hits == 0 ? 0 : EfficiencySum / Hits;
    public double TrackFraction => TrackSeconds <= 0 ? 0 : TrackOnTarget / TrackSeconds;
    public double TrackErrorDegrees => TrackSeconds <= 0 ? 0 : TrackErrorSum / TrackSeconds;
}

/// <summary>
/// THE AIM TEST, WITHOUT A SCREEN.
///
/// A small 3D-like shooting range in angles: the player is a camera that
/// turns by yaw and pitch, targets sit at angular positions. Mouse counts
/// turn the camera exactly as a game would (counts x degrees-per-count, from
/// the DPI and the cm/360 under test), so what is measured is the real thing:
/// how well THIS sensitivity lets this hand hit and follow targets.
///
/// Rounds: one unscored warm-up at the player's own sensitivity, then one
/// scored round per multiplier, each a burst of flick targets (hit one, the
/// next appears elsewhere) and a stretch of tracking a moving target. The
/// session knows nothing about windows or drawing; the test window feeds it
/// mouse counts, clicks and the elapsed time, and draws what it exposes.
/// </summary>
public sealed class AimSession
{
    public const double DefaultHorizontalFov = 103.0;   // degrees, a common shooter value
    public const double FlickRadius = 1.6;          // degrees, 3.2 wide
    public const double TrackRadius = 2.2;
    public const double TrackTolerance = 1.25;      // on target = within this x the radius

    private const int FlickTargets = 10;
    private const int WarmupFlickTargets = 6;
    private const double FlickTimeout = 2.5;
    private const double TrackSeconds = 8.0;
    private const double WarmupTrackSeconds = 4.0;
    private const double IntroSeconds = 3.0;
    private const double BreakSeconds = 2.5;

    private readonly double _dpi;
    private readonly double _centerCm360;
    private readonly List<double> _multipliers;     // [0] = warm-up (1.0), then the scored ones
    private readonly List<AimRoundResult> _results = new();
    private readonly Random _rng;

    private AimRoundResult _round;
    private double _phaseTime;
    private int _targetsDone;
    private double _targetAge;
    private double _pathSinceSpawn;
    private double _spawnDistance;
    private double _trackP1, _trackP2, _trackP3, _trackP4, _trackAz0, _trackEl0;

    /// <param name="horizontalFov">The game's horizontal field of view in degrees, as the player has it set.</param>
    /// <param name="aspect">Width / height of the game's picture (from its resolution).</param>
    public AimSession(double dpi, double centerCm360, IReadOnlyList<double> scoredMultipliers, int seed,
                      double horizontalFov = DefaultHorizontalFov, double aspect = 16.0 / 9.0)
    {
        HorizontalFov = Math.Clamp(horizontalFov, 40, 160);
        Aspect = Math.Clamp(aspect, 1.0, 4.0);
        _dpi = dpi;
        _centerCm360 = centerCm360;
        _rng = new Random(seed);
        _multipliers = new List<double> { 1.0 };
        _multipliers.AddRange(scoredMultipliers);
        _round = NewRound(0);
    }

    /// <summary>How wide the player's view is: this decides how large and how fast the world looks on screen.</summary>
    public double HorizontalFov { get; }
    public double Aspect { get; }

    public AimPhase Phase { get; private set; } = AimPhase.Intro;
    /// <summary>0 = warm-up, 1..ScoredRounds = the scored rounds.</summary>
    public int RoundIndex { get; private set; }
    public int ScoredRounds => _multipliers.Count - 1;
    public IReadOnlyList<AimRoundResult> Results => _results;
    public AimRoundResult CurrentRound => _round;

    public double Yaw { get; private set; }
    public double Pitch { get; private set; }

    public bool HasFlickTarget { get; private set; }
    public double FlickAz { get; private set; }
    public double FlickEl { get; private set; }
    public int TargetsLeft => (RoundIndex == 0 ? WarmupFlickTargets : FlickTargets) - _targetsDone;

    public double PhaseTime => _phaseTime;
    public double PhaseLength => Phase switch
    {
        AimPhase.Intro => IntroSeconds,
        AimPhase.Track => RoundIndex == 0 ? WarmupTrackSeconds : TrackSeconds,
        AimPhase.Break => BreakSeconds,
        _ => 0
    };

    /// <summary>Where the tracked target is now (valid while Phase is Track).</summary>
    public (double Az, double El) TrackTarget => TrackPosition(_phaseTime);

    private AimRoundResult NewRound(int index) => new()
    {
        Multiplier = _multipliers[index],
        Cm360 = _centerCm360 / _multipliers[index],
        Scored = index > 0
    };

    private double DegreesPerCount => SensMath.DegreesPerCount(_dpi, _round.Cm360);

    // ------------------------------------------------------------------ input

    /// <summary>Raw mouse counts since the last call: this turns the view.</summary>
    public void Mouse(int dx, int dy)
    {
        if (Phase is AimPhase.Done or AimPhase.Aborted) return;
        var deg = DegreesPerCount;
        Yaw += dx * deg;
        Pitch = Math.Clamp(Pitch - dy * deg, -85, 85);
        _pathSinceSpawn += Math.Sqrt((double)dx * dx + (double)dy * dy) * deg;
    }

    /// <summary>The fire button went down.</summary>
    public void Click()
    {
        if (Phase != AimPhase.Flick || !HasFlickTarget) return;
        if (AngularDistance(Yaw, Pitch, FlickAz, FlickEl) <= FlickRadius)
        {
            _round.Hits++;
            _round.TotalTimeToHit += _targetAge;
            _round.EfficiencySum += _pathSinceSpawn <= 0 ? 1.0 : Math.Min(1.0, _spawnDistance / _pathSinceSpawn);
            NextFlickTarget();
        }
        else
        {
            _round.Misses++;
        }
    }

    public void Abort() => Phase = AimPhase.Aborted;

    // ------------------------------------------------------------------- time

    public void Update(double dt)
    {
        if (Phase is AimPhase.Done or AimPhase.Aborted) return;
        _phaseTime += dt;

        switch (Phase)
        {
            case AimPhase.Intro:
                if (_phaseTime >= IntroSeconds) StartRound(0);
                break;

            case AimPhase.Flick:
                _round.FlickSeconds += dt;
                _targetAge += dt;
                if (_targetAge >= FlickTimeout)
                {
                    _round.Misses++;
                    NextFlickTarget();
                }
                break;

            case AimPhase.Track:
            {
                var (az, el) = TrackPosition(_phaseTime);
                var dist = AngularDistance(Yaw, Pitch, az, el);
                _round.TrackSeconds += dt;
                if (dist <= TrackRadius * TrackTolerance) _round.TrackOnTarget += dt;
                _round.TrackErrorSum += dist * dt;
                if (_phaseTime >= PhaseLength) EndRound();
                break;
            }

            case AimPhase.Break:
                if (_phaseTime >= BreakSeconds)
                {
                    if (RoundIndex + 1 < _multipliers.Count) StartRound(RoundIndex + 1);
                    else Phase = AimPhase.Done;
                }
                break;
        }
    }

    // ----------------------------------------------------------------- rounds

    private void StartRound(int index)
    {
        RoundIndex = index;
        _round = NewRound(index);
        _targetsDone = 0;
        Phase = AimPhase.Flick;
        _phaseTime = 0;
        SpawnFlickTarget();
    }

    private void EndRound()
    {
        if (_round.Scored) _results.Add(_round);
        Phase = AimPhase.Break;
        _phaseTime = 0;
        HasFlickTarget = false;
    }

    private void NextFlickTarget()
    {
        _targetsDone++;
        if (_targetsDone >= (RoundIndex == 0 ? WarmupFlickTargets : FlickTargets))
        {
            StartTracking();
            return;
        }
        SpawnFlickTarget();
    }

    private void SpawnFlickTarget()
    {
        // Somewhere new, far enough that it takes a real flick, but always inside the picture the
        // player sees (a narrower view or a taller screen gives shorter flicks, as in the game).
        var halfH = HorizontalFov / 2;
        var halfV = Math.Atan(Math.Tan(halfH * Math.PI / 180) / Aspect) * 180 / Math.PI;
        var reach = Math.Min(55, halfH * 0.92);
        var near = Math.Min(20, reach * 0.45);
        var distance = near + _rng.NextDouble() * (reach - near);
        var angle = _rng.NextDouble() * Math.PI * 2;
        var up = Math.Clamp(distance * Math.Sin(angle) * 0.6, -halfV * 0.85, halfV * 0.85);
        FlickAz = Yaw + distance * Math.Cos(angle);
        FlickEl = Math.Clamp(Pitch + up, -40, 40);
        HasFlickTarget = true;
        _targetAge = 0;
        _pathSinceSpawn = 0;
        _spawnDistance = AngularDistance(Yaw, Pitch, FlickAz, FlickEl);
    }

    private void StartTracking()
    {
        Phase = AimPhase.Track;
        _phaseTime = 0;
        HasFlickTarget = false;
        _trackAz0 = Yaw;
        _trackEl0 = Math.Clamp(Pitch, -10, 10);
        _trackP1 = _rng.NextDouble() * Math.PI * 2;
        _trackP2 = _rng.NextDouble() * Math.PI * 2;
        _trackP3 = _rng.NextDouble() * Math.PI * 2;
        _trackP4 = _rng.NextDouble() * Math.PI * 2;
    }

    // The tracked target drifts on two overlapping waves per axis: never still, never predictable,
    // peaking at about 40 degrees per second sideways.
    private (double Az, double El) TrackPosition(double t) => (
        _trackAz0 + 22 * Math.Sin(0.8 * t + _trackP1) + 9 * Math.Sin(2.1 * t + _trackP2),
        _trackEl0 + 8 * Math.Sin(1.3 * t + _trackP3) + 3 * Math.Sin(2.9 * t + _trackP4));

    // ------------------------------------------------------------------ maths

    /// <summary>Angle between two viewing directions, in degrees.</summary>
    public static double AngularDistance(double az1, double el1, double az2, double el2)
    {
        const double rad = Math.PI / 180.0;
        var a1 = az1 * rad; var e1 = el1 * rad; var a2 = az2 * rad; var e2 = el2 * rad;
        var dot = Math.Cos(e1) * Math.Cos(e2) * Math.Cos(a1 - a2) + Math.Sin(e1) * Math.Sin(e2);
        return Math.Acos(Math.Clamp(dot, -1.0, 1.0)) / rad;
    }

    /// <summary>Where a point at (az, el) lands on a screen of this size for a camera at (yaw, pitch).
    /// False when it is behind the camera.</summary>
    public static bool TryProject(double yaw, double pitch, double az, double el, double width, double height,
                                  out double x, out double y, out double pixelsPerDegreeRadius,
                                  double horizontalFov = DefaultHorizontalFov)
    {
        const double rad = Math.PI / 180.0;
        var relAz = (az - yaw) * rad;
        var e = el * rad;
        var vx = Math.Cos(e) * Math.Sin(relAz);
        var vy = Math.Sin(e);
        var vz = Math.Cos(e) * Math.Cos(relAz);
        var p = pitch * rad;
        var y2 = vy * Math.Cos(p) - vz * Math.Sin(p);
        var z2 = vy * Math.Sin(p) + vz * Math.Cos(p);
        var f = (width / 2) / Math.Tan(horizontalFov / 2 * rad);
        pixelsPerDegreeRadius = f * rad;     // good enough for a target near the middle of the view
        if (z2 < 0.05) { x = y = 0; return false; }
        x = width / 2 + f * vx / z2;
        y = height / 2 - f * y2 / z2;
        return true;
    }
}
