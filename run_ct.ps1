cd C:\Tankaq\bin\Release
& .\TankaqClient.exe --classtest *> C:\Tankaq\ct_out.txt
"EXIT: $LASTEXITCODE" | Out-File C:\Tankaq\ct_exit.txt
