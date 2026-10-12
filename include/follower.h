#ifndef GUARD_FOLLOWER_H
#define GUARD_FOLLOWER_H

// PC port: the first party Pokémon walks behind the player, as in
// HeartGold/SoulSilver. It is an ordinary object event (graphics id
// OBJ_EVENT_GFX_FOLLOWER, local id LOCALID_FOLLOWER) driven by
// Follower_Update; it is never written to the save, so saves stay the same as
// the original game's. The HD-2D option "follower" turns it off.

#ifdef PORTABLE

// Every overworld frame, after the objects have moved (OverworldBasic)
void Follower_Update(void);

bool8 Follower_IsFollower(const struct ObjectEvent *objectEvent);

// Removes it (another object needs the slot); TRUE if there was one
bool8 Follower_Remove(void);

const struct ObjectEventGraphicsInfo *Follower_GetGraphicsInfo(void);
const u8 *Follower_GetScript(void);

// Script helper (callnative): gSpecialVar_0x8004 = species for the cry,
// gStringVar4 = what the player sees when talking to it
void Follower_PrepareTalk(void);

#endif // PORTABLE

#endif // GUARD_FOLLOWER_H
