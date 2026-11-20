#!/usr/bin/env bash
# Prüft den Messrechner für MTL mit Intel E830 (bzw. E810): Karte, Treiber, PCIe, PTP, Hugepages,
# IOMMU, CPU-Isolation und installiertes MTL. Ändert nichts; ohne root fehlen nur einzelne Angaben.
#
# Aufruf: scripts/check-e830.sh
# Rückgabe: 0 = E830 gefunden und keine Fehler, 1 = Fehler (Details in der Ausgabe)

set -u

errors=0
warnings=0

ok() { printf '  [ OK ] %s\n' "$*"; }
warn() {
	printf '  [WARN] %s\n' "$*"
	warnings=$((warnings + 1))
}
fail() {
	printf '  [FEHL] %s\n' "$*"
	errors=$((errors + 1))
}
info() { printf '         %s\n' "$*"; }

# Geräte-IDs aus DPDK drivers/net/intel/ice/base/ice_devids.h
variant_of() {
	case "$1" in
	12d1) echo "E830-CC Backplane" ;;
	12d2) echo "E830-CC QSFP56" ;;
	12d3) echo "E830-CC SFP" ;;
	12d5) echo "E830-C Backplane" ;;
	12d8) echo "E830-C QSFP" ;;
	12da) echo "E830-C SFP" ;;
	12dc) echo "E830-L Backplane" ;;
	12dd) echo "E830-L QSFP" ;;
	12de) echo "E830-L SFP (z. B. E830-XXVDA2)" ;;
	1591) echo "E810-C Backplane" ;;
	1592) echo "E810-C QSFP" ;;
	1593) echo "E810-C SFP" ;;
	159b) echo "E810-XXV SFP (z. B. E810-XXVDA2)" ;;
	1599) echo "E810-XXV Backplane" ;;
	1889) echo "Adaptive VF (iavf)" ;;
	*) echo "" ;;
	esac
}

# Von MTL geprüft (doc/ci_runner_setup.md: e830 = 12d2, 12d3; e810 = 1592, 1593, 159b)
mtl_validated() {
	case "$1" in
	12d2 | 12d3 | 1592 | 1593 | 159b) return 0 ;;
	*) return 1 ;;
	esac
}

echo "== Netzwerkkarten (Intel E8xx)"
found_e830=0
found_any=0
for dev in /sys/bus/pci/devices/*; do
	[ -r "$dev/vendor" ] || continue
	[ "$(cat "$dev/vendor")" = "0x8086" ] || continue
	id=$(sed 's/^0x//' "$dev/device")
	variant=$(variant_of "$id")
	[ -n "$variant" ] || continue
	found_any=1
	bdf=$(basename "$dev")
	driver="-"
	[ -e "$dev/driver" ] && driver=$(basename "$(readlink -f "$dev/driver")")
	numa=$(cat "$dev/numa_node" 2>/dev/null || echo "?")

	echo "-- $bdf  8086:$id  $variant"
	case "$id" in
	12d*) found_e830=1 ;;
	esac
	if mtl_validated "$id"; then
		ok "Gerät steht in der Testliste von MTL"
	elif [ "$id" != "1889" ]; then
		warn "Gerät steht nicht in der Testliste von MTL (dort: 12d2, 12d3, 1592, 1593, 159b)"
	fi
	info "Treiber: $driver, NUMA-Knoten: $numa"

	if [ -r "$dev/current_link_speed" ]; then
		cur="$(cat "$dev/current_link_speed") x$(cat "$dev/current_link_width")"
		max="$(cat "$dev/max_link_speed") x$(cat "$dev/max_link_width")"
		if [ "$(cat "$dev/current_link_width")" -lt 8 ] && [ "$id" != "1889" ]; then
			warn "PCIe: $cur (maximal $max) – Steckplatz mit weniger als x8"
		else
			info "PCIe: $cur (maximal $max)"
		fi
	fi

	case "$driver" in
	ice)
		for net in "$dev"/net/*; do
			[ -e "$net" ] || continue
			ifname=$(basename "$net")
			speed=$(cat "$net/speed" 2>/dev/null || echo "?")
			info "Schnittstelle: $ifname, Link: ${speed} Mbit/s"
			if command -v ethtool >/dev/null 2>&1; then
				fw=$(ethtool -i "$ifname" 2>/dev/null | awk -F': ' '/^firmware-version/ {print $2}')
				ver=$(ethtool -i "$ifname" 2>/dev/null | awk -F': ' '/^version/ {print $2}')
				info "ice-Treiber: ${ver:-?}, Firmware: ${fw:-?}"
				ts=$(ethtool -T "$ifname" 2>/dev/null)
				# PTP-Uhr aus sysfs; ethtool ab 6.x schreibt statt "PTP Hardware Clock"
				# "Hardware timestamp provider index".
				phc=""
				for p in "$dev"/ptp/ptp*; do
					[ -e "$p" ] && phc="/dev/$(basename "$p")" && break
				done
				if printf '%s\n' "$ts" | grep -q "hardware-receive"; then
					ok "Hardware-Zeitstempel beim Empfang, PTP-Uhr: ${phc:-keine}"
				else
					fail "ethtool -T meldet keine Hardware-Zeitstempel beim Empfang"
				fi
			else
				warn "ethtool fehlt (apt install ethtool)"
			fi
		done
		if [ "$(cat "$dev/sriov_numvfs" 2>/dev/null || echo 0)" -gt 0 ]; then
			info "VFs angelegt: $(cat "$dev/sriov_numvfs")"
		fi
		;;
	vfio-pci)
		ok "an vfio-pci gebunden (DPDK)"
		;;
	iavf) info "VF im Kernel (für MTL an vfio-pci binden)" ;;
	-) warn "kein Treiber gebunden" ;;
	*) warn "unerwarteter Treiber: $driver" ;;
	esac
done
if [ "$found_any" -eq 0 ]; then
	fail "keine Intel-E810/E830-Karte gefunden"
elif [ "$found_e830" -eq 0 ]; then
	fail "keine E830 gefunden (nur E810 bzw. VFs)"
fi

echo
echo "== Betriebssystem"
cmdline=$(cat /proc/cmdline)
info "Kernel: $(uname -r)"
if [ -n "$(ls -A /sys/class/iommu 2>/dev/null)" ]; then
	ok "IOMMU aktiv"
else
	fail "IOMMU nicht aktiv (BIOS: VT-d; Kernel: intel_iommu=on iommu=pt)"
fi
case " $cmdline " in
*" iommu=pt "*) ok "iommu=pt gesetzt" ;;
*) warn "iommu=pt fehlt in der Kernel-Kommandozeile" ;;
esac
case " $cmdline " in
*" isolcpus="*) ok "isolierte CPU-Kerne: $(printf '%s\n' "$cmdline" | tr ' ' '\n' | grep '^isolcpus=')" ;;
*) warn "keine isolierten CPU-Kerne (isolcpus=…): Messwerte schwanken stärker" ;;
esac

hp_total=$(awk '/^HugePages_Total/ {print $2}' /proc/meminfo)
hp_free=$(awk '/^HugePages_Free/ {print $2}' /proc/meminfo)
hp_size=$(awk '/^Hugepagesize/ {print $2}' /proc/meminfo)
hp_1g=$(cat /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages 2>/dev/null || echo 0)
if [ "${hp_total:-0}" -gt 0 ] || [ "$hp_1g" -gt 0 ]; then
	ok "Hugepages: ${hp_total} x ${hp_size} kB (frei ${hp_free}), 1G-Seiten: ${hp_1g}"
else
	fail "keine Hugepages (scripts/setup-host.sh hugepages 2048)"
fi

rmem=$(cat /proc/sys/net/core/rmem_max 2>/dev/null || echo 0)
if [ "$rmem" -ge 4194304 ]; then
	ok "net.core.rmem_max = $rmem"
else
	warn "net.core.rmem_max = $rmem (unter 4 MiB; nur für kernel:-Ports relevant)"
fi

if [ -d /sys/module/vfio_pci ]; then
	ok "Modul vfio-pci geladen"
else
	warn "Modul vfio-pci nicht geladen (modprobe vfio-pci)"
fi

ddp=""
for f in /lib/firmware/updates/intel/ice/ddp/ice.pkg /lib/firmware/intel/ice/ddp/ice.pkg; do
	[ -e "$f" ] && ddp="$f" && break
done
if [ -n "$ddp" ]; then
	ok "DDP-Paket: $ddp -> $(readlink -f "$ddp")"
else
	# Neuere Distributionen liefern die Firmware komprimiert (ice.pkg.zst/.xz). Der Kernel entpackt sie
	# selbst, DPDK liest nur das unkomprimierte ice.pkg und läuft sonst im Safe Mode (wenige Queues, kein
	# Flow Director).
	packed=""
	for f in /lib/firmware/intel/ice/ddp/ice.pkg.zst /lib/firmware/intel/ice/ddp/ice.pkg.xz; do
		[ -e "$f" ] && packed="$f" && break
	done
	if [ -n "$packed" ]; then
		fail "DDP-Paket nur komprimiert ($packed); DPDK braucht es entpackt:"
		info "sudo scripts/setup-host.sh ddp"
	else
		fail "kein DDP-Paket (ice.pkg) gefunden (Paket linux-firmware)"
	fi
fi

echo
echo "== Software"
if command -v pkg-config >/dev/null 2>&1; then
	if pkg-config --exists libdpdk; then
		ok "DPDK $(pkg-config --modversion libdpdk)"
	else
		fail "DPDK nicht gefunden (pkg-config libdpdk; PKG_CONFIG_PATH prüfen)"
	fi
	if pkg-config --exists mtl; then
		ok "MTL $(pkg-config --modversion mtl)"
	else
		fail "MTL nicht gefunden (scripts/install-mtl.sh)"
	fi
else
	fail "pkg-config fehlt"
fi
for tool in dpdk-devbind.py ptp4l; do
	if command -v "$tool" >/dev/null 2>&1; then
		ok "$tool vorhanden"
	else
		info "$tool nicht gefunden (optional)"
	fi
done

echo
echo "Ergebnis: $errors Fehler, $warnings Warnungen"
[ "$errors" -eq 0 ]
