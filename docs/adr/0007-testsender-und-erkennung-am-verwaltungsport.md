# ADR 0007: Testsender in derselben MTL-Instanz und Erkennung am Verwaltungsport

- Status: angenommen (Bau gegen MTL v26.09 und Demo geprüft, Senden und Erkennung noch nicht auf Hardware)
- Datum: 2026-11-23

## Kontext

Ohne Sender im Netz lässt sich der Messplatz nicht prüfen. Die E830 hat zwei Ports; der zweite kann als
Referenzsender dienen. Außerdem soll man Flows entdecken, ohne eine Schnittstelle für die Erkennung
bewusst angeben zu müssen.

## Entscheidung

**Testsender**

- Der Sender läuft im selben Prozess und in derselben MTL-Instanz wie der Empfang, als dritter Port (nach P und
  R). MTL erlaubt nur eine Instanz je Prozess, und beide Ports nutzen so dieselbe PTP-Zeit.
- Die Verwaltung (`TestSender`, `src/core/`) kennt keine MTL-Typen. Ein `TxBackend` öffnet die Sitzungen
  (`MtlSource`: `st20p_tx`/`st30p_tx`, Demo: nur Buchführung). Zustände: gestoppt, startet, sendet, stoppt,
  Fehler. Das **Starten läuft im Hintergrund**: MTL wartet beim Anlegen einer Sendesitzung, bis PTP
  eingerastet ist (höchstens 3 min), und das darf weder die Web-Oberfläche noch den Empfang aufhalten.
- Einstellungen (Video: Auflösung, Rate, Zeilensprung; Ton: Rate, Bittiefe, Kanäle, Paketzeit, Ziel, Payload-Typ)
  gelten je Sitzung und ändern sich nur im gestoppten Zustand (Stoppen, Ändern, Starten). Die Beschreibung ist
  dieselbe wie bei `--tx-video`/`--tx-audio` (`parseFlowSpec`), damit Kommandozeile und Oberfläche gleich
  prüfen.
- MTL legt die Sende-Queues beim Start fest. Ein Videosender braucht eine, Ton mit Paketzeit unter 0,5 ms zwei
  (Ratenbegrenzung), Ton ab 1 ms teilt sich eine. Reserviert wird für `--max-tx-flows` (Vorgabe 4): 2 je Sender
  plus eine, höchstens 7, weil die E830 mit 9 Queues nicht startete (`--tx-queues` überschreibt). Der Sendeport
  (`--tx-port`) muss beim Start feststehen.
- Pacing: `--tx-pacing auto|rl|tsc|tsn` (MTL `pacing`); auf der E830 wählt MTL von selbst die Ratenbegrenzung, TSN
  (Launch-Time) nur auf Wunsch. Je Videosender lässt sich der Vorlauf `start-vrx` einstellen. Gemessen: Mit
  Ratenbegrenzung ist der Sender „wide“ (VRX 15 statt höchstens 8), siehe `docs/MESSUNGEN.md`.
- Das Testbild entsteht direkt im Transportformat (YCbCr 4:2:2, 10 Bit, RFC 4175; `TestPatternGenerator`),
  die Bänder einmal vorberechnet, je Bild nur Zeilen kopieren und das Quadrat zeichnen. MTL wandelt nichts.
  Der Sender ist ein „narrow“-Sender; unter E8xx taktet MTL per Rate-Limit statt TSN, am Empfänger
  erscheint er deshalb meist als „wide“.

**Erkennung am Verwaltungsport**

- Ohne `--discover` liest das Programm im DPDK-Betrieb auf der Schnittstelle mit der Standardroute mit
  (`defaultRouteInterface()`, aus `/proc/net/route`). `--no-discover` schaltet das ab. Die Karte im
  DPDK-Betrieb hat keine Kernel-Schnittstelle und sieht nur abonnierte Flows.
- Der Verwaltungsport sieht nur, was der Switch ihm liefert: Mit IGMP-Snooping (der Regelfall im 2110-Netz)
  nur SAP und Flows, die er selbst abonniert hat. Gelesen wird passiv (kein IGMP-Join).

## Folgen

- Ein Start nur mit `--port` ist gültig: Flows und Testsender legt man in der Oberfläche an.
- Ein Switch, der unbekannte Multicast-Gruppen flutet, kann den Verwaltungsport (oft 1 GbE) mit UHD-Strömen
  überschwemmen; dann `--no-discover` verwenden oder die Flutung am Switch abstellen.
- Ohne Standardroute (isoliertes Testnetz) bleibt die Erkennung aus; dann `--discover <schnittstelle>`.
