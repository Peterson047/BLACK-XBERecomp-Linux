#pragma once
// Guest objects that several hook files read
#include <cstdint>

#include "xdk/hle.h"

namespace layout {

// float seconds, the frame time the game was made for
constexpr uint32_t kGameFrameTime = 0x2F3A38;

// game object
constexpr uint32_t kGame = 0x2D45F0;
constexpr uint32_t kGameState = 0x21070;
constexpr uint32_t kGameClockStep = 0x2013C;  // float: its clock's step
constexpr uint32_t kFrontEnd = 0x20220;
constexpr uint32_t kFilmPlayer = 0x4C;      // in the front end
constexpr uint32_t kCurrentFilm = 0x2042C;  // the front end's +0x20C

// film table
constexpr uint32_t kFilmTable = 0x29D9C0;
constexpr uint32_t kFilmEntry = 12;
constexpr uint32_t kFilmBehindMenu = 8;  // byte: the film runs behind a menu
constexpr uint32_t kFilms = 27;

// timer: whole steps and parts of a step over the divisor, the step's dt and the time
constexpr uint32_t kTimerWholeSteps = 0x0C;
constexpr uint32_t kTimerPartSteps = 0x10;
constexpr uint32_t kTimerDivisor = 0x18;
constexpr uint32_t kTimerDt = 0x1C;
constexpr uint32_t kTimerTime = 0x20;

// world container: its tick reads as a timer
constexpr uint32_t kWorld = 0x2D45D4;
constexpr uint32_t kWorldPlayer = 0x30;

// display: +0 width
constexpr uint32_t kDisplay = 0x2D67F4;
constexpr uint32_t kDisplayWidescreen = 0x0C;
constexpr uint32_t kDisplayWidth = 0xD428;
constexpr uint32_t kDisplayHeight = 0xD42C;
constexpr uint32_t kDisplayViewWidth = 0xD4C0;
constexpr uint32_t kDisplayAspect = 0xD4C4;
constexpr uint32_t kDisplayDrawLists = 0xCA6C;
constexpr uint32_t kGunDrawList = 5;  // the first person models
// draw list
constexpr uint32_t kDrawListSize = 0x18;
constexpr uint32_t kDrawListRoom = 0;
constexpr uint32_t kDrawListCap = 0xC;
constexpr uint32_t kStageX = 0x295DA4;  // float: the 640 Flash stage sits this far in on a widescreen

// UI object, the front end the menu hooks get in ebp
constexpr uint32_t kUi = 0x2D199C;
constexpr uint32_t kCallWaits = 0x374;  // a call to the game waits, the page script's FEFunctionTrigger
constexpr uint32_t kMenuInput = 0x44C;  // the menu input of this tick
constexpr uint32_t kWaitsForStart = 0x450;
constexpr uint32_t kInputOn = 0x454;
constexpr uint32_t kInputHeldOff = 0x455;
constexpr uint32_t kStageScaleX = 0x3710;  // Flash stage to screen
constexpr uint32_t kStageScaleY = 0x3714;
constexpr uint32_t kPageReady = 0x381C;
constexpr uint32_t kTargetPage = 0x3820;  // the page asked for
constexpr uint32_t kPageName = 0x3840;
constexpr uint32_t kPageAsked = 0x3884;    // 1 once a page is asked for
constexpr uint32_t kPageLeaving = 0x388C;  // 1 as it goes
constexpr uint32_t kUiReady = 0x390F;
constexpr uint32_t kPageRunning = 2;  // kPageReady once the page is ready

// font: its glyphs by code & 0x7F
constexpr uint32_t kFontScale = 0x08;
constexpr uint32_t kFontSpacing = 0x10;
constexpr uint32_t kFontLastGlyph = 0x1C;
constexpr uint32_t kFontGlyphs = 0x20;
// glyph
constexpr uint32_t kGlyphSize = 0x28;
constexpr uint32_t kGlyphWidth = 0x08;
constexpr uint32_t kGlyphLeft = 0x10;
constexpr uint32_t kGlyphAdvance = 0x18;
constexpr uint32_t kGlyphCode = 0x1C;
constexpr uint32_t kGlyphSpaced = 0x20;

enum MenuInput : uint32_t { kNoInput, kInputUp, kInputDown, kInputLeft, kInputRight, kInputConfirm, kInputBack };

// the page takes the menu input of this tick
inline bool InputTaken(uint32_t ui) {
    using kernel::G;
    return G<uint8_t>(ui + kUiReady) && G<uint8_t>(ui + kInputOn) && !G<uint8_t>(ui + kInputHeldOff);
}

// The co-op world: the manager of the actors and the actors themselves.
// Every offset here is proved in .agents/teamwork/coop/LAYOUT.md.
namespace coop {

// the manager of the actors
constexpr uint32_t kActorManager = 0x2D1980;
// its live list: the array of the actor pointers, how many of them are in
// use and how many slots the array has
constexpr uint32_t kActorList = 0x79BC;
constexpr uint32_t kActorCount = 0x79B8;
constexpr uint32_t kActorCapacity = 0x79B4;
// the actor
constexpr uint32_t kActorVtable = 0x28C1E8;
constexpr uint32_t kActorKeyA = 0x08;   // the engine's object key, first half
constexpr uint32_t kActorKeyB = 0x0C;   // the engine's object key, second half
constexpr uint32_t kActorType = 0xC4;
constexpr uint32_t kActorGeneration = 0x374;
constexpr uint32_t kActorHealth = 0x2F0;   // float
constexpr uint32_t kActorState = 0x384;    // 0 alive, 1 recycled, 2 dead
constexpr uint32_t kActorPosition = 0xA0;  // x, y, z: the matrix's last row
constexpr uint32_t kActorDirection = 0x20; // the world direction the engine keeps
constexpr uint32_t kActorLocalDirection = 0x60;  // the direction in the actor's own frame
constexpr uint32_t kActorBasis = 0x70;           // the matrix's three rows: 0x70, 0x80, 0x90

// the engine's own calls the world layer drives
constexpr uint32_t kSetDirection = 0x001CE600;  // the actor's direction
constexpr uint32_t kSetPosition = 0x001CE660;   // the actor's position
constexpr uint32_t kDamageMethod = 0x001DF830;  // the actor's damage
constexpr uint32_t kActorTick = 0x001F08D0;     // the actor's tick, once a frame

// the damage event the engine hands to the damage routine: the origin, the
// damage, the radius, the type and the flag
constexpr uint32_t kDamageEventOrigin = 0x00;
constexpr uint32_t kDamageEventDamage = 0x10;
constexpr uint32_t kDamageEventRadius = 0x14;
constexpr uint32_t kDamageEventType = 0x18;
constexpr uint32_t kDamageEventFlag = 0x1C;
constexpr uint32_t kDamageEventSize = 0x28;

// The mission. The objective table lives in the world container and the named
// events of the level script travel in a bus. The proof of each offset is in
// .agents/teamwork/coop/LAYOUT.md, section 7.
// the bus of the named events
constexpr uint32_t kEventBus = 0x2D45D8;
// the bus: the queue of the events, of eight places
constexpr uint32_t kEventQueueTail = 0x1964;   // the place the next event goes to
constexpr uint32_t kEventQueueHead = 0x1966;   // the place the next event is read from
constexpr uint32_t kEventQueueCount = 0x1968;  // the number of the events in the queue
constexpr uint32_t kEventQueueSlot = 0x100;    // the stride of a place
constexpr uint32_t kEventQueueCounts = 0x9C0;  // the count of a place
constexpr uint32_t kEventQueuePos = 0x940;     // the position of a place
constexpr uint32_t kEventQueueName = 0x10E4;   // the name of a place
constexpr uint32_t kEventQueueText = 0x1164;   // the text of a place
constexpr uint32_t kEventQueueNames = 0xCE8;   // the table of the names of the bus, eleven of them
// the objective table, in the world container
constexpr uint32_t kObjectiveTable = 0x930;    // the objectives
constexpr uint32_t kObjectiveCount = 0x934;    // how many
constexpr uint32_t kObjectiveCounters = 0x938; // the counters of the categories, six of them
constexpr uint32_t kObjectiveCounterCount = 6;
constexpr uint32_t kObjectiveRecord = 0x0C;    // the size of an objective
constexpr uint32_t kObjectiveState = 0x08;     // in an objective: the live state, 3 is done
constexpr uint32_t kObjectiveDone = 3;
// the screen of the end of the level: the names of the lines and their values
constexpr uint32_t kEndScreenLabels = 0x2AB820;
constexpr uint32_t kEndScreenValues = 0x2E79C8;
constexpr uint32_t kEndScreenSlot = 0x200;  // the stride of a line
// the requests of "use": the list of the objects the player used
constexpr uint32_t kUseRequests = 0x2D12D0;  // the manager of the requests
constexpr uint32_t kUseRequestSlots = 36;    // how many places the list has
constexpr uint32_t kUseRequestFirst = 0x20;  // the first place of the list
constexpr uint32_t kUseRequestSize = 0x30;   // the size of a place
// the engine's own calls the mission layer drives
constexpr uint32_t kFireEvent = 0x0012B8E0;    // the level script fires a named event
constexpr uint32_t kQueueEvent = 0x0012B790;   // the bus takes an event
constexpr uint32_t kUseRequest = 0x001BAA40;   // the "use" of an object is registered
constexpr uint32_t kFrameTick = 0x000BE550;    // the frame counter
constexpr uint32_t kFrameCount = 0x2F4368;     // the number of the frames that went

}  // namespace coop

}  // namespace layout
