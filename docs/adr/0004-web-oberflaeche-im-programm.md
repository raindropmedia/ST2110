# ADR 0004: Web-Oberfläche im Messprogramm, Diagramme ohne Bibliothek

- Status: angenommen
- Datum: 2026-11-19

## Kontext

Der Messplatz soll wie ein Messgerät bedienbar sein (Übersicht, Histogramme, Verläufe, Ereignisse),
auch vom Tablet in der Regie. Messnetze sind oft vom Internet getrennt.

## Entscheidung

`st2110-monitor` enthält einen HTTP-Server (cpp-httplib 0.65.0, wie im Studio-Gateway) und liefert die
statischen Dateien aus `webui/` sowie eine JSON-API (`/api/snapshot`, `/api/reset`, `/api/demo`). Die
Oberfläche ist HTML/CSS/JavaScript ohne Build-Schritt; Diagramme zeichnet eine eigene kleine
Canvas-Bibliothek (`webui/charts.js`). Die Oberfläche fragt den Zustand zweimal je Sekunde ab.

## Begründung

- Keine CDN-Abhängigkeit: läuft im abgeschotteten Messnetz.
- Gleiche Bausteine und Konventionen wie das Studio-Gateway.
- Abfrage statt Push: einfach, robust gegen Verbindungsabbrüche; 0,5 s genügt für die Anzeige.

## Alternativen

- Grafana mit Prometheus: gut für Langzeitverläufe, aber keine Paketprofile, zusätzlicher Dienst.
- Diagrammbibliothek (Chart.js, uPlot) über CDN: nicht offline nutzbar; lokal eingebunden wäre es eine
  weitere Abhängigkeit für wenige Diagrammtypen.

## Folgen

- Keine Anmeldung: Vorgabe `127.0.0.1`, Freigabe im Messnetz nur bewusst (`--http 0.0.0.0:8090`, Hinweis
  beim Start). Für den Betrieb außerhalb des Messnetzes Anmeldung nach ADR 0008 des Studio-Gateways ergänzen.
- Ein Snapshot umfasst rund 200–300 kB JSON; bei vielen gleichzeitigen Betrachtern Abfrage drosseln.
