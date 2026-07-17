Set-Location C:\Tankaq\bin\Release
# host: advertises, grants stealth to both players at match start, parks them
$hostP = Start-Process -PassThru .\TankaqClient.exe -ArgumentList `
    '--host','--demo=stealth','--autoready','--winsize=1280x720','--winpos=0x0'
Start-Sleep -Seconds 8
# client: self-join loopback, ready up, screenshot its OWN view of the host
$cliP = Start-Process -Wait -PassThru .\TankaqClient.exe -ArgumentList `
    '--join=76561198789675339','--autoready','--frames=900', `
    '--winsize=1280x720','--winpos=100x100', `
    '--screenshot=C:\Tankaq\bin\Release\client_view.png'
Start-Sleep -Seconds 1
Stop-Process -Id $hostP.Id -Force -ErrorAction SilentlyContinue
Write-Output 'LOOPDONE'
