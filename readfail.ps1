$f = Get-ChildItem C:\Tankaq\bin\Release\tankaq_log_*.txt |
     Sort-Object LastWriteTime -Descending | Select-Object -First 1
Select-String -Path $f.FullName -Pattern 'FAIL|diag|done|s-trace' |
    ForEach-Object Line | Select-Object -Last 22
