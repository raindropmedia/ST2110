# CLAUDE.md – ST2110

## Projektziel

Messplatz für SMPTE ST 2110 unter Linux auf Basis der Intel Media Transport Library (MTL): Ströme
empfangen und bewerten wie ein Broadcast-Messgerät (Telestream PRISM, Phabrix QxL als Vorbild):
Video-Timing nach ST 2110-21, Ton nach ST 2110-30, PTP, ST 2022-7, Web-Oberfläche, JSON-Berichte.
Einsatz: Studio und Lehre an der HSD; Vorarbeit für ST 2110 im Studio-Gateway (`raindropmedia/studio-gateway`).

- Entscheidungen: `docs/adr/` (vor größeren Änderungen lesen, neue Entscheidungen als ADR ablegen)
- Inbetriebnahme der Karte: `docs/hardware/e830-xxvda2.md`
- Messungen und offene Prüffragen: `docs/MESSUNGEN.md`

## Aktueller Stand

Branch `tests/initial-setup`. `st2110-monitor` mit Demo, Web-Oberfläche, Video/Ton/PTP/2022-7-Auswertung für mehrere
Flows (zur Laufzeit änderbar), Flow-Erkennung (AF_PACKET, SAP; am Verwaltungsport automatisch), Bild-/Tonvorschau,
SDP-Import und Testsender (Tab „Testsender“) ist getestet (doctest inkl. ThreadSanitizer-Lauf der Sender-Verwaltung,
Playwright-Screenshots der Oberfläche).

Hardware: Referenzkarte E830-XXVDA2, geliefert mit E830-L-Controller (`8086:12de`) im CPU-Steckplatz eines i7-4790,
Ubuntu Server 26.04.1, MTL v26.09 mit drei Patches (`patches/mtl/v26.09/`), Switch MikroTik CRS309 ohne PTP-Funktion.
Empfang, Senden und PTP laufen; erste Messung in `docs/MESSUNGEN.md`: keine Verluste, der Testsender mit
Ratenbegrenzung ist „wide“ (Pakete rund 15 Paketzeiten zu früh). Zu prüfen: TSN-Pacing (`--tx-pacing tsn`) und
`start-vrx`, dann Abgleich mit EBU LIST.

## Tech-Stack

- C++20, CMake ≥ 3.24 mit Presets (`dev`: ohne MTL, Oberfläche aus `webui/`; `release`: MTL Pflicht), Ninja
- MTL `v26.09` mit eigenem Patch (`patches/mtl/v26.09/`, ADR 0003) über `scripts/install-mtl.sh`,
  gefunden über `pkg-config mtl`
- nlohmann/json 3.11.3, cpp-httplib 0.65.0, stb_image_write (Commit 61e70c5, JPEG der Vorschau), doctest 2.4.11
  (FetchContent, keine Systempakete)
- Web-Oberfläche: HTML/CSS/JS ohne Build-Schritt, eigene Canvas-Diagramme (`webui/charts.js`), keine CDN

## Build und Test

```bash
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
./build/dev/st2110-monitor --demo --output -            # Oberfläche: http://127.0.0.1:8090/
sudo ./build/dev/st2110-monitor --discover lo --output -  # reiner Netz-Scanner (ohne MTL)
shellcheck scripts/*.sh
clang-format --dry-run --Werror src/*/*.cpp src/*/*.h tests/*.cpp
```

MTL-Pfad ohne Karte prüfen: DPDK und MTL (mit Patch) in ein Präfix bauen und
`PKG_CONFIG_PATH=<präfix>/lib/x86_64-linux-gnu/pkgconfig cmake -S . -B build/mtl -DST2110_REQUIRE_MTL=ON`;
`mtl_init` scheitert ohne Karte sauber mit Exit-Code 2.

Oberfläche prüfen: Demo starten, Störung über `POST /api/demo {"preset": "burst"}` setzen, mit
Playwright Screenshots je Ansicht aufnehmen; Konsolenfehler müssen leer sein, bei 390 px Breite kein
horizontales Scrollen.

## Verzeichnisse

| Pfad | Inhalt |
| --- | --- |
| `src/core/` | Ohne MTL: `Options` (Flows als `FlowConfig`), `Sdp`, `FrameRate`, `AudioFormat`, `VideoTimingModel` (Grenzwerte und Urteil wie MTL), `VideoPacketAnalyzer`, `AudioPacketAnalyzer`, `PtpMonitor`/`PathSkew`, `Histogram`, `TimeSeries`, `TimingStats`, `FlowMonitor` (ein Flow, threadsicher), `MonitorHub` (alle Flows, PTP, Ereignisse, Erkennung), `FlowController` (Flows zur Laufzeit), `Preview` (RFC 4175 → RGB, Ton-Ringpuffer, Pegel, Testbild/-ton), `PacketParser`, `FlowTable` (erkannte Flows, Formatschätzung), `SapDirectory`, `Report`, `SyntheticSender`, `TestSender` (Testsender zur Laufzeit: Zustände, Hintergrundstart, `TxBackend`), `TestSignal` (Testbild/Testton im Transportformat; `PixelPatternGenerator` für alle 16 Pixelformate), `NetworkInfo` (Standardroute), `PixelFormat` (Abtastung, Tiefe, Pixelgruppen, Packmodus nach ST 2110-20), `AncAnalyzer` (ST 2110-40, Bilder von MTL `st40_rx`), `HbrmtAnalyzer` (ST 2022-6, RTP-Pakete im Passthrough, Header nach Abschnitt 6.4), `System10` (Systemprüfung ST 2110-10 je Flow), `SystemInfo` (Tab „System“: Host, Karten, SFP über `SystemProbe`/`ethtool`; Auswertung der Ausgaben einzeln testbar) |
| `src/mtl/` | `MtlSource`: MTL-Instanz, je Flow `st20_rx`/`st30_rx` mit Timing-Parser, Paket-Rückruf, PTP, Zähler, `st40_rx` (ANC, Frame-Ebene), `st20_rx` auf RTP-Ebene für ST 2022-6 (Abholthread); `PreviewWorker` wandelt Vorschaubilder außerhalb des Datenpfads; Testsender als dritter Port (`TxSession`, `st20p_tx`/`st30p_tx`) |
| `src/discovery/` | `Discovery`: AF_PACKET-Mitschnitt und SAP-Empfang auf einer Kernel-Schnittstelle (nur Linux) |
| `src/demo/` | `DemoSource`: synthetische Flows mit umschaltbaren Störungen, simulierte Erkennung und SAP; `DemoTxBackend` (Testsender ohne Senden), `DemoSystemProbe` (feste Beispieldaten für System-Tab und Tests); die Demo enthält je einen ANC- und ST-2022-6-Flow |
| `src/web/` | `WebServer`: statische Dateien und JSON-API |
| `src/app/` | `main.cpp` von `st2110-monitor` |
| `webui/` | Oberfläche (`index.html`, `style.css`, `charts.js`, `app.js`) |
| `tests/` | doctest-Tests für `src/core/` |
| `patches/mtl/` | eigene Patches für MTL je Version |
| `scripts/` | `install-mtl.sh`, `check-e830.sh`, `setup-host.sh`, `install-service.sh`, `systemd/` (Units, `/etc/default/st2110`-Vorlage) |
| `docs/` | ADRs, Inbetriebnahme, Messungen, Bilder der Oberfläche |
| `config/local/` | lokale Daten, SDP-Dateien und Messergebnisse (nicht eingecheckt) |

## Konventionen

**Sprache**
- Bezeichner (Klassen, Funktionen, Variablen, Dateinamen) auf **Englisch**; JSON-Feldnamen englisch mit
  Einheit im Namen (`fptNs`, `dpvrUs`).
- Kommentare, Log-Meldungen, Fehlertexte, UI-Texte und Dokumentation auf **Deutsch** (klar, formal, präzise).

**C++**
- Namespace `st2110`; Klassen `PascalCase`, Funktionen `camelCase`, Member mit Suffix `_`, Konstanten `kName`.
- Ein Header/Quelle-Paar pro Klasse, `#pragma once`; Formatierung mit `.clang-format`; keine Warnungen.
- MTL-Header nur in `src/mtl/`; `src/core/` bleibt ohne MTL testbar. Neue Messwerte zuerst als
  MTL-unabhängige Struktur in `core/` mit Test, dann die Übernahme in `src/mtl/`.
- Formeln und Grenzwerte folgen MTL (`st_rx_timing_parser.c`), damit eigene Auswertung und MTL-Urteil
  übereinstimmen. Abweichungen nur begründet und in `docs/MESSUNGEN.md` festhalten (Beispiel: ganzzahlige
  Zeitdifferenzen statt `double`).
- Zeitstempel als `uint64_t` ns (TAI); absolute Zeiten in `long double`, Differenzen ganzzahlig.
- Rückrufe aus MTL laufen im Datenpfad: nichts blockieren, `FlowMonitor` nur kurz sperren. Teure Arbeit
  (JSON) außerhalb der Sperre (`FlowMonitor::snapshot` kopiert erst).
- Sperrreihenfolge: Flow vor Hub (Ereignisse eines Flows gehen an den Hub). Der Hub ruft Flows nie unter
  seiner eigenen Sperre auf; Quellen (`MtlSource`, `DemoSource`) rufen `MonitorHub::addFlow` ohne eigene Sperre.
- MTL lässt nur eine Instanz je Prozess sinnvoll zu (Einstellungen der Karte gelten prozessweit).
- Vorschau nur auf Anforderung (`VideoPreview::due`, `AudioPreview::wanted`); Bildrahmen nie im Rückruf
  wandeln, sondern an den `PreviewWorker` geben, dort nach der Farbumrechnung sofort an MTL zurückgeben
  und erst danach JPEG kodieren (ADR 0006).
- Der Webserver braucht `set_tcp_nodelay(true)`; sonst kostet jede Antwort ~40 ms (Nagle, verzögertes ACK).

**MTL-Patch**
- Änderungen an MTL nur als Patch in `patches/mtl/<version>/`, mit Makro zur Erkennung
  (`ST20_RX_TP_PACKET_HOOK`); der Code muss ohne Patch bauen und laufen.
- Bei MTL-Versionswechsel: Patch mit `git apply --check` prüfen, `scripts/install-mtl.sh`, ADR 0003 und
  `docs/MESSUNGEN.md` anpassen.

**Web-Oberfläche**
- Keine externen Skripte oder Schriften (Messnetz ohne Internet). Texte deutsch, Zahlen mit `de-DE`.
- Neue Werte zuerst in `FlowMonitor::snapshot()` bzw. `MonitorHub` (Test in `tests/test_monitor.cpp`), dann in
  `webui/app.js`. Die Ansichten lesen `s.video`/`s.audio`/`s.counters` des gewählten Flows (in `poll()` aus
  `snapshot.flow` abgeleitet).
- Jeder neue Endpunkt im Kommentar von `WebServer.h` und in der README-Tabelle.

**Messen**
- Werte gelten nur mit Hardware-Zeitstempeln und PTP; Programm und Oberfläche warnen, wenn eines fehlt.
- Jede Messung mit Versionen und Aufbau in `docs/MESSUNGEN.md`; neue Funktionen erst gegen EBU LIST abgleichen.

**Skripte**
- Bash mit `set -euo pipefail` (Prüfskripte ohne `-e`), `shellcheck` sauber.
- Nie den Port an DPDK binden, über den die Verwaltung läuft.

**Git**
- Commits auf Deutsch im Imperativ, mit Issue-Verweis, sobald es Issues gibt.
- Keine Build-Ordner, Mitschnitte (`*.pcap`), Messdaten (`*.jsonl`) oder SDP-Dateien aus dem Betrieb einchecken.

## Bekannte offene Punkte

- Erste Messung auf Hardware, Abgleich mit EBU LIST (Prüffragen in `docs/MESSUNGEN.md`).
- PF- gegen VF-Betrieb (MTL bevorzugt VFs, das eingebaute PTP braucht die PF).
- MTL liefert keine Grandmaster-Kennung, Domäne oder Pfadverzögerung; dafür ggf. `pmc` (linuxptp) im VF-Betrieb.
- ST 2110-22, NMOS und pcap-Mitschnitt fehlen. ST 2110-40 und ST 2022-6 (Empfang und Testsender) sind gebaut und in der
  Demo geprüft, aber noch nicht auf Hardware (ADR 0008).
- Erkennung an einem echten Spiegelport mit mehreren Video-Flows prüfen (Rahmenverluste beim Mitlesen).
- Web-Oberfläche ohne Anmeldung.
