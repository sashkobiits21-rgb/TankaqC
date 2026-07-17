Get-Content C:\Tankaq\build_log.txt -Tail 1
$f = Get-ChildItem C:\Tankaq\bin\Release\tankaq_log_*.txt |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
Select-String -Path $f.FullName -Pattern 'FAIL', 'classtest done' |
    ForEach-Object { $_.Line }
Write-Output 'CHECKDONE'
