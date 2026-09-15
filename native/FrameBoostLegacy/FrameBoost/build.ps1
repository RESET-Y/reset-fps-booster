$ErrorActionPreference = 'Stop'
$vsDevCmd = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat'
$cmake = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ninjaDir = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
$root = 'C:\Users\Eto jA\FPS Booster\native\FrameBoost'

cmd.exe /c "set PATH=$ninjaDir;%PATH% && `"$vsDevCmd`" -arch=x64 && `"$cmake`" -B `"$root\build`" -S `"$root`" -G Ninja -DCMAKE_BUILD_TYPE=Release && `"$cmake`" --build `"$root\build`" --config Release"
