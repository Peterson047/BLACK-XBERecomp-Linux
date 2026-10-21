#pragma once
// The animation probe: the state a character is in and the clip that state
// picks.
//
// The engine drives the bones of a character from a five field record its
// controller writes at `actor+0x32C`: the state in that record picks the
// clip (`0x0017F270`), and the clip is what moves the bones. The world layer
// only writes the position and the direction, so a body the network drives
// slides across the floor with a frozen pose until something writes that
// record.
//
// This probe reads the record and the clip the engine chose. The table of
// the states and the poses was measured with it:
// `.agents/teamwork/coop/anim/STATE_TABLE.md`.
//
// It is quiet unless `BLACK_COOP_ANIM_PROBE=1` is in the environment: with
// the variable unset the hooks return at once and nothing is written.

#include <cstdint>
#include <string>

namespace game {
namespace coop_anim {

// Whether the probe logs. Nothing at all happens when it does not.
bool Enabled();

// The name of the clip the actor's animation controller is playing.
std::string ClipOf(uint32_t actor);

// The state in the actor's animation state record.
uint32_t StateOf(uint32_t actor);

// How many times the actor's clip changed since the probe started. The
// co-op's log gate wants to see the number grow while the partner moves and
// stay while the partner stands still.
uint32_t ClipChanges(uint32_t actor);

}  // namespace coop_anim
}  // namespace game
