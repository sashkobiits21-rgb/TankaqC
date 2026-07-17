& 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build C:\Tankaq\build --config Release 2>&1 |
    Out-File -FilePath C:\Tankaq\build_log.txt -Encoding utf8
Add-Content -Path C:\Tankaq\build_log.txt -Value 'BUILDDONE'
Set-Location C:\Tankaq\bin\Release
$p = Start-Process -Wait -PassThru .\TankaqClient.exe -ArgumentList '--classtest'
Add-Content -Path C:\Tankaq\build_log.txt -Value ("CLASSTEST_EXIT=" + $p.ExitCode)
