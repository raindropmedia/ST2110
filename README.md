# ST2110 – Messplatz für SMPTE ST 2110

Werkzeuge zum Empfangen und Bewerten von ST-2110-Strömen unter Linux, aufgebaut auf der
[Intel Media Transport Library (MTL)](https://github.com/OpenVisualCloud/Media-Transport-Library).
Einsatz: Studio und Lehre an der Hochschule Düsseldorf (HSD), Inbetriebnahme von ST-2110-Geräten,
Vorarbeit für ST 2110 im [Studio-Gateway](https://github.com/raindropmedia/studio-gateway).

**Stand:** Grundgerüst. Kern und Tests bauen ohne Karte; das Messprogramm ist gegen die Header von
MTL v26.09 geprüft, aber noch nicht auf Hardware gelaufen.

## Was es gibt

| Teil | Zweck |
| --- | --- |
| `st20-timing` | Empfängt einen ST-2110-20-Strom (optional mit ST 2022-7) und gibt je Intervall die Timing-Werte nach ST 2110-21 als JSON Lines aus |
| `scripts/check-e830.sh` | Prüft Karte, Firmware, PCIe, PTP, Hugepages, IOMMU und MTL (ändert nichts) |
| `scripts/install-mtl.sh` | Installiert MTL in fester Version mit DPDK und gepatchtem `ice`-Treiber |
| `scripts/setup-host.sh` | Hugepages, Puffer, Port an DPDK binden bzw. zurückgeben |

## Hardware

Referenz ist die **Intel E830-XXVDA2** mit E830-CC-Controller (`8086:12d3`), siehe
[ADR 0002](docs/adr/0002-netzwerkkarte-e830.md) und [Inbetriebnahme](docs/hardware/e830-xxvda2.md).
Eine E810 funktioniert ebenfalls (ohne TSN-Pacing, ungenauerer Uhrenabgleich).

## Bauen

```bash
# Kern und Tests (jeder Linux-Rechner, ohne MTL)
cmake --preset dev && cmake --build --preset dev && ctest --preset dev

# Messplatz (MTL installiert, sonst bricht CMake ab)
cmake --preset release && cmake --build --preset release
```

## Messen

```bash
sudo ./build/release/st20-timing --port 0000:af:00.0 --local-ip 192.168.100.10 \
     --ip 239.100.0.1 --udp-port 20000 --width 1920 --height 1080 --fps 50 --interval 1
```

`--help` zeigt alle Optionen. 1080i50 ist `--fps 50 --interlaced` (Halbbilder je Sekunde, Höhe 1080).

Ausgabe (eine Zeile je Ereignis):

| `type` | Inhalt |
| --- | --- |
| `start` | Format, Adressen des Stroms, Hardware-Zeitstempel und PTP an/aus |
| `passCriteria` | Grenzwerte des Timing-Parsers (Cinst/VRX narrow und wide, TR_OFFSET, Latenz, RTP-Offset) |
| `interval` | je Pfad `P`/`R`: Urteil, Zähler narrow/wide/failed, Min/Max/Mittel von Cinst, VRX, IPT, FPT, Latenz, RTP-Offset, RTP-Delta, Fehlerursachen; Bildzähler (vollständig, rekonstruiert, beschädigt) |
| `summary` | dasselbe über die ganze Messung |

Rückgabewerte: 0 = Bilder empfangen, 1 = Optionen fehlerhaft, 2 = MTL-Start fehlgeschlagen,
3 = keine Bilder empfangen.

**Aussagekraft:** Nur mit Hardware-Zeitstempeln und PTP (beides Vorgabe) sind die Werte belastbar.
`--no-hw-timestamp`/`--no-ptp` und `kernel:`-Ports dienen nur als Funktionstest. Vor dem ersten
Einsatz gegen [EBU LIST](https://github.com/ebu/pi-list) abgleichen (`docs/MESSUNGEN.md`).

## Dokumentation

- [Architekturentscheidungen](docs/adr/README.md)
- [Inbetriebnahme E830-XXVDA2](docs/hardware/e830-xxvda2.md)
- [Messungen](docs/MESSUNGEN.md)

## Lizenz

Noch festzulegen. MTL steht unter BSD-3-Clause.
