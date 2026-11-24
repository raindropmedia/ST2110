# ADR 0008: ST 2110-40, ST 2022-6 und Systemprüfung ST 2110-10

- Status: angenommen (Bau gegen MTL v26.09, Unit-Tests und Demo geprüft; Empfang von ANC und ST 2022-6 noch nicht auf Hardware)
- Datum: 2026-11-24

## Kontext

Der Messplatz soll alle Teile des Rahmenwerks messen, die MTL unterstützt: ST 2110-10 (System), -20/-21 (Video,
Timing), -30 (Ton), -40 (Zusatzdaten) und ST 2022-6 (SDI über IP). Ein Blick in Header und Quelltext von MTL v26.09
zeigt, was davon wie vorliegt.

## Entscheidung

**ST 2110-21** hat MTL bereits als Timing-Parser (`st20_rx_tp_meta`, `st20_rx_tp_pass`); der Messplatz übernimmt alle
Felder. Neu ist nur die Tabelle „Kriterien ST 2110-21“ im Video-Tab (letztes Bild gegen die Grenzen von MTL).

**ST 2110-40** empfängt MTL nativ (`st40_rx`, Frame-Ebene). Je Bild liefert es RTP-Zeitstempel, Ankunftszeit des ersten
Pakets, Paketverluste und Sprünge, Marker, Interlace-Kennung sowie je ANC-Paket Zeile, Offset, DID, SDID und Größe.
Parität und Prüfsumme der Nutzdatenwörter prüft MTL und verwirft fehlerhafte Pakete, ohne das nach außen zu melden
(`stat_anc_pkt_parse_err` ist intern); sie fehlen dann in der Liste. `AncAnalyzer` zählt je DID/SDID, prüft den
RTP-Zeitstempelschritt (90 kHz gegen die Bildrate) und den Zeitbezug des ersten Pakets zur PTP-Zeit. ST 2110-40 hat
kein Netzwerk-Zeitmodell wie ST 2110-21; das Urteil (ok, Warnung, Fehler) beruht auf beobachteten Fehlern (Verlust,
beschädigtes Bild, fehlender Marker, Schrittfehler, Zeitbezug mehr als eine Bildperiode daneben, Letzteres als
Richtwert).

**ST 2022-6** kennt MTL nicht; die Doku nennt nur „RTP passthrough“. `HbrmtAnalyzer` wertet deshalb den
RTP-Header und den Nutzdaten-Header (ST 2022-6:2012, Abschnitt 6.4: Ext, F, VSID, FRCount, R, S, FEC, CF, MAP, FRAME,
FRATE, SAMPLE, Video-Zeitstempel) selbst aus und prüft Sequenznummern, Nutzlast (1376 Byte), Marker und Bildaufbau
(Datagramme je Bild nach Abschnitt 6.5 aus den Rastermaßen von ST 274/296/259), FRCount und den
Zeitstempelschritt (27 MHz). Der Empfang geht über eine `st20_rx`-Sitzung auf RTP-Ebene (MTL prüft nur Payload-Typ, die
Pakete holt ein eigener Thread). MTL ordnet dort nach RTP-Zeitstempel in Bild-Slots ein; bei ST 2022-6 hat jedes Datagramm
einen eigenen Zeitstempel, die Zähler und das Bildmodell von MTL sind dafür bedeutungslos und werden nicht benutzt.
MTL liefert im Passthrough keine Ankunftszeiten, ST 2022-6 hat kein Zeitmodell: Es gibt keine Paketabstände und keine
Zeitwerte gegen PTP. Nicht auf Hardware geprüft; unsicher ist, ob MTL die Platzhalter für Bildformat/-rate der
`st20_rx`-Sitzung ohne Weiteres annimmt.

**ST 2110-10** ist keine Sitzungsart in MTL. Die Systemprüfung (`System10`) fasst vorhandene Messwerte je Flow zusammen:
PTP, RTP-Zeitstempel gegen PTP-Zeit, Zeitstempelschritt, Verluste, vertauschte und doppelte Pakete, fremder
Payload-Typ oder SSRC, Payload-Typ im dynamischen Bereich, Hardware-Zeitstempel. Für Video kommen die Grenzen von MTL;
sonst sind sie in der Oberfläche als Richtwert gekennzeichnet.

**Flow-Arten:** `FlowConfig` bekommt `anc` und `hbrmt` neben `video` und `audio`; Kommandozeile `--anc` und `--hbrmt`,
SDP (`smpte291`, `SMPTE2022-6`), Erkennung (RTP-Takt 27 MHz = ST 2022-6; wenige Pakete bei 90 kHz = ANC).

## Folgen

- Der Empfang von ANC und ST 2022-6 muss auf Hardware geprüft werden (Hinweis im Tab).
- Ein ST-2022-6-Strom mit 3G-Rate (rund 270 000 Datagramme/s) belastet den Abholthread; bei Verlusten im MTL-Ring
  (`rtp_ring_size` 16384) den Ring vergrößern.
- Testsender: ANC über `st40_tx` (Frame-Ebene, drei Puffer mit gleichem Inhalt, `get_next_frame` reicht sie reihum weiter;
  Nutzdaten Platzhalter). ST 2022-6 über `st20_tx` auf RTP-Ebene (SD 625, 25 Hz, 982 Datagramme zu 1376 Byte): MTL überschreibt
  im RTP-Passthrough den Zeitstempel mit einem 90-kHz-Wert und erkennt Bildwechsel am Zeitstempel (`tv_build_rtp`). Mit
  `ST20_TX_FLAG_USER_TIMESTAMP` bleibt der eigene 27-MHz-Wert, der je Bild gilt; die Norm verlangt ihn je Datagramm. Der
  Testsender setzt CF = 0, damit kein davon abweichender Video-Zeitstempel im Header steht. Der Strom ist daher nur
  eingeschränkt normgerecht und dient dem Test des Messplatzes; nicht auf Hardware geprüft.
