#pragma once
// The partner's body: the actor that stands in for the partner in the
// guest's world.
//
// The co-op runs two copies of the level: the host owns the world and the
// guest mirrors it. The partner is a player in the host's world and there is
// nothing of it in the guest's world - the guest has a player of its own and
// the level's actors, and the partner is none of them. This layer is the
// actor that fills that hole.
//
// The body is an actor of the engine, made the way the engine makes one
// (`0x001DF340`): it comes from the pool of the actor manager and goes into
// the list of the living actors, and being in that list is what makes the
// engine tick it, and the tick is what runs the animation.
//
// The animation is driven by the state that comes over the network:
//
//  - the record at `actor+0x32C` says what the body is doing: standing (1),
//    aiming (13) and the shot (7). The table of the states and the clips
//    they produce is in `.agents/teamwork/coop/anim/STATE_TABLE.md`;
//  - the speed at `actor+0x2D8` says how fast the body is going, and the
//    anim set of the character type picks the walk clip by itself when the
//    body moves (the record does not carry the speed);
//  - the position and the direction go in with the engine's own calls
//    (`0x001CE660` and `0x001CE600`).
//
// The body is not an object of the level: the pair of the identity stays at
// 0/0, so the level script never gets a handle on it and the matcher of the
// objectives never matches it (LAYOUT.md §2 and §8).
//
// The engine's memory is only touched on the engine's thread: `CurrentContext()`
// dereferences a thread local that is null on the thread that draws.
//
// The offsets and their proofs are in `.agents/teamwork/coop/LAYOUT.md`.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "net/coop_protocol.h"
#include "net/network.h"

namespace game {
namespace coop {

// The body of the partner.
//
// The guest owns one; the host has none - the host is the partner. The host
// does publish the shots of its own weapon, because the guest's body has to
// play the shot clip when the partner fires.
class Body {
public:
    static Body& Get();

    // The round. The epoch travels with every event, and an event of an
    // older round is dropped by the session, so the body has to know which
    // round it is in to publish an event the guest will apply.
    void SetEpoch(uint32_t epoch) { epoch_ = epoch; }
    uint32_t GetEpoch() const { return epoch_; }

    // The host publishes the events of the world through the session. The
    // adapter gives the body the same publisher the world layer has.
    using EventPublisher = std::function<void(const net::coop::Event&)>;
    void SetEventPublisher(EventPublisher publisher) { publisher_ = std::move(publisher); }

    // (A) The adapter's event handler hands the events of the world to the
    // body as well: the shot of the partner is the state 7 of the record.
    bool ApplyEvent(const net::coop::Event& event);

    // (B) The engine's thread, right after the engine's own tick of the
    // actor. The body writes its state here, because the tick of the engine
    // would overwrite it otherwise.
    void AfterActorTick(uint32_t actor);

    // The frame of the engine, on the engine's thread. The body follows
    // the partner and animates once a frame. The body drives its own
    // animation, so it does not need the engine to tick it.
    void Frame();

    // What the gate reads.
    uint32_t Actor() const { return actor_; }
    uint32_t CreatedCount() const { return created_; }
    uint32_t ShotEvents() const { return shots_; }
    uint32_t ClipChanges() const { return clip_changes_; }
    const std::string& Clip() const { return clip_; }

    // The end of the session: one line, so a test can read the round.
    void LogSummary();

private:
    Body() = default;

    // The guest: the body of the partner, made and driven.
    bool Create();
    void Update();
    // The position and the direction of the body, with the engine's own
    // calls. It goes in again after the tick of the engine, because the
    // tick may have moved the actor.
    void WriteTransform();
    // The host: the shot of the local player's weapon, when the magazine
    // goes down.
    void PublishLocalShot();
    // The event of the shot, the way the magazine publishes it.
    void PublishShot();
    // The test harness: the host's own player fires, through the engine's
    // trigger, so the shot crosses and the guest's body plays the clip.
    void TestFire();
    // The partner's state, as the network has it. Null when there is no
    // partner yet.
    const net::PlayerStateData* PartnerState() const;
    // A soldier of the level: the anim set the body walks with, and the
    // unit of the level it comes from. The attach of the animation
    // controller reads the unit with no null test, so the body waits until
    // the level has a soldier of its own type.
    bool FindSoldier(uint32_t* animset, uint32_t* unit) const;
    // The animation of the body: the state switch, the anim set's own
    // choice and the animation tick.
    void Animate();
    // The body is still the one this layer made.
    bool Alive() const;

    uint32_t epoch_ = 0;
    EventPublisher publisher_;

    // The body itself.
    uint32_t actor_ = 0;      // the actor of the body
    uint32_t manager_ = 0;    // the manager it came from
    uint32_t controller_ = 0;  // its AI controller
    uint32_t anim_ = 0;       // its animation controller
    uint32_t animset_ = 0;    // the anim set of its character type

    // The state of the partner the body is showing.
    float last_pos_[3] = {0.0f, 0.0f, 0.0f};
    uint32_t last_frame_ = 0;
    bool have_pos_ = false;
    float speed_ = 0.0f;
    // The time the shot state is still held. The event of the shot comes
    // from the network thread and the state is read by the engine's thread,
    // so it is atomic.
    std::atomic<float> shot_left_{0.0f};
    uint32_t shots_ = 0;      // the shots the body played
    uint32_t shot_serial_ = 0;

    // What the log says.
    std::string clip_;
    uint32_t clip_changes_ = 0;
    uint32_t created_ = 0;
    uint32_t heartbeat_ = 0;
    uint32_t failed_ = 0;
    uint32_t test_fire_frame_ = 0;  // the test harness's trigger
    uint32_t test_fire_epoch_ = 0;  // the round the harness already shot in

    // The host: the magazine of the local player's weapon.
    uint16_t last_ammo_ = 0;
    bool have_ammo_ = false;
};

}  // namespace coop
}  // namespace game
