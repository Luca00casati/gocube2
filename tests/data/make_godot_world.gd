extends SceneTree
# Makes a world with the Godot version, edits it, saves it, and writes what it
# changed to expected.json next to it: the fixture for gocube2's save tests.

func _initialize() -> void:
	_run.call_deferred()

func _run() -> void:
	root.get_node("ModLoader").load_all()
	var reg = root.get_node("Registry")
	var World = load("res://src/world/world.gd")
	var w = World.new()
	root.add_child(w)
	w.build("_fixture_m3", 128, 4242, true, "creative")
	w.set_viewer(Vector3(0.5, 20, 0.5))
	var t0 := Time.get_ticks_msec()
	while not w.is_area_meshed(AABB(Vector3(-24, -20, -24), Vector3(48, 80, 48))):
		await process_frame
		if Time.get_ticks_msec() - t0 > 60000:
			print("TIMEOUT")
			quit(1)
			return
	var h: int = w.engine.generated_height(3, 3)
	var edits := []
	var put := func(p: Vector3i, id: String):
		w.set_block(p, reg.block_index(id))
		edits.append({"pos": [p.x, p.y, p.z], "id": id})
	for dy in range(1, 4):
		put.call(Vector3i(3, h + dy, 3), "core:stone")          # a pillar
	var h0: int = w.engine.generated_height(0, 0)
	put.call(Vector3i(0, h0, 0), "core:air")                    # dug out
	var hd: int = w.engine.generated_height(6, 3)
	put.call(Vector3i(6, hd + 1, 3), "core:wood_door")          # a door, both halves
	put.call(Vector3i(6, hd + 2, 3), "core:wood_door_n_upper")
	var hg: int = w.engine.generated_height(-20, 9)
	put.call(Vector3i(-20, hg + 1, 9), "core:glass")             # another chunk (x < 0)
	put.call(Vector3i(-20, hg + 2, 9), "core:torch")
	put.call(Vector3i(-21, hg + 1, 9), "core:stone_slab_top")
	w.save({"position": [0.5, float(h0) + 1.0, 0.5], "rotation": [-0.2, 0.75], "flying": false, "selected": 2,
			"inventory": [{"id": "core:torch", "count": 5}]})
	var out := FileAccess.open(w.dir.path_join("expected.json"), FileAccess.WRITE)
	out.store_string(JSON.stringify({"edits": edits}, "\t"))
	out.close()
	print("FIXTURE ", ProjectSettings.globalize_path(w.dir))
	quit()
