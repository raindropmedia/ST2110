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
