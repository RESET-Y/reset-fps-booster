# Offene Punkte

Aus den Besprechungen vom 2. und 6. Oktober 2026. Erledigtes einfach löschen.

## Admin-Rechte für FrameBoost (entschieden: Variante B)

**Problem:** Die App startet ohne Admin (`app.manifest`: `asInvoker`), und die
Engine erbt diese Rechte (`FrameBoostBetaService.cs`, `UseShellExecute = false`).
Ohne Admin lehnt Windows die GPU-Klasse REALTIME ab, und `main.cpp` fällt auf
HIGH zurück - laut Messung die schlechteste Stufe (Pipeline 31 ms, Spitzen bis
89 ms, gegenüber 14,4 ms bei NORMAL und 10,5 ms bei REALTIME, Apex).

**Entschieden: B** - nur die FrameBoost-Engine wird mit Admin-Rechten gestartet
(UAC beim Start von FrameBoost/Smooth Motion), die App selbst bleibt normal.
Ist die App schon als Admin gestartet, kein zusätzliches UAC-Fenster.

- [ ] Engine per `Verb = "runas"` starten, wenn die App kein Admin ist
      (`UseShellExecute = true`; Prozess-Handle zum Beenden prüfen)
- [ ] UAC abgelehnt: FrameBoost trotzdem starten (dann NORMAL), mit Hinweis
- [ ] Fallback in `native/FrameBoostV2/src/main.cpp`: REALTIME abgelehnt ->
      **NORMAL statt HIGH**; HIGH nur noch über `gpuclass.txt`
- [ ] Kommentar "REALTIME needs admin rights (RFB has them)" korrigieren
- [ ] Später: Code-Signing-Zertifikat, damit UAC nicht "Unbekannter Herausgeber" zeigt

## REALTIME-Test

`%LOCALAPPDATA%\ResetFpsBooster\gpuclass.txt` = `realtime` / `normal` / `high`,
nach jeder Änderung neu starten. Log: `%LOCALAPPDATA%\ResetFpsBooster\Logs\framebooost_beta.log`.

- [ ] Apex (~100 FPS) und ein schweres Spiel (50-60 FPS)
- [ ] FrameBoost und Smooth Motion
- [ ] je 60 s gleiche Szene, Reihenfolge einmal umdrehen
- [ ] notieren: Klasse laut Log, Pipeline-Latenz, On-screen age, Capture-FPS vs.
      Spiel-FPS, Ausgabe-FPS, p95/p99 Bildabstand, ME-Zeit, Spiel-FPS (1 % low),
      Gefühl 1-5, Desktop/Maus-Ruckler

## Latenz FrameBoost: vereinbarter Plan (NOCH NICHT UMGESETZT, wartet auf Freigabe)

Besprochen am 6. Oktober 2026, ausdrücklich "noch nichts ändern" bis Lukas die
Umsetzung freigibt. Die Engine ist C++/WinRT und lässt sich nur auf dem
Windows-Entwickler-PC bauen und testen, nicht in der Cloud-Sitzung.

**Wichtig vorab**
- *Baseline:* Die 10,5 ms (Apex, ~100 FPS, REALTIME) sind sehr wahrscheinlich
  OHNE Hold gemessen: Pipeline = Bildalter 4,3 ms + Capture-Latenz ~6,2 ms
  (`main.cpp`: `shownAt - arrivalMs + captureLatencyMs`), und ein Hold von ~5 ms
  würde allein das Bildalter über 4,3 ms heben. Der Hold senkt die 10,5 ms also
  nicht, er würde sie im Standardmodus ERHÖHEN. Gewinn von Schritt 1 ist die
  Erkenntnis, wie wenig Hold das Pacing wirklich braucht.
- *Ziel:* "Pipeline < 8 ms" ist nur erreichbar, wenn Capture ≲ 4,7 ms UND der
  Hold ~0 ist, denn Bewegungsschätzung + Zwischenbild kosten schon ~2,3 ms.
  Besser als Ziel: **Bildalter (Engine-Anteil) ≤ 3 ms im p95** plus ein
  Pacing-Kriterium. Den Capture-Anteil (4,7-6,2 ms) bestimmt Windows.
- *Log-Vertrag:* Die Zeile mit `Native FPS:` wird von der App gelesen
  (`telemetry.h`, elf Felder). Sie NICHT ändern. Neue Werte kommen in eine
  eigene zusätzliche Log-Zeile, z. B. `[FrameBoostV2][tuning] ...`.
- Der Kommentar in `main.cpp` "THE HALF-INTERVAL HOLD IS OFF BY DEFAULT" ist
  veraltet: im Code ist der Hold an, außer bei `lowlatency` oder `nohold`.

**Reihenfolge**
1. **Hold konfigurierbar** (`holdRequestedMs = cadenceMs * 0.5` in `main.cpp`):
   Anteil als Startargument, z. B. `hold 0.5`, getestet mit 0.5 / 0.35 / 0.25 /
   0.2 / 0. `lowlatency` und `nohold` bleiben wirksam. Pro Modus loggen:
   Pipeline-Latenz (Mittel und p95), Present-Abstand (min/p50/p95/p99, gibt es
   schon), Dropped/s (gibt es schon), neu: Abstand Zwischenbild → echtes Bild
   und echtes Bild → nächstes Zwischenbild (p5/p50/p95), Anteil der Abstände
   < 2 ms. Dazu `mark` nutzen: beide Spalten (links echt, rechts berechnet)
   müssen sichtbar sein. Test: gleiche Szene, je 60 s, Reihenfolge umdrehen.
   Hintergrund (`main.cpp` ~Zeile 1115): ohne Hold war die kürzeste Lücke im
   Mittel 1,53 ms (min 0,28), mit Hold 0,88 ms (0,25): der Hold verhindert das
   "Begraben" eines Zwischenbilds durch Tearing nicht, er hilft nur gegen
   lange Lücken.
2. **`Sleep(1)` durch Ereignis ersetzen** (`main.cpp`, Schleife bei "Nothing
   new"): `Capture` bekommt ein Auto-Reset-Event, gesetzt direkt nach `++m_head`
   in `ProcessArrival`; die Hauptschleife wartet mit `WaitForSingleObject(evt, 1)`
   (Timeout bleibt 1 ms, damit `PumpMessages` und WM_QUIT weiter laufen).
   Handle in `Stop()`/Destruktor schließen. Synthetic-Modus behält `Sleep(1)`.
   Erwarteter Gewinn ~0,3-1 ms.
3. **Skip-to-newest nur als Notbremse** (`SkipToNewest()` gibt es schon, bisher
   nur für Smooth Motion allein): im FrameBoost-Modus erst ab >= 3 wartenden
   Bildern, nicht ständig. Vorher loggen, wie oft `QueueDepth() > 1` überhaupt
   passiert. Zähler: `queueDepthMax`, `skipEvents`, `framesSkipped`,
   `ageBeforeSkip`, `ageAfterSkip`. Nach einem Skip ist das Paar größer (ein
   Intervall mehr), `pairIntervalMs < 200` fängt das ab; der Schätzer wird
   weiter der Reihe nach gefüttert.
4. **Zwischenbild direkt in den Swapchain-Puffer** - ZURÜCKGESTELLT. Erst messen,
   ob die Kopie (`copyMs`, GPU-Zeit) überhaupt ins Gewicht fällt. Hürden:
   typed-UAV-Store auf BGRA8 ist nicht garantiert, der Swapchain hat 3 rotierende
   Puffer (UAV pro Puffer, nach Resize neu), und der Interpolator liegt in
   `FrameBoostCore` und wird auch vom Legacy-Code benutzt. Das echte Bild wird
   ohnehin kopiert, gespart würde nur die Kopie des Zwischenbilds.

**Weitere Ideen (ungeordnet)**
- [ ] Klick-bis-Bild messen (Handy-Zeitlupe 240 FPS) + PresentMon (Independent Flip?)
- [ ] In der App Reflex/Anti-Lag + FPS-Limit empfehlen
- [ ] Je nach Messung: Independent Flip / MPO oder Extrapolations-Modus

## Smooth Motion

- [ ] Doppelte Bilder (Fingerprint) nicht scharf zeigen - letztes Bild stehen lassen
- [ ] G-Sync "Fenster- und Vollbildmodus" testen
- [ ] Unschärfe in halber Auflösung ausprobieren
- [ ] Messen: Spiel-FPS + 1 % low, p95/p99, Duplikate/s - an gegen aus

## Website / Rechtliches

- [ ] Demo-Video: einmal mit FrameBoost 120 FPS aufnehmen (SuperTuxKart),
      Aus-Version = jedes zweite Bild entfernt, Zeitlupe-Knopf für 60-Hz-Bildschirme
- [ ] AGB: Smooth Motion und Fadenkreuz als Teil von Premium nennen, Beta-Hinweis
