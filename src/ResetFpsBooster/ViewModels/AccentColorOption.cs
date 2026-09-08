using System.Windows.Media;

namespace ResetFpsBooster.ViewModels;

public sealed class AccentColorOption
{
    public string Name { get; }
    public string Hex { get; }
    public Brush Swatch { get; }

    public AccentColorOption(string name, string hex)
    {
        Name = name;
        Hex = hex;
        Swatch = new SolidColorBrush((Color)ColorConverter.ConvertFromString(hex));
        Swatch.Freeze();
    }
}
