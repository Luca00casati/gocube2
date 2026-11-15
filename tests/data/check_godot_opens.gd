extends SceneTree
# Opens the world gocube2 edited and saved, and checks its blocks.

func _initialize() -> void:
	_run.call_deferred()

func _run() -> void:
	root.get_node("ModLoader").load_all()
	var reg = root.get_node("Registry")
	var World = load("res://src/world/world.gd")
	var w = World.new()
	root.add_child(w)
	var problem: String = w.build("_cpp_edited", 128, -1, true)
	print("BUILD '", problem, "' seed ", w.meta.seed, " player ", w.meta.player)
	w.set_viewer(Vector3(0.5, 20, 0.5))
	var t0 := Time.get_ticks_msec()
	for c in [Vector3i(0, 0, 0), Vector3i(-1, 0, 0), Vector3i(-2, 0, 0)]:
		while not w.engine.is_chunk_loaded(c):
			await process_frame
			if Time.get_ticks_msec() - t0 > 60000:
				print("TIMEOUT")
				quit(1)
				return
	var expect := {Vector3i(3, 5, 3): "core:air", Vector3i(3, 4, 3): "core:stone", Vector3i(1, 2, 1): "core:glass",
			Vector3i(-40, 0, 3): "core:planks", Vector3i(-20, 0, 9): "core:glass", Vector3i(-20, 1, 9): "core:torch",
			Vector3i(6, 4, 3): "core:wood_door_n_upper", Vector3i(0, 2, 0): "core:air"}
	var bad := 0
	for p in expect:
		var got: String = reg.block_ids[w.get_block(p)]
		print("  ", p, " ", got, "" if got == expect[p] else "   <-- expected " + expect[p])
		bad += 0 if got == expect[p] else 1
	print("RESULT ", "OK" if bad == 0 else "%d WRONG" % bad)
	quit()
