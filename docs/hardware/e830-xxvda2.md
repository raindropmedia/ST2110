# Inbetriebnahme: Intel E830-XXVDA2

Schritte vom leeren Rechner bis zur ersten Messung. Die Befehle beziehen sich auf MTL `v26.09`
(siehe `scripts/install-mtl.sh`). Ungeprüft auf echter Hardware – Abweichungen hier nachtragen.

## 1. Hardware

| Punkt | Vorgabe |
| --- | --- |
| Karte | E830-XXVDA2; geplant E830-CC (`8086:12d3`), geliefert E830-L (`8086:12de`) |
| Steckplatz | PCIe 4.0 x8 (3.0 x8 reicht für 2 × 25G; x4 bremst) |
| CPU/NUMA | Karte und isolierte Kerne am selben NUMA-Knoten |
| Verwaltung | eigene Netzwerkkarte (Onboard) – der Messport gehört DPDK |
| Module | SFP28 (25G) oder SFP+ (10G) nach Intel-Kompatibilitätsliste, alternativ DAC |
| Netz | PTP-fähiger Switch, Grandmaster; IGMP-Snooping/Querier für Multicast |

## 2. BIOS

- VT-d (IOMMU) einschalten, SR-IOV nach Bedarf.
- Energiesparfunktionen für die Messkerne reduzieren (C-States, Turbo nach Bedarf festlegen).

## 3. Betriebssystem (Ubuntu Server 24.04 oder 26.04 LTS)

MTL v26.09 unterstützt beide; die Hardware-Tests von MTL laufen auf 24.04 (Kernel 6.8). Der Messplatz der HSD
läuft mit Ubuntu Server 26.04.1. Ohne Desktop installieren, den Kernel festhalten und Updates nur bewusst einspielen
(danach `scripts/install-mtl.sh ice`):

```bash
sudo apt-mark hold linux-image-generic linux-headers-generic linux-generic
grep -E 'CONFIG_NO_HZ_FULL|CONFIG_VFIO_PCI=' /boot/config-$(uname -r)   # nohz_full und vfio-pci müssen da sein
```

Ohne `CONFIG_NO_HZ_FULL=y` wirkt `nohz_full` nicht (MTL `doc/isolation.md`).

Kernel-Kommandozeile in `/etc/default/grub` (`GRUB_CMDLINE_LINUX_DEFAULT`), Kerne an die Maschine anpassen:

```text
intel_iommu=on iommu=pt default_hugepagesz=2M hugepagesz=2M hugepages=2048 isolcpus=4-7 nohz_full=4-7 rcu_nocbs=4-7
```

Danach `sudo update-grub` und Neustart. Ohne Eintrag in GRUB: `sudo scripts/setup-host.sh hugepages 2048`.

## 4. Software

```bash
scripts/install-mtl.sh                 # Pakete, DPDK, ice-Treiber, MTL (dauert)
scripts/check-e830.sh                  # Karte, Firmware, PTP, Hugepages, IOMMU, MTL prüfen
```

Firmware (NVM) bei Bedarf mit dem NVM-Update-Paket von Intel aktualisieren
(MTL `doc/e800_series_drivers.md`, Abschnitt NVM). Nach jedem Kernel-Update `scripts/install-mtl.sh ice`.

## 5. Port an DPDK übergeben

```bash
sudo -E scripts/setup-host.sh bind 0000:af:00.0   # PF von Port 0 (eingebautes PTP!)
```

Zurück an den Kernel: `sudo -E scripts/setup-host.sh unbind 0000:af:00.0`.

PF oder VF: MTL selbst arbeitet bevorzugt mit VFs (`nicctl.sh create_vf`). Das eingebaute PTP von MTL
braucht aber die Hardware-Uhr der PF; dieses Grundgerüst nutzt deshalb die PF direkt. Ob der Betrieb mit
VF (PF im Kernel mit `ptp4l`, VF mit Hardware-Zeitstempeln) gleichwertig misst, ist offen.

## 6. Erste Messung

```bash
cmake --preset release && cmake --build --preset release
sudo ./build/release/st2110-monitor --port 0000:af:00.0 --local-ip 192.168.100.10 \
     --video 239.100.0.1:20000,format=1920x1080p50,name=Testsender --duration 60 \
     --http 0.0.0.0:8090 --output config/local/messung.jsonl
```

Im Log von MTL prüfen, dass PTP synchronisiert und Hardware-Zeitstempel aktiv sind; die Oberfläche warnt,
wenn Pakete ohne Hardware-Zeitstempel ankommen.

Ohne Sender lässt sich die Kette mit dem Beispielprogramm von MTL auf Port 1 prüfen (Referenzsender,
`RxTxApp` bzw. `app/sample`), mit TSN-Pacing (`--pacing_way tsn`, nur PF, PTP an).

## Offene Punkte

- [x] Karte geliefert (2026-11-20): Controller **E830-L SFP** (`8086:12de`), nicht `12d3`; Firmware
  `1.00 0x80016d80 1.3833.0`, Ubuntu Server 26.04.1, Kernel 7.0.0-44, ice aus dem Kernel
- [ ] PCIe: Karte steckt in einem Steckplatz mit 5 GT/s x2 (rund 8 Gbit/s brutto) – reicht nicht für
  2160p, für 2 × 25 GbE nötig: PCIe 4.0 x8 (Steckplatz an der CPU)
- [ ] Messrechner (Stand 2026-11-20): Core i7-4790 (Haswell, 4 Kerne/8 Threads, AVX2, kein ART für
  `PTP_SYS_OFFSET_PRECISE`). Der Steckplatz der Karte hängt am Chipsatz (PCIe 2.0), der x16-Steckplatz an der CPU
  (PCIe 3.0) trug anfangs die Grafikkarte. VT-d war im BIOS aus und ist eingeschaltet
- [x] Erste Karte defekt: Die Firmware antwortete nach dem Start nicht mehr (`ethtool`/`devlink` mit
  `Input/output error`, zeitweise scheinbar Recovery-Modus), kein Link. Am 2026-11-23 getauscht, die Ersatzkarte
  bekommt Link. Vermutungen zu Steckplatz, IOMMU und `irdma` als Ursache waren damit unbelegt und sind zurückgenommen.
- [ ] Ersatzkarte: PCI-ID, Firmware (`devlink dev info`), Steckplatz und PCIe-Anbindung nachtragen
- [x] ice aus Kernel 7.0 mit DDP „ICE OS Default Package“ 1.3.43.0 (Ubuntu liefert es komprimiert, entpackt mit
  `setup-host.sh ddp`), PTP initialisiert
- [ ] E830-L: TSN-Pacing beim Senden und `PTP_SYS_OFFSET_PRECISE` auf der Karte prüfen (MTL testet `12de` nicht)
- [ ] Firmware-, Treiber- und DPDK-Version notiert (`docs/MESSUNGEN.md`)
- [ ] Erste Messung gegen bekannten Sender, Abgleich mit EBU LIST
- [ ] Rechte: Betrieb ohne root (MTL kann das laut Doku, Einrichtung offen)
- [ ] Patch aus `patches/mtl/` auf Hardware prüfen (Histogramme, Paketprofil)
- [ ] Datenpfad unter Last: keine Verluste, während die Web-Oberfläche abfragt
- [ ] PF- gegen VF-Betrieb vergleichen
