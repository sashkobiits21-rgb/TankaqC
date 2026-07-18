Start-Sleep -Seconds 12
$logs = Get-ChildItem C:\Tankaq\bin\Release\tankaq_log_*.txt |
    Sort-Object LastWriteTime -Descending | Select-Object -First 2
foreach ($f in $logs) {
    Write-Output ('== ' + $f.Name)
    Select-String -Path $f.FullName -Pattern 'cmdline','match started','granted','stealth: clip','Upgrade event' -ErrorAction SilentlyContinue |
        Select-Object -First 8 | ForEach-Object { $_.Line }
}
Copy-Item C:\Tankaq\bin\Release\host_view.png C:\Tankaq\bin\Release\hv6.png -Force -ErrorAction SilentlyContinue
Copy-Item C:\Tankaq\bin\Release\client_view.png C:\Tankaq\bin\Release\cv7.png -Force -ErrorAction SilentlyContinue
Write-Output 'CHECKDONE'
