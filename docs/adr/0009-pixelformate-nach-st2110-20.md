# ADR 0009: Alle Pixelformate von ST 2110-20 erkennen, überwachen und senden

- Status: angenommen (Bau gegen MTL v26.09, Unit-Tests und Demo geprüft; Empfang und Senden der Nicht-Standardformate noch nicht auf Hardware)
- Datum: 2026-11-24

## Kontext

Der Messplatz kannte nur YCbCr 4:2:2 mit 10 Bit. ST 2110-20 beschreibt viele weitere Formate; MTL v26.09 kennt als
RFC-4175-Transportformate `st20_fmt` genau 16: YCbCr 4:2:2, 4:2:0, 4:4:4 und RGB mit je 8, 10, 12 und 16 Bit (dazu die
nicht normgerechten Formate PLANAR10LE und V210). Nicht unterstützt sind 16-Bit-Gleitkomma (16f), XYZ, CLYCbCr, ICtCp,
Key und Tiefe 14. „Alle Formate von ST 2110-20“ heißt hier: alle, die MTL kann.

## Entscheidung

- **`PixelFormat`** (Abtastung, Tiefe) in `src/core/`; Pixelgruppen (Bytes und Bildpunkte) werden aus Blockgröße und Tiefe
  berechnet und stimmen mit `st20_pgroups[]` von MTL überein (Test). `VideoFormat` trägt ein `PixelFormat`, `VideoFlow`
  zusätzlich den Packmodus (BPM, GPM, GPM_SL; SDP `PM=2110BPM|2110GPM`). Vorgabe bleibt 4:2:2 10 Bit, BPM; Formatnamen ändern
  sich dafür nicht.
- **Überwachen:** `ops.fmt` und `ops.packing` der `st20_rx`-Sitzung kommen aus dem Flow. Der Timing-Parser von MTL ist
  formatunabhängig. Die Vorschau rechnet nur 4:2:2 10 Bit um und ist sonst aus.
- **Erkennen:** Aufeinanderfolgende Einzelsegmente derselben Zeile liefern Bytes und Bildpunkte je Segment; jedes der 16
  Formate, dessen Pixelgruppe dazu passt (mindestens 90 % von mindestens 8 Stichproben), ist Kandidat. Die Breite folgt aus
  dem Ende des letzten Segments der letzten Zeile, die Höhe aus der Zeilenzahl, die Rate aus den Zeitstempeln. **Formate mit
  gleicher Pixelgruppe sind auf der Leitung nicht zu unterscheiden** (YCbCr 4:4:4 und RGB gleicher Tiefe; 3 Byte je Bildpunkt
  trifft auf RGB 8, 4:4:4 8, 4:2:2 12 und 4:2:0 16 Bit zu). Der Messplatz nennt das häufigste und führt die übrigen auf; die
  Rangfolge ist eine Annahme. GPM ist nur sicher, wenn ein Paket mehrere Zeilensegmente trägt; sonst „BPM (oder GPM)“.
  Bei 4:2:0 gilt wie in MTL: Der Puffer besteht aus Zeilen zu 1,5 Byte je Bildpunkt; Breite und Höhe werden entsprechend
  aus den Zeilennummern der Pakete gelesen.
- **Senden:** `PixelPatternGenerator` erzeugt das Testbild (gleicher Inhalt wie bisher) für alle 16 Formate selbst als Folge
  von Pixelgruppen (RFC-4175-Reihenfolge, MSB zuerst; YCbCr begrenzter Bereich, BT.709; RGB voller Bereich). Für 4:2:2 10 Bit
  ist die Ausgabe bitgleich zum bisherigen `TestPatternGenerator` (Test). Der Standardfall (4:2:2 10 Bit, BPM) läuft
  unverändert über `st20p_tx`; alle anderen Formate und GPM senden über `st20_tx` auf Frame-Ebene (drei Puffer, Erzeuger-Thread,
  `get_next_frame` liefert nur gefüllte Puffer). MTL-Wandler wären auf wenige Paare beschränkt (`st_frame_fmt_from_transport`
  kennt für 8- und 16-Bit sowie 4:2:0 keinen Rahmenformat-Gegenpart). 4:2:0 gibt es nur progressiv.

## Folgen

- Nicht auf Hardware geprüft: `st20_tx` auf Frame-Ebene mit den neuen Formaten, die Pufferdaten von 4:2:0 (Reihenfolge der
  Pixelgruppen je Blockzeile nach der MTL-Packetierung) und der Empfang der Nicht-Standardformate.
- Beim Start prüft der Testsender die Puffergröße von MTL (`st20_tx_get_framebuffer_size`) gegen die erzeugte und meldet
  eine Abweichung als Fehler.
