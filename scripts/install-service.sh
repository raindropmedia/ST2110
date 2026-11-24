#!/usr/bin/env bash
# Installiert den Dienst st2110-monitor (systemd): Programm, Skripte, Units und /etc/default/st2110.
#
# Aufruf (root), nach "cmake --preset release && cmake --build --preset release":
#   sudo scripts/install-service.sh            installieren (vorhandene /etc/default/st2110 bleibt)
#   sudo scripts/install-service.sh --enable   zusätzlich beim Start aktivieren und jetzt starten
#   sudo scripts/install-service.sh --remove   Dienst entfernen (Einstellungen bleiben)
#
# Danach /etc/default/st2110 anpassen (PCI-Adressen, IPs). Der Dienst bindet die Ports beim Start an
# vfio-pci; das Verwaltungsnetz darf nicht über diese Ports laufen.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
prefix=/opt/st2110
binary="$root/build/release/st2110-monitor"
units=(st2110-host.service st2110-monitor.service)

if [ "$(id -u)" -ne 0 ]; then
	echo "Bitte mit sudo ausführen." >&2
	exit 1
fi

if [ "${1:-}" = "--remove" ]; then
	systemctl disable --now st2110-monitor.service st2110-host.service 2>/dev/null || true
	for u in "${units[@]}"; do rm -f "/etc/systemd/system/$u"; done
	rm -f /usr/local/bin/st2110-monitor
	rm -rf "$prefix"
	systemctl daemon-reload
	echo "Dienst entfernt. /etc/default/st2110 bleibt erhalten."
	exit 0
fi

if [ ! -x "$binary" ]; then
	echo "Programm nicht gefunden: $binary (zuerst cmake --preset release bauen)." >&2
	exit 1
fi

install -d "$prefix/scripts/systemd" "$prefix/webui"
install -m 0755 "$binary" /usr/local/bin/st2110-monitor
install -m 0755 "$root/scripts/setup-host.sh" "$prefix/scripts/setup-host.sh"
install -m 0755 "$root/scripts/systemd/service-host.sh" "$prefix/scripts/systemd/service-host.sh"
cp -r "$root/webui/." "$prefix/webui/"
for u in "${units[@]}"; do
	install -m 0644 "$root/scripts/systemd/$u" "/etc/systemd/system/$u"
done
if [ -e /etc/default/st2110 ]; then
	echo "/etc/default/st2110 vorhanden, bleibt unverändert."
else
	install -m 0644 "$root/scripts/systemd/st2110.default" /etc/default/st2110
	echo "/etc/default/st2110 angelegt: bitte PCI-Adressen und IPs anpassen."
fi
systemctl daemon-reload

if [ "${1:-}" = "--enable" ]; then
	systemctl enable --now st2110-host.service st2110-monitor.service
	systemctl --no-pager status st2110-monitor.service || true
else
	echo "Installiert. Starten mit: sudo systemctl enable --now st2110-host st2110-monitor"
fi
