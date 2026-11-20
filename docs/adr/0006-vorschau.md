# ADR 0006: Bild- und Tonvorschau im Browser ohne Codec

- Status: angenommen, geändert am 2026-11-20 (JPEG, wählbare Größe und Bildrate; Demo und Wandlung
  getestet, MTL-Pfad nicht auf Hardware)
- Datum: 2026-11-19

## Kontext

Wie bei PRISM und QxL soll man den Inhalt des gewählten Flows sehen bzw. hören, um Verwechslungen
(falsche Kamera, falsches Kanalpaar, Stille) schnell zu erkennen. Der Messplatz soll dafür keine
Codec-Bibliothek brauchen und den Datenpfad von MTL nicht belasten.

## Änderung 2026-11-20: höhere Qualität

Rohes RGB begrenzte die Vorschau auf kleine, langsame Bilder (480 Pixel, 5 Bilder/s ≈ 2 MB/s; volle
Auflösung mit 25 Bildern/s wären über 1 Gbit/s). Seitdem gilt:

- Vorschaubilder werden als **JPEG** übertragen (`stb_image_write`, gemeinfrei, nur Header, fester Commit
  über FetchContent, nur Callback-Ausgabe ohne Dateizugriff). Größe (480, 960, voll, UHD), Bildrate
  (1–50 Bilder/s) und Qualität wählt die Oberfläche je Abruf.
- Der `PreviewWorker` gibt den MTL-Rahmen direkt nach der Farbumrechnung zurück; kodiert wird danach.
- Farbumrechnung über Tabellen in Festkomma; Halbbilder werden für volle Höhe zeilenverdoppelt.
- Abruf mit **Long Polling** (`after=<Bildnummer>`, höchstens 250 ms Wartezeit): Den Takt gibt das
  Messgerät auf einem festen Raster der Bildrate vor; bei langsamer Verbindung sinkt die Rate von selbst.
- Der Webserver setzt **TCP_NODELAY**: Ohne das wartete jede Antwort rund 40 ms auf das verzögerte ACK
  des Browsers (Nagle), was die Vorschau bei etwa 23 Bildern/s deckelte.

Gemessen (Demo, Release-Build, ein Kern je Aufgabe; Testbild, echte Bilder sind größer):

| Größe | Umrechnung | JPEG (Qualität 80) | Größe je Bild | erreichte Rate |
| --- | --- | --- | --- | --- |
| 480 × 270 | 0,8 ms | 1,5 ms | 7 kB | wie eingestellt |
| 960 × 540 | 2,4 ms | 3,9 ms | 17 kB | 50 Bilder/s |
| 1920 × 1080 | 7,4 ms | 13,6 ms | 49 kB | 25 bzw. 49 Bilder/s |

## Entscheidung (ursprünglich)

- **Bild:** Nur solange die Oberfläche die Vorschau abfragt (Abruf hält sie 3 s aktiv), gibt der
  Bildrückruf höchstens alle 200 ms einen Rahmen an einen eigenen Thread (`PreviewWorker`). Dieser
  wandelt YCbCr 4:2:2 10 Bit (RFC 4175, BT.709) durch Auslassen in ein RGB-Bild von höchstens 480 Pixel
  Breite und gibt den Rahmen danach an MTL zurück. Die Oberfläche holt es roh über
  `GET /api/preview/video` und zeichnet es in einen Canvas.
- **Ton:** Der Tonrückruf schreibt PCM (L16/L24) in einen Ringpuffer von 2 s und führt Spitzenpegel je
  Kanal. `GET /api/preview/audio` liefert ab einem Abtastzähler ein Kanalpaar als 16 Bit stereo; die
  Oberfläche spielt es über die Web Audio API mit rund 0,3 s Vorlauf ab und zeigt Pegel aller Kanäle.
- **Demo:** Testbild (Farbbalken, Grautreppe, bewegtes Quadrat, je Kamera anders) und Testton
  (1 kHz links mit EBU-Kennung, 2 kHz rechts, −18 dBFS).

## Begründung

- Keine Abhängigkeit (kein JPEG/H.264), funktioniert im Messnetz ohne Internet.
- Rohdaten sind klein genug: 480 × 270 × 3 Byte ≈ 390 kB je Bild, bei 5 Bildern/s knapp 2 MB/s im LAN.
- Der Datenpfad von MTL wartet nie: Der Rückruf reicht nur einen Zeiger weiter; ist die Wandlung noch
  beschäftigt, wird der Rahmen sofort zurückgegeben.

## Alternativen

- JPEG mit libjpeg-turbo: schneller, aber Systempaket bzw. größerer Bau; `stb_image_write` reicht für
  Vorschauraten (siehe Änderung).
- Weiterleitung als Videostrom (z. B. über GStreamer nach WebRTC): echte Bewegtbilder, aber deutlich
  aufwendiger; das ist eher Aufgabe des Studio-Gateways.
- Vorschau über einen zweiten Empfänger (z. B. ffplay mit SDP): braucht Zugang zum Messnetz am Arbeitsplatz.

## Folgen

- Die Vorschau ist eine Inhaltskontrolle, keine Bildbeurteilung (Auslassen statt Filtern, JPEG).
- Ein Rahmen ist während der Farbumrechnung belegt (bei voller Größe etwa 7 ms); `framebuff_cnt` ist
  deshalb 4.
- Bei mehreren Betrachtern gelten die zuletzt angeforderten Einstellungen.
- Beim Entfernen eines Flows wartet `FlowSession::free()`, bis der Rahmen zurückgegeben ist.
- Beschädigte Bilder (Paketverlust) werden nicht gewandelt.
