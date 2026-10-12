@ PC port: talking to the Pokémon that follows the player (src/follower.c)
Follower_EventScript_Talk::
	lock
	faceplayer
	callnative Follower_PrepareTalk
	playmoncry VAR_0x8004, CRY_MODE_NORMAL
	msgbox gStringVar4, MSGBOX_DEFAULT
	waitmoncry
	release
	end
