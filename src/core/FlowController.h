#pragma once

#include "core/Options.h"

#include <string>

namespace st2110 {

// Flows zur Laufzeit hinzufügen und entfernen (umgesetzt von der Demo und vom Empfang über MTL).
class FlowController {
  public:
    virtual ~FlowController() = default;
    // Legt den Flow im Hub an und startet den Empfang; liefert die ID oder leer mit Fehlermeldung.
    virtual std::string addFlow(const FlowConfig& flow, std::string& error) = 0;
    virtual bool removeFlow(const std::string& id, std::string& error) = 0;
};

} // namespace st2110
