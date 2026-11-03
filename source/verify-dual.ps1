param([string]$OutputDirectory, [string]$GameRoot64, [string]$GameRoot65, [string]$GameRoot66, [string]$BuildDirectory)
$ErrorActionPreference = 'Stop'
$sourceRoot = $PSScriptRoot
# The repository is kept beside the three local game fixtures under the
# versioned project directory.  Two parent hops from source resolve to that
# project directory, which contains the default fixture paths.
$workspace = [IO.Path]::GetFullPath((Join-Path $sourceRoot '..\..'))
$buildRoot = if ($BuildDirectory) { [IO.Path]::GetFullPath($BuildDirectory) } else { Join-Path $sourceRoot 'build' }
if (!$OutputDirectory) {
    $projectRoot = Split-Path (Split-Path $sourceRoot -Parent) -Parent
    $OutputDirectory = Join-Path $projectRoot ('双版本验收-20261022\regression-' + (Get-Date -Format 'HHmmss'))
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$fixture64 = if ($GameRoot64) { [IO.Path]::GetFullPath($GameRoot64) } else { Join-Path $workspace '基底\洗隋' }
$fixture65 = if ($GameRoot65) { [IO.Path]::GetFullPath($GameRoot65) } else { Join-Path $workspace '基底\加强版6.5未加密版' }
$fixture66 = if ($GameRoot66) { [IO.Path]::GetFullPath($GameRoot66) } else { Join-Path $workspace '基底\复刻版6.6山贼 国庆' }
$exe64 = Join-Path $fixture64 'Ekd5.exe'
$exe65 = Join-Path $fixture65 'Ekd5.exe'
$exe66 = Join-Path $fixture66 'Ekd5.exe'
$originalAudio = Join-Path $fixture65 'Koeicda_org.dll'
$installer = Join-Path $buildRoot 'package\加速补丁安装器.exe'
$script:results = @()
function Invoke-Check([string]$Label, [string]$Program, [string[]]$TestArguments) {
    $log = Join-Path $OutputDirectory ($Label + '.log')
    & $Program @TestArguments *> $log
    $code = $LASTEXITCODE
    $script:results += [pscustomobject]@{Test=$Label; ExitCode=$code; Log=$log}
    if ($code) {
        Get-Content -LiteralPath $log -Tail 30
        $script:results | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'results.json') -Encoding utf8
        throw "$Label failed with exit code $code"
    }
    Write-Output "PASS $Label"
}
foreach ($generation in @('64','65','66')) {
    $targetRoot = Join-Path $buildRoot $generation
    $fixture = switch ($generation) { '64' { $fixture64 } '65' { $fixture65 } default { $fixture66 } }
    $exe = Join-Path $fixture 'Ekd5.exe'
    Push-Location $targetRoot
    try {
        foreach ($test in @('EngineTests','ConfigTests','GuideBookFileTests','GuideWindowTests','BattleStatsPersistenceTests','BattleStatsExportTests','StatsDiagnosticsTests','BattleInfoTests','SaveTransactionsTests','GearTests','UiIconTests')) {
            Invoke-Check "$generation-$test" (Join-Path $targetRoot ($test + '.exe')) @()
        }
        # ExtendData.dll is a 6.6 native-extension fixture.  The test is
        # independent of the generated proxy, so run it once in the 6.6
        # pass instead of pretending the same 6.6 module belongs to all
        # three engine generations.
        if ($generation -eq '66') {
            Invoke-Check "$generation-NativeExtensionFixtureTests" (Join-Path $targetRoot 'SaveTransactionsTests.exe') @('--native-extension-fixture',(Join-Path $fixture66 'ExtendData.dll'))
        }
        Invoke-Check "$generation-LayoutTests" (Join-Path $targetRoot 'LayoutTests.exe') @($exe)
        Invoke-Check "$generation-EngineProfileTests" (Join-Path $targetRoot 'EngineProfileTests.exe') @($exe64,$exe65,$exe66)
        Invoke-Check "$generation-BattleRuntimeTests" (Join-Path $targetRoot 'BattleRuntimeTests.exe') @($exe,$fixture)
        Invoke-Check "$generation-BattleStatsTests" (Join-Path $targetRoot 'BattleStatsTests.exe') @($exe)
        Invoke-Check "$generation-SaveEnhancerTests" (Join-Path $targetRoot 'SaveEnhancerTests.exe') @($exe)
        # ScenarioLifecycleTests consumes the historical 6.5, 6.6, then
        # 6.4 image order; EngineProfileTests below intentionally uses the
        # explicit 6.4, 6.5, 6.6 order.
        Invoke-Check "$generation-ScenarioLifecycleTests" (Join-Path $targetRoot 'ScenarioLifecycleTests.exe') @($exe65,$exe66,$exe64)
        Invoke-Check "$generation-IconResourceTests" (Join-Path $targetRoot 'IconResourceTests.exe') @($installer,(Join-Path $targetRoot 'Koeicda.dll'))
        $scenarios = switch ($generation) { '64' { @('S_01.eex','S_61.eex') } '65' { @('S_00.eex','S_58.eex') } default { @('S_00.eex','S_85.eex') } }
        foreach ($scenario in $scenarios) {
            $guideMode = if ($generation -eq '64') {
                if ($scenario -eq 'S_01.eex') { '--xisui-s01' } else { '--xisui-s61' }
            } elseif ($generation -eq '65') {
                if ($scenario -eq 'S_00.eex') { '--standard65-s00' } else { '--standard65-s58' }
            } else {
                if ($scenario -eq 'S_00.eex') { '--shan-ze-s00' } else { '--shan-ze-s85' }
            }
            Invoke-Check "$generation-Guide-$scenario" (Join-Path $targetRoot 'GuideTests.exe') @((Join-Path $fixture ('RS\' + $scenario)),$fixture,$guideMode)
        }
    } finally { Pop-Location }
}
$testRoot = Join-Path $buildRoot '66'
Invoke-Check 'InstallerResources' (Join-Path $testRoot 'InstallerTests.exe') @($installer,(Join-Path $testRoot 'Koeicda.dll'),(Join-Path $testRoot 'GearNtKe.dll'),$originalAudio,(Join-Path $buildRoot '65\Koeicda.dll'),(Join-Path $buildRoot '64\Koeicda.dll'))
Invoke-Check 'InstallerTransactions' (Join-Path $testRoot 'InstallerProfileTests.exe') @($installer,$exe64,$exe65,$exe66,$originalAudio,(Join-Path $OutputDirectory 'transaction-fixtures'))
$script:results | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'results.json') -Encoding utf8
Write-Output "All $($script:results.Count) test groups passed. Evidence: $OutputDirectory"
