#pragma once
// The animation probe: the state a character is in and the clip that plays.
//
// The engine drives the bones of a character from a five field record its
// controller writes at `actor+0x32C`: the state in that record picks the
// clip (`0x0017F270`), and the clip is what moves the bones. The world layer
// only writes the position and the direction, so a body the network drives
// slides across the floor with a frozen pose until something writes that
// record.
//
// The probe reads the record and the clip of the actors that animate and
// writes down what changed. The table of the states and the poses was
// measured with it:
// `.agents/teamwork/coop/anim/STATE_TABLE.md`.
//
// It is quiet unless `BLACK_COOP_ANIM_PROBE=1` is in the environment: with
// the variable unset the hook returns at once and nothing is written.

#include <cstdint>
#include <string>

namespace game {
namespace coop_anim {

// Whether the probe logs. Nothing at all happens when it does not.
bool Enabled();

// The state and the clip of an actor, when they changed. The state is the
// one in the actor's animation state record (`actor+0x32C`), the clip the
// name in the actor's animation controller (`actor+0x328`, `+0x5C`).
//
// The world layer can call this from its actor tick, right after the
// engine's own tick, where the state and the clip are the ones the engine
// just chose. The probe calls it by itself from the animation tick, so a
// run needs nobody at the keyboard.
//
// The name in `anim+0x5C` is the clip that is *playing*: the walk and the
// crouch of a character are clips of the anim set's own choice, not of the
// state record. The clip the *state* chose is what `Chosen` reports.
void Report(uint32_t actor);

// The clip a state chose, and the state that chose it. The state switch
// (`0x0017F270`) reads a record and calls the anim set's method, and the
// method names the clip; this is that clip, with the state of the record
// the switch was reading.
//
// `record` is the address of that record (the actor's own at `actor+0x32C`
// in the AI's path, a record on the stack when the level script asks for
// an animation).
//
// `from` is the address the engine returns to after the call: inside the
// switch it is the arm the state took, so the line says which of the
// twenty four states it was.
void Chosen(uint32_t actor, uint32_t record, const std::string& clip, uint32_t from);

// The record the state switch is reading, and whether the switch is
// running. The switch (`0x0017F270`) reads a record and starts the clip
// the state chose; the clip that starts inside it is the one the state
// chose.
uint32_t Record();
bool Switching();

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
