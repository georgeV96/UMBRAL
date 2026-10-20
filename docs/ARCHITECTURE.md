# UMBRAL — Arquitectura DSP

Este documento explica **qué hace el motor, por qué se eligió así y qué se descartó**. El código está en
`Source/DSP/UmbralEngine.{h,cpp}` (sin dependencia de JUCE); `Source/PluginProcessor.*` solo lo envuelve.

## 1. Idea en una frase

Un STFT de alta resolución analiza la señal, **detecta las parciales (sinusoides estables)**, y a cada parcial se le
aplica —según su frecuencia, su *edad* (cuánto lleva sonando) y el estado de la memoria— una pérdida de coherencia
(se disuelve en ruido de banda estrecha) y un desplazamiento inarmónico. El resultado se re-sintetiza por solapamiento y suma.
La identidad del instrumento se conserva porque los graves y las notas recién tocadas se transforman poco o nada.

```
 in ─► ventana Hann ─► FFT (L+iR, una sola FFT) ─► [separar L/R]
                                │
        ┌───────────────────────┴───────────────────────────────────────────────┐
        │ nivel · puerta THRESHOLD · guarda de transitorios · detección de parciales
        │ (coherencia de fase entre fotogramas)                                  │
        ▼                                                                        │
   MEMORIA  ── forma espectral M[k] (media con fugas, ponderada por la puerta)   │
            ── persistencia A[k] por bin ("edad" de cada parcial)                │
            ── huella de parciales recordadas                                    │
        │                                                                        │
        ▼                                                                        ▼
   por parcial: λ (pérdida) · Δf (desplazamiento) · ganancia de huella   DRIFT: reloj + campos de ruido
        │                                                                  de valor, deterministas
        ▼
   copia rígida del lóbulo (±3 bins) + rotación de fase acumulada  +  ruido de banda estrecha (fase aleatoria)
        │
        ▼
   imprint de bandas · deriva espectral (energía neutra) ─► IFFT ─► ventana Hann ─► solape-suma ─► wet
                                                                                                    │
 dry (retardado N−1) ──────────────────────────────────────────────────── BLEND (potencia compensada) ─► OUTPUT ─► out
```

## 2. Por qué STFT y no un banco de filtros

| | Banco de filtros / IIR por bandas | **STFT + solape-suma (elegido)** |
|---|---|---|
| Resolución en graves | Bandas anchas; separar los armónicos de una cuerda grave exige muchas bandas estrechas y filtros largos | 23 Hz/bin a 48 kHz: los armónicos de 110 Hz (4,7 bins) y los de 82 Hz (3,5 bins) son distinguibles |
| Operar sobre **parciales** (no sobre bandas) | No hay noción de pico/parcial | Detección de picos, copia de lóbulos, fase por parcial |
| Desplazar frecuencias | Requiere un desplazador de banda lateral única por banda (+ Hilbert + filtros de separación) | Es un cambio de índice de bin + rotación de fase |
| Latencia | Baja | **N−1 muestras** (≈ 42,6 ms) |
| Coste | Proporcional a bandas × orden | O(N log N) por salto; ≈ 2 % de un núcleo en estéreo |

La transformación que se busca (desplazar y desintegrar armónicos individuales de forma dependiente de la señal) es natural
en el dominio de la frecuencia y muy incómoda en un banco de filtros. Por eso se asume la latencia, que se **informa al
host** (`setLatencySamples`) y el *dry* de BLEND se retarda lo mismo.

### Parámetros de la transformada

* **N = 2048 a 44,1/48 kHz** (4096 a 88,2/96 kHz, 8192 a 192 kHz): la menor potencia de 2 con ventana ≥ 40 ms. Menos
  (1024) no resuelve la cuerda MI grave; más duplicaría una latencia ya notable.
* **Solape 8× (salto N/8 = 256 muestras)**. Con 4× el desajuste fraccionario de la rotación de fase (§4) produce
  modulaciones de amplitud apreciables; con 8× la ondulación de la envolvente de una parcial aislada queda en < 1 dB a
  ajustes moderados (medido: 0,9 dB, el batido normal entre la sinusoide y su banda de ruido). Coste: el doble de FFT, aún despreciable.
* **Hann de análisis × Hann de síntesis**, normalizada numéricamente (`olaGain`): reconstrucción perfecta a −140 dB con DEPTH = 0
  (comprobado a 44,1/48/88,2/96/192 kHz y tamaños de bloque arbitrarios).
* **Una sola FFT compleja para los dos canales** (L real, R imaginaria) y separación por simetría hermítica.
* Latencia exacta: salida del instante *n* = entrada de *n−(N−1)*; el *dry* se lee del mismo anillo de entrada.

## 3. Detección de parciales

Un pico del espectro es una parcial si (1) es máximo local, (2) cae ≥ 4,8 dB (3×) a ambos lados en ±4 bins (histéresis:
3 dB si ya era parcial en el fotograma anterior) y (3) **es una sinusoide estable**: la frecuencia instantánea estimada a
partir del avance de fase respecto al fotograma anterior coincide en los bins p−1, p, p+1 (|Δ| < 0,35 bins; 0,5 con histéresis).

Alternativa descartada: «el pico destaca del promedio local». Falla justo en el caso principal: a 110 Hz las parciales
están a 4,7 bins y a 82 Hz a 3,5, de modo que el promedio local las incluye a ellas mismas y casi ninguna «destaca»
(medido: con ese criterio DEPTH apenas alcanzaba las parciales altas de una nota grave). La coherencia de fase, además,
**descarta ruido y ataques** sin umbrales de nivel: los transitorios pasan sin tocar, que es lo deseado.

Cada parcial recibe una región de ±3 bins (recortada en el valle entre parciales vecinas) y una frecuencia
interpolada parabólicamente sobre el logaritmo de la magnitud.

## 4. Transformación de una parcial

Para la parcial *j* con frecuencia *f*, edad efectiva *A*, y amplitudes globales de DEPTH/THRESHOLD:

```
q   = Eg · D^1.3 · ω(f; D, drift) · A^0.75            intensidad local, 0..1  (= 0 al nacer la nota: timbre natural)
λ   = clamp( q · EROSION^1.1 · 1.5 · h(f, t), 0, 0.97 )   fracción «perdida» de la parcial
Δf  = f · q · ( 0.010·E^1.3·β(t)·(f/1 kHz)²  +  0.010·E^1.2·w(f,t)·min(2,√(f/500)) )   desplazamiento (Hz)
```

* **Pérdida con transferencia a ruido.** La parcial conserva amplitud √(1−λ) y su lóbulo se copia como **banda de ruido
  de amplitud λ·|X|** con fases aleatorias deterministas. Con EROSION baja casi no hay ruido (λ² de potencia); con
  EROSION alta la estructura armónica se *disuelve* en una textura con la misma envolvente. Con DEPTH × EROSION altos
  el ruido se ensancha (hasta 3 pasadas de suavizado `[¼ ½ ¼]`, renormalizadas en potencia). Es la ruta hacia
  «sonido abstracto».
* **Desplazamiento inarmónico.** Término determinista ∝ f² (rigidez, como una barra/campana: los graves casi no se mueven,
  los agudos se estiran) más un término aleatorio suave por parcial (clave: su frecuencia absoluta, no su número de
  armónico, que no se conoce en señales polifónicas).
* **Cómo se desplaza una parcial sin vocoder de fase completo.** Un desplazamiento complejo de frecuencia *u* bins sobre
  la señal es, en el STFT, `Y_t[k] = e^{j2π·u·t·H/N} · X_t[k−s]`. Se copia el lóbulo `X[k]` a `k+s` (s = entero más
  próximo) y se multiplica por una rotación de fase **acumulada por parcial** (`θ += 2π·Δf·H/fs` por salto, heredada del
  parcial coincidente del fotograma anterior). El lóbulo viaja rígido, así que conserva sus fases relativas (no hay
  «phasiness»), y no se reconstruyen fases. El desajuste entre la posición del lóbulo (entera) y la frecuencia real
  (fraccionaria) es ≤ ½ bin: por eso se usa solape 8×. Medido: con EROSION ≤ 0,3, el componente espurio más
  fuerte de un seno está ≥ 44 dB por debajo de la señal.
* **Ruido inter-parcial.** Los bins que no pertenecen a ninguna parcial se atenúan lentamente (hasta −8 dB) según un campo
  suave ligado a la persistencia de las parciales cercanas: «erosión del suelo espectral» sin ser un filtro (el campo
  cambia con la señal y la deriva, y a EROSION ≤ 0,4 ninguna banda de octava se mueve más de 2 dB).

## 5. MEMORY — memoria tímbrica

No se guarda audio. Hay tres estados, todos espectrales:

1. **Forma espectral `M[k]`**: media con fugas de la potencia normalizada (suma 1), ponderada por la puerta de THRESHOLD, con
   constante de tiempo `τ = 0,04 s · 250^MEMORY` (0,04 s … 10 s). En silencio decae con ese mismo `τ`: **la memoria se
   disipa de forma controlada** (medido: al 10 % en 0,20 / 1,50 / 13,3 s para MEMORY 0,1 / 0,5 / 0,9).
2. **Persistencia `A[k]`** («edad» por bin): sube hacia 1 mientras el bin pertenece a una parcial sostenida (constante
   `0,08 + 4,5·THRESHOLD² s`) y baja con `τ` (mínimo 120 ms para que un parpadeo de detección no reinicie el desarrollo).
   **Es el mecanismo de la evolución en una nota sostenida:** al nacer, `A = 0` y la nota suena natural; cuanto más dura,
   más se transforma.
3. **Huella de parciales**: al cambiar de nota, las parciales nuevas que coinciden con parciales recordadas se favorecen y las
   que no se retienen (hasta ±9 dB, energía neutra entre parciales); a nivel de bandas ocurre lo mismo con el equilibrio
   espectral general (*imprint*). Con una nota mantenida la memoria converge a la nota y el efecto desaparece (medido:
   MEMORY 0 vs 1 en una nota ya desarrollada difiere 0,12 dB por bandas, 1,6 dB en los primeros segundos).

Además, una **parcial recién nacida hereda** `0,7·MEMORY` de la persistencia de las parciales cercanas (±16 bins) en lugar
de la edad que tuviera el bin bajo la nota anterior. Sin esta regla, en espectros armónicos densos el mapa por bin está
casi lleno y cualquier nota nueva «caería» sobre bins envejecidos (observado: edad 0,85 al instante, independientemente
de MEMORY).

La memoria **no produce repeticiones**: solo modula amplitudes de los bins de la señal *actual*. Test: tras terminar la nota,
la salida es exactamente silencio aunque MEMORY = 1.

## 6. DRIFT — evolución temporal

* Un **reloj** `τ` avanza a `(0,015 + 0,9·DRIFT^1,5)·(1 + 1,5·flujo)` celdas/s; el *flujo* es la distancia entre la forma
  espectral actual y la memoria, de modo que **el material que cambia hace evolucionar más rápido el timbre**.
* Los campos son **ruido de valor 2-D** (retícula en frecuencia × reloj, interpolación quíntica C²): suaves, sin saltos, y
  **funciones puras de (frecuencia, reloj, semilla)**. Misma semilla + mismo material ⇒ misma evolución, bit a bit
  (comprobado). No hay generador secuencial en este camino.
* `campo = (1−w)·ruido(f, 0) + w·ruido(f, τ)`, `w = DRIFT^0,8`. **DRIFT = 0 ⇒ w = 0 y el reloj no avanza: estático** (medido:
  las bandas se mueven 0,29 dB en 8 s). A DRIFT = 1 la parcial 11 se desvía ≈ 9 cents (desv. típica) y los niveles por bandas se mueven 2,5–5 dB (desv. típica).
* Qué se mueve: la susceptibilidad de cada parcial a la erosión, su micro-desafinación, el coeficiente de rigidez β, el
  corte de alcance de DEPTH (±0,9 octavas) y un campo de ganancias sobre el eje log-frecuencia (hasta ±7,5 dB, energía
  neutra) que desplaza la distribución espectral.
* **Semilla** (`SEED`, 1–99): cambia el patrón; se aplica mediante un reinicio sin clics. El reloj y el ruido de fase se
  reinician con la transportación (`requestReset` al pasar a *play*).

## 7. THRESHOLD

* **Nivel**: RMS del fotograma suavizado (ataque 20 ms, relajación 200 ms) frente a `−66 + 48·THRESHOLD` dBFS con rodilla
  suave de 12 dB. El ruido de fondo por debajo del umbral no activa el motor (medido: hum a −60 dBFS ⇒ transformación
  0,000 con THRESHOLD 0,9 y > 0,05 con 0,05).
* **Persistencia**: determina `τ_subida` de la edad (`0,08 + 4,5·T² s`): THRESHOLD alto = hay que *sostener* la nota (medido:
  mitad del efecto a 0,18 / 0,77 / 1,94 s para THRESHOLD 0,1 / 0,5 / 0,9).
* **Compromiso de enganche** global con ataque 50 ms y relajación 350 ms (evita bombeo).
* **Transitorios**: detector de *subida* (nivel > nivel de hace ~50 ms + 3 dB) ⇒ guarda con ataque 12 ms y relajación
  80–530 ms (crece con THRESHOLD) que atenúa hasta el 90 % de la intensidad. Los ataques pasan casi intactos
  (medido: los primeros 50 ms de una nota difieren −33 dB del original frente a +4 dB una vez desarrollada).

## 8. DEPTH

No es mezcla. Controla **cuánto** y **hasta dónde** transforma el motor: `D^1,3` escala todas las cantidades y el
corte `fc = 12 kHz·(80/12000)^D` define el *alcance*: ω(f) sube de 0 a 1 en una octava alrededor de `fc`. A DEPTH bajo solo
se tocan las parciales altas (medido a DEPTH 0,3: parciales 2–4 cambian < 1 dB, las 20–28 se desafinan ≈ 10 cents); a DEPTH 1
llega a las fundamentales. DEPTH 0 es transparente (−141 dB). Comprobado que DEPTH 0,5/BLEND 100 y DEPTH 1/BLEND 50 son
sonidos distintos (8 dB de distancia por bandas; la parcial 25 queda en 4 dB frente a 28 dB).

## 9. BLEND y OUTPUT

* `out = (1−b)·dry + b·wet`, con *dry* retardado N−1. **b = 0 devuelve la entrada bit a bit.**
* **Compensación de nivel**: se estiman (150 ms) la potencia del dry, del wet y su correlación cruzada; se escala la mezcla
  para que su potencia sea la media ponderada `(1−b)·P_d + b·P_w`, limitada a ±3 dB y suavizada 50 ms. Señales
  correlacionadas (efecto sutil) suman coherentemente y no se tocan; decorrelacionadas (efecto fuerte) pierden 3 dB a
  b = 0,5 y se compensan. Como los estimadores son lentos, las diferencias dinámicas entre dry y wet se respetan.
* OUTPUT: −24…+12 dB suavizado 20 ms (sin saltos ni *zipper*). Sin limitador: el procesado interno es en coma flotante y la
  suma de energía está acotada (energía conservada por la transferencia parcial→ruido).

## 10. Estéreo, mono, frecuencias de muestreo y bloques

* El plan de transformación (qué parciales, cuánto, hacia dónde) se calcula sobre el **espectro de potencia ligado** de
  L y R y se aplica **idéntico a ambos canales**: las diferencias de fase y nivel entre canales dentro de cada parcial se
  conservan, y el campo estéreo no se desplaza. Entrada L = R ⇒ salida L = R (L−R a −140 dB, solo redondeo); solo izquierda ⇒ derecha en
  silencio. El ruido de disolución usa la misma fase en ambos canales, con amplitud proporcional a la de cada uno.
* Mono: mismo código con R = 0.
* Todo el estado depende de la frecuencia de muestreo solo a través de N (≈ 40 ms) y de las constantes de tiempo en
  segundos. Salida bit-idéntica para tamaños de bloque 1, 7, 64, 480, 1024 y 4096.

## 11. Tiempo real

* `prepare()` reserva todo; `process()` no reserva memoria, no usa mutex, no hace E/S (comprobado con un `operator new`
  contador: 0 asignaciones en 10 s de audio estéreo con automatización y reinicios).
* Los únicos datos compartidos con otros hilos son `atomic` relajados (diagnósticos) y una bandera `atomic<bool>` de reinicio.
* Entradas no finitas se anulan y se limitan a ±10⁴; las salidas no finitas se anulan. Último recurso: si un
  fotograma saliera no finito se limpia el estado y se cuenta un fallo (`numericalFaults`); **las pruebas exigen 0 fallos**
  para que esta red de seguridad no esconda defectos (ya detectó uno durante el desarrollo: una suma deslizante mal
  inicializada daba una persistencia negativa y `sqrt(<0)`).
* Estados recursivos con *flush* a cero (sin desnormales); `ScopedNoDenormals` en el plugin.

## 12. Reinicios

* `requestReset()` (cualquier hilo): el wet se funde hacia el dry en 6 ms, se limpia el estado, y el wet vuelve en 40 ms.
  Sin clics (paso máximo entre muestras 0,020 frente a 0,017 de la onda seca). El plugin lo usa al empezar la reproducción y al cambiar SEED.
* `restart()` (solo hilo de audio, inmediato): deja el motor **idéntico a una instancia nueva**, incluida la rejilla de
  fotogramas (bit a bit). Para renders *offline* y herramientas.

## 13. Límites conocidos (honestos)

* **Latencia de 42,6 ms** (46,4 ms a 44,1 kHz): inherente a la resolución buscada. Compensada por el host.
* **Suavizado temporal de la ventana (43 ms)**: los ataques se protegen con la guarda de transitorios, pero la
  transformación de una nota ya desarrollada no sigue cambios de menos de ~40 ms.
* **El desplazamiento de parciales depende de que la detección las encuentre**: en ruido puro o percusión sin
  afinación casi no hay parciales; ahí solo actúan el suelo espectral, la deriva y el *imprint* (el efecto es mucho más
  sutil). Voces y platos metálicos sí se procesan.
* **Parciales que cruzan** (glissandos rápidos, vibrato extremo) pueden perder la continuidad de la rotación de fase y
  producir un pequeño «bache» de fase de un fotograma (suavizado por el solape).
* **La frecuencia fundamental no se estima**; todo se hace por frecuencia absoluta. Es una decisión (polifonía) y una
  limitación: la rigidez ∝ f² no sabe a qué armónico corresponde cada parcial.
* **Nadie ha escuchado esto.** Las pruebas son mediciones objetivas (espectros, frecuencias de parciales, niveles,
  trazas de estado). Demuestran que cada control hace algo medible, distinto y acotado; no pueden decidir si suena bien. Las
  constantes (`1.5`, `0.010`, `0.7`, etc.) son puntos de partida razonados que necesitan oído.
