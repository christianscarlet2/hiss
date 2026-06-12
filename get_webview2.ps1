$ErrorActionPreference = 'Stop'
$dir = 'C:\www\openholdembot_old\Hiss\webview2'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$ver = '1.0.2792.45'
$url = "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/$ver"
$zip = "$dir\webview2.zip"
Write-Output "downloading WebView2 SDK $ver ..."
Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
Expand-Archive -Path $zip -DestinationPath $dir -Force
Write-Output "=== key files ==="
Get-ChildItem "$dir\build\native\include\WebView2.h" -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName
Get-ChildItem "$dir\build\native\x86\WebView2LoaderStatic.lib" -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName
