param(
  [string]$WorkDir = (Join-Path (Get-Location) ".vendor/v8"),
  [string]$InstallPrefix = (Join-Path (Get-Location) ".vendor/v8-install"),
  [ValidateSet("x64", "arm64")]
  [string]$TargetCpu = "x64",
  [switch]$Debug
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Require-Command([string]$Name) {
  if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
    throw "Required tool not found on PATH: $Name"
  }
}

<#
Build and install V8 for fxe using depot_tools.

Required tools:
  - git, python3, ninja
  - depot_tools on PATH (fetch, gclient, gn). If depot_tools is not on PATH,
    set DEPOT_TOOLS_DIR before running this script.

After install:
  $env:V8_ROOT = "C:\\path\\to\\install-prefix"
  cmake -S . -B build -DFXE_ENABLE_V8=ON
#>

if ($env:DEPOT_TOOLS_DIR) {
  $env:PATH = "$($env:DEPOT_TOOLS_DIR);$($env:PATH)"
}

foreach ($tool in @("git", "python3", "fetch", "gclient", "gn", "ninja")) {
  Require-Command $tool
}

New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
Set-Location $WorkDir

if (-not (Test-Path "v8")) {
  fetch v8
} else {
  Push-Location v8
  gclient sync -D --with_branch_heads --with_tags
  Pop-Location
}

Set-Location v8
$configName = if ($Debug) { "debug" } else { "release" }
$buildDir = "out/fxe-$TargetCpu-$configName"
$isDebug = if ($Debug) { "true" } else { "false" }

$gnArgs = @"
is_debug = $isDebug
target_cpu = "$TargetCpu"
is_component_build = false
v8_monolithic = false
v8_use_external_startup_data = false
v8_enable_i18n_support = true
v8_enable_pointer_compression = true
v8_enable_sandbox = true
treat_warnings_as_errors = false
"@

$gnArgsOneLine = ($gnArgs -split "`r?`n" | Where-Object { $_.Trim().Length -gt 0 }) -join " "
gn gen $buildDir --args=$gnArgsOneLine
ninja -C $buildDir v8 v8_libbase v8_libplatform

$includeDir = Join-Path $InstallPrefix "include"
$libDir = Join-Path $InstallPrefix "lib"
$shareDir = Join-Path $InstallPrefix "share/v8"
New-Item -ItemType Directory -Force -Path $includeDir, $libDir, $shareDir | Out-Null

Copy-Item -Recurse -Force "include/*" $includeDir

$libraryRoots = @(
  (Join-Path $buildDir "obj"),
  $buildDir
)
$libraries = @()
foreach ($root in $libraryRoots) {
  if (Test-Path $root) {
    $libraries += Get-ChildItem -Path $root -Filter "v8*.lib" -Recurse -ErrorAction SilentlyContinue
    $libraries += Get-ChildItem -Path $root -Filter "v8*.dll" -Recurse -ErrorAction SilentlyContinue
  }
}

if ($libraries.Count -eq 0) {
  throw "No V8 libraries were found under $buildDir; inspect the V8 GN targets for this revision."
}

foreach ($library in $libraries) {
  Copy-Item -Force $library.FullName $libDir
}

$icu = Join-Path $buildDir "icudtl.dat"
if (Test-Path $icu) {
  Copy-Item -Force $icu (Join-Path $shareDir "icudtl.dat")
}

$envScript = Join-Path $InstallPrefix "fxe-v8-env.ps1"
@"
`$env:V8_ROOT = "$InstallPrefix"
`$env:V8_DIR = "$InstallPrefix"
"@ | Set-Content -NoNewline -Encoding UTF8 $envScript

Write-Host "V8 installed to: $InstallPrefix"
Write-Host "Use it with:"
Write-Host "  . '$envScript'"
Write-Host "  cmake -S . -B build -DFXE_ENABLE_V8=ON"
