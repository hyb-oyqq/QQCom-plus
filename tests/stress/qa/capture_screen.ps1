# 全屏截图（主显示器）到指定路径。QA 验收用：抓取 easyDebug 窗口 + 弹窗 + 状态栏。
# 用法：powershell -NoProfile -File capture_screen.ps1 -Path out.png
param(
    [Parameter(Mandatory = $true)][string]$Path
)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bmp = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
$bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose()
$bmp.Dispose()
Write-Output "SAVED $Path"
