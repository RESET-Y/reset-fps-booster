$vsDevCmd = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat'
$dll = 'C:\Users\Eto jA\FPS Booster\native\FrameBoost\build\bin\d3d11.dll'
cmd.exe /c "`"$vsDevCmd`" -arch=x64 >nul && dumpbin /exports `"$dll`""
