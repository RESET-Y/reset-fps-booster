# Offene Punkte

Aus der Besprechung vom 2. Oktober 2026. Erledigtes einfach löschen.

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

## Latenz FrameBoost

- [ ] Klick-bis-Bild messen (Handy-Zeitlupe 240 FPS) + PresentMon (Independent Flip?)
- [ ] Event aus `FrameArrived` statt `Sleep(1)`-Abfrage
- [ ] Zwischenbild direkt in den Backbuffer, eine volle Kopie sparen
- [ ] In der App Reflex/Anti-Lag + FPS-Limit empfehlen
- [ ] Je nach Messung: Independent Flip / MPO oder Extrapolations-Modus
- [ ] Hold als Regler statt an/aus

## Smooth Motion

- [ ] Doppelte Bilder (Fingerprint) nicht scharf zeigen - letztes Bild stehen lassen
- [ ] G-Sync "Fenster- und Vollbildmodus" testen
- [ ] Unschärfe in halber Auflösung ausprobieren
- [ ] Messen: Spiel-FPS + 1 % low, p95/p99, Duplikate/s - an gegen aus

## Website / Rechtliches

- [ ] Demo-Video: einmal mit FrameBoost 120 FPS aufnehmen (SuperTuxKart),
      Aus-Version = jedes zweite Bild entfernt, Zeitlupe-Knopf für 60-Hz-Bildschirme
- [ ] AGB: Smooth Motion und Fadenkreuz als Teil von Premium nennen, Beta-Hinweis
