#include "core/TestSender.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <doctest/doctest.h>
#include <mutex>
#include <thread>

using namespace st2110;

namespace {

// Backend ohne MTL: zählt offene Sitzungen; open() kann angehalten werden (wie das Warten auf PTP) oder
// scheitern.
class FakeBackend : public TxBackend {
  public:
    class FakeSession : public Session {
      public:
        explicit FakeSession(FakeBackend& b) : backend_(b) { ++backend_.open_; }
        ~FakeSession() override { --backend_.open_; }

      private:
        FakeBackend& backend_;
    };

    std::unique_ptr<Session> open(const FlowConfig&, std::string& error) override {
        ++opens_;
        {
            std::unique_lock lock(mutex_);
            entered_ = true;
            cv_.notify_all();
            cv_.wait(lock, [&] { return !blocked_; });
        }
        if (fail_) {
            error = "kein Link";
            return nullptr;
        }
        return std::make_unique<FakeSession>(*this);
    }

    void block() {
        std::lock_guard lock(mutex_);
        blocked_ = true;
        entered_ = false;
    }
    void waitEntered() {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return entered_; });
    }
    void release() {
        std::lock_guard lock(mutex_);
        blocked_ = false;
        cv_.notify_all();
    }

    std::atomic<int> open_{0};
    std::atomic<int> opens_{0};
    bool fail_ = false;

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool blocked_ = false;
    bool entered_ = false;
};

FlowConfig videoFlow(const std::string& spec = "239.211.0.1:20000,format=1920x1080p50") {
    FlowConfig f;
    std::string error;
    REQUIRE(parseFlowSpec(Media::Video, spec, f, error));
    return f;
}

FlowConfig audioFlow(const std::string& spec = "239.211.0.2:30000,format=L24/48000/2") {
    FlowConfig f;
    std::string error;
    REQUIRE(parseFlowSpec(Media::Audio, spec, f, error));
    return f;
}

// Wartet, bis der Zustand erreicht ist (Startvorgänge laufen im Hintergrund).
bool waitFor(TestSender& sender, const std::string& id, TxState state) {
    for (int i = 0; i < 500; ++i) {
        for (const auto& e : sender.list()) {
            if (e.id == id && e.state == state) {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

} // namespace

TEST_CASE("Testsender: anlegen, starten, stoppen, löschen") {
    FakeBackend backend;
    TestSender sender(backend, 4, "0000:01:00.1", "192.168.211.101");
    std::string id;
    std::string error;
    REQUIRE(sender.add(videoFlow(), false, id, error));
    CHECK(id == "t1");
    CHECK(sender.list()[0].state == TxState::Stopped);
    CHECK(sender.list()[0].flow.label == "Testbild t1");
    CHECK(backend.open_ == 0);

    REQUIRE(sender.start(id, error));
    REQUIRE(waitFor(sender, id, TxState::Sending));
    CHECK(backend.open_ == 1);

    REQUIRE(sender.stop(id, error));
    CHECK(sender.list()[0].state == TxState::Stopped);
    CHECK(backend.open_ == 0);

    // erneut starten, dann löschen (ein sendender wird vorher gestoppt)
    REQUIRE(sender.start(id, error));
    REQUIRE(waitFor(sender, id, TxState::Sending));
    REQUIRE(sender.remove(id, error));
    CHECK(sender.list().empty());
    CHECK(backend.open_ == 0);
}

TEST_CASE("Testsender: Anlegen mit sofortigem Start, mehrere Einträge, eindeutige IDs") {
    FakeBackend backend;
    TestSender sender(backend, 4, "p", "ip");
    std::string a, b, error;
    REQUIRE(sender.add(videoFlow(), true, a, error));
    REQUIRE(sender.add(audioFlow(), true, b, error));
    CHECK(a == "t1");
    CHECK(b == "t2");
    REQUIRE(waitFor(sender, a, TxState::Sending));
    REQUIRE(waitFor(sender, b, TxState::Sending));
    CHECK(backend.open_ == 2);
    REQUIRE(sender.remove(a, error));
    std::string c;
    REQUIRE(sender.add(videoFlow(), false, c, error));
    CHECK(c == "t3"); // IDs werden nicht wiederverwendet
}

TEST_CASE("Testsender: Prüfungen beim Anlegen und Ändern") {
    FakeBackend backend;
    TestSender sender(backend, 2, "p", "ip");
    std::string id, error;
    REQUIRE(sender.add(videoFlow(), false, id, error));

    // Ziel schon vergeben
    CHECK_FALSE(sender.add(videoFlow(), false, id, error));
    CHECK(error.find("schon von t1") != std::string::npos);
    // Ungültige Adresse
    FlowConfig bad = audioFlow();
    bad.audio->primary.ip = "keine-ip";
    CHECK_FALSE(sender.add(bad, false, id, error));
    // Redundanz wird nicht gesendet
    FlowConfig red = audioFlow();
    red.audio->redundant = FlowAddress{"239.212.0.2", 30000, ""};
    CHECK_FALSE(sender.add(red, false, id, error));
    CHECK(error.find("ST 2022-7") != std::string::npos);
    // Höchstzahl
    REQUIRE(sender.add(audioFlow(), false, id, error));
    CHECK_FALSE(sender.add(audioFlow("239.211.0.9:30000"), false, id, error));
    CHECK(error.find("Höchstzahl") != std::string::npos);

    // Ändern: gleicher Eintrag darf sein Ziel behalten, ein anderes Ziel nicht übernehmen
    CHECK(sender.update("t1", videoFlow("239.211.0.1:20000,format=1280x720p50"), error));
    CHECK(sender.list()[0].flow.video->format.width == 1280);
    CHECK_FALSE(sender.update("t1", videoFlow("239.211.0.2:30000"), error));
    // Video lässt sich nicht in Ton umwandeln
    CHECK_FALSE(sender.update("t1", audioFlow("239.211.0.7:30000"), error));
    CHECK_FALSE(sender.update("t9", videoFlow(), error));
    CHECK_FALSE(sender.start("t9", error));
}

TEST_CASE("Testsender: Ändern nur im gestoppten Zustand") {
    FakeBackend backend;
    TestSender sender(backend, 4, "p", "ip");
    std::string id, error;
    REQUIRE(sender.add(videoFlow(), true, id, error));
    REQUIRE(waitFor(sender, id, TxState::Sending));
    CHECK_FALSE(sender.update(id, videoFlow("239.211.0.1:20000,format=1280x720p50"), error));
    CHECK(error.find("gestoppt") != std::string::npos);
    REQUIRE(sender.stop(id, error));
    CHECK(sender.update(id, videoFlow("239.211.0.1:20000,format=1280x720p50"), error));
}

TEST_CASE("Testsender: Fehler beim Öffnen, danach erneut starten") {
    FakeBackend backend;
    backend.fail_ = true;
    TestSender sender(backend, 4, "p", "ip");
    std::string id, error;
    REQUIRE(sender.add(videoFlow(), true, id, error));
    REQUIRE(waitFor(sender, id, TxState::Error));
    CHECK(sender.list()[0].message == "kein Link");
    CHECK(backend.open_ == 0);

    backend.fail_ = false;
    REQUIRE(sender.start(id, error));
    REQUIRE(waitFor(sender, id, TxState::Sending));
    CHECK(sender.list()[0].message.empty());
}

TEST_CASE("Testsender: Start blockiert (PTP-Wartezeit), Stoppen währenddessen") {
    FakeBackend backend;
    TestSender sender(backend, 4, "p", "ip");
    std::string id, error;
    backend.block();
    REQUIRE(sender.add(videoFlow(), true, id, error));
    backend.waitEntered();
    CHECK(sender.list()[0].state == TxState::Starting);
    // Während des Starts: nicht löschen, Starten ist ein No-op
    CHECK_FALSE(sender.remove(id, error));
    CHECK(sender.start(id, error));
    REQUIRE(sender.stop(id, error));
    CHECK(sender.list()[0].state == TxState::Stopping);
    CHECK_FALSE(sender.start(id, error)); // erst nach dem Stoppen
    backend.release();
    REQUIRE(waitFor(sender, id, TxState::Stopped));
    CHECK(backend.open_ == 0); // die fertig geöffnete Sitzung wurde sofort verworfen
    REQUIRE(sender.start(id, error));
    REQUIRE(waitFor(sender, id, TxState::Sending));
}

TEST_CASE("Testsender: Zerstören wartet auf den Start und gibt alles frei") {
    FakeBackend backend;
    std::thread release;
    {
        TestSender sender(backend, 4, "p", "ip");
        std::string a, b, error;
        REQUIRE(sender.add(videoFlow(), true, a, error));
        REQUIRE(waitFor(sender, a, TxState::Sending));
        backend.block();
        REQUIRE(sender.add(audioFlow(), true, b, error));
        backend.waitEntered();
        release = std::thread([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            backend.release();
        });
        // Zerstören während des Starts von b: wartet, bis open() zurückkehrt
    }
    release.join();
    CHECK(backend.open_ == 0);
}

TEST_CASE("Testsender: JSON für die Oberfläche") {
    FakeBackend backend;
    TestSender sender(backend, 4, "0000:01:00.1", "192.168.211.101");
    std::string id, error;
    REQUIRE(sender.add(videoFlow("239.211.0.1:20000,format=1920x1080i50,name=Kamera"), false, id, error));
    REQUIRE(sender.add(audioFlow("239.211.0.2:30000,format=L16/96000/8,ptime=0.125"), false, id, error));
    const auto json = sender.toJson();
    CHECK(json["port"] == "0000:01:00.1");
    CHECK(json["maxFlows"] == 4);
    REQUIRE(json["flows"].size() == 2);
    const auto& v = json["flows"][0];
    CHECK(v["kind"] == "video");
    CHECK(v["label"] == "Kamera");
    CHECK(v["state"] == "stopped");
    CHECK(v["video"]["interlaced"] == true);
    CHECK(v["video"]["rate"] == "50");
    CHECK(v["video"]["width"] == 1920);
    CHECK(v["video"]["startVrx"] == 0);
    const auto& a = json["flows"][1];
    CHECK(a["kind"] == "audio");
    CHECK(a["audio"]["channels"] == 8);
    CHECK(a["audio"]["sampleRate"] == 96000);
    CHECK(a["audio"]["bitDepth"] == 16);
    CHECK(a["audio"]["ptime"] == "125us");
}

TEST_CASE("Testsender: ANC und ST 2022-6 anlegen, starten, Art nicht änderbar") {
    FakeBackend backend;
    TestSender sender(backend, 4, "0000:01:00.1", "192.168.211.101");
    std::string id, error;
    FlowConfig anc;
    REQUIRE(parseFlowSpec(Media::Anc, "239.211.0.3:40000,format=1920x1080p25,pt=100", anc, error));
    REQUIRE(sender.add(anc, true, id, error));
    CHECK(sender.list()[0].flow.label == "Testdaten t1");
    REQUIRE(waitFor(sender, id, TxState::Sending));
    FlowConfig sdi;
    std::string id2;
    REQUIRE(parseFlowSpec(Media::Hbrmt, "239.211.0.4:5004,pt=98", sdi, error));
    REQUIRE(sender.add(sdi, true, id2, error));
    CHECK(sender.list()[1].flow.label == "Test-SDI t2");
    REQUIRE(waitFor(sender, id2, TxState::Sending));
    const auto j = sender.toJson();
    CHECK(j["flows"][0]["kind"] == "anc");
    CHECK(j["flows"][0]["anc"]["rate"] == "25");
    CHECK(j["flows"][1]["kind"] == "hbrmt");
    CHECK(j["flows"][1]["hbrmt"]["datagramsPerFrame"] == 982);
    REQUIRE(sender.stop(id2, error));
    // Art ändern geht nicht
    CHECK_FALSE(sender.update(id2, videoFlow("239.211.0.9:20000"), error));
    CHECK(error.find("Art") != std::string::npos);
    CHECK(sender.update(id2, sdi, error));
}

TEST_CASE("Testsender: Pixelformate und Packmodus, 4:2:0 nur progressiv") {
    FakeBackend backend;
    TestSender sender(backend, 4, "0000:01:00.1", "192.168.211.101");
    std::string id, error;
    REQUIRE(sender.add(videoFlow("239.211.0.7:20000,format=1920x1080p50,sampling=RGB,depth=12,packing=GPM"),
                       true, id, error));
    REQUIRE(waitFor(sender, id, TxState::Sending));
    const auto j = sender.toJson();
    CHECK(j["flows"][0]["video"]["sampling"] == "RGB");
    CHECK(j["flows"][0]["video"]["depth"] == 12);
    CHECK(j["flows"][0]["video"]["packing"] == "GPM");
    CHECK(j["flows"][0]["format"] == "1920x1080p50 RGB 12 Bit");
    std::string id2;
    CHECK_FALSE(sender.add(videoFlow("239.211.0.8:20000,format=1920x1080i50,sampling=YCbCr-4:2:0,depth=8"),
                           false, id2, error));
    CHECK(error.find("4:2:0") != std::string::npos);
    CHECK(sender.add(videoFlow("239.211.0.8:20000,format=1920x1080p50,sampling=YCbCr-4:2:0,depth=8"), false,
                     id2, error));
}
