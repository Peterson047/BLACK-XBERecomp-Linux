#include "game/coop_adapter.h"

#include "core/log.h"
#include "game/coop_mission.h"
#include "game/coop_world.h"
#include "game/layout.h"
#include "game/multiplayer.h"

#include <algorithm>
#include <cstdlib>

namespace game {
namespace coop {
namespace {

// How often the host publishes the state of the world.
constexpr float kSnapshotInterval = 0.1f;  // 10 Hz

// The test harness: BLACK_COOP_TEST_KILL=1 makes the host kill an enemy, so
// the replication of the death can be checked by the log gate.
bool TestKillEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("BLACK_COOP_TEST_KILL");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

}  // namespace

Adapter& Adapter::Get() {
    static Adapter instance;
    return instance;
}

Adapter::Adapter() {
    // The session hands the world layer what the host sent, and the mission
    // layer fires the host's events in the guest's own bus.
    session_.SetSnapshotHandler([](const net::coop::Snapshot& snapshot) {
        World::Get().ApplySnapshot(snapshot);
    });
    session_.SetEventHandler([](const net::coop::Event& event) {
        World::Get().ApplyEvent(event);
        Mission::Get().ApplyEvent(event);
    });
    session_.SetRoundHandler([this](uint32_t epoch, bool started) {
        if (!started) return;
        World::Get().ResetRound(epoch);
        Mission::Get().ResetRound(epoch);
        // The test harness kills an enemy, so the replication of the death
        // can be checked without a player at the keyboard.
        if (TestKillEnabled()) test_kill_armed_ = true;
    });
}

void Adapter::Enable() {
    if (enabled_) return;
    enabled_ = true;
    session_.SetEnabled(true);
    LOGI("coop: enabled");
}

void Adapter::SetRole() {
    if (!enabled_) return;
    auto& network = net::NetworkManager::Get();
    const net::NetMode mode = network.GetMode();
    if (mode == net::NetMode::Host) {
        if (World::Get().GetRole() != Role::kHost) {
            World::Get().SetRole(Role::kHost);
            Mission::Get().SetHost(true);
            World::Get().SetEventPublisher([](const net::coop::Event& event) {
                Adapter::Get().session_.PublishEvent(event);
            });
            LOGI("coop: role host");
        }
    } else if (mode == net::NetMode::Client) {
        if (World::Get().GetRole() != Role::kGuest) {
            World::Get().SetRole(Role::kGuest);
            Mission::Get().SetHost(false);
            LOGI("coop: role guest");
        }
    }
}

void Adapter::OnRoomPhase(net::coop::Phase phase) {
    if (phase != net::coop::Phase::Loading) return;
    // The mission is named: the level has to load on both sides, and both
    // sides start loading at about the same time, so the two copies of the
    // level script begin together.
    if (!launch_requested_) {
        launch_requested_ = true;
        MultiplayerManager::Get().RequestDirectMapLaunch();
        LOGI("coop: mission named, loading the level");
    }
}

bool Adapter::LevelLoaded() const {
    // The level is loaded when its table of objectives has been parsed: the
    // engine reads LevelDat.bin when the level comes up, and the table is
    // empty before that. The game state alone is not enough, it is already
    // out of the front end while the level is still loading.
    return Mission::Get().ReadObjectives().count > 0;
}

void Adapter::StartMission() {
    if (!enabled_) return;
    if (!World::Get().IsHost()) return;
    if (mission_named_) return;
    mission_named_ = true;
    session_.HostStartMission(0);
}

void Adapter::Tick(float dt) {
    if (!enabled_) return;

    SetRole();
    session_.Update(dt);

    const net::coop::Phase phase = session_.GetPhase();
    if (World::Get().IsHost()) {
        if (!mission_named_ && net::NetworkManager::Get().GetPlayers().size() > 1) {
            // The mission is named when there is somebody to name it for: the
            // two copies of the level have to start loading at the same time,
            // or their scripts begin seconds apart.
            StartMission();
        }
        OnRoomPhase(session_.GetPhase());
    } else {
        // The guest follows the room the host publishes.
        const uint8_t room_phase = session_.GetRoom().state.phase;
        OnRoomPhase(static_cast<net::coop::Phase>(room_phase));
    }

    // The level finished loading: the barrier can close.
    if (!level_loaded_ && LevelLoaded()) {
        level_loaded_ = true;
        session_.SetLoaded(true);
        LOGI("coop: level loaded");
    }

    if (World::Get().IsHost()) {
        snapshot_timer_ += dt;
        if (snapshot_timer_ >= kSnapshotInterval) {
            snapshot_timer_ = 0.0f;
            net::coop::Snapshot snapshot;
            if (World::Get().BuildSnapshot(snapshot)) session_.PublishSnapshot(snapshot);
        }
        // The events the world layer produced: the host's damage.
        for (const net::coop::Event& event : World::Get().TakePendingEvents()) {
            session_.PublishEvent(event);
        }
    }

    if (Mission::Get().IsHost()) {
        // The events the level script fired.
        net::coop::Event event;
        while (Mission::Get().NextEvent(&event)) session_.PublishEvent(event);
    }

    // The test harness's kill, once there is an actor to kill. The level
    // spawns its actors while it comes up, so this may take a moment.
    if (test_kill_armed_ && session_.GetPhase() == net::coop::Phase::InMission) {
        // ArmTestKill says false when there is no living actor yet; the
        // question is asked again the next frame.
        if (World::Get().ArmTestKill()) test_kill_armed_ = false;
    }

    // The mission watches the state of the objectives and the end of the
    // level. It is pumped here, on the thread that draws, the same way the
    // Deathmatch reads the engine's memory. The engine's own frame counter
    // (0x000BE550) is never called, so it is not a place to pump it from.
    //
    // What the mission does to the engine - firing the event of the host in
    // the guest's own bus - happens on the thread that runs the guest, in
    // the tick of the bus.
    Mission::Get().Pump();

    if (phase == net::coop::Phase::Ended && !summary_logged_) {
        LogSummary();
    }
}

void Adapter::LogSummary() {
    if (summary_logged_) return;
    summary_logged_ = true;
    // Each layer prints its own line, and then the session prints one line
    // with everything in it. The checker reads the last one, so it is the
    // one that has to be complete.
    World::Get().LogSummary();
    Mission::Get().LogSummaryOnce();
    const uint32_t epoch = session_.GetStartEpoch();
    const uint32_t snapshots = session_.GetSentSnapshotCount() + session_.GetReceivedSnapshotCount();
    const uint32_t events = session_.GetSentEventCount() + session_.GetAppliedEventCount();
    const uint32_t unpaired = World::Get().UnpairedCount() + Mission::Get().Unpaired();
    const uint32_t divergent =
        World::Get().DivergentCount() + Mission::Get().Divergent() + session_.GetDivergenceCount();
    LOGI("coop: summary epoch=%u snapshots=%u events=%u acked=%u unpaired=%u divergent=%u", epoch, snapshots, events,
         session_.GetAppliedEventCount(), unpaired, divergent);
}

}  // namespace coop
}  // namespace game
