param(
    [Parameter(Mandatory = $true)]
    [string] $IncludeRoot,

    [Parameter(Mandatory = $true)]
    [string] $TargetRoot,

    [string] $Config = (Join-Path $PSScriptRoot 'SimdNamingTidy.yaml')
)

$ErrorActionPreference = 'Stop'

$includeRoot = [System.IO.Path]::GetFullPath($IncludeRoot)
$targetRoot = [System.IO.Path]::GetFullPath($TargetRoot)
$config = [System.IO.Path]::GetFullPath($Config)
$workRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("sora-simd-naming-" + [guid]::NewGuid())
$source = Join-Path $workRoot 'SimdNaming.cpp'
$fixes = Join-Path $workRoot 'fixes.yaml'
$utf8 = [System.Text.UTF8Encoding]::new($false)

try {
    New-Item -ItemType Directory -Path $workRoot | Out-Null
    [System.IO.File]::WriteAllText($source, "#include <Sora/Math/Simd/Simd.h>`n", $utf8)

    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & clang-tidy $source "--config-file=$config" "--export-fixes=$fixes" -- `
            -std=gnu++26 -freflection-latest -I $includeRoot 2> $null | Out-Null
        $tidyExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    if ($tidyExitCode -ne 0) {
        throw "clang-tidy failed with exit code $tidyExitCode"
    }

    & clang-apply-replacements $workRoot -format=false
    if ($LASTEXITCODE -ne 0) {
        throw "clang-apply-replacements failed with exit code $LASTEXITCODE"
    }

    $lines = Get-Content -LiteralPath $fixes
    $mappings = @{}
    $constantNames = [System.Collections.Generic.HashSet[string]]::new()
    $methodNames = [System.Collections.Generic.HashSet[string]]::new()
    $functionNames = [System.Collections.Generic.HashSet[string]]::new()
    for ($i = 0; $i -lt $lines.Count; ++$i) {
        if ($lines[$i] -notmatch "Message:\s+'invalid case style for (.+) ''([^']+)'''") {
            continue
        }

        $kind = $Matches[1]
        $oldName = $Matches[2]
        for ($j = $i + 1; $j -lt [Math]::Min($i + 30, $lines.Count); ++$j) {
            if ($lines[$j] -match '^\s+Level:') {
                break
            }
            if ($lines[$j] -notmatch 'ReplacementText:\s+(.+)$') {
                continue
            }

            $newName = $Matches[1].Trim(" '")
            if (-not $mappings.ContainsKey($oldName)) {
                $mappings[$oldName] = [System.Collections.Generic.HashSet[string]]::new()
            }
            [void] $mappings[$oldName].Add($newName)
            if ($kind -in @('class constant', 'static constant', 'global constant')) {
                [void] $constantNames.Add($oldName)
            } elseif ($kind -eq 'method') {
                [void] $methodNames.Add($oldName)
            } elseif ($kind -eq 'function') {
                [void] $functionNames.Add($oldName)
            }
            break
        }
    }

    $uniqueMappings = @($mappings.GetEnumerator() | Where-Object {
        $_.Value.Count -eq 1
    })

    foreach ($header in Get-ChildItem -LiteralPath $targetRoot -Filter '*.h') {
        $text = [System.IO.File]::ReadAllText($header.FullName)
        foreach ($mapping in $uniqueMappings) {
            $replacement = @($mapping.Value)[0]
            $qualifiedPattern = '(?<=\bSimd::)' + [regex]::Escape($mapping.Key) + '\b'
            $text = [regex]::Replace($text, $qualifiedPattern, $replacement)
            if ($mapping.Key -match '^_') {
                $pattern = '\b' + [regex]::Escape($mapping.Key) + '\b'
                $text = [regex]::Replace($text, $pattern, $replacement)
                continue
            }
            if ($constantNames.Contains($mapping.Key) -and $mapping.Key -ne 'size') {
                $pattern = '\b' + [regex]::Escape($mapping.Key) + '\b'
                $text = [regex]::Replace($text, $pattern, $replacement)
                continue
            }
            if ($methodNames.Contains($mapping.Key) -and $mapping.Key -ne 'size') {
                $pattern = '(?<!std::)(?<!ranges::)\b' + [regex]::Escape($mapping.Key) + '\b'
                $text = [regex]::Replace($text, $pattern, $replacement)
                continue
            }
            if ($functionNames.Contains($mapping.Key)) {
                $pattern = '(?<!::)\b' + [regex]::Escape($mapping.Key) + '\b'
                $text = [regex]::Replace($text, $pattern, $replacement)
                continue
            }
            if ($mapping.Key -notmatch '^_|_' -and $mapping.Key -notin @('signmask', 'storageSize')) {
                continue
            }
            $pattern = '(?<!::)\b' + [regex]::Escape($mapping.Key) + '\b'
            $text = [regex]::Replace($text, $pattern, $replacement)
        }
        [System.IO.File]::WriteAllText($header.FullName, $text, $utf8)
    }
}
finally {
    if (Test-Path -LiteralPath $workRoot) {
        Remove-Item -LiteralPath $workRoot -Recurse -Force
    }
}
