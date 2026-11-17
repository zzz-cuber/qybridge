# qybridge 编译脚本（Windows PowerShell）
# 用法:
#   .\build.ps1                 只编译
#   .\build.ps1 -Flash -Port COM6   编译并烧录+监视
#   .\build.ps1 -SetTarget      重新生成 sdkconfig（会覆盖 sdkconfig）
#
# 说明: 本机 ESP-IDF 装在 C:\esp\v6.0.1\esp-idf，工具链在 C:\Espressif\tools。
# 如果换电脑，只改下面这一段路径即可。
param(
    [switch]$Flash,
    [string]$Port = "",
    [switch]$SetTarget,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$env:IDF_PATH           = 'C:\esp\v6.0.1\esp-idf'
$env:IDF_TOOLS_PATH     = 'C:\Espressif\tools'
$env:IDF_PYTHON_ENV_PATH= 'C:\Espressif\tools\python\v6.0.1\venv'
$env:ESP_IDF_VERSION    = '6.0.1'

$tools = @(
    'C:\Espressif\tools\cmake\4.0.3\bin',
    'C:\Espressif\tools\ninja\1.12.1',
    'C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin',
    'C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin',
    'C:\Espressif\tools\idf-exe\1.0.3'
)
$env:PATH = ($tools -join ';') + ';' + $env:PATH

$py  = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
$idf = Join-Path $env:IDF_PATH 'tools\idf.py'
$proj = $PSScriptRoot
$buildDir = Join-Path $proj 'build'

if ($Clean -and (Test-Path $buildDir)) { Remove-Item -Recurse -Force $buildDir }

if ($SetTarget) {
    & $py $idf -C $proj -B $buildDir set-target esp32s3
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

& $py $idf -C $proj -B $buildDir build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Flash) {
    $args = @('-C', $proj, '-B', $buildDir)
    if ($Port) { $args += @('-p', $Port) }
    $args += @('flash', 'monitor')
    & $py $idf @args
    exit $LASTEXITCODE
}

Write-Host "编译完成: $(Join-Path $buildDir 'qybridge.bin')"
