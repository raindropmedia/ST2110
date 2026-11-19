# ADR 0006: Bild- und Tonvorschau im Browser ohne Codec

- Status: angenommen (Demo und Wandlung getestet, MTL-Pfad nicht auf Hardware)
- Datum: 2026-11-19

## Kontext

Wie bei PRISM und QxL soll man den Inhalt des gewählten Flows sehen bzw. hören, um Verwechslungen
(falsche Kamera, falsches Kanalpaar, Stille) schnell zu erkennen. Der Messplatz soll dafür keine
Codec-Bibliothek brauchen und den Datenpfad von MTL nicht belasten.

## Entscheidung

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

- JPEG im Messgerät (libjpeg-turbo): kleinere Datenmengen, aber eine weitere Abhängigkeit.
- Weiterleitung als Videostrom (z. B. über GStreamer nach WebRTC): echte Bewegtbilder, aber deutlich
  aufwendiger; das ist eher Aufgabe des Studio-Gateways.
- Vorschau über einen zweiten Empfänger (z. B. ffplay mit SDP): braucht Zugang zum Messnetz am Arbeitsplatz.

## Folgen

- Die Vorschau ist eine Inhaltskontrolle, keine Bildbeurteilung (Auslassen statt Filtern, ≤ 5 Bilder/s).
- Ein Rahmen ist während der Wandlung belegt; bei `framebuff_cnt = 3` bleiben zwei für den Empfang.
- Beim Entfernen eines Flows wartet `FlowSession::free()`, bis der Rahmen zurückgegeben ist.
- Beschädigte Bilder (Paketverlust) werden nicht gewandelt.
