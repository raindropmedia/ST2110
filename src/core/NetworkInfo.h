#pragma once

#include <string>

namespace st2110 {

// Name der Schnittstelle mit der Standardroute aus dem Inhalt von /proc/net/route (bei mehreren
// Standardrouten die mit der kleinsten Metrik); leer, wenn es keine gibt. Das ist der Verwaltungsport des
// Messrechners: Die Karte im DPDK-Betrieb hat keine Kernel-Schnittstelle mehr.
std::string parseDefaultRouteInterface(const std::string& procNetRoute);

// Liest /proc/net/route; leer unter Systemen ohne diese Datei.
std::string defaultRouteInterface();

} // namespace st2110
