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
- **Verdacht (noch zu bestätigen):** Beide Ports der E830 teilen sich einen Quell-Timer (DPDK `ice`:
  „shared source timer“); MTL startet je Port eine PTP-Instanz, und beide stellen denselben Timer nach. Hinweis
  im Log: Port 1 sieht nach dem Start keinen 37-s-Sprung, Port 0 schon. Gegenprobe: Start ohne `--tx-port`
  (nur eine Instanz) oder mit `patches/mtl/v26.09/0002-…`; dann sollte die Streuung deutlich sinken.
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
