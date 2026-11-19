# ADR 0001: Intel MTL als Grundlage für Empfang und Timing-Analyse

- Status: vorläufig angenommen (bis zur ersten Messung auf echter Hardware)
- Datum: 2026-11-19

## Kontext

Für Studio und Lehre an der HSD soll ein Messplatz entstehen, der ST-2110-Ströme empfängt und deren
Timing nach ST 2110-21 bewertet (Cinst, VRX, IPT, FPT, Latenz, RTP-Offset, Urteil narrow/wide).
Kommerzielle Analyzer sind teuer; EBU LIST wertet nur Mitschnitte offline aus.

## Entscheidung

Der Messplatz nutzt die Media Transport Library (MTL, OpenVisualCloud) in einer festen Version
(Start: `v26.09`). Empfang über `st20_rx` (Bildebene) mit `ST20_RX_FLAG_TIMING_PARSER_META` und
`st30_rx` mit `ST30_RX_FLAG_TIMING_PARSER_META`; die Werte je Bild kommen aus `struct st20_rx_tp_meta`,
je 200 ms aus `struct st30_rx_tp_meta`, die Grenzwerte aus `st20_rx_timing_parser_critical()`.
Werte je Paket über einen eigenen Patch (ADR 0003). Eigener Code bleibt in C++20, die Kernlogik (Optionen,
Auswertung, Ausgabe) ist von MTL getrennt und ohne Karte testbar.

## Begründung

- MTL enthält einen Timing-Parser nach ST 2110-21, dessen Definitionen laut Header EBU LIST folgen.
- Hardware-Zeitstempel je Paket (`MTL_FLAG_ENABLE_HW_TIMESTAMP`) und eingebautes PTP
  (`MTL_FLAG_PTP_ENABLE`, laut MTL etwa 30 ns) auf Intel E810/E830.
- Lizenz BSD-3-Clause; aktive Entwicklung mit regelmäßigen Releases.

## Alternativen

- Eigene Auswertung aus pcap-Mitschnitten (wie EBU LIST): genau, aber nicht live.
- NVIDIA Rivermax: Windows-tauglich, aber proprietär und an ConnectX-Karten gebunden.
- Kernel-Sockets ohne DPDK: Zeitstempel zu ungenau für Cinst/VRX (Paketabstand bei 1080p50 ≈ 4,4 µs).

## Folgen

- Nur Linux (Ubuntu 24.04), DPDK, Hugepages, IOMMU; der Messport ist für das Betriebssystem nicht nutzbar.
- MTL lässt nur eine Instanz je Prozess sinnvoll zu: ein Prozess je Messung bzw. Kartenkonfiguration.
- Der Timing-Parser liefert Kennwerte je Bild, keine Paketverläufe; Histogramme und ST 2022-7-Versatz
  sind spätere Erweiterungen. ST 2110-30 hat einen eigenen Parser (DPVR, TSDF), ST 2110-40 keinen.
- Die Werte müssen gegen EBU LIST abgeglichen werden, bevor sie als belastbar gelten.
- Versionswechsel von MTL nur mit erneutem Abgleich; Version in `scripts/install-mtl.sh` und
  `docs/MESSUNGEN.md` festhalten.
