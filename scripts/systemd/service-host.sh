#!/usr/bin/env bash
# Hilfsskript der Unit st2110-host.service: bereitet den Host vor (start) bzw. gibt die Ports zurück (stop).
# Liest ST2110_BIND_PORTS, ST2110_HUGEPAGES und MTL_SRC aus der Umgebung (EnvironmentFile).

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
setup="$here/../setup-host.sh"

case "${1:-}" in
start)
	"$setup" hugepages "${ST2110_HUGEPAGES:-2048}"
	for pci in ${ST2110_BIND_PORTS:-}; do
		"$setup" bind "$pci"
	done
	;;
stop)
	for pci in ${ST2110_BIND_PORTS:-}; do
		"$setup" unbind "$pci" || echo "Warnung: $pci konnte nicht zurückgegeben werden." >&2
	done
	;;
*)
	echo "Aufruf: $0 start|stop" >&2
	exit 1
	;;
esac
