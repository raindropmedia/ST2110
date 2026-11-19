#!/usr/bin/env bash
# Richtet den Messrechner zur Laufzeit ein (nicht dauerhaft; nach einem Neustart erneut ausführen).
#
# Aufruf (als root bzw. mit sudo):
#   scripts/setup-host.sh hugepages [anzahl]   2-MB-Hugepages anlegen (Vorgabe 2048 = 4 GiB)
#   scripts/setup-host.sh rmem                 net.core.rmem_max auf 4 MiB (nur für kernel:-Ports)
#   scripts/setup-host.sh bind <pci>           Port für DPDK an vfio-pci binden (PF, ohne VFs)
#   scripts/setup-host.sh unbind <pci>         Port zurück an den ice-Treiber geben
#
# "bind" und "unbind" nutzen script/nicctl.sh aus dem MTL-Quellordner (MTL_SRC).
# Achtung: Ein an vfio-pci gebundener Port verschwindet aus dem Betriebssystem. Nie den Port binden,
# über den die Verwaltung (SSH) läuft.

set -euo pipefail

MTL_SRC="${MTL_SRC:-$HOME/src/Media-Transport-Library}"

usage() {
	sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
	exit 1
}

require_root() {
	if [ "$(id -u)" -ne 0 ]; then
		echo "Bitte mit sudo ausführen (sudo -E erhält MTL_SRC)." >&2
		exit 1
	fi
}

check_pci() {
	if [[ ! "$1" =~ ^[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]$ ]]; then
		echo "Ungültige PCI-Adresse: $1 (Format 0000:af:00.0)" >&2
		exit 1
	fi
	if [ ! -e "/sys/bus/pci/devices/$1" ]; then
		echo "Gerät $1 nicht vorhanden." >&2
		exit 1
	fi
}

nicctl() {
	if [ ! -x "$MTL_SRC/script/nicctl.sh" ]; then
		echo "nicctl.sh nicht gefunden unter $MTL_SRC/script (MTL_SRC setzen)." >&2
		exit 1
	fi
	"$MTL_SRC/script/nicctl.sh" "$@"
}

cmd="${1:-}"
case "$cmd" in
hugepages)
	require_root
	count="${2:-2048}"
	[[ "$count" =~ ^[0-9]+$ ]] || usage
	sysctl -w vm.nr_hugepages="$count"
	grep -E '^HugePages_(Total|Free)' /proc/meminfo
	;;
rmem)
	require_root
	sysctl -w net.core.rmem_max=4194304
	;;
bind)
	require_root
	[ $# -eq 2 ] || usage
	check_pci "$2"
	for net in /sys/bus/pci/devices/"$2"/net/*; do
		[ -e "$net" ] || continue
		ifname=$(basename "$net")
		if ip -o -4 addr show dev "$ifname" | grep -q inet && ip route get 1.1.1.1 2>/dev/null | grep -q "dev $ifname"; then
			echo "Abbruch: $ifname ($2) trägt die Standardroute – vermutlich die Verwaltungsverbindung." >&2
			exit 1
		fi
	done
	modprobe vfio-pci
	nicctl bind_pmd "$2"
	;;
unbind)
	require_root
	[ $# -eq 2 ] || usage
	check_pci "$2"
	nicctl bind_kernel "$2"
	;;
*)
	usage
	;;
esac
