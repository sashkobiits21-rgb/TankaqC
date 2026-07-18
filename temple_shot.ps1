# Run the loopback QA pair and grab desktop screenshots while the match is
# live, so the new TEMPLE centerpiece can be verified visually.
$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

function Shot($path) {
    $b = [System.Windows.Forms.SystemInformation]::VirtualScreen
    $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($b.X, $b.Y, 0, 0, $bmp.Size)
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
}

Start-Process powershell -ArgumentList '-ExecutionPolicy','Bypass','-File','C:\Tankaq\loopback_test.ps1' -WindowStyle Hidden
Start-Sleep -Seconds 30
Shot 'C:\Tankaq\temple_shot1.png'
Start-Sleep -Seconds 15
Shot 'C:\Tankaq\temple_shot2.png'
Start-Sleep -Seconds 15
Shot 'C:\Tankaq\temple_shot3.png'
'SHOTSDONE' | Out-File 'C:\Tankaq\temple_shot_done.txt'
