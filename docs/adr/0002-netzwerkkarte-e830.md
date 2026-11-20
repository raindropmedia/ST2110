# ADR 0002: Intel E830-XXVDA2 (E830-CC) als Referenzkarte

- Status: vorläufig angenommen (bis die Karte geliefert und geprüft ist)
- Datum: 2026-11-19

## Kontext

MTL unterstützt Hardware-Zeitstempel für jedes empfangene Paket und genaues Pacing beim Senden nur
auf Intel E810 und E830 mit DPDK. Für den Messplatz reichen 25 GbE (1080p50 10 Bit ≈ 2,6 Gbit/s je
Strom, 2160p50 ≈ 10,4 Gbit/s).

## Entscheidung

Referenzkarte ist die Intel Ethernet E830-XXVDA2 (2 × SFP28, PCIe 4.0 x8) mit E830-CC-Controller
(PCI-ID `8086:12d3`). Port 0 bleibt immer Teil der MTL-Konfiguration, weil das eingebaute PTP die
Uhr von Port 0 nutzt.

## Begründung

- Gegenüber der E810: TSN-Pacing beim Senden (Sendezeitpunkt aus der Karte, `ST21_TX_PACING_WAY_TSN`,
  nur PF) – damit taugt Port 1 als Referenzsender zum Abgleich des Messgeräts.
- Exakter Abgleich zwischen Kartenuhr und Systemuhr (`PTP_SYS_OFFSET_PRECISE`); die E810 erreicht
  laut MTL nur etwa ±1 µs je Abfrage.
- `12d3` steht in der Testliste von MTL (`doc/ci_runner_setup.md`); die C- und L-Varianten
  (`12da`, `12de`) kennt DPDK, MTL prüft sie aber nicht.

## Alternativen

- E810-XXVDA2 (`159b`): für reines Messen ausreichend, kein TSN-Pacing, ungenauerer Uhrenabgleich.
- E810-XXVDA4T: zusätzlich GNSS/1PPS an der Karte; für das Messen nicht nötig.
- 100-GbE-Karten (E810-CQDA2, E830-CQDA2): erst nötig bei vielen UHD-Strömen.

## Folgen

- Vor dem Kauf die Controller-Variante klären; nach dem Einbau mit `scripts/check-e830.sh` prüfen.
- Aktuelle NVM-Firmware, `ice`-Treiber mit MTL-Patches und DPDK nach `versions.env` der MTL-Version.
- SFP28-Module bzw. DAC-Kabel nach Intel-Kompatibilitätsliste.
- Nachtrag 2026-11-20: Geliefert wurde eine E830-XXVDA2 mit E830-L-Controller (`12de`). DPDK 26.07 führt
  `12de` als E830 (gleicher MAC-Typ); ob TSN-Pacing und der genaue Uhrenabgleich darauf gehen, ist offen.
- Ein PTP-fähiger Switch (Boundary- oder Transparent-Clock) und ein Grandmaster im Messnetz.
