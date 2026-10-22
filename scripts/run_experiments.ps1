param(
    [Parameter(Mandatory=$true)][string]$Corpus,
    [string]$BuildDir = "build",
    [int[]]$Threads = @(1,2,4,8),
    [int]$FixedP = 8,
    [int]$ScalingK = 64,
    [int]$Repeats = 5,
    [ValidateSet("defined","unsafe")][string]$V1Mode = "defined",
    [switch]$Demo
)
$ErrorActionPreference = "Stop"
if ($Repeats -lt 5) { throw "Formal measurements require at least five repeats" }
if ($FixedP -lt 2) { throw "FixedP must be at least two" }
if ($FixedP -gt 4096 -or $ScalingK -lt 1 -or $ScalingK -gt 65536) { throw "Invalid FixedP or ScalingK" }
if ($Threads.Count -eq 0 -or @($Threads | Where-Object { $_ -lt 1 -or $_ -gt 4096 }).Count -gt 0) { throw "Threads must be in 1..4096" }
$root = Split-Path -Parent $PSScriptRoot
$corpusPath = (Resolve-Path $Corpus).Path
Push-Location $root
try {
    $exe = Join-Path $BuildDir "indexer.exe"
    $falseSharing = Join-Path $BuildDir "false_sharing.exe"
    if (!(Test-Path $exe)) { $exe = Join-Path $BuildDir "Release/indexer.exe"; $falseSharing = Join-Path $BuildDir "Release/false_sharing.exe" }
    if (!(Test-Path $exe)) { throw "Build first; see README.md" }
    $features = & $exe --info
    if ($LASTEXITCODE) { throw "Cannot inspect indexer build" }
    if ($V1Mode -eq "unsafe" -and $features -notcontains "unsafe_v1=1") { throw "Unsafe V1 needs an explicit enabled build; use -BuildDir build-unsafe" }
    $prefix = if ($Demo) { "rehearsal-" } else { "run-" }
    $dest = Join-Path $root ("results/" + $prefix + (Get-Date -Format "yyyyMMdd-HHmmss-fff"))
    New-Item -ItemType Directory -Force $dest | Out-Null
    $csv = Join-Path $dest "results.csv"
    $reference = Join-Path $dest "v0.tsv"
    @("time=$(Get-Date -Format o)", "corpus=$corpusPath", "threads=$Threads", "fixedP=$FixedP", "scalingK=$ScalingK", "repeats=$Repeats", "v1mode=$V1Mode") | Set-Content (Join-Path $dest "environment.txt")
    $features | Out-File (Join-Path $dest "environment.txt") -Append
    & $falseSharing --info | Out-File (Join-Path $dest "environment.txt") -Append
    if ($LASTEXITCODE) { throw "Cannot inspect false-sharing hardware configuration" }
    powercfg /getactivescheme | Out-File (Join-Path $dest "environment.txt") -Append
    if ($Demo) { "FUNCTIONAL REHEARSAL: not a qualifying corpus/performance campaign" | Out-File (Join-Path $dest "environment.txt") -Append }
    try {
        Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors,L2CacheSize,L3CacheSize | Format-List | Out-File (Join-Path $dest "environment.txt") -Append
        Get-CimInstance Win32_OperatingSystem | Select-Object Caption,Version,TotalVisibleMemorySize | Format-List | Out-File (Join-Path $dest "environment.txt") -Append
    } catch {
        "OS metadata query unavailable: $($_.Exception.Message)" | Out-File (Join-Path $dest "environment.txt") -Append
        Write-Warning "Fill in CPU/OS/RAM metadata manually; OS query failed."
    }
    if (Test-Path (Join-Path $BuildDir "CMakeCache.txt")) { Copy-Item (Join-Path $BuildDir "CMakeCache.txt") (Join-Path $dest "CMakeCache.txt") }
    if (Test-Path (Join-Path $BuildDir "compile_commands.json")) { Copy-Item (Join-Path $BuildDir "compile_commands.json") (Join-Path $dest "compile_commands.json") }
    $formalArgs = if ($Demo) { @() } else { @("--require-corpus", "yes") }
    function Invoke-Index([string[]]$Arguments) {
        & $exe --input $corpusPath --csv $csv @formalArgs @Arguments | Tee-Object -FilePath (Join-Path $dest "run.log") -Append
        if ($LASTEXITCODE) { throw "Index experiment failed; results retained in $dest" }
    }
    Invoke-Index @("--version","v0","--experiment","scaling","--output",$reference,"--repeats","$Repeats")
    # V1 runs
    Write-Warning "V1 mode=$V1Mode. Defined mode is a teaching model, not the literal UB/data-race version."
    for ($i=1; $i -le 10; $i++) {
        Invoke-Index @("--version","v1","--experiment","v1_repro","--repeat-start","$i","--v1-mode",$V1Mode,"--threads","$FixedP","--output",(Join-Path $dest "v1-$i.tsv"),"--verify",$reference)
    }
    foreach ($p in $Threads) {
        foreach ($version in @("v1","v2","v3","v4")) {
            foreach ($method in @("sequential","tree")) {
                if ($version -ne "v4" -and $method -eq "tree") { continue }
                Invoke-Index @("--version",$version,"--experiment","scaling","--v1-mode",$V1Mode,"--threads","$p","--shards","$ScalingK","--merge",$method,"--output",(Join-Path $dest "$version-$p-$method.tsv"),"--verify",$reference,"--repeats","$Repeats")
            }
        }
    }
    foreach ($k in @(1,4,16,64,256,1024,4096)) {
        Invoke-Index @("--version","v3","--experiment","v3_sweep","--threads","$FixedP","--shards","$k","--output",(Join-Path $dest "v3-k$k.tsv"),"--verify",$reference,"--repeats","$Repeats")
    }
    & $falseSharing --repeats $Repeats --csv $csv | Tee-Object -FilePath (Join-Path $dest "run.log") -Append
    if ($LASTEXITCODE) { throw "False sharing experiment failed" }
    Write-Host "Raw results: $csv"
    Write-Host "Run: python scripts/analyze.py `"$csv`" --scaling-k $ScalingK --fixed-p $FixedP"
    Write-Host "Add cache hierarchy, actual compiler version/flags, corpus provenance, and power conditions to environment.txt."
} finally { Pop-Location }
