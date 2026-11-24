#pragma once

#include "core/Options.h"

#include <string>

namespace st2110 {

// SDP-Beschreibung eines Flows (ST 2110-10 bis -40, ST 2022-6) für Sender, die der Messplatz erzeugt. Mit ST
// 2022-7 (red=) entstehen zwei Medien einer DUP-Gruppe. sourceIp ist die Adresse des Senders (Quellfilter und
// o=); leer = ohne Quellfilter. Der Grandmaster ist MTL nicht bekannt: Die Zeitbasis steht als "traceable"
// (RFC 7273). Nutzdaten ohne Angabe in der SDP (z. B. ST 2110-40 Datenarten) folgen dem, was der Testsender
// wirklich sendet.
std::string flowToSdp(const FlowConfig& flow, const std::string& sourceIp);

} // namespace st2110
