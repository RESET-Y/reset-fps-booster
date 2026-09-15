# FrameBoost Legacy — eingefroren

Diese beiden Bäume sind der Stand vor der V2-Engine. Sie werden **nicht mehr
gebaut und nicht mehr ausgeliefert**, bleiben aber vollständig erhalten.

## FrameBoost/

Die ursprüngliche Engine: baut `d3d11.dll` und `dxgi.dll` als **Proxy-DLLs**,
also DLL-Injection in Spiele. Das widerspricht der Projektregel "keine
DLL-Injection" und ist der Grund, warum dieser Pfad nie ausgeliefert wurde.

Der bewährte GPU-Teil daraus — Bewegungsschätzung, Interpolation und die
sieben HLSL-Shader — wurde nach `native/FrameBoostCore/` **kopiert**, nicht
verschoben. Beide Bäume sind dadurch unabhängig: eine Änderung hier hat keine
Wirkung auf V2, und umgekehrt.

## FrameBoostBeta/

Der Host um diesen GPU-Kern: eine 5.244 Zeilen lange serielle Schleife mit
fünfzehn Schaltern. Sie liefert bei einer Quelle von 72 fps gemessen 143–144
fps aus, und versagt reproduzierbar, sobald das Doppelte der Quelle die
Bildwiederholrate nicht exakt teilt.

Die Messungen, die zu V2 geführt haben, stehen in den Commit-Nachrichten
dieses Repositories — jede mit ihren Zahlen.

## Wieder bauen

Beide Bäume enthalten ihre eigene `CMakeLists.txt` und sind self-contained.
Die mitverschobenen `build/`-Ordner tragen absolute Pfade von vor dem
Verschieben; für einen Neubau muss CMake dort neu konfiguriert werden.
