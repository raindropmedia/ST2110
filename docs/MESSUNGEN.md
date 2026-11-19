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

Noch keine Messungen auf Hardware.

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
