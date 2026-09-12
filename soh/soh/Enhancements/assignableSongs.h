#ifndef ASSIGNABLE_SONGS_H
#define ASSIGNABLE_SONGS_H

#include "soh/cvar_prefixes.h"

#define CVAR_ASSIGNABLE_SONGS CVAR_ENHANCEMENT("AssignableSongs")

// A song assigned to a C-Button or D-pad direction sits in equips.buttonItems like any other item.
#define IS_ASSIGNED_SONG(item) (((item) >= ITEM_SONG_MINUET) && ((item) <= ITEM_SONG_STORMS))

#endif // ASSIGNABLE_SONGS_H
