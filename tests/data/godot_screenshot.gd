extends SceneTree
# Takes a screenshot with the Godot version, to compare with this one's
# --screenshot (milestone 6: rendering parity). Run from the Godot project:
#
#   Godot_v4.7.2-stable_win64_console.exe --path ..\gocube --resolution 1280x720 \
#       -s <this file> -- --world=_shot --seed=777 --time=0.4 --view=128 \
#       --pos=X,Y,Z --look=YAW,PITCH --out=<file>.png
#
# --pos (feet) and --look (degrees; 0, 0: towards -z) mean what they mean for
# gocube.exe; --world, --seed, --time and --view go to the Godot version's main.
# --clouds=S: the clouds as S seconds after the start (default 3; they drift).
# The world (saved in the Godot user data folder) is deleted afterwards.

var _args := {}


func _initialize() -> void:
	for a in OS.get_cmdline_user_args():
		var kv := a.trim_prefix("--").split("=", true, 1)
		_args[kv[0]] = kv[1] if kv.size() > 1 else "1"
	_run.call_deferred()


func _floats(text: String) -> PackedFloat32Array:
	var out := PackedFloat32Array()
	for s in text.split(","):
		out.append(float(s))
	return out


func _run() -> void:
	var main = load("res://src/main.tscn").instantiate()
	main.use_cmdline = true
	root.add_child(main)
	var t0 := Time.get_ticks_msec()
	while main._player == null or main._loading != null:
		await process_frame
		if Time.get_ticks_msec() - t0 > 60000:
			print("TIMEOUT loading")
			quit(1)
			return
	var player = main._player
	if _args.has("pos"):
		var p := _floats(_args.pos)
		player.flying = true
		player.global_position = Vector3(p[0], p[1], p[2])
		player.velocity = Vector3.ZERO
	if _args.has("look"):
		var l := _floats(_args.look)
		player.set_look(deg_to_rad(l[1]), deg_to_rad(l[0]))
	# Until the area around is meshed, then a few more frames.
	main._world.set_viewer(player.global_position)
	var r := float(main._world.view_distance) * 0.7 - 16.0
	var size := Vector3(2 * r, 64, 2 * r)
	t0 = Time.get_ticks_msec()
	while not main._world.is_area_meshed(AABB(main._cam.global_position - size / 2, size)):
		await process_frame
		if Time.get_ticks_msec() - t0 > 15000:
			print("TIMEOUT meshing")
			break
	for i in 30:
		player.velocity = Vector3.ZERO
		# Clouds drift from the start: put them (and their shadows) where
		# gocube.exe's are a few seconds in.
		main._day._cloud_offset = Vector2(0.35, 0.12) * float(_args.get("clouds", "3"))
		await process_frame
	await RenderingServer.frame_post_draw
	var img := root.get_texture().get_image()
	var out: String = _args.get("out", "godot_shot.png")
	img.save_png(out)
	print("saved ", out, " ", img.get_size())
	var world_dir := OS.get_user_data_dir().path_join("worlds").path_join(_args.get("world", "_shot"))
	main._leaving = true
	main.queue_free()
	await process_frame
	if _args.get("world", "") == "_shot" and DirAccess.dir_exists_absolute(world_dir):
		OS.move_to_trash(world_dir)
	quit(0)
