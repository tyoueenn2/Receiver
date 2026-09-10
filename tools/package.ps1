param(
    [string]$BuildDirectory = 'build/tests/Release',
    [string]$Destination = 'dist/receiver',
    [string]$RuntimeDirectory = ''
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath (Join-Path $BuildDirectory 'receiver.exe'))) { throw 'Build receiver.exe first.' }
New-Item -ItemType Directory -Force -Path $Destination | Out-Null
foreach ($name in @('receiver.exe','receiver_headless.exe','receiver_verify.exe','receiver_tests.exe','receiver_humanization_tests.exe','receiver_tracking_tests.exe','receiver_local_mouse_tests.exe','receiver_decode.exe','receiver_test_hub_runner.exe')) {
    $source = Join-Path $BuildDirectory $name
    if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination $Destination }
}
foreach ($name in @('tools','docs','profiles','integrations','tests')) { Copy-Item -LiteralPath $name -Destination $Destination -Recurse -Force }
Copy-Item -LiteralPath 'README.md' -Destination $Destination
Copy-Item -LiteralPath 'Start Demo.cmd' -Destination $Destination
if (Test-Path -LiteralPath 'models/yolo11n.onnx') {
    New-Item -ItemType Directory -Force -Path (Join-Path $Destination 'models') | Out-Null
    Copy-Item -LiteralPath 'models/yolo11n.onnx','models/yolo11n.onnx.json' -Destination (Join-Path $Destination 'models')
}
if ($RuntimeDirectory) {
    foreach ($name in @('libc++.dll','libunwind.dll','libwinpthread-1.dll')) {
        $source = Join-Path $RuntimeDirectory $name
        if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination $Destination }
    }
}
Write-Output "Packaged at $Destination. CUDA/TensorRT SDK DLLs are not bundled. See README for the required runtime."

New-Item -ItemType Directory -Force -Path (Join-Path $Destination "src") | Out-Null
Copy-Item -LiteralPath "src/preprocess_kernel.cuh" -Destination (Join-Path $Destination "src")
