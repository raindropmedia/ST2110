# CLAUDE.md – ST2110

## Projektziel

Messplatz für SMPTE ST 2110 unter Linux auf Basis der Intel Media Transport Library (MTL): Ströme
empfangen und das Timing nach ST 2110-21 bewerten (Cinst, VRX, IPT, FPT, Latenz, RTP-Offset, Urteil
narrow/wide), später ST 2110-30/-40, ST 2022-7, PTP-Überwachung und Mitschnitt für EBU LIST.
Einsatz: Studio und Lehre an der HSD; Vorarbeit für ST 2110 im Studio-Gateway (`raindropmedia/studio-gateway`).

- Entscheidungen: `docs/adr/` (vor größeren Änderungen lesen, neue Entscheidungen als ADR ablegen)
- Inbetriebnahme der Karte: `docs/hardware/e830-xxvda2.md`
- Messungen: `docs/MESSUNGEN.md`

## Aktueller Stand

Grundgerüst (Branch `tests/initial-setup`): Kernbibliothek mit Tests, Messprogramm `st20-timing`
(nur gegen die MTL-v26.09-Header kompiliert, nicht gelinkt, nicht auf Hardware gelaufen), Skripte
für Installation und Prüfung. Referenzkarte E830-XXVDA2 (`8086:12d3`), noch nicht geliefert.

## Tech-Stack

- C++20, CMake ≥ 3.24 mit Presets (`dev` ohne MTL, `release` mit MTL als Pflicht), Ninja
- MTL `v26.09` (Tag, über `scripts/install-mtl.sh`), gefunden über `pkg-config mtl`; DPDK und
  `ice`-Treiber in den Versionen aus `versions.env` von MTL
- nlohmann/json 3.11.3, doctest 2.4.11 (FetchContent, keine Systempakete)
- Nur Linux (Ubuntu 24.04 auf dem Messrechner)

## Build und Test

```bash
cmake --preset dev && cmake --build --preset dev && ctest --preset dev      # ohne MTL
cmake --preset release && cmake --build --preset release                     # mit MTL
shellcheck scripts/*.sh
```

Ohne Karte lässt sich der MTL-Teil nur kompilieren: Header von MTL v26.09 mit einem
`mtl_build_config.h`-Ersatz und `g++ -fsyntax-only` prüfen.

## Verzeichnisse

| Pfad | Inhalt |
| --- | --- |
| `src/core/` | Ohne MTL: `Options` (Kommandozeile, Prüfung), `FrameRate`, `TimingStats` (Auswertung), `Report` (JSON Lines) |
| `src/mtl/` | `St20TimingReceiver`: MTL-Instanz und `st20p_rx`-Sitzung mit Timing-Parser |
| `src/app/` | `main.cpp` von `st20-timing` |
| `tests/` | doctest-Tests für `src/core/` |
| `scripts/` | `install-mtl.sh`, `check-e830.sh`, `setup-host.sh` |
| `docs/` | ADRs, Inbetriebnahme, Messungen |
| `config/local/` | lokale Daten und Messergebnisse (nicht eingecheckt) |

## Konventionen

**Sprache**
- Bezeichner (Klassen, Funktionen, Variablen, Dateinamen) auf **Englisch**; JSON-Feldnamen englisch.
- Kommentare, Log-Meldungen, Fehlertexte und Dokumentation auf **Deutsch** (klar, formal, präzise).

**C++**
- Namespace `st2110`; Klassen `PascalCase`, Funktionen `camelCase`, Member mit Suffix `_`, Konstanten `kName`.
- Ein Header/Quelle-Paar pro Klasse, `#pragma once`; Formatierung mit `.clang-format`.
- Keine Warnungen (`-Wall -Wextra -Wpedantic`).
- MTL-Header nur in `src/mtl/` einbinden; `src/core/` bleibt ohne MTL testbar. Neue Messwerte zuerst als
  MTL-unabhängige Struktur in `core/` mit Test, dann die Übernahme in `src/mtl/`.
- MTL lässt nur eine Instanz je Prozess sinnvoll zu (Einstellungen der Karte gelten prozessweit).

**Messen**
- Werte gelten nur mit Hardware-Zeitstempeln und PTP; Programme müssen deutlich warnen, wenn eines fehlt.
- Jede Messung mit Versionen und Aufbau in `docs/MESSUNGEN.md`; neue Funktionen erst gegen EBU LIST abgleichen.
- MTL-Version nur zusammen mit `scripts/install-mtl.sh`, Doku und erneutem Abgleich wechseln.

**Skripte**
- Bash mit `set -euo pipefail` (Prüfskripte ohne `-e`), `shellcheck` sauber.
- Nie den Port an DPDK binden, über den die Verwaltung läuft.

**Git**
- Commits auf Deutsch im Imperativ, mit Issue-Verweis, sobald es Issues gibt.
- Keine Build-Ordner, Mitschnitte (`*.pcap`) oder Messdaten (`*.jsonl`) einchecken.

## Bekannte offene Punkte

- PTP-Status (Abstand zum Master, Synchronisation) wird noch nicht ausgegeben (`ptp_sync_notify`).
- Ob der Timing-Parser ohne Synchronisation verlässliche Urteile liefert, ist ungeprüft.
- PF- gegen VF-Betrieb (MTL bevorzugt VFs, das eingebaute PTP braucht die PF).
- Erste Messung auf Hardware, Abgleich mit EBU LIST.
