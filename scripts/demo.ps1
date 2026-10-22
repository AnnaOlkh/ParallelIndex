param([string]$BuildDir = "build")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    $exe = Join-Path $BuildDir "indexer.exe"
    $falseSharing = Join-Path $BuildDir "false_sharing.exe"
    if (!(Test-Path $exe)) { $exe = Join-Path $BuildDir "Release/indexer.exe"; $falseSharing = Join-Path $BuildDir "Release/false_sharing.exe" }
    if (!(Test-Path $exe)) { throw "Build the project first; see README.md" }
    $dest = Join-Path $root "results/demo"
    New-Item -ItemType Directory -Force $dest | Out-Null
    $reference = Join-Path $dest "v0.tsv"
    & $exe --input tests/fixtures --output $reference --version v0
    if ($LASTEXITCODE) { throw "V0 failed" }
    foreach ($version in @("v2", "v3", "v4")) {
        foreach ($method in @("sequential", "tree")) {
            if ($version -ne "v4" -and $method -eq "tree") { continue }
            & $exe --input tests/fixtures --output (Join-Path $dest "$version-$method.tsv") --version $version --threads 4 --merge $method --verify $reference
            if ($LASTEXITCODE) { throw "$version failed" }
        }
    }
    & $falseSharing --repeats 1 --iterations 100000
    if ($LASTEXITCODE) { throw "False sharing failed" }
    Write-Host "Demo passed. Outputs: $dest. This tiny corpus is NOT a formal benchmark."
} finally { Pop-Location }
