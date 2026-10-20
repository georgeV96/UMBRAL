# UMBRAL — notas para Claude Code

Plugin VST3 (JUCE 8) de transformación espectral para Lazarus Noise. El usuario trabaja en **Windows** y habla español.
Arquitectura y decisiones: `docs/ARCHITECTURE.md`. Controles y presets: `README.md`.

## Flujo de trabajo cuando corres en el PC del usuario (Windows)

1. Haz el cambio (el motor está en `Source/DSP/UmbralEngine.cpp`, sin JUCE).
2. Pruebas del motor (~1 min, sin JUCE):
   ```
   cmake -S . -B build_tests -DUMBRAL_BUILD_PLUGIN=OFF
   cmake --build build_tests --config Release --target umbral_tests
   .\build_tests\Release\umbral_tests.exe
   ```
   Deben pasar todas y "0 faults". No relajes un umbral de test para que pase: arregla el motor o explica por qué la medida estaba mal.
3. Compila e instala el VST3: `.\install_windows.ps1` (compila Release x64 y copia `Umbral.vst3` a
   `C:\Program Files\Common Files\VST3`; usa `-InstallDir` si el usuario indica otra carpeta, o abre PowerShell como administrador).
   **Ciérralo del DAW antes** (el .vst3 queda bloqueado mientras el DAW lo tiene cargado).
4. Haz commit y push a `main`.

Para escuchar sin DAW: `umbral_render` genera WAV (dry/wet) de fuentes sintéticas con cada preset.

## Reglas del proyecto

* Nada de asignaciones, mutex ni E/S en `Engine::process`. Todo se reserva en `prepare()`.
* Cada control debe modificar de verdad el procesamiento; los medidores del editor muestran estado real, no decoración.
* Cambios de latencia: el plugin la informa con `setLatencySamples`; no la cambies sin avisar.
* JUCE es AGPLv3/comercial: avisa antes de cualquier distribución.

## Estado de verificación (sé honesto con esto)

* Verificado: pruebas objetivas del motor, host mínimo de pruebas (`UmbralHostTest`), compilación VST3 con Visual Studio (GitHub Actions) y GCC.
* **Nadie ha escuchado UMBRAL ni lo ha cargado en un DAW.** Las constantes de mapeo (ganancias, curvas, tiempos) son puntos de partida razonados.
  Lo siguiente útil es que el usuario escuche y diga qué suena mal; ajusta con sus comentarios y mide el efecto con los tests.
* Pendiente de oído: fuerza de MEMORY (sutil: ~1–2 dB por bandas en un arpegio), latencia de ~43 ms, aspecto del editor.
