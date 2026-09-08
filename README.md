# RESET FPS BOOSTER

Ein Windows-Optimierungstool für PC-Gaming, gebaut mit WPF (.NET 8, MVVM). Der Fokus liegt auf **echten, überprüfbaren Optimierungen** — keine Placebo-Tweaks, keine erfundenen FPS-Werte.

## Features

- **Hardware-Erkennung** — CPU, GPU, RAM und Storage werden real über WMI ausgelesen
- **Optimizer** — kategorisierte Module (Gaming, Windows, CPU, GPU, RAM, Storage, Netzwerk) mit Risikostufen (Low/Medium/Experimental), inkl. Game Mode, Game DVR, Energieplan, HAGS, GPU-Task-Priorität, Netzwerk-Drosselung, Temp-Datei-Bereinigung und RAM Cleaner
- **Backup & Restore** — jede Registry-Änderung wird vor dem Schreiben gesichert und ist jederzeit rückgängig machbar
- **Änderungsprotokoll** — vollständiger, nachvollziehbarer Log aller angewendeten Optimierungen
- **Spiele-Profile** — automatische Erkennung von Steam- und Epic-Bibliotheken mit spielspezifischen Optimierungen
- **Performance-Monitor** — Live-Anzeige von CPU/GPU/RAM-Auslastung und GPU-Temperatur
- **Admin-Handling** — erkennt fehlende Rechte und bietet einen sauberen UAC-Elevation-Weg an, statt abzustürzen
- **Auto-Updater** — prüft GitHub Releases auf neue Versionen und installiert sie auf Wunsch automatisch

## Voraussetzungen

- Windows 10/11 (x64)
- [.NET 8 SDK](https://dotnet.microsoft.com/download/dotnet/8.0) zum Bauen
- [Inno Setup 6](https://jrsoftware.org/isinfo.php) zum Bauen des Installers (optional)

## Build

```bash
dotnet build ResetFpsBooster.sln
```

### Veröffentlichbare Standalone-EXE erzeugen

```bash
dotnet publish src/ResetFpsBooster/ResetFpsBooster.csproj -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -o publish/ResetFpsBooster
```

### Installer bauen

Nach dem Publish-Schritt mit Inno Setup 6:

```bash
iscc installer/ResetFpsBooster.iss
```

Das fertige Setup landet in `dist/`.

## Projektstruktur

```
src/ResetFpsBooster/
├── Core/            # Modelle, Utilities, Composition Root (AppServices)
├── Optimization/     # IOptimizationModule und alle Optimierungsmodule
├── Services/          # Hardware-, Spiele-, Backup-, Update-Services etc.
├── ViewModels/        # MVVM-ViewModels je View
├── Views/             # WPF-Views (XAML)
└── Controls/          # Custom Controls (z.B. RadialGauge)
```

## Lizenz

Kein öffentliches Lizenzmodell definiert — alle Rechte vorbehalten.

---

Made by Lukas Reschke
