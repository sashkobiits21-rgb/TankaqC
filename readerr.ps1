Select-String -Path C:\Tankaq\build_log.txt -Pattern 'error' |
    Select-Object -First 5 | ForEach-Object { $_.Line.Trim().Substring(0, [Math]::Min(160, $_.Line.Trim().Length)) }
Write-Output 'CHECKDONE'
