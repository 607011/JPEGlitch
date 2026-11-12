# Projekt: qlitch-obs – JPEG-Glitch-Filter als natives OBS-Studio-Plug-in

## Ziel

Baue ein natives OBS-Studio-Plug-in (C, notfalls C++17), das den „qlitch“-Effekt live auf Videoquellen anwendet. Der Ablauf: Webcam-Bild rein → als JPEG kodieren → Bits in den komprimierten Bilddaten kippen, setzen oder löschen → wieder dekodieren → verglitchtes Bild raus. Die Ausgabe läuft über die normalen OBS-Wege (Stream, Aufnahme, **virtuelle Kamera**).

Es soll ein **richtiges Plug-in** werden, das man unter Windows, macOS und Linux installieren kann. Ausdrücklich nicht gewollt sind Shader-Nachbauten, Browserquellen oder Fensteraufnahme-Tricks.

## Hintergrund

- Original: Qt-Desktop-App von 2013, https://github.com/607011/qlitch (GPLv3). Der Glitch-Kern steht in `qlitch/mainwindow.cpp`, Methode `MainWindow::updateImageWidget()`.
- Es gibt bereits eine Web-Portierung als einzelne HTML-Datei (`reference/qlitch.html`, falls sie im Repo liegt). Ihre Funktion `glitchOnce()` enthält denselben Kern in kompaktem JavaScript inklusive MT19937 und ist die maßgebliche Referenz für das Verhalten.
- Ein Pixel-Shader kann den Effekt nicht nachbilden, weil er im **komprimierten JPEG-Bitstrom** entsteht. Es braucht also einen echten JPEG-Codec auf der CPU.

## Glitch-Kern (exakt so umsetzen)

Eingaben: JPEG-Bytes `raw[0..n)`, Parameter wie unten unter „Filter-Eigenschaften“ beschrieben.

1. Bestimme `headerSize`: die erste Fundstelle `i` mit `raw[i] == 0xFF && raw[i+1] == 0xDA` (SOS-Marker); `headerSize = i + 2`. Wenn nichts gefunden wird, ist `headerSize = 0`.
2. `firstPos = min(n - 3, headerSize + floor((n - headerSize) * start))`, wobei `start` ∈ [0, 1] ist.
3. Ist **„Fester Seed“** aktiv, wird der MT19937 vor **jedem** Frame mit `seed` neu initialisiert. Dann liegen die Defekte stabil an denselben Stellen. Andernfalls läuft der Generator einfach weiter.
4. Algorithmus auf ein Byte `o` mit Bitmaske `bit`: XOR → `o ^ bit`, Eins → `o | bit`, Null → `o & ~bit` (jeweils `& 0xFF`).
5. **Einzelbit-Modus:** Es wird nur `raw[firstPos]` verändert, mit `bit = 1 << (flipBit++ % 8)`. Der Zähler `flipBit` bleibt über die Frames hinweg erhalten.
6. **Normalmodus:** Der folgende Schritt wird `defects`-mal ausgeführt:
   - `pos = rnd(firstPos, n - 3)`, wobei gilt: `rnd(lo, hi) = lo + mt() % (hi - lo + 1)`; vertausche `lo` und `hi`, falls `lo > hi`.
   - Ist „0xFF nicht anfassen“ aktiv und `raw[pos] == 0xFF`, wird diese Iteration übersprungen.
   - `bit = 1 << (mt() % 8)`, danach den Algorithmus anwenden.
7. In beiden Modi gilt: Ist „0xFF verhindern“ aktiv und das neue Byte wäre `0xFF`, bleibt das alte Byte stehen.
8. Die letzten beiden Bytes (EOI-Marker `FF D9`) werden nie angefasst. Das ergibt sich aus der Obergrenze `n - 3`.

Implementiere den Kern als **eigenständige, OBS-freie C-Bibliothek** (`src/glitch/`) mit eigenem MT19937. Nur das Plug-in hängt an libobs.

## Architektur des Plug-ins

- **Grundlage:** die offizielle Vorlage https://github.com/obsproject/obs-plugintemplate (CMake, GitHub Actions für alle drei Plattformen). Prüfe die aktuelle Version der Vorlage und der OBS-API, statt Annahmen aus dem Gedächtnis zu treffen.
- **Filtertyp:** asynchroner Videofilter (`OBS_SOURCE_TYPE_FILTER`, `OBS_SOURCE_VIDEO | OBS_SOURCE_ASYNC`) mit `filter_video`-Callback. Er greift bei asynchronen Quellen wie „Videoaufnahmegerät“ (Webcam) und „Medienquelle“ und bekommt die Bilder als `struct obs_source_frame` im RAM.
  - Kläre anhand der OBS-Doku und des Quellcodes bestehender Async-Filter, ob der Frame direkt verändert werden darf oder kopiert werden muss. Halte diese Entscheidung im Code fest.
- **Codec:** libjpeg-turbo mit TurboJPEG-3-API. Kodiere und dekodiere **direkt in YUV-Ebenen**, damit keine RGB-Umrechnung nötig ist (aus YUV-Ebenen komprimieren, in YUV-Ebenen dekomprimieren). Prüfe die genauen Funktionsnamen und Parameter in den Headern der eingebundenen Version.
  - Kaputte Daten sind hier der Normalfall: `TJPARAM_STOPONWARNING` bleibt aus, und ein Fehler vom Typ `TJERR_WARNING` gilt als Erfolg.
  - Bei einem echten (fatalen) Fehler wird das letzte gute Ergebnis ausgegeben, ersatzweise das unveränderte Bild. Der Filter darf niemals abstürzen oder ein schwarzes Bild liefern.
  - Achte auf Ebenenbreiten und -höhen bei Unterabtastung und MCU-Padding. Nutze dafür die Hilfsfunktionen von TurboJPEG.
- **Pixelformate:**
  - Mindestens `VIDEO_FORMAT_NV12` (häufigstes Webcam-Format): UV entschachteln, als 4:2:0 kodieren und wieder verschachteln.
  - Außerdem `I420`, `YUY2` (als 4:2:2), `I444`, falls sinnvoll auch `UYVY`.
  - Nicht unterstützte Formate werden unverändert durchgereicht, mit **einmaliger** Warnung im Log (`blog(LOG_WARNING, …)`).
  - Farbraum- und Farbbereichs-Metadaten des Frames bleiben unverändert, weil nur der YUV-Inhalt durch den Codec geht.
- **Threading:** `update()` und `filter_video()` laufen auf verschiedenen Threads. Schütze die Parameter deshalb (Mutex oder atomarer Snapshot pro Frame).
- **Performance:**
  - TurboJPEG-Handles und Puffer werden wiederverwendet; keine Allokation pro Frame, außer wenn sich die Auflösung ändert.
  - Ziel: 1080p30 auf einem aktuellen Rechner. Logge im Debug-Build die Zeit pro Frame.
  - Optional ist eine Eigenschaft „Verarbeitungsauflösung“, die vor dem Glitch verkleinert und danach wieder hochskaliert. Das kommt erst, wenn die Messungen es nötig machen.

## Filter-Eigenschaften (`obs_properties`)

| Eigenschaft | Typ | Bereich / Standard |
|---|---|---|
| Start (Position im Bitstrom) | Float-Slider in % | 0–100, Schritt 0.01, Standard 0.6 |
| Defekte | Int-Slider | 0–200, Standard 100 |
| JPEG-Qualität | Int-Slider | 0–100, Standard 50 |
| Algorithmus | Liste | XOR (Standard), Eins, Null |
| Einzelbit-Modus | Bool | aus |
| Fester Seed | Bool | aus |
| Seed | Int | 0–100000000, Standard 10000; nur aktiv, wenn „Fester Seed“ an ist (`obs_property_set_modified_callback`) |
| 0xFF-Bytes verhindern | Bool | an |
| 0xFF-Bytes nicht anfassen | Bool | an |
| Neues Muster alle N Frames | Int | 1–300, Standard 1 (1 = jeder Frame neu; größere Werte halten dasselbe Zufallsmuster N Frames lang) |

Lokalisierung: `data/locale/en-US.ini` und `data/locale/de-DE.ini`. Der Anzeigename des Filters ist „Qlitch (JPEG Glitch)“.

## Tests

- Unit-Tests für den Glitch-Kern mit CTest, ohne OBS:
  - MT19937-Testvektor: Seed 5489 → erste Ausgabe 3499211612.
  - `rnd()`-Grenzen und das Vertauschen von `lo` und `hi`.
  - SOS-Suche, auch ohne Marker.
  - Determinismus mit festem Seed (gleicher Input → gleiche Bytes).
  - EOI bleibt intakt.
  - Die 0xFF-Optionen.
- Ein kleines CLI-Testprogramm (`tools/qlitch-cli`): JPEG rein, Parameter, verglitchtes JPEG und PNG raus. Damit lässt sich der Kern ohne OBS ausprobieren und mit der Web-Version vergleichen.
- Ein Codec-Test, der absichtlich zerschossene JPEGs dekodiert und prüft, dass immer ein Bild herauskommt.

## Build & Verteilung

- libjpeg-turbo wird **statisch** eingebunden (FetchContent oder ExternalProject), damit Nutzer nichts nachinstallieren müssen. Unter Linux ist die Systembibliothek per CMake-Option als Alternative erlaubt.
- CI über die GitHub-Actions der Vorlage: Windows (x64), macOS (Universal: arm64 + x86_64), Linux (Ubuntu, `.deb`).
- Die macOS-Signierung und -Notarisierung wird vorbereitet (Secrets-Platzhalter wie in der Vorlage), muss aber nicht laufen.
- Lizenz: GPLv3 wie das Original-qlitch (verträglich mit OBS' GPLv2-or-later). Die Lizenzhinweise von libjpeg-turbo (IJG/BSD-artig) gehören in die Distribution. Prüfe die Kompatibilität und dokumentiere sie im README.

## Vorgehen in Meilensteinen

1. Repo aus `obs-plugintemplate` aufsetzen, leerer Filter, der sichtbar in OBS auftaucht.
2. Glitch-Kern als C-Bibliothek plus Unit-Tests plus CLI.
3. Filter mit NV12 auf der eigenen Plattform, Ende-zu-Ende mit Webcam und virtueller Kamera.
4. Weitere Pixelformate, Fehlerrobustheit, Performance-Messung.
5. Eigenschaften komplett, Lokalisierung, „Neues Muster alle N Frames“.
6. CI für alle Plattformen grün, README mit Installation, Screenshots-Platzhaltern und Lizenzhinweisen.

Arbeite die Meilensteine nacheinander ab. Committe nach jedem abgeschlossenen Schritt und fasse kurz zusammen, was jetzt geht. Frag mich, bevor du von dieser Architektur abweichst, also zum Beispiel FFmpeg statt libjpeg-turbo nimmst, auf RGB umstellst oder auf C++ wechselst. Wenn eine Annahme hier im Prompt nicht zur aktuellen OBS- oder TurboJPEG-API passt, gilt die API. Sag mir dann, was du anders gemacht hast.

## Bewusst nicht im ersten Wurf

- Unterstützung für **synchrone** Quellen (Szenen, Bildschirmaufnahme). Dafür bräuchte man einen Render-Filter mit Rücklesen über Stage-Surfaces von der GPU. Diese Variante ist für später vorgesehen; die Kernbibliothek soll sie aber nicht verbauen.
- Eigene Qt-Docks oder eigene Oberflächen. Die Eigenschaften-API reicht.

## Abnahmekriterien

- In OBS 30+ (aktuelle Version prüfen) gibt es unter „Filter“ bei einem Videoaufnahmegerät den Eintrag „Qlitch (JPEG Glitch)“.
- Webcam-Bild in 720p und 1080p wird flüssig (≥ 30 fps bei 720p) verglitcht. Alle Regler wirken live, ohne Neustart der Quelle.
- Mit festem Seed ist das Glitch-Muster stabil, ohne festen Seed flackert es zufällig.
- Die virtuelle Kamera zeigt das verglitchte Bild in einer Videokonferenz-App.
- Keine Abstürze, keine Speicherlecks (unter Linux mit ASan oder Valgrind prüfen), auch bei extremen Einstellungen (Qualität 0, Start 0 %, 200 Defekte).
- Unit-Tests und CI sind grün.
