$ErrorActionPreference = "Stop"

$KernelSectors = 32
$KernelPadBytes = $KernelSectors * 512
$FloppyBytes = 1474560

nasm -f bin boot.asm -o boot.bin
nasm -f bin kernel.asm -o kernel.bin

$boot = [System.IO.File]::ReadAllBytes("boot.bin")
if ($boot.Length -ne 512) {
    throw "boot.bin must be exactly 512 bytes, got $($boot.Length)"
}
if ($boot[510] -ne 0x55 -or $boot[511] -ne 0xAA) {
    throw ("boot.bin signature must be 55 aa, got {0:x2} {1:x2}" -f $boot[510], $boot[511])
}

$kernel = [System.IO.File]::ReadAllBytes("kernel.bin")
if ($kernel.Length -gt $KernelPadBytes) {
    throw "kernel.bin must fit in $KernelSectors sectors, got $($kernel.Length) bytes"
}

$kernelPad = New-Object byte[] $KernelPadBytes
[Array]::Copy($kernel, $kernelPad, $kernel.Length)
[System.IO.File]::WriteAllBytes("kernel.pad.bin", $kernelPad)

$zero = New-Object byte[] 4096
$fs = [System.IO.File]::Create("photon.img")
try {
    $fs.Write($boot, 0, $boot.Length)
    $fs.Write($kernelPad, 0, $kernelPad.Length)
    while ($fs.Length -lt $FloppyBytes) {
        $remaining = $FloppyBytes - $fs.Length
        $count = [Math]::Min($zero.Length, [int]$remaining)
        $fs.Write($zero, 0, $count)
    }
}
finally {
    $fs.Dispose()
}

Write-Host "Built photon.img"
Write-Host "  boot.bin   : $($boot.Length) bytes"
Write-Host "  kernel.bin : $($kernel.Length) bytes"
Write-Host "  photon.img : $((Get-Item photon.img).Length) bytes"
