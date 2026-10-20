<#
  UMBRAL - compila e instala el VST3 en Windows.

  Requisitos (una sola vez):
    * Git for Windows            https://git-scm.com/download/win
    * CMake 3.22+                https://cmake.org/download/   (marca "Add CMake to the system PATH")
    * Visual Studio 2019/2022 o "Build Tools" con la carga de trabajo "Desarrollo de escritorio con C++"

  Uso (desde la carpeta del repositorio, en PowerShell):
    .\install_windows.ps1                              # instala en C:\Program Files\Common Files\VST3 (pide admin)
    .\install_windows.ps1 -InstallDir "D:\Plugins\VST3" # o en tu carpeta de plugins
    .\install_windows.ps1 -RunTests                     # ademas compila y ejecuta las pruebas del motor
#>
param(
    [string]$InstallDir = (Join-Path $env:CommonProgramFiles 'VST3'),
    [switch]$RunTests
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

function Need([string]$cmd, [string]$hint) {
    if (-not (Get-Command $cmd -ErrorAction SilentlyContinue)) { throw "Falta '$cmd'. $hint" }
}
Need git   'Instala Git for Windows: https://git-scm.com/download/win'
Need cmake 'Instala CMake: https://cmake.org/download/ (marca "Add CMake to the system PATH")'

Set-Location $root
Write-Host '== 1/4  Descargando JUCE (submodulo)...'
git submodule update --init --depth 1
if ($LASTEXITCODE -ne 0) { throw 'No se pudo descargar JUCE (git submodule update --init).' }

Write-Host '== 2/4  Configurando (Visual Studio, x64)...'
cmake -S . -B build -A x64
if ($LASTEXITCODE -ne 0) { throw 'CMake no pudo configurar el proyecto. Comprueba que Visual Studio (C++) esta instalado.' }

Write-Host '== 3/4  Compilando UMBRAL (Release). La primera vez tarda varios minutos...'
cmake --build build --config Release --target Umbral_VST3 --parallel
if ($LASTEXITCODE -ne 0) { throw 'La compilacion fallo. Copia el error y avisame.' }

if ($RunTests) {
    Write-Host '== Pruebas del motor...'
    cmake --build build --config Release --target umbral_tests --parallel
    if ($LASTEXITCODE -ne 0) { throw 'No se pudieron compilar las pruebas.' }
    & (Join-Path $root 'build\Release\umbral_tests.exe')
}

$bundle = Join-Path $root 'build\Umbral_artefacts\Release\VST3\Umbral.vst3'
if (-not (Test-Path $bundle)) { throw "No encuentro el resultado: $bundle" }

Write-Host "== 4/4  Instalando en $InstallDir ..."
try {
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    $dest = Join-Path $InstallDir 'Umbral.vst3'
    if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
    Copy-Item $bundle $InstallDir -Recurse -Force
} catch {
    Write-Warning "No pude copiar a '$InstallDir' ($($_.Exception.Message))."
    Write-Warning 'Si es una carpeta del sistema, abre PowerShell como Administrador, o usa -InstallDir con una carpeta tuya.'
    Write-Host "El plugin ya compilado esta en: $bundle"
    exit 1
}
Write-Host ''
Write-Host "Listo. Umbral.vst3 esta en: $dest"
Write-Host 'Abre tu DAW y vuelve a escanear plugins VST3 (si la carpeta no es la estandar, anadela a las rutas de escaneo).'
Write-Host 'Latencia: 2047 muestras (el DAW debe compensarla; el plugin la informa).'
