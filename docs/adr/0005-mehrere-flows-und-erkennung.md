# ADR 0005: Mehrere Flows und Flow-Erkennung über eine Kernel-Schnittstelle

- Status: vorläufig angenommen (Erkennung auf `lo` getestet, nicht an einem Spiegelport)
- Datum: 2026-11-19

## Kontext

Wie bei Telestream PRISM oder Phabrix QxL soll man mehrere Flows gleichzeitig überwachen, einen davon
im Detail ansehen und **alle im Netz ankommenden Flows** mit Adresse, Datenrate und Format sehen.
Ein an DPDK gebundener Port empfängt über MTL aber nur abonnierte Flows: MTL setzt je Sitzung
Flow-Regeln in der Karte und tritt den Multicast-Gruppen per IGMP bei. Alles andere verwirft die Karte.

## Entscheidung

1. **Mehrere Flows:** `MonitorHub` hält beliebig viele `FlowMonitor` (je Video- oder Tonflow mit P/R),
   PTP und das Ereignisprotokoll. MTL reserviert beim Start `--max-flows` Empfangs-Queues je Port; Flows
   lassen sich zur Laufzeit über die Oberfläche bzw. `POST/DELETE /api/flows` anlegen und entfernen
   (`FlowController`, umgesetzt von `MtlSource` und `DemoSource`).
2. **Erkennung:** `--discover <schnittstelle>` liest alle Rahmen einer **Kernel**-Schnittstelle über einen
   AF_PACKET-Socket mit (je Rahmen 160 Byte, nur Kopfdaten) und empfängt SAP-Ankündigungen
   (239.255.255.255:9875). `FlowTable` bildet daraus Flows (Ziel, Quelle, Port) mit Raten, RTP-Takt,
   Sequenzfehlern und einer Formatschätzung (Video aus RFC-4175-Zeilenköpfen und RTP-Zeitstempeln,
   Ton aus Takt, Paketrate und Nutzlast). Aus der Liste lässt sich ein Flow per Klick überwachen.
3. Ohne Port und Flows läuft das Programm als **reiner Netz-Scanner** (ohne MTL und ohne Karte).

## Begründung

- Die Erkennung braucht keine Änderung an MTL und keine zweite DPDK-Instanz.
- Der Mitschnitt nur der Kopfdaten hält die Last klein; Raten stammen aus der Rahmenlänge auf der Leitung.
- SAP ist bei AES67/ST 2110-Geräten verbreitet und liefert die SDP gleich mit.

## Alternativen

- Promiscuous-Modus des DPDK-Ports über MTL: MTL bietet keinen Empfang ohne Sitzung; ein eigener
  DPDK-Pfad wäre ein zweiter Treiber für dieselbe Karte.
- NMOS IS-04 (Registry): liefert angemeldete Sender, aber nicht, was tatsächlich ankommt. Später ergänzbar.
- Mitschnitt mit libpcap: zusätzliche Abhängigkeit ohne Vorteil gegenüber AF_PACKET unter Linux.

## Folgen

- Die Erkennung sieht nur, was der Switch an die Schnittstelle liefert: Spiegelport (SPAN), Port ohne
  IGMP-Snooping oder abonnierte Gruppen. Am Messplatz eine eigene Kernel-Schnittstelle dafür vorsehen
  (z. B. Onboard-Karte am Spiegelport oder Port 1 der E830 im Kernel-Betrieb).
- Braucht CAP_NET_RAW. Bei hoher Paketrate verwirft der Kernel Rahmen; die Oberfläche zeigt die Zahl
  (Sequenzfehler können dann vom Mitschnitt stammen, nicht vom Netz).
- Formatschätzung nur für YCbCr 4:2:2 10 Bit mit bekannten Breiten und für L16/L24; ST 2110-40 wird als
  „Zusatzdaten“ erkannt, aber noch nicht ausgewertet.
- `--video`/`--audio` sind jetzt wiederholbar und tragen ihre Angaben selbst
  (`--video 239.1.1.1:20000,format=1920x1080i50,red=…`); die früheren Einzeloptionen entfallen.
