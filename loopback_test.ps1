Set-Location C:\Tankaq\bin\Release
$hostP = Start-Process -PassThru .\TankaqClient.exe -ArgumentList `
    '--host','--testmode','--demo=stealth','--autoready','--winsize=1280x720','--winpos=0x0'
Start-Sleep -Seconds 8
$cliP = Start-Process -Wait -PassThru .\TankaqClient.exe -ArgumentList `
    '--join=76561198789675339','--autoready','--frames=900', `
    '--winsize=1280x720','--winpos=100x100', `
    '--screenshot=C:\Tankaq\bin\Release\client_view.png'
Start-Sleep -Seconds 1
Stop-Process -Id $hostP.Id -Force -ErrorAction SilentlyContinue
Write-Output 'LOOPDONE'
