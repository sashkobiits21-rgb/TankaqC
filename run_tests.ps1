cd C:\Tankaq
& 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build build --config Release 2>&1 | Select-String -Pattern 'error' | Select-Object -First 8
cd bin\Release
$p = Start-Process -Wait -PassThru .\TankaqClient.exe -ArgumentList '--classtest'
Write-Output ("CLASSTEST_EXIT=" + $p.ExitCode)
Write-Output "ALLDONE"
