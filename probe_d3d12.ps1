# Boot the game briefly to capture D3D12 init logs, then kill it.
& powershell -ExecutionPolicy Bypass -File C:\Tankaq\build_only.ps1
cd C:\Tankaq\bin\Release
$p = Start-Process .\TankaqClient.exe -PassThru
Start-Sleep -Seconds 7
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1
$f = Get-ChildItem C:\Tankaq\bin\Release\tankaq_log_*.txt |
     Sort-Object LastWriteTime -Descending | Select-Object -First 1
"--- $($f.Name)"
Select-String -Path $f.FullName -Pattern 'D3D12|D3D11|Renderer|root|failed|hr=' |
    ForEach-Object Line | Select-Object -First 20
