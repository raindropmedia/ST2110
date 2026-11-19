#!/usr/bin/env bash
# Installiert MTL in einer festen Version mit den Skripten von MTL selbst:
# Build-Pakete (Ubuntu), DPDK mit MTL-Patches, ice-Treiber mit Rate-Limit-Patches, MTL.
#
# Aufruf: scripts/install-mtl.sh [schritt ...]
#   Schritte: packages dpdk ice mtl (Vorgabe: alle in dieser Reihenfolge)
# Umgebung:
#   MTL_VERSION  Tag von MTL (Vorgabe siehe unten; Wechsel in docs/MESSUNGEN.md vermerken)
#   MTL_SRC      Quellordner (Vorgabe: $HOME/src/Media-Transport-Library)
#
# Braucht sudo für Pakete, Treiber und die Installation nach /usr/local.
# Nach einem Kernel-Update den Schritt "ice" wiederholen.

set -euo pipefail

MTL_VERSION="${MTL_VERSION:-v26.09}"
MTL_SRC="${MTL_SRC:-$HOME/src/Media-Transport-Library}"
MTL_REPO="https://github.com/OpenVisualCloud/Media-Transport-Library.git"

steps=("$@")
[ "${#steps[@]}" -gt 0 ] || steps=(packages dpdk ice mtl)

log() { printf '\n==> %s\n' "$*"; }

need_ubuntu() {
	if ! grep -q '^ID=ubuntu' /etc/os-release 2>/dev/null; then
		echo "Schritt 'packages' ist nur für Ubuntu ausgelegt; Pakete laut MTL doc/build.md selbst installieren." >&2
		exit 1
	fi
}

checkout() {
	if [ ! -d "$MTL_SRC/.git" ]; then
		log "MTL $MTL_VERSION nach $MTL_SRC klonen"
		mkdir -p "$(dirname "$MTL_SRC")"
		git clone --branch "$MTL_VERSION" --depth 1 "$MTL_REPO" "$MTL_SRC"
	fi
	local current
	current=$(git -C "$MTL_SRC" describe --tags --exact-match 2>/dev/null || echo "?")
	if [ "$current" != "$MTL_VERSION" ]; then
		echo "Fehler: $MTL_SRC steht auf '$current', erwartet '$MTL_VERSION'." >&2
		echo "Ordner entfernen oder MTL_SRC auf einen anderen Ordner setzen." >&2
		exit 1
	fi
}

for step in "${steps[@]}"; do
	case "$step" in
	packages)
		need_ubuntu
		log "Build-Pakete installieren (laut MTL doc/build.md)"
		sudo apt-get update
		sudo apt-get install -y git gcc g++ meson ninja-build cmake python3 python3-pyelftools pkg-config \
			libnuma-dev libjson-c-dev libpcap-dev libgtest-dev libssl-dev systemtap-sdt-dev llvm clang \
			flex byacc ethtool pciutils linuxptp "linux-headers-$(uname -r)"
		;;
	dpdk)
		checkout
		log "DPDK mit MTL-Patches bauen und installieren"
		(cd "$MTL_SRC" && ./script/build_dpdk.sh)
		;;
	ice)
		checkout
		log "ice-Treiber mit MTL-Patches bauen und laden"
		echo "Hinweis: Das Laden des Treibers unterbricht kurz alle Netzwerkverbindungen über E8xx-Karten."
		(cd "$MTL_SRC" && ./script/build_drivers.sh --driver ice)
		;;
	mtl)
		checkout
		log "MTL bauen und installieren"
		(cd "$MTL_SRC" && ./build.sh)
		sudo ldconfig
		;;
	*)
		echo "Unbekannter Schritt: $step (erlaubt: packages dpdk ice mtl)" >&2
		exit 1
		;;
	esac
done

log "Fertig. Prüfen mit: scripts/check-e830.sh"
if ! pkg-config --exists mtl 2>/dev/null; then
	echo "mtl.pc nicht im Suchpfad, z. B.:"
	echo "  export PKG_CONFIG_PATH=/usr/local/lib/x86_64-linux-gnu/pkgconfig:/usr/local/lib/pkgconfig:\${PKG_CONFIG_PATH:-}"
fi
