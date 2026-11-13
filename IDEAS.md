# Ideen für weitere Filter/Effekte

Lose Sammlung möglicher Folgeprojekte, nicht Teil des aktuellen Scopes.

* **Audio-Glitch** (analog zum JPEG-Trick): PCM verlustbehaftet enkodieren
  (z. B. Opus/MP3), Bits im komprimierten Stream flippen, wieder dekodieren
  -> Codec-Artefakte (Knacken, Musical Noise). Für einen
  Kurzwellen-Störfunk-Sound zusätzlich mit Bandpass/AM-Rauschen kombinieren.
* **"Leierndes Band" / Wow & Flutter**: periodische Tonhöhenschwankung durch
  LFO-modulierte Resampling-Rate, wie ein ausgeleiertes Kassettenband.
  Einfacher Audio-Filter, kein Bitflip-Ansatz nötig -- eigenständiger Filter
  neben Qlitch, nicht dessen Erweiterung.
* **Codec-Glitch auf Stream-Ebene**: statt JPEG die tatsächlichen
  Video-Codec-Frames (H.264/AV1 NAL-Units) glitchen -- näher an echten
  Broadcast-Störungen.
* **Simulierter Netzwerk-Paketverlust**: Freeze-Frames/Audio-Stottern wie
  bei schlechter Videotelefonie.
