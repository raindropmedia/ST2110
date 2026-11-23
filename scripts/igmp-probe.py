#!/usr/bin/env python3
"""Sendet einen IGMPv3-Join wie MTL (ohne IP-Option) oder wie Linux (mit Router Alert).

Prüft, ob der Switch die Anmeldung von MTL annimmt. MTL v26.09 (lib/src/mt_mcast.c) schickt
IGMPv3-Reports ohne die Option Router Alert, die RFC 3376 verlangt; Linux setzt sie. Der Port muss
dafür am Kernel hängen (nicht an vfio-pci). Danach mit tcpdump prüfen, ob die Gruppe ankommt.

Aufruf (root):
  scripts/igmp-probe.py <schnittstelle> <eigene-ip> <gruppe> [--router-alert] [--leave]
Beispiel:
  sudo scripts/igmp-probe.py enp1s0f0np0 192.168.211.100 224.0.1.129
"""

import argparse
import socket
import struct
import sys


def checksum(data: bytes) -> int:
    if len(data) % 2:
        data += b"\0"
    total = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return ~total & 0xFFFF


def build_frame(src_mac: bytes, src_ip: str, group: str, router_alert: bool, leave: bool) -> bytes:
    # Gruppeneintrag: Join = CHANGE_TO_EXCLUDE ohne Quellen (wie MTL), Leave = CHANGE_TO_INCLUDE
    record_type = 3 if leave else 4
    record = struct.pack("!BBH4s", record_type, 0, 0, socket.inet_aton(group))
    igmp = struct.pack("!BBHHH", 0x22, 0, 0, 0, 1) + record
    igmp = igmp[:2] + struct.pack("!H", checksum(igmp)) + igmp[4:]

    options = b"\x94\x04\x00\x00" if router_alert else b""
    ihl = 5 + len(options) // 4
    total_length = ihl * 4 + len(igmp)
    # Wie MTL: TOS 0xc0, DF, TTL 1, Protokoll 2
    ip = struct.pack("!BBHHHBBH4s4s", (4 << 4) | ihl, 0xC0, total_length, 0, 0x4000, 1, 2, 0,
                     socket.inet_aton(src_ip), socket.inet_aton("224.0.0.22")) + options
    ip = ip[:10] + struct.pack("!H", checksum(ip)) + ip[12:]

    dst_mac = bytes.fromhex("01005e000016")
    return dst_mac + src_mac + b"\x08\x00" + ip + igmp


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument("interface")
    parser.add_argument("source_ip")
    parser.add_argument("group")
    parser.add_argument("--router-alert", action="store_true", help="IP-Option Router Alert setzen (wie Linux)")
    parser.add_argument("--leave", action="store_true", help="Gruppe verlassen statt beitreten")
    args = parser.parse_args()

    with open(f"/sys/class/net/{args.interface}/address", encoding="ascii") as f:
        src_mac = bytes.fromhex(f.read().strip().replace(":", ""))
    frame = build_frame(src_mac, args.source_ip, args.group, args.router_alert, args.leave)
    with socket.socket(socket.AF_PACKET, socket.SOCK_RAW) as s:
        s.bind((args.interface, 0))
        for _ in range(2):  # MTL sendet jeden Report zweimal
            s.send(frame)
    action = "Leave" if args.leave else "Join"
    alert = "mit" if args.router_alert else "ohne"
    print(f"IGMPv3-{action} für {args.group} {alert} Router Alert über {args.interface} gesendet.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
