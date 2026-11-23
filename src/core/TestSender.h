#pragma once

#include "core/Options.h"

#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

namespace st2110 {

// Sendet ein Testsignal (siehe TestSignal.h). Die Verwaltung (TestSender) kennt keine MTL-Typen; das
// Backend öffnet die eigentlichen Sendesitzungen (MtlSource: st20p/st30p, Demo: nur Buchführung).
class TxBackend {
  public:
    // Eine laufende Sendesitzung. Das Zerstören beendet das Senden und gibt alles frei.
    class Session {
      public:
        virtual ~Session() = default;
    };

    virtual ~TxBackend() = default;

    // Öffnet eine Sendesitzung. Darf lange blockieren (MTL wartet bis zu 3 min auf PTP) und wird deshalb
    // nie unter einer Sperre der Verwaltung aufgerufen. nullptr mit Fehlermeldung, wenn es nicht geht.
    virtual std::unique_ptr<Session> open(const FlowConfig& flow, std::string& error) = 0;
};

enum class TxState { Stopped, Starting, Sending, Stopping, Error };

const char* txStateName(TxState state);

struct TxEntryInfo {
    std::string id; // "t1", "t2", …
    FlowConfig flow;
    TxState state = TxState::Stopped;
    std::string message; // Fehlertext bei TxState::Error
};

// Verwaltet die Testsender eines Sendeports: anlegen, ändern, starten, stoppen, löschen, zur Laufzeit und
// threadsicher. Das Starten läuft im Hintergrund (Zustand "Starting"), damit weder die Web-Oberfläche noch
// der Empfang auf MTL warten, bis PTP eingerastet ist. Die Verwaltung hält höchstens maxFlows Einträge.
class TestSender {
  public:
    TestSender(TxBackend& backend, unsigned maxFlows, std::string portName, std::string localIp);
    // Beendet alle Sitzungen und wartet auf laufende Startvorgänge (bei MTL bis zum Ende der PTP-Wartezeit).
    ~TestSender();
    TestSender(const TestSender&) = delete;
    TestSender& operator=(const TestSender&) = delete;

    // Prüft den Flow und legt ihn an (Zustand Stopped); mit start gleich danach starten. Liefert die ID.
    // Prüfungen: genau Video oder Ton, gültige Adresse und Format, keine Redundanz (der Testsender sendet
    // ohne ST 2022-7), Ziel nicht schon von einem anderen Eintrag benutzt, Höchstzahl.
    bool add(FlowConfig flow, bool start, std::string& id, std::string& error);
    // Ersetzt die Einstellungen; nur im Zustand Stopped oder Error.
    bool update(const std::string& id, FlowConfig flow, std::string& error);
    bool start(const std::string& id, std::string& error);
    // Beendet das Senden. Im Zustand Starting wartet MTL noch auf PTP: der Eintrag wechselt dann auf
    // Stopping und stoppt, sobald der Start durch ist.
    bool stop(const std::string& id, std::string& error);
    // Löscht einen Eintrag (ein sendender wird vorher gestoppt); nicht während Starting oder Stopping.
    bool remove(const std::string& id, std::string& error);

    std::vector<TxEntryInfo> list() const;
    nlohmann::json toJson() const;

    const std::string& portName() const { return portName_; }
    const std::string& localIp() const { return localIp_; }
    unsigned maxFlows() const { return maxFlows_; }

  private:
    struct Entry;
    std::shared_ptr<Entry> find(const std::string& id) const;
    bool validate(const FlowConfig& flow, const std::string& ignoreId, std::string& error) const;
    void launch(const std::shared_ptr<Entry>& entry);

    TxBackend& backend_;
    unsigned maxFlows_;
    std::string portName_;
    std::string localIp_;
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<Entry>> entries_;
    unsigned nextId_ = 1;
};

} // namespace st2110
