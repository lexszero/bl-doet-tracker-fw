# SPDX-License-Identifier: Apache-2.0

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet("init", "update", "build", "shell", "clean")]
    [string]$Command = "build",

    [string]$Image = "zephyrprojectrtos/zephyr-build:main",
    [string]$WorkspaceVolume = "tracker-zephyr-workspace-v4.4.1",
    [string]$Board = "trackerd_ls/esp32/procpu",
    [string]$BuildDir = "build/trackerd_ls",
    [string]$ArtifactName = "trackerd_ls",

    [string]$SdkDir = "/opt/toolchains/zephyr-sdk-1.0.1",
    [string]$SdkVolume = "",
    [switch]$UseImageSdk,
    [switch]$Pristine
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).ProviderPath
$artifactDir = Join-Path $repoRoot ".codex-local\artifacts\$ArtifactName"
New-Item -ItemType Directory -Force $artifactDir | Out-Null

function New-DockerArgs {
    param([switch]$Interactive)

    $args = @("run", "--rm")
    if ($Interactive) {
        $args += "-it"
    }

    $args += @(
        "--user", "root",
        "--entrypoint", "/bin/bash",
        "-e", "ZEPHYR_SDK_INSTALL_DIR=$SdkDir",
        "-v", "${WorkspaceVolume}:/workspace"
    )

    if (-not $UseImageSdk -and $SdkVolume) {
        $args += @("-v", "${SdkVolume}:$SdkDir")
    }

    $args += @(
        "-v", "${repoRoot}:/workspace/bl-doet-tracker-fw",
        "-v", "${artifactDir}:/artifacts",
        "-w", "/workspace",
        $Image
    )

    return $args
}

function Invoke-DockerScript {
    param(
        [string]$Script,
        [switch]$Interactive
    )

    $dockerArgs = New-DockerArgs -Interactive:$Interactive
    & docker @dockerArgs "-lc" $Script
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

switch ($Command) {
    "init" {
        Invoke-DockerScript @'
set -euo pipefail
if [ ! -d /workspace/.west ]; then
    west init -l /workspace/bl-doet-tracker-fw
fi
west update
west zephyr-export
west list zephyr
'@
    }

    "update" {
        Invoke-DockerScript @'
set -euo pipefail
test -d /workspace/.west || {
    echo "Workspace is not initialized. Run: .\scripts\dev\zephyr-docker.ps1 init"
    exit 2
}
west update
west zephyr-export
west list zephyr
'@
    }

    "build" {
        $pristineArg = if ($Pristine) { "-p always" } else { "-p auto" }
        Invoke-DockerScript @"
set -euo pipefail
test -d /workspace/.west || {
    echo "Workspace is not initialized. Run: .\scripts\dev\zephyr-docker.ps1 init"
    exit 2
}
west build $pristineArg -b '$Board' /workspace/bl-doet-tracker-fw/app -d '/workspace/$BuildDir' -- -DBOARD_ROOT=/workspace/bl-doet-tracker-fw
mkdir -p /artifacts
cp '/workspace/$BuildDir/zephyr/zephyr.bin' /artifacts/zephyr.bin
cp '/workspace/$BuildDir/zephyr/zephyr.elf' /artifacts/zephyr.elf
cp '/workspace/$BuildDir/zephyr/zephyr.map' /artifacts/zephyr.map
cp '/workspace/$BuildDir/zephyr/runners.yaml' /artifacts/runners.yaml
ls -l /artifacts
"@
    }

    "shell" {
        Invoke-DockerScript "cd /workspace && exec bash" -Interactive
    }

    "clean" {
        Invoke-DockerScript "rm -rf '/workspace/$BuildDir'"
    }
}
