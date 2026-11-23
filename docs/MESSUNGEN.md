# Messungen

Jede Messung mit Datum, Aufbau und Versionen festhalten. Rohdaten (`*.jsonl`) gehören nach
`config/local/` und nicht ins Repository; hier nur Ergebnis und Bewertung.

## Vorlage

| Feld | Wert |
| --- | --- |
| Datum | |
| Rechner, CPU, isolierte Kerne | |
| Karte, PCI-ID, Firmware (NVM), ice-Treiber | |
| MTL-Version, DPDK-Version | |
| Port-Betrieb | PF / VF |
| PTP | Grandmaster, Switch, Domäne, Abstand zum Master |
| Sender | Gerät, Firmware, Pacing laut Hersteller |
| Format | z. B. 1920x1080p50, 10 Bit 4:2:2, Payload-Typ |
| Ergebnis st20-timing | Urteil, Cinst max, VRX max, FPT, Latenz |
| Abgleich EBU LIST | Werte aus dem Mitschnitt |
| Bewertung | |

## Ergebnisse

Noch keine Messungen von Strömen. Beobachtungen beim Einrichten (2026-11-23, i7-4790, E830-L, Ubuntu 26.04.1,
Kernel 7.0, MTL v26.09, DPDK 26.07, Switch MikroTik CRS309 ohne PTP-Funktion, Grandmaster ST 2059-2, Domäne 127):

- **PTP im Betrieb:** `ptp4l` am Kernel-Port: 20–30 ns rms, Laufzeit rund 5 µs. Mit MTL streut die Korrektur
  je Sync um ±1,0–1,4 µs (`correct_delta` −1357 … +1454 ns auf beiden Ports) und liegt oft knapp über der
  Einrastschwelle von 1000 ns. Der erste Sync stellt die Uhr um 37 s (UTC → TAI); das zählt als Zeitsprung.
- **Erste Messung der Kette Sender → Switch → Empfänger (2026-11-23, 17:14, MTL mit Patch 2, `--lcores 2,3`):**
  Testbild 1920x1080p50 (YCbCr 4:2:2, 10 Bit) und drei Töne (L24, 1 ms) von Port 1 auf Port 0 über den CRS309,
  Sender und Empfänger in einer MTL-Instanz. Im 10-s-Fenster:

  | Strom | gesendet | empfangen |
  | --- | --- | --- |
  | Video | 2 057 464 Pakete, 50,0000 Bilder/s, 2178 Mbit/s | 2 057 456 Pakete, 50,0005 Bilder/s |
  | Ton ×3 | je 10 000 Pakete, 100,001 Blöcke/s | je 10 000 Pakete |

  Keine Verluste in den Zählern von MTL (Paketzahlen gleich bis auf die Fensterkante, `drop 0`, `epoch drop` nur
  beim Start). Der Sender gibt zusätzlich rund 3,3 % Füllpakete ab (`pkts burst` 2 125 962 gegenüber 2 057 464
  Videopaketen; `DEV(1)` 2262 gegenüber `DEV(0)` 2190 Mbit/s); der Switch gibt sie nicht weiter. CPU: Scheduler-Kern
  `avg loop` 381 ns bei 7 Tasklets, Video-Empfang „cpu busy“ 2,7 %.
- **PTP mit Patch 2 und Last (ebenda):** Korrektur Mittel 1368 ns (Drift, erwartet 1410) bei Streuung 883 bis 1853 ns;
  `correct_delta` im Mittel 105 ns (−512 … +486); Pfadlaufzeit 5054 bis 5594 ns (ohne Last 5042 bis 5093). Das
  doppelte Nachstellen ist weg (Mittel 1368 statt 706), die größere Streuung kommt von der Warteschlange im Switch
  ohne PTP-Unterstützung (rund +500 ns Laufzeitschwankung bei 2,2 Gbit/s).
- **PTP, was die Korrektur bedeutet:** MTL stellt die Kartenuhr bei jedem Sync um den gemessenen Versatz
  zurück. Die gemeldete Korrektur ist deshalb im Kern die Drift des Oszillators je Sync-Intervall: 11,3 ppm
  (`ptp4l`: −11,3 ppm) × 125 ms = 1,41 µs. Mit einer PTP-Instanz bleibt sie konstant (beobachtet 1374 bis 1433 ns,
  also ±30 ns Messrauschen, wie bei `ptp4l`). Die Güte zeigt die Streuung um diesen Mittelwert, nicht der
  Absolutwert; der Monitor bewertet deshalb die Abweichung vom Median der letzten 8 Syncs.
- **Beide Ports stellen denselben Timer nach (belegt, 2026-11-23, 16:36 bis 16:38):** Die Ports der E830 teilen sich
  einen Quell-Timer (DPDK `ice`: „shared source timer“); MTL startet je Port eine PTP-Instanz, beide stellen nach. Das
  Muster passt genau: Korrektur im Mittel 706 ns (= halbe Drift) mit wechselnden Werten zwischen etwa 0 und 1440 ns;
  ein Modell mit doppelter Korrektur ergibt 701 ns, mit einfacher 1412 ns (beobachtet in den Phasen, in denen nur
  eine Instanz nachstellte: 1408 ns). Folge: Spannweite des Zeitfehlers doppelt so groß, Port 1 sieht keinen
  37-s-Sprung. Behebung: `patches/mtl/v26.09/0002-…`. Nach dem Patch auf Hardware noch zu bestätigen
  (Erwartung: konstante Korrektur um 1,4 µs, `correct_delta` nahe 0).
- **Audio mit Paketzeit unter 0,5 ms** (125, 250, 333 µs): MTL taktet per Ratenbegrenzung, belegt je Sitzung
  zwei Sende-Queues (`ST30_TX_RL_QUEUES_USED`) und sendet je Tonpaket 3 Füllpakete in voller Größe (rund 380 Mbit/s
  bei einem Tonsender mit 125 µs; `DEV(1)` 2556 Mbit/s bei 2178 Mbit/s Video). Ton ab 1 ms taktet per TSC und teilt
  sich eine Queue. Das Anlegen eines dritten Senders dieser Art scheiterte mit `init rl fail -5`, weil nur fünf Queues
  reserviert waren; jetzt `2 × --max-tx-flows + 1`.
- **Sende-Queues am E830-Port (2026-11-23):** Mit 9 angeforderten Sende-Queues scheiterte der Start:
  `ICE_DRIVER: ice_tx_queue_start(): Failed to add lan txq`, `fail to start Tx queue 8`, danach
  `Device failed to Start`, MTL fiel für Port 1 auf TSC-Pacing zurück und `mtl_init` scheiterte (die Queues 0 bis 7
  liefen; Ursache in Firmware oder Scheduler, nicht aufgeklärt). Seit dem Fix für die Tonsitzungen verlangte
  das Programm 2 × `--max-tx-flows` + 1 = 9; jetzt höchstens 7 (`--tx-queues` überschreibt). Mit höchstens 7 Queues
  passen ein Video und drei Töne mit 125 µs, oder ein Video und beliebig viele Töne ab 1 ms (teilen sich eine Queue).
- **`--lcores 2,3`:** MTL übergibt `-l 0,2,3` und DPDK nummeriert die Kerne neu (`--remap-lcore-ids`); „lcore 1“ im
  Log ist damit CPU 2. Ohne die Angabe lief der Scheduler auf CPU 1, die nicht isoliert war.

## Offene Prüffragen

Aus dem Quelltext von MTL v26.09 abgeleitet, auf Hardware und gegen EBU LIST zu bestätigen:

1. **IPT in MTL auf 256 ns gerastert.** `rv_tp_on_packet` rechnet `(double)pkt_time - prev_pkt_time`.
   Bei TAI-Zeit um 1,8·10¹⁸ ns hat ein `double` eine Auflösung von 256 ns, die Differenz ist damit auf
   Vielfache von 256 ns gerundet. `ipt_min`/`ipt_max` von MTL sind entsprechend grob; die eigene
   Auswertung (`VideoPacketAnalyzer`) rechnet ganzzahlig. Ähnlich, aber schwächer betroffen: FPT und VRX
   (Sollzeiten in `double`, Fehler bis etwa 0,06 TRS).
2. **Cinst nach MTL-Formel.** MTL berechnet Cinst kumulativ ab dem ersten Paket des Bildes mit
   Abflussfaktor 1,1 (`pkt_idx - (t - t_first)/TRS·1,1`, nach unten auf 0 begrenzt). Bündel spät im Bild
   wirken damit kaum (Demo „Bündel“: Cinst nur am Bildanfang hoch). Ob das dem Modell C aus ST 2110-21
   entspricht oder ein Eimermodell mit Untergrenze je Paket gemeint ist, mit Normtext und EBU LIST klären.
   Die eigene Auswertung folgt bis dahin MTL.
3. **Grenzwerte Ton.** MTL setzt DPVR narrow auf 3 und wide auf 19 Paketzeiten, TSDF narrow auf 1 und
   wide auf 17 Paketzeiten. Mit ST 2110-30/AES67 und Herstellergeräten vergleichen.
4. **FPT und Netzlaufzeit.** MTL prüft `fpt > tr_offset` ohne Abzug der Netzlaufzeit; ein Pfad mit mehr
   Laufzeit (ST 2022-7) kann daran scheitern, obwohl der Sender korrekt ist.
5. **PTP-Zustand.** „Eingerastet“ heißt hier: die letzten 8 Korrekturen unter 1 µs. Schwelle an die
   gemessene Streuung anpassen.
