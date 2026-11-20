#!/usr/bin/env bash
# Installiert MTL in einer festen Version mit den Skripten von MTL selbst:
# Build-Pakete (Ubuntu), DPDK mit MTL-Patches, ice-Treiber mit Rate-Limit-Patches, MTL.
# Vor dem Bau von MTL wendet es die eigenen Patches aus patches/mtl/<version>/ an (Paket-Rückruf
# für Interpacket-Histogramme und Paketprofile).
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
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PATCH_DIR="$REPO_ROOT/patches/mtl/$MTL_VERSION"

steps=("$@")
[ "${#steps[@]}" -gt 0 ] || steps=(packages dpdk ice mtl)

# Fehlwarnungen neuerer GCC-Versionen, die MTL mit -Werror sonst zum Fehler macht
MTL_GCC_RELAX="-Wno-error=format-truncation -Wno-error=stringop-truncation -Wno-error=stringop-overflow -Wno-error=array-bounds"

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

apply_patches() {
	if [ ! -d "$PATCH_DIR" ]; then
		echo "Hinweis: keine eigenen Patches für $MTL_VERSION ($PATCH_DIR); Histogramme und Paketprofile fehlen."
		return
	fi
	local patch
	for patch in "$PATCH_DIR"/*.patch; do
		[ -e "$patch" ] || continue
		if git -C "$MTL_SRC" apply --reverse --check "$patch" >/dev/null 2>&1; then
			echo "Patch bereits angewendet: $(basename "$patch")"
		elif git -C "$MTL_SRC" apply --check "$patch"; then
			git -C "$MTL_SRC" apply "$patch"
			echo "Patch angewendet: $(basename "$patch")"
		else
			echo "Fehler: Patch $(basename "$patch") passt nicht zu $MTL_SRC." >&2
			exit 1
		fi
	done
}

for step in "${steps[@]}"; do
	case "$step" in
	packages)
		need_ubuntu
		log "Build-Pakete installieren (laut MTL doc/build.md)"
		sudo apt-get update
		sudo apt-get install -y git gcc g++ meson ninja-build cmake python3 python3-pyelftools pkg-config \
			libnuma-dev libjson-c-dev libpcap-dev libgtest-dev libssl-dev systemtap-sdt-dev llvm clang \
			flex byacc ethtool pciutils linuxptp unzip wget curl patch zstd "linux-headers-$(uname -r)"
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
		log "Eigene Patches anwenden"
		apply_patches
		log "MTL bauen und installieren"
		# MTL baut ohne debug mit -Werror. GCC 15 (Ubuntu 26.04) meldet dabei Fehlwarnungen, z. B.
		# format-truncation in st40_pipeline_rx.c (Index ist auf MTL_SESSION_PORT_MAX begrenzt, GCC sieht
		# das nicht). Diese Warnungen bleiben sichtbar, brechen den Bau aber nicht ab. Meson liest CFLAGS
		# nur beim ersten Einrichten, deshalb die Build-Ordner vorher entfernen.
		rm -rf "$MTL_SRC/build" "$MTL_SRC/tests/tools/RxTxApp/build"
		(cd "$MTL_SRC" && CFLAGS="${CFLAGS:-} $MTL_GCC_RELAX" CXXFLAGS="${CXXFLAGS:-} $MTL_GCC_RELAX" ./build.sh)
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
