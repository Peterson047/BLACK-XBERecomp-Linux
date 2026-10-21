#pragma once

// The adapter: the piece that puts the co-op into the game.
//
// The session talks to the network, the world layer talks to the engine and
// the mission layer talks to the level script. The adapter is what connects
// them, and what decides which side of the co-op this process is.
//
// It runs in the frame tick the game already has, behind the same
// clock. It does not add a second one.

#include <cstdint>

#include "net/coop_session.h"

namespace game {
namespace coop {

class Adapter {
public:
    static Adapter& Get();

    // The co-op is off until the command line or the menu asks for it.
    void Enable();
    bool IsEnabled() const { return enabled_; }

    // The frame tick. Sends the snapshot, publishes the events, watches the
    // load barrier and the end of the mission.
    void Tick(float dt);

    // The host names the mission. The menu calls it; the tick calls it when
    // a partner shows up and nobody has named it yet.
    void StartMission();

    // The mission ended, or the session is over: the summary goes to the log
    // so the gate can read it.
    void LogSummary();

    net::CoopSession& Session() { return session_; }

private:
    Adapter();

    void SetRole();
    void OnRoomPhase(net::coop::Phase phase);
    bool LevelLoaded() const;

    bool enabled_ = false;
    bool mission_named_ = false;
    bool test_kill_armed_ = false;
    bool level_loaded_ = false;
    bool launch_requested_ = false;
    bool summary_logged_ = false;
    float snapshot_timer_ = 0.0f;
    net::CoopSession session_{net::NetworkManager::Get()};
};

}  // namespace coop
}  // namespace game
