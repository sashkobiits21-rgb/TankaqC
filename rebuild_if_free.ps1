$p = Get-Process TankaqClient -ErrorAction SilentlyContinue
if ($p) {
    Write-Output 'STILL_RUNNING'
} else {
    Start-Process powershell -ArgumentList '-ExecutionPolicy','Bypass','-File','C:\Tankaq\build_only.ps1' -WindowStyle Hidden
    Write-Output 'REBUILDING'
}
