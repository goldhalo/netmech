<#
.SYNOPSIS
    NetMechanism 一键构建 + 单元测试脚本（Windows / MSVC 2019 / x64）。

.DESCRIPTION
    流水线：
      1. 调用 vcvars64.bat 初始化 MSVC 环境
      2. cmake -S . -B build -G "Visual Studio 16 2019" -A x64
      3. cmake --build build --config Release
      4. 运行 netmech_ut.exe，解析通过/失败统计
      5. 失败时 exit 1

.PARAMETER Config
    Release（默认）或 Debug。Debug 会使用 /MDd 并链接 Boost 的 -gd- 版本。

.PARAMETER Clean
    先删除 build 目录再配置。

.PARAMETER BuildDir
    构建目录，默认 <脚本目录>\build。

.PARAMETER BoostRoot
    Boost 根目录，默认 D:\3rdlibsTms\3rdlibs。

.PARAMETER NoExamples
    不构建示例程序。

.PARAMETER TestFilter
    只运行名称包含该子串的测试用例。

.EXAMPLE
    .\build_and_test.ps1
    .\build_and_test.ps1 -Clean
    .\build_and_test.ps1 -Config Debug
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release',

    [switch]$Clean,

    [string]$BuildDir,

    [string]$BoostRoot = 'D:\3rdlibsTms\3rdlibs',

    [switch]$NoExamples,

    [string]$TestFilter
)

$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $BuildDir) { $BuildDir = Join-Path $ScriptDir 'build' }

$Vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat'

# ---------------------------------------------------------------------------
# 辅助输出
# ---------------------------------------------------------------------------
function Write-Step([string]$msg) {
    Write-Host ''
    Write-Host ('=' * 70) -ForegroundColor Cyan
    Write-Host "  $msg" -ForegroundColor Cyan
    Write-Host ('=' * 70) -ForegroundColor Cyan
}

function Write-Ok([string]$msg)    { Write-Host "[ OK ] $msg"   -ForegroundColor Green }
function Write-Warn2([string]$msg) { Write-Host "[WARN] $msg"   -ForegroundColor Yellow }
function Write-Err([string]$msg)   { Write-Host "[FAIL] $msg"   -ForegroundColor Red }

function Invoke-Native {
    param(
        [Parameter(Mandatory)][string]$Command,
        [string[]]$Arguments = @(),
        [string]$WorkDir,
        [switch]$AllowFailure
    )
    $prev = $null
    if ($WorkDir) { $prev = Get-Location; Set-Location -LiteralPath $WorkDir }
    try {
        # ★ 关键：原生命令的 stdout 会进入 PowerShell 的**成功输出流**，
        #   若不拦截，它会和函数末尾 `return $code` 一起成为函数的返回值，
        #   导致 $rc 变成一个大数组（真机上表现为 "CMake 配置失败 (exit=<一大段日志> 0)"）。
        #   用 Out-Host 让它只显示、不进入返回值；$LASTEXITCODE 单独取退出码。
        & $Command @Arguments | Out-Host
        $code = $LASTEXITCODE
    }
    finally {
        if ($prev) { Set-Location -LiteralPath $prev }
    }
    if ($code -ne 0 -and -not $AllowFailure) {
        throw "命令失败 (exit=$code): $Command $($Arguments -join ' ')"
    }
    return [int]$code
}

# ---------------------------------------------------------------------------
# 0/5 环境检查
# ---------------------------------------------------------------------------
Write-Step '0/5 环境检查'

$cmakeExe = $null
$cmakeCmd = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCmd) {
    $cmakeExe = $cmakeCmd.Source
} else {
    $fallback = 'C:\Program Files\CMake\bin\cmake.exe'
    if (Test-Path $fallback) {
        $cmakeExe = $fallback
        $env:Path = (Split-Path $fallback) + ';' + $env:Path
    }
}
if (-not $cmakeExe) {
    Write-Err 'cmake 不在 PATH 中，且 C:\Program Files\CMake\bin\cmake.exe 不存在'
    exit 1
}
Write-Ok "cmake: $cmakeExe"
(& $cmakeExe --version | Select-Object -First 1) | ForEach-Object { Write-Host "       $_" }

if (-not (Test-Path $Vcvars)) {
    Write-Err "找不到 vcvars64.bat: $Vcvars"
    exit 1
}
Write-Ok "vcvars64: $Vcvars"

$boostVerHpp = Join-Path $BoostRoot 'include\boost\version.hpp'
if (-not (Test-Path $boostVerHpp)) {
    Write-Err "Boost 头文件不存在: $boostVerHpp"
    exit 1
}
$verMatch = Select-String -Path $boostVerHpp -Pattern '#define BOOST_LIB_VERSION "([^"]+)"' | Select-Object -First 1
if ($verMatch) {
    Write-Ok "boost: $BoostRoot ($($verMatch.Matches[0].Groups[1].Value))"
}

$netHeader = Join-Path $ScriptDir 'include\netmech\net.hpp'
if (-not (Test-Path $netHeader)) {
    Write-Err '缺少汇总头 include\netmech\net.hpp'
    exit 1
}
Write-Ok '公共头文件已就绪: include\netmech\net.hpp'

# ---------------------------------------------------------------------------
# 1/5 vcvars64
# ---------------------------------------------------------------------------
Write-Step '1/5 初始化 MSVC 环境 (vcvars64)'

# 关键实现细节（踩过的坑，勿简化）：
#   不能写 `cmd /c "..." > file` —— PowerShell 重定向会先占用文件句柄，
#   而 vcvars64 内部又要写同一文件，导致 "file is being used by another
#   process"，环境变量一个都拿不到。正确做法：生成临时 .cmd，在批处理内部
#   完成重定向，PowerShell 只读取结果文件。
$tmpEnv   = Join-Path $env:TEMP ("netmech_env_{0}.txt"    -f [guid]::NewGuid().ToString('N'))
$tmpBatch = Join-Path $env:TEMP ("netmech_vcvars_{0}.cmd" -f [guid]::NewGuid().ToString('N'))

try {
    $batchLines = @(
        '@echo off',
        ('call "' + $Vcvars + '" >nul 2>&1'),
        'if errorlevel 1 exit /b 1',
        'set'
    )
    Set-Content -LiteralPath $tmpBatch -Value $batchLines -Encoding ASCII

    cmd.exe /d /c ('"' + $tmpBatch + '" > "' + $tmpEnv + '" 2>&1')
    $vcExit = $LASTEXITCODE
    if ($vcExit -ne 0) {
        if (Test-Path $tmpEnv) {
            Get-Content -LiteralPath $tmpEnv | Select-Object -First 20 | ForEach-Object { Write-Host "       $_" }
        }
        Write-Err "vcvars64.bat 执行失败 (exit=$vcExit)"
        exit 1
    }

    $imported = 0
    foreach ($line in (Get-Content -LiteralPath $tmpEnv)) {
        # `set` 输出形如 NAME=VALUE，VALUE 里可能含 '='（如 PATH），只切第一个
        if ($line -match '^([A-Za-z_][A-Za-z0-9_()#]{0,80})=(.*)$') {
            [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
            $imported++
        }
    }
    Write-Ok "vcvars64 成功，已导入 $imported 个环境变量"
}
finally {
    Remove-Item -LiteralPath $tmpEnv   -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $tmpBatch -ErrorAction SilentlyContinue
}

if ($env:VSCMD_ARG_TGT_ARCH) {
    Write-Ok "目标架构: $($env:VSCMD_ARG_TGT_ARCH)"
} else {
    Write-Warn2 'VSCMD_ARG_TGT_ARCH 未设置（vcvars 版本可能较旧）'
}

$clCmd = Get-Command cl -ErrorAction SilentlyContinue
if (-not $clCmd) {
    Write-Err 'vcvars64 之后仍找不到 cl.exe —— 环境导入失败'
    exit 1
}
Write-Ok "cl: $($clCmd.Source)"
# 注意：cl.exe 会把版本横幅写到 **stderr**。在 PowerShell 中 `2>&1` 会把
# stderr 记录包装成 ErrorRecord，配合 $ErrorActionPreference='Stop' 会直接
# 终止脚本。这里必须用 try/catch 吞掉，或改用 cmd 包装。
$clVer = $null
try {
    $clVer = (& cmd.exe /d /c "`"$($clCmd.Source)`" 2>&1" | Select-Object -First 1)
} catch {
    $clVer = $_.Exception.Message
}
if ($clVer) { Write-Host "       $clVer" }

# ---------------------------------------------------------------------------
# 2/5 CMake 配置
# ---------------------------------------------------------------------------
Write-Step "2/5 CMake 配置 ($Config / x64)"

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "       删除旧构建目录: $BuildDir"
    Remove-Item -LiteralPath $BuildDir -Recurse -Force
}
if (-not (Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir | Out-Null
}

$examplesOn = 'ON'
if ($NoExamples) { $examplesOn = 'OFF' }

$cmakeArgs = @(
    '-S', $ScriptDir,
    '-B', $BuildDir,
    '-G', 'Visual Studio 16 2019',
    '-A', 'x64',
    ("-DNETMECH_BOOST_ROOT=" + $BoostRoot),
    '-DNETMECH_BUILD_TESTS=ON',
    ("-DNETMECH_BUILD_EXAMPLES=" + $examplesOn)
)

try {
    $rc = Invoke-Native -Command $cmakeExe -Arguments $cmakeArgs -AllowFailure
} catch {
    Write-Err $_.Exception.Message
    exit 1
}
if ($rc -ne 0) {
    Write-Err "CMake 配置失败 (exit=$rc)"
    exit 1
}
Write-Ok 'CMake 配置成功'

# ---------------------------------------------------------------------------
# 3/5 构建
# ---------------------------------------------------------------------------
Write-Step "3/5 构建 ($Config)"

$buildArgs = @('--build', $BuildDir, '--config', $Config, '--parallel')
try {
    $rc = Invoke-Native -Command $cmakeExe -Arguments $buildArgs -AllowFailure
} catch {
    Write-Err $_.Exception.Message
    exit 1
}
if ($rc -ne 0) {
    Write-Err "构建失败 (exit=$rc)"
    exit 1
}
Write-Ok '构建成功'

# ---------------------------------------------------------------------------
# 4/5 运行测试
# ---------------------------------------------------------------------------
Write-Step '4/5 运行单元测试'

$exe = $null
foreach ($candidate in @((Join-Path $BuildDir 'bin\netmech_ut.exe'), (Join-Path $BuildDir 'bin\Release\netmech_ut.exe'))) {
    if (Test-Path $candidate) { $exe = $candidate; break }
}
if (-not $exe) {
    $found = Get-ChildItem -Path $BuildDir -Recurse -Filter 'netmech_ut.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { $exe = $found.FullName }
}
if (-not $exe) {
    Write-Err "找不到测试可执行文件 netmech_ut.exe (在 $BuildDir)"
    exit 1
}
Write-Ok "测试程序: $exe"

# 让运行时能找到 Boost DLL
$boostBin = Join-Path $BoostRoot 'bin'
if (Test-Path $boostBin) { $env:Path = $boostBin + ';' + $env:Path }
$env:Path = (Split-Path $exe) + ';' + $env:Path

$testArgs = @()
if ($TestFilter) { $testArgs += $TestFilter }

Write-Host ''
# 测试程序的 stderr 也可能带内容；$ErrorActionPreference='Stop' 下不能让它中断脚本。
$output = @()
try {
    $output = & $exe @testArgs 2>&1
} catch {
    $output = @($_.Exception.Message)
}
$testExit = $LASTEXITCODE
$outputText = ($output | ForEach-Object { $_.ToString() } | Out-String)
Write-Host $outputText

# 解析统计行（来自 test_framework.hpp 的汇总输出）
$casesPass = $null; $casesFail = $null; $checksPass = $null; $checksFail = $null
$mCases = [regex]::Match($outputText, '用例:\s*(\d+)\s*通过\s*/\s*(\d+)\s*失败')
if ($mCases.Success) {
    $casesPass = [int]$mCases.Groups[1].Value
    $casesFail = [int]$mCases.Groups[2].Value
}
$mChecks = [regex]::Match($outputText, '断言:\s*(\d+)\s*通过\s*/\s*(\d+)\s*失败')
if ($mChecks.Success) {
    $checksPass = [int]$mChecks.Groups[1].Value
    $checksFail = [int]$mChecks.Groups[2].Value
}

# ---------------------------------------------------------------------------
# 5/5 结果汇总
# ---------------------------------------------------------------------------
Write-Step '5/5 结果汇总'

if ($null -ne $casesPass)  { Write-Host "  用例通过 : $casesPass" }
if ($null -ne $casesFail)  { Write-Host "  用例失败 : $casesFail" }
if ($null -ne $checksPass) { Write-Host "  断言通过 : $checksPass" }
if ($null -ne $checksFail) { Write-Host "  断言失败 : $checksFail" }
Write-Host "  构建配置 : $Config / x64"
Write-Host "  Boost    : $BoostRoot"
Write-Host "  测试程序 : $exe"
Write-Host ''

$summaryOk = ($testExit -eq 0) -and ($casesFail -eq 0) -and ($checksFail -eq 0)

if ($summaryOk) {
    Write-Host ('-' * 70) -ForegroundColor Green
    Write-Host '  BUILD & TEST: PASS' -ForegroundColor Green
    if ($null -ne $casesPass) {
        Write-Host "  $casesPass 个用例 / $checksPass 个断言全部通过" -ForegroundColor Green
    }
    Write-Host ('-' * 70) -ForegroundColor Green
    exit 0
} else {
    Write-Host ('-' * 70) -ForegroundColor Red
    Write-Host "  BUILD & TEST: FAIL (测试进程 exit=$testExit)" -ForegroundColor Red
    if ($null -ne $casesFail) {
        Write-Host "  $casesFail 个用例失败 / $checksFail 个断言失败" -ForegroundColor Red
    }
    Write-Host ('-' * 70) -ForegroundColor Red
    exit 1
}
