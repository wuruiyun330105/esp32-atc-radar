# 下载 LVGL 9.3.0 到 third_party\lvgl（PC 模拟器用，设备端不需要）
# 用法：powershell -ExecutionPolicy Bypass -File scripts\setup_lvgl.ps1
$ErrorActionPreference = "Stop"

$Ver  = "9.3.0"
$Root = Split-Path -Parent $PSScriptRoot
$Dest = Join-Path $Root "third_party\lvgl"

if (Test-Path (Join-Path $Dest "src\lvgl.h")) {
    Write-Host "LVGL 已存在：$Dest"
    exit 0
}

# PowerShell 5.1 默认可能还是 TLS 1.0，GitHub 会拒
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

New-Item -ItemType Directory -Force -Path (Join-Path $Root "third_party") | Out-Null
$Tmp = Join-Path $env:TEMP ("lvgl_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $Tmp | Out-Null

$Urls = @(
    "https://github.com/lvgl/lvgl/archive/refs/tags/v$Ver.tar.gz",
    "https://gh-proxy.com/https://github.com/lvgl/lvgl/archive/refs/tags/v$Ver.tar.gz",
    "https://ghfast.top/https://github.com/lvgl/lvgl/archive/refs/tags/v$Ver.tar.gz"
)

$Tar = Join-Path $Tmp "lvgl.tar.gz"
$ok = $false
foreach ($u in $Urls) {
    Write-Host "下载：$u"
    try {
        Invoke-WebRequest -UseBasicParsing -Uri $u -OutFile $Tar -TimeoutSec 600
        $ok = $true
        break
    } catch {
        Write-Host "  失败：$($_.Exception.Message)"
    }
}

if (-not $ok) {
    Write-Host ""
    Write-Host "三个源都下不动。手动下载后放到 third_party\lvgl 即可："
    Write-Host "  https://github.com/lvgl/lvgl/archive/refs/tags/v$Ver.tar.gz"
    Write-Host "  解压后把 lvgl-$Ver 改名成 lvgl，放进 $Root\third_party\"
    exit 1
}

Write-Host "解压..."
tar -xzf $Tar -C $Tmp

$Src = Join-Path $Tmp "lvgl-$Ver"
if (-not (Test-Path (Join-Path $Src "src"))) {
    $Src = Get-ChildItem -Path $Tmp -Directory -Filter "lvgl*" | Select-Object -First 1 -ExpandProperty FullName
}
if (-not (Test-Path (Join-Path $Src "src"))) {
    Write-Host "解压结果异常：$Src"
    exit 1
}

if (Test-Path $Dest) { Remove-Item -Recurse -Force $Dest }
Move-Item $Src $Dest
Set-Content -Path (Join-Path $Dest ".lvgl_version") -Value $Ver
Remove-Item -Recurse -Force $Tmp

Write-Host ""
Write-Host "完成：$Dest"
Write-Host "下一步（需已安装 CMake 和 SDL2）："
Write-Host "  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release"
Write-Host "  cmake --build build --config Release"
Write-Host "  .\build\sim.exe"
