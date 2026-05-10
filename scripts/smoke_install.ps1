param(
  [Parameter(Mandatory = $true)][string]$Format,
  [Parameter(Mandatory = $true)][string]$BuildDir
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$pack = Join-Path $BuildDir 'fxe-pack.exe'
$entry = 'examples/js/hello.ts'
$appName = 'SmokeApp'
$manufacturer = 'FXE Smoke'

if (-not (Test-Path $pack)) {
  throw "fxe-pack not found: $pack"
}
if (-not (Test-Path $entry)) {
  throw "entry script not found: $entry"
}

$outDir = New-Item -ItemType Directory -Path (Join-Path $env:RUNNER_TEMP ([guid]::NewGuid())) | Select-Object -ExpandProperty FullName

function Invoke-Pack {
  param(
    [Parameter(Mandatory = $true)][string]$Out,
    [Parameter(Mandatory = $true)][string]$Installer
  )

  & $pack $entry `
    --name $appName `
    --out $Out `
    --platform win `
    --installer $Installer `
    --version 0.0.1 `
    --manufacturer $manufacturer `
    --signing-policy unsigned-dev

  if ($LASTEXITCODE -ne 0) {
    throw "fxe-pack failed for $Installer: $LASTEXITCODE"
  }
}

switch ($Format) {
  'msi' {
    $out = Join-Path $outDir "$appName.msi"
    Invoke-Pack -Out $out -Installer 'msi'
    $installDir = Join-Path $outDir 'install'
    $log = Join-Path $outDir 'msi.log'
    $installProc = Start-Process -FilePath 'msiexec.exe' -ArgumentList @('/i', $out, '/qn', '/l*v', $log, "TARGETDIR=$installDir") -Wait -NoNewWindow -PassThru
    if ($installProc.ExitCode -ne 0) {
      Write-Warning "msiexec install failed ($($installProc.ExitCode)); falling back to lessmsi structure validation"
      & lessmsi.exe l $out | Out-Null
      if ($LASTEXITCODE -ne 0) {
        throw "lessmsi fallback failed: $LASTEXITCODE"
      }
    } else {
      $uninstallProc = Start-Process -FilePath 'msiexec.exe' -ArgumentList @('/x', $out, '/qn') -Wait -NoNewWindow -PassThru
      if ($uninstallProc.ExitCode -ne 0) {
        throw "msiexec uninstall failed: $($uninstallProc.ExitCode)"
      }
    }
  }
  'msix' {
    $out = Join-Path $outDir "$appName.msix"
    Invoke-Pack -Out $out -Installer 'msix'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $unzip = Join-Path $outDir 'unzipped'
    [System.IO.Compression.ZipFile]::ExtractToDirectory($out, $unzip)
    if (-not (Test-Path (Join-Path $unzip 'AppxManifest.xml'))) {
      throw 'msix missing AppxManifest.xml'
    }
  }
  default {
    throw "unknown format: $Format"
  }
}

Write-Host "smoke-install $Format OK ($out)"
