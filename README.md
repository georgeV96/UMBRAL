# UMBRAL — Evolving Spectral Transformation · Lazarus Noise

Plugin de audio experimental (VST3; AU en macOS) que transforma **progresivamente** el timbre de cualquier señal: una guitarra
limpia puede irse volviendo algo entre un instrumento real, una resonancia metálica y un sintetizador espectral, sin perder
del todo su identidad musical. No es distorsión, chorus, reverb ni filtro: trabaja sobre las **parciales** de la señal
(análisis STFT), con una **memoria tímbrica** y una **deriva** determinista.

> Arquitectura, fórmulas y decisiones: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## Clonar y compilar

```
git clone --recurse-submodules https://github.com/georgeV96/UMBRAL.git
cd UMBRAL
```

JUCE 8.0.12 es un submódulo (`git submodule update --init` si ya clonaste sin él). JUCE es AGPLv3 / licencia comercial: decide antes de distribuir.

```
# plugin + pruebas (Linux/macOS/Windows)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target Umbral_VST3
# salida: build/Umbral_artefacts/Release/VST3/Umbral.vst3

# solo el motor y sus pruebas (sin JUCE)
cmake -S . -B build -DUMBRAL_BUILD_PLUGIN=OFF
cmake --build build --target umbral_tests umbral_render
./build/umbral_tests
```

En Linux hacen falta las cabeceras habituales de JUCE (`libasound2-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libfreetype-dev libfontconfig1-dev`). Se puede apuntar a otra copia de JUCE con `-DUMBRAL_JUCE_DIR=<ruta>`.

## Controles

Todos modifican el procesamiento real (y todos menos SEED son automatizables).

| Control | Qué hace | Cómo se nota |
|---|---|---|
| **MEMORY** | Cuánto tiempo (0,04 s … 10 s) las notas anteriores siguen influyendo: olvido de la memoria espectral y de la persistencia, herencia de las parciales nuevas y «huella» de parciales recordadas. | Bajo: responde al sonido actual. Alto: en un arpegio cada nota hereda algo del carácter de las anteriores, y ese carácter tarda en disiparse al callar. No hace casi nada en una sola nota mantenida. |
| **EROSION** | Degrada selectivamente las parciales: las **disuelve en bandas de ruido** y las **desafina de forma inarmónica** (rigidez ∝ f² + micro-desafinación por parcial). | Bajo: las parciales altas se aflojan y se vuelven ligeramente metálicas. Alto: el sonido pierde su estructura armónica y se vuelve una textura abstracta con la misma envolvente. Los graves se conservan más. |
| **DRIFT** | Evolución lenta, suave y determinista de los campos de erosión, desafinación, rigidez, alcance y distribución espectral. | **0 % = totalmente estático.** Bajo: apenas perceptible. Alto: el timbre evoluciona continuamente; el material que cambia (flujo espectral) acelera la deriva. |
| **THRESHOLD** | Cuándo empieza la transformación: nivel (−66 … −18 dBFS, rodilla suave) **y** persistencia (cuánto hay que sostener la nota). Protege los transitorios. | Se toca una nota: suena natural y *después* empieza a cambiar (≈0,2 s con THRESHOLD bajo, ≈2 s con alto). El ruido de fondo por debajo del umbral no activa el motor. |
| **DEPTH** | Intensidad **y alcance** de la transformación (no es mezcla). | Bajo: solo cambian las parciales agudas. Alto: se llega hasta las fundamentales. 0 % = transparente. |
| **BLEND** | Mezcla paralela dry/procesado (0 % = original bit a bit, 100 % = procesado), con compensación de potencia limitada a ±3 dB. | |
| **OUTPUT** | −24 … +12 dB, suavizado. | |
| SEED | 1–99. Elige el patrón (reproducible) de la deriva. Se aplica con un reinicio sin clics. No automatizable. | |

Medidores: IN / OUT y cuatro barras con el **estado real del motor** (engagement de THRESHOLD, memoria tímbrica almacenada,
erosión media de las parciales, guarda de transitorios).

**Latencia:** 2047 muestras a 44,1/48 kHz (42,6–46,4 ms), 4095 a 88,2/96 kHz, 8191 a 192 kHz. Se informa al host; el *dry* de BLEND se retarda lo mismo.
**CPU:** ≈ 2 % de un núcleo en estéreo a 48 kHz (medido en un PC de nube; sin optimización específica).
La reproducción reinicia el reloj de la deriva y la memoria (con fundido), así que cada pasada del mismo material evoluciona igual.

## Presets de fábrica (programas)

| Preset | MEM | ERO | DRIFT | THR | DEPTH | BLEND | Idea |
|---|---|---|---|---|---|---|---|
| Glass Veil | .35 | .15 | .15 | .30 | .30 | .50 | casi nada: las parciales agudas se aflojan |
| Slow Patina | .60 | .30 | .25 | .35 | .45 | .65 | una nota limpia envejece hacia otra más opaca y rara |
| Bell Rust | .50 | .50 | .20 | .30 | .60 | .75 | parciales altas estiradas, metálicas |
| Ghost Strings | .85 | .30 | .35 | .25 | .55 | .70 | memoria larga: los arpegios se tiñen entre sí |
| Dark Bloom | .70 | .55 | .50 | .65 | .70 | .80 | exige una nota fuerte y mantenida; florece tarde |
| Spectral Decay | .50 | .75 | .45 | .35 | .80 | 1.0 | la estructura armónica se disuelve |
| Frozen Metal | .60 | .60 | **0** | .20 | .70 | .85 | molde inarmónico fijo (sin deriva) |
| Whisper Drift | .40 | .10 | 1.0 | .30 | .50 | .60 | poca erosión, movimiento constante |
| Percussion Rust | .30 | .45 | .30 | .70 | .55 | .60 | ataques naturales, resonancias metálicas |
| Abyss Texture | .90 | 1.0 | .80 | .15 | 1.0 | 1.0 | todo: textura espectral abstracta |

## Pruebas (`umbral_tests`, `UmbralHostTest`)

Mediciones objetivas sobre material sintético (guitarra con armónicos que decaen, arpegios con notas que suenan por debajo,
acordes disonantes, guitarra distorsionada, pads estéreo, voz con vibrato, batería). Cubren:

* **Reconstrucción perfecta** (DEPTH 0: −140 dB) a 44,1/48/88,2/96/192 kHz con bloques arbitrarios, latencia exacta, estéreo/mono.
* **Determinismo:** salida idéntica bit a bit para bloques 1…4096, misma semilla = misma evolución, `restart()` = instancia nueva.
* **Robustez:** silencio exacto → silencio exacto; NaN/Inf/1e9 en la entrada; ruido a fondo de escala con 12 ajustes aleatorios; automatización aleatoria de todos los controles con reinicios; sin asignaciones de memoria en el hilo de audio; sin ralentización por desnormales; **0 fotogramas rescatados por la red de seguridad numérica**.
* **Cada control**: MEMORY (herencia de persistencia, disipación 0,2/1,5/13 s, ninguna repetición de audio), EROSION (inarmonicidad y pérdida de estructura crecientes, sin comportarse como filtro), DRIFT (0 = estático, crece, suave), THRESHOLD (floración progresiva, ruido de fondo ignorado, sin bombeo, ataque natural), DEPTH (alcance y no es wet/dry), BLEND (0 % exacto, nivel estable), OUTPUT (±0,05 dB, sin saltos).
* **Independencia:** una matriz «control × efecto» comprueba que cada control domina su propia firma (EROSION → desafinación; DRIFT → movimiento temporal; THRESHOLD → rapidez de llegada, sin tocar el sonido ya desarrollado; MEMORY → arpegio, no nota mantenida).
* **Diferenciación frente a filtro + saturación + modulación:** una cadena convencional mantiene las parciales en la serie armónica (≈ 1 cent) y no evoluciona; UMBRAL las desplaza (≈ 20 cents) y evoluciona.
* **Utilidad musical:** con ajustes moderados las parciales 1–4 se mueven 0,1 cents y el balance espectral 0,6 dB (límites de la prueba: 4 cents / 5 dB).
* **Capa del plugin** (`UmbralHostTest`, instancia el `AudioProcessor` real): cada parámetro llega al motor, programas, estado, layouts, el editor se construye.

## Escuchar sin DAW

```
./build/umbral_render salida/            # todos los presets sobre las 7 fuentes sintéticas (WAV 16 bit)
./build/umbral_render salida/ Abyss      # solo los presets cuyo nombre contenga «Abyss»
```

## Estado de verificación

**Verificado** (ver arriba): compila (VST3 en Linux); las pruebas objetivas pasan; el plugin se instancia, procesa y construye su editor en un host mínimo.

**Sin verificar — necesita oídos y un DAW:**
* **Nadie ha escuchado UMBRAL.** Las pruebas demuestran que cada control produce un efecto medible, distinto y acotado; no pueden decir si suena bien. Las constantes de mapeo (ganancias, curvas, tiempos) son puntos de partida razonados, no calibrados a oído.
* No se ha probado en un DAW real (carga, automatización, reinicio con la transportación, compensación de latencia) ni en Windows/macOS.
* El editor no se ha visto en pantalla (solo se comprueba que se construye y se pinta).
* Material real (DI de guitarra, voces grabadas, mezclas): las pruebas usan señales sintéticas.
* Latencia de ~43 ms: puede no ser aceptable para monitorizar en directo.

## Estructura

```
Source/DSP/            motor (Fft.h, Noise.h, UmbralEngine.*): C++17 puro, sin JUCE
Source/Presets.h       presets de fábrica (compartidos por plugin, pruebas y herramienta)
Source/Plugin*.{h,cpp} envoltorio JUCE (parámetros, programas, estado, editor)
Tests/                 pruebas del motor (UmbralTests) y de la capa del plugin (PluginHostTest)
Tools/Render.cpp       render offline a WAV
docs/ARCHITECTURE.md   arquitectura DSP y decisiones
third_party/JUCE       submódulo
```
