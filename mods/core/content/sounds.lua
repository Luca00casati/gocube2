-- The base game's content (it was the Godot version's content/base.json).
-- Sound sets: the sounds blocks make (by their "sound").

registry.register_sound_set("grass", {
	step = {"sounds/grass_step_1.wav", "sounds/grass_step_2.wav", "sounds/grass_step_3.wav"},
	["break"] = {"sounds/grass_break_1.wav", "sounds/grass_break_2.wav", "sounds/grass_break_3.wav"},
	place = {"sounds/grass_place.wav"},
})

registry.register_sound_set("stone", {
	step = {"sounds/stone_step_1.wav", "sounds/stone_step_2.wav", "sounds/stone_step_3.wav"},
	["break"] = {"sounds/stone_break_1.wav", "sounds/stone_break_2.wav", "sounds/stone_break_3.wav"},
	place = {"sounds/stone_place.wav"},
})

registry.register_sound_set("wood", {
	step = {"sounds/wood_step_1.wav", "sounds/wood_step_2.wav", "sounds/wood_step_3.wav"},
	["break"] = {"sounds/wood_break_1.wav", "sounds/wood_break_2.wav", "sounds/wood_break_3.wav"},
	place = {"sounds/wood_place.wav"},
})

registry.register_sound_set("sand", {
	step = {"sounds/sand_step_1.wav", "sounds/sand_step_2.wav", "sounds/sand_step_3.wav"},
	["break"] = {"sounds/sand_break_1.wav", "sounds/sand_break_2.wav", "sounds/sand_break_3.wav"},
	place = {"sounds/sand_place.wav"},
})

registry.register_sound_set("gravel", {
	step = {"sounds/gravel_step_1.wav", "sounds/gravel_step_2.wav", "sounds/gravel_step_3.wav"},
	["break"] = {"sounds/gravel_break_1.wav", "sounds/gravel_break_2.wav", "sounds/gravel_break_3.wav"},
	place = {"sounds/gravel_place.wav"},
})

registry.register_sound_set("effects", {
	splash = {"sounds/splash_1.wav", "sounds/splash_2.wav"},
	hurt = {"sounds/hurt.wav"},
	pickup = {"sounds/pickup.wav"},
})

registry.register_sound_set("ambience", {day = {"sounds/ambience_day.wav"}, night = {"sounds/ambience_night.wav"}})
