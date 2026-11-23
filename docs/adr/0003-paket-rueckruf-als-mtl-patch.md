# ADR 0003: Paket-Rückruf als eigener Patch für MTL

- Status: vorläufig angenommen (bis zur ersten Messung auf Hardware)
- Datum: 2026-11-19

## Kontext

Interpacket-Ansichten wie bei Telestream PRISM oder Phabrix QxL (IPT-, Cinst- und VRX-Histogramme,
Ankunft jedes Pakets eines Bildes) brauchen die Ankunftszeit **jedes Pakets**. MTL ermittelt diese Zeit
intern (Hardware-Zeitstempel der Karte, auf PTP korrigiert), gibt nach außen aber nur Kennwerte je Bild
(`st20_rx_tp_meta`) bzw. je 200 ms (`st30_rx_tp_meta`) weiter. Die RTP-Ebene von MTL liefert Pakete ohne
diese Zeit; die Korrektur auf PTP (`ptp_correct_ts`) ist intern.

## Entscheidung

Ein kleiner Patch für MTL v26.09 (`patches/mtl/v26.09/`) ergänzt `st20_rx_ops` und `st30_rx_ops` um den
optionalen Rückruf `notify_tp_packet(priv, port, rtp_tmstamp, pkt_ns, [pkt_idx,] hw_timestamp)`. MTL ruft
ihn an derselben Stelle auf, an der der eigene Timing-Parser das Paket auswertet, auch für redundante
Kopien. Die Makros `ST20_RX_TP_PACKET_HOOK`/`ST30_RX_TP_PACKET_HOOK` zeigen an, dass der Patch vorhanden
ist; ohne Patch baut und läuft das Programm ohne Paketansichten. `scripts/install-mtl.sh` wendet den
Patch vor dem Bau an.

## Zweiter Patch: PTP stellt die Uhr nur auf Port P nach (2026-11-23)

`0002-st2110-PTP-only-steers-the-clock-on-port-P-of-a-shar.patch`: Beide Ports einer Intel-E8xx-Karte lesen und
stellen denselben Quell-Timer. MTL startet je Port eine PTP-Instanz (auch auf dem reinen Sendeport des
Testsenders), und jede korrigierte denselben Fehler, der Fehler wurde zweimal ausgeglichen. Der Patch lässt
`ptp_adjust_delta` die Uhr nur auf Port P ändern, wenn der zweite Port dieselbe PCI-Karte ist (gleiches
`dddd:bb:dd`, Treiber `ice`); dessen Instanz misst weiter. Keine Schnittstelle zum Programm, deshalb kein
Erkennungsmakro. Bestätigt auf der E830-L (siehe `docs/MESSUNGEN.md`).

`0003-st2110-skip-only-the-timer-writes-on-the-second-port.patch` verfeinert das: Patch 2 verließ
`ptp_adjust_delta()` vorzeitig und übersprang damit auch die Delta-Statistik des zweiten Ports (MTL-Log:
„PTP(1): not connected“). Jetzt entfallen nur die Schreibzugriffe auf den Timer
(`ptp_timesync_adjust_time()` und `ptp_timesync_adjust_freq()`); die zweite Instanz misst und wertet weiter aus. Als
eigener Patch auf Patch 2, damit Rechner mit eingespieltem Patch 2 keinen Konflikt bekommen.

## Begründung

- 38 Zeilen in vier Dateien, keine Änderung an bestehendem Verhalten.
- Gleiche Zeitbasis wie das Urteil von MTL, keine zweite Erfassung (pcap) nötig.
- Die eigene Auswertung rechnet ganzzahlig; MTL rechnet IPT in `double` und rastert damit auf 256 ns.

## Alternativen

- RTP-Ebene von MTL und Zeitstempel selbst aus dem DPDK-mbuf lesen: braucht interne Strukturen von MTL
  (Offset des Zeitstempelfeldes, PTP-Korrektur), bricht bei jeder Version.
- Mitschnitt mit Hardware-Zeitstempeln und Auswertung offline (EBU LIST): genau, aber nicht live.
- Nur Kennwerte je Bild darstellen: keine Histogramme, kein Paketprofil.

## Folgen

- Der Patch muss bei jedem Versionswechsel von MTL neu geprüft werden (`git apply --check`).
- Der Rückruf läuft im Datenpfad von MTL: `Monitor` hält seine Sperre nur kurz, `snapshot()` kopiert
  unter der Sperre und erzeugt JSON danach (gemessen: 0,4–0,6 ms Sperrzeit im Debug-Build).
- Den Patch bei MTL als Vorschlag einreichen, damit er künftig entfällt.
