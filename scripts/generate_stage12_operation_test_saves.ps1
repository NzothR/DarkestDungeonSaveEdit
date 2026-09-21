param(
    [string]$BuildDirectory = "cmake-build-debug",
    [string]$SourceProfile = "test_save_profile\profile_0",
    [string]$GameRoot = "D:\SteamLibrary\steamapps\common\DarkestDungeon",
    [string]$WorkshopRoot = "D:\SteamLibrary\steamapps\workshop\content\262060",
    [string]$LocalModRoot = "D:\SteamLibrary\steamapps\common\DarkestDungeon\modes",
    [string]$OutputRoot = "test_save_profile\stage12_operation_tests",
    [ValidateSet("all", "acceptance-followups")]
    [string]$Focus = "all"
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

function Resolve-RepoPath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) { return [System.IO.Path]::GetFullPath($Path) }
    return [System.IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

$buildPath = Resolve-RepoPath $BuildDirectory
$sourcePath = Resolve-RepoPath $SourceProfile
$gamePath = Resolve-RepoPath $GameRoot
$workshopPath = Resolve-RepoPath $WorkshopRoot
$localPath = Resolve-RepoPath $LocalModRoot
$outputPath = Resolve-RepoPath $OutputRoot

$cmake = $null
$cacheValues = @{}
$cachePath = Join-Path $buildPath "CMakeCache.txt"
if (Test-Path -LiteralPath $cachePath -PathType Leaf) {
    foreach ($cacheLine in Get-Content -LiteralPath $cachePath) {
        if ($cacheLine -match '^([^:=]+):[^=]+=([^=]*)$') { $cacheValues[$Matches[1]] = $Matches[2] }
    }
    if ($cacheValues.ContainsKey('CMAKE_COMMAND') -and (Test-Path -LiteralPath $cacheValues['CMAKE_COMMAND'] -PathType Leaf)) {
        $cmake = $cacheValues['CMAKE_COMMAND']
    }
}
if (-not $cmake) {
    $cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmakeCommand) { $cmake = $cmakeCommand.Source }
}
if (-not $cmake) { throw "CMake was not found in the build cache or PATH" }

$runtimeDirectories = @()
if ($cacheValues.ContainsKey('CMAKE_CXX_COMPILER')) { $runtimeDirectories += Split-Path -Parent $cacheValues['CMAKE_CXX_COMPILER'] }
if ($cacheValues.ContainsKey('SQLite3_LIBRARY')) {
    $sqliteLibraryDirectory = Split-Path -Parent $cacheValues['SQLite3_LIBRARY']
    $runtimeDirectories += Join-Path (Split-Path -Parent $sqliteLibraryDirectory) "bin"
}
$runtimeDirectories = $runtimeDirectories | Where-Object { Test-Path -LiteralPath $_ -PathType Container } | Select-Object -Unique
if ($runtimeDirectories.Count -gt 0) { $env:PATH = ($runtimeDirectories -join ';') + ';' + $env:PATH }

Write-Host "Building Stage 12 Operation test-save generator..."
& $cmake --build $buildPath --target ddse_generate_stage12_operation_test_saves --config Debug
if ($LASTEXITCODE -ne 0) { throw "CMake build failed with exit code $LASTEXITCODE" }

$candidateExecutables = @(
    (Join-Path $buildPath "ddse_generate_stage12_operation_test_saves.exe"),
    (Join-Path $buildPath "Debug\ddse_generate_stage12_operation_test_saves.exe"),
    (Join-Path $buildPath "ddse_generate_stage12_operation_test_saves")
)
$generator = $candidateExecutables | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if (-not $generator) { throw "Built generator executable was not found under $buildPath" }

& $generator `
    --source-profile $sourcePath `
    --game-root $gamePath `
    --workshop-root $workshopPath `
    --local-mod-root $localPath `
    --output-root $outputPath `
    --focus $Focus
if ($LASTEXITCODE -ne 0) { throw "Save generation failed with exit code $LASTEXITCODE" }

Write-Host "Finished. Review: $(Join-Path $outputPath 'README.md')"
