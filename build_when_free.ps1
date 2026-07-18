# Wait (up to 30 min) for the running game to close, then build + classtest.
$deadline = (Get-Date).AddMinutes(30)
while ((Get-Date) -lt $deadline) {
    if (-not (Get-Process TankaqClient -ErrorAction SilentlyContinue)) { break }
    Start-Sleep -Seconds 5
}
& powershell -ExecutionPolicy Bypass -File C:\Tankaq\build_only.ps1
