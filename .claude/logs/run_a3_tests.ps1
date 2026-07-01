python T:\toolchains\coca-toolchain-p2996\setup.py | Invoke-Expression
Set-Location G:\Teaching\Vulkan
$tests = @(
    'RootObjectTest','RefcountInvariantTest','ComPtrTest','MakeOwnedTest',
    'EagerChainTest','RegistryTest','QueryTest',
    'IntrospectionTest','ClosureTest','EpochRcuTest','D16FlattenTest',
    'ArmKindsTest','NucleusDtorWalkerTest','A3IntegrationTest'
)
foreach ($t in $tests) {
    $exe = "build/x64-asan/bin/Test.Core.$t.exe"
    if (Test-Path $exe) {
        $out = & $exe 2>&1
        $tail = ($out | Select-Object -Last 2) -join ' | '
        Write-Host "$t : $tail"
    } else {
        Write-Host "$t : (not built)"
    }
}
