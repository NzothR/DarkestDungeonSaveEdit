param(
    [string]$BuildDirectory = "cmake-build-release",
    [string]$OutputArchive = "dist/DarkestDungeonSaveEditor-win64.zip",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

function Resolve-RepoPath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) { return [System.IO.Path]::GetFullPath($Path) }
    return [System.IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

$buildPath = Resolve-RepoPath $BuildDirectory
$cachePath = Join-Path $buildPath "CMakeCache.txt"
if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
    throw "Release build cache not found: $cachePath"
}
$cache = @{}
foreach ($line in Get-Content -LiteralPath $cachePath) {
    if ($line -match '^([^:=]+):[^=]+=(.*)$') { $cache[$Matches[1]] = $Matches[2] }
}
if ($cache['CMAKE_BUILD_TYPE'] -and $cache['CMAKE_BUILD_TYPE'] -ne 'Release') {
    throw "The build directory is configured for $($cache['CMAKE_BUILD_TYPE']), not Release."
}

if (-not $SkipBuild) {
    $cmake = $cache['CMAKE_COMMAND']
    if (-not $cmake -or -not (Test-Path -LiteralPath $cmake -PathType Leaf)) {
        $command = Get-Command cmake -ErrorAction SilentlyContinue
        if ($command) { $cmake = $command.Source }
    }
    if (-not $cmake) { throw "CMake was not found in the build cache or PATH." }
    & $cmake --build $buildPath --config Release --target ddse_http
    if ($LASTEXITCODE -ne 0) { throw "Release build failed with exit code $LASTEXITCODE." }
}

$candidates = @((Join-Path $buildPath "ddse_http.exe"), (Join-Path $buildPath "Release/ddse_http.exe"))
$executable = $candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if (-not $executable) { throw "ddse_http.exe was not found in the Release build." }
$binaryRoot = Split-Path -Parent $executable
$webRoot = Join-Path $binaryRoot "web"
foreach ($required in @("index.html", "assets/app.js", "assets/gateway.js", "assets/styles.css",
                       "assets/locales/en_us.json", "assets/locales/zh_cn.json")) {
    if (-not (Test-Path -LiteralPath (Join-Path $webRoot $required) -PathType Leaf)) {
        throw "Frontend file is missing from the Release build: $required"
    }
}

$compilerBin = if ($cache['CMAKE_CXX_COMPILER']) { Split-Path -Parent $cache['CMAKE_CXX_COMPILER'] }
$sqliteBin = if ($cache['SQLite3_LIBRARY']) {
    Join-Path (Split-Path -Parent (Split-Path -Parent $cache['SQLite3_LIBRARY'])) "bin"
}
$searchDirectories = @($binaryRoot, $compilerBin, $sqliteBin) |
    Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Container) } | Select-Object -Unique
$objdump = if ($compilerBin) { Join-Path $compilerBin "objdump.exe" }
if (-not $objdump -or -not (Test-Path -LiteralPath $objdump -PathType Leaf)) {
    $command = Get-Command objdump.exe -ErrorAction SilentlyContinue
    if ($command) { $objdump = $command.Source }
}
if (-not $objdump) { throw "objdump.exe is required to verify packaged DLL dependencies." }

$systemDlls = @(
    "advapi32.dll", "bcrypt.dll", "comdlg32.dll", "crypt32.dll", "gdi32.dll",
    "imm32.dll", "kernel32.dll", "msvcrt.dll", "ntdll.dll", "ole32.dll",
    "oleaut32.dll", "rpcrt4.dll", "secur32.dll", "shell32.dll", "shlwapi.dll",
    "user32.dll", "version.dll", "winmm.dll", "ws2_32.dll"
)
$dependencies = [System.Collections.Generic.Dictionary[string,string]]::new(
    [System.StringComparer]::OrdinalIgnoreCase)
$queue = [System.Collections.Generic.Queue[string]]::new()
$queue.Enqueue($executable)
while ($queue.Count -gt 0) {
    $binary = $queue.Dequeue()
    $imports = & $objdump -p $binary
    if ($LASTEXITCODE -ne 0) { throw "Could not inspect DLL imports: $binary" }
    foreach ($line in $imports) {
        if ($line -notmatch '^\s*DLL Name:\s*(\S+)') { continue }
        $name = $Matches[1]
        $lower = $name.ToLowerInvariant()
        if ($lower -in $systemDlls -or $lower.StartsWith("api-ms-win-") -or
            $lower.StartsWith("ext-ms-")) { continue }
        if ($dependencies.ContainsKey($name)) { continue }
        $found = $null
        foreach ($directory in $searchDirectories) {
            $candidate = Join-Path $directory $name
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { $found = $candidate; break }
        }
        if (-not $found) { throw "Required runtime DLL was not found: $name (imported by $binary)" }
        $dependencies.Add($name, $found)
        $queue.Enqueue($found)
    }
}

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zipPath = Resolve-RepoPath $OutputArchive
$zipDirectory = Split-Path -Parent $zipPath
New-Item -ItemType Directory -Force -Path $zipDirectory | Out-Null
$temporaryArchive = Join-Path $zipDirectory ("." + [System.IO.Path]::GetFileName($zipPath) + "." +
    [guid]::NewGuid().ToString("N") + ".tmp")
$archive = $null
try {
    $archive = [System.IO.Compression.ZipFile]::Open($temporaryArchive,
        [System.IO.Compression.ZipArchiveMode]::Create)
    $root = "DarkestDungeonSaveEditor/"
    $compression = [System.IO.Compression.CompressionLevel]::Optimal
    [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
        $archive, $executable, ($root + "ddse_http.exe"), $compression) | Out-Null
    foreach ($name in ($dependencies.Keys | Sort-Object)) {
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $archive, $dependencies[$name], ($root + $name), $compression) | Out-Null
    }
    foreach ($file in (Get-ChildItem -LiteralPath $webRoot -Recurse -File | Sort-Object FullName)) {
        $relative = $file.FullName.Substring($binaryRoot.Length).TrimStart([char]'\', [char]'/')
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $archive, $file.FullName, ($root + $relative.Replace('\', '/')), $compression) | Out-Null
    }
    $launcher = $archive.CreateEntry($root + "Start Editor.cmd")
    $writer = [System.IO.StreamWriter]::new($launcher.Open(), [System.Text.Encoding]::ASCII)
    try { $writer.Write("@echo off`r`ncd /d `"%~dp0`"`r`n`"%~dp0ddse_http.exe`" %*`r`n") }
    finally { $writer.Dispose() }
    $readme = $archive.CreateEntry($root + "README.txt")
    $writer = [System.IO.StreamWriter]::new($readme.Open(), [System.Text.Encoding]::UTF8)
    try {
        $writer.Write("Darkest Dungeon Save Editor`r`n`r`nExtract the whole folder, then run Start Editor.cmd. " +
            "The local browser UI opens automatically. On first launch, choose your installed game and save profile folders. " +
            "The application builds its own content databases in ddse-data beside the launcher. " +
            "No separate SQLite installation is required. Game, save and Mod files are not included.`r`n")
    } finally { $writer.Dispose() }
} finally {
    if ($archive) { $archive.Dispose() }
}
try {
    if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
    Move-Item -LiteralPath $temporaryArchive -Destination $zipPath
} finally {
    if (Test-Path -LiteralPath $temporaryArchive) { Remove-Item -LiteralPath $temporaryArchive -Force }
}
Write-Host "Release package: $zipPath"
Write-Host "Runtime DLLs: $(($dependencies.Keys | Sort-Object) -join ', ')"
