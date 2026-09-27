extends Node2D

## First Epoch-owned Godot slice: fixed-grid movement, solid terrain, one
## persistent interaction, and original-resolution nearest-neighbor drawing.
## All art in this scene is temporary programmer art; no upstream project
## source or game assets are copied into this prototype.

const TILE_SIZE := 8
const ROOM_WIDTH := 20
const ROOM_HEIGHT := 18
const STEP_SECONDS := 0.12
const CHEST_CELL := Vector2i(15, 8)
const SAVE_PATH := "user://ages_lab_save.json"
const ROOM := [
	"####################",
	"#..................#",
	"#..................#",
	"#..####............#",
	"#..#..#............#",
	"#..#..#....~~~.....#",
	"#..####....~~~.....#",
	"#..................#",
	"#......####....C...#",
	"#......#..#........#",
	"#......####........#",
	"#..................#",
	"#..........~~~~....#",
	"#..........~~~~....#",
	"#..................#",
	"#..................#",
	"#.........D........#",
	"####################",
]

var player_cell := Vector2i(2, 3)
var chest_open := false
var _step_clock := 0.0
var _message := "Arrows / WASD move   Z / A interact"


func _ready() -> void:
	_ensure_input_actions()
	_load_state()
	if "--smoke-test" in OS.get_cmdline_user_args():
		_run_smoke_test()
		return
	queue_redraw()


func _physics_process(delta: float) -> void:
	if Input.is_action_just_pressed("interact"):
		_interact()
		queue_redraw()
	_step_clock += delta
	if _step_clock < STEP_SECONDS:
		return
	_step_clock -= STEP_SECONDS
	var direction := _read_direction()
	if direction != Vector2i.ZERO:
		_try_step(direction)
		queue_redraw()


func _draw() -> void:
	for y in range(ROOM_HEIGHT):
		for x in range(ROOM_WIDTH):
			var cell := Vector2i(x, y)
			var tile := _tile_at(cell)
			var rect := Rect2i(x * TILE_SIZE, y * TILE_SIZE, TILE_SIZE, TILE_SIZE)
			match tile:
				"#":
					draw_rect(rect, Color("304d42"))
					draw_rect(Rect2i(rect.position + Vector2i(1, 1), Vector2i(6, 1)), Color("557161"))
				"~":
					draw_rect(rect, Color("326a88"))
					draw_rect(Rect2i(rect.position + Vector2i(1, 2), Vector2i(3, 1)), Color("6c9ab0"))
				_:
					draw_rect(rect, Color("668d4f"))
					if (x * 7 + y * 3) % 5 == 0:
						draw_rect(Rect2i(rect.position + Vector2i(2, 5), Vector2i(1, 1)), Color("83a95d"))
			if tile == "D":
				draw_rect(Rect2i(rect.position, Vector2i(TILE_SIZE, TILE_SIZE)), Color("594431"))
				draw_rect(Rect2i(rect.position + Vector2i(2, 1), Vector2i(4, 7)), Color("ad8953"))

	_draw_chest()
	_draw_player()
	draw_rect(Rect2i(0, 0, 160, 10), Color(0.04, 0.07, 0.09, 0.86))
	draw_string(ThemeDB.fallback_font, Vector2(3, 8), "AGES LAB / ROOM PROTOTYPE", HORIZONTAL_ALIGNMENT_LEFT, -1, 6, Color("e3ddae"))
	draw_string(ThemeDB.fallback_font, Vector2(3, 141), _message, HORIZONTAL_ALIGNMENT_LEFT, 154, 6, Color("fff2b2"))


func _draw_chest() -> void:
	var origin := Vector2i(CHEST_CELL.x * TILE_SIZE, CHEST_CELL.y * TILE_SIZE)
	draw_rect(Rect2i(origin + Vector2i(1, 2), Vector2i(6, 5)), Color("644125"))
	draw_rect(Rect2i(origin + Vector2i(1, 1), Vector2i(6, 2)), Color("bd843d"))
	draw_rect(Rect2i(origin + Vector2i(3, 3), Vector2i(2, 2)), Color("f1ce65"))
	if chest_open:
		draw_rect(Rect2i(origin + Vector2i(1, 0), Vector2i(6, 2)), Color("d6a64b"))
		draw_rect(Rect2i(origin + Vector2i(2, 1), Vector2i(4, 1)), Color("5f472a"))


func _draw_player() -> void:
	var origin := Vector2i(player_cell.x * TILE_SIZE, player_cell.y * TILE_SIZE)
	draw_rect(Rect2i(origin + Vector2i(1, 6), Vector2i(6, 2)), Color(0.12, 0.2, 0.12, 0.55))
	draw_rect(Rect2i(origin + Vector2i(2, 1), Vector2i(4, 3)), Color("e4bd83"))
	draw_rect(Rect2i(origin + Vector2i(1, 0), Vector2i(6, 2)), Color("397646"))
	draw_rect(Rect2i(origin + Vector2i(2, 4), Vector2i(4, 3)), Color("3d8e52"))
	draw_rect(Rect2i(origin + Vector2i(2, 7), Vector2i(2, 1)), Color("75452d"))
	draw_rect(Rect2i(origin + Vector2i(5, 7), Vector2i(2, 1)), Color("75452d"))


func _read_direction() -> Vector2i:
	var x := int(Input.is_action_pressed("move_right")) - int(Input.is_action_pressed("move_left"))
	var y := int(Input.is_action_pressed("move_down")) - int(Input.is_action_pressed("move_up"))
	if x != 0:
		return Vector2i(x, 0)
	if y != 0:
		return Vector2i(0, y)
	return Vector2i.ZERO


func _try_step(direction: Vector2i) -> bool:
	var destination := player_cell + direction
	if not _is_walkable(destination):
		return false
	player_cell = destination
	return true


func _is_walkable(cell: Vector2i) -> bool:
	var tile := _tile_at(cell)
	return tile != "#" and tile != "~" and tile != "C"


func _tile_at(cell: Vector2i) -> String:
	if cell.x < 0 or cell.y < 0 or cell.x >= ROOM_WIDTH or cell.y >= ROOM_HEIGHT:
		return "#"
	return ROOM[cell.y].substr(cell.x, 1)


func _interact() -> void:
	if absi(player_cell.x - CHEST_CELL.x) + absi(player_cell.y - CHEST_CELL.y) > 1:
		_message = "Nothing close enough to interact with."
		return
	if chest_open:
		_message = "The chest is open."
		return
	chest_open = true
	_message = "Chest opened! (prototype save written)"
	_save_state()


func _save_state() -> void:
	var file := FileAccess.open(SAVE_PATH, FileAccess.WRITE)
	if file == null:
		push_error("Could not write Ages Lab save: %s" % FileAccess.get_open_error())
		return
	file.store_string(JSON.stringify({"chest_open": chest_open}))


func _load_state() -> void:
	if not FileAccess.file_exists(SAVE_PATH):
		return
	var file := FileAccess.open(SAVE_PATH, FileAccess.READ)
	if file == null:
		return
	var data: Variant = JSON.parse_string(file.get_as_text())
	if data is Dictionary:
		chest_open = bool(data.get("chest_open", false))


func _ensure_input_actions() -> void:
	_add_key_action("move_up", [KEY_UP, KEY_W])
	_add_key_action("move_down", [KEY_DOWN, KEY_S])
	_add_key_action("move_left", [KEY_LEFT, KEY_A])
	_add_key_action("move_right", [KEY_RIGHT, KEY_D])
	_add_key_action("interact", [KEY_Z, KEY_SPACE, KEY_ENTER])
	var pad_directions := {
		"move_up": JOY_BUTTON_DPAD_UP,
		"move_down": JOY_BUTTON_DPAD_DOWN,
		"move_left": JOY_BUTTON_DPAD_LEFT,
		"move_right": JOY_BUTTON_DPAD_RIGHT,
	}
	for action in pad_directions:
		var event := InputEventJoypadButton.new()
		event.button_index = pad_directions[action]
		InputMap.action_add_event(action, event)
	var pad_interact := InputEventJoypadButton.new()
	pad_interact.button_index = JOY_BUTTON_A
	InputMap.action_add_event("interact", pad_interact)


func _add_key_action(action: StringName, keycodes: Array[int]) -> void:
	if not InputMap.has_action(action):
		InputMap.add_action(action)
	for keycode in keycodes:
		var event := InputEventKey.new()
		event.physical_keycode = keycode
		InputMap.action_add_event(action, event)


func _run_smoke_test() -> void:
	var errors: Array[String] = []
	if ROOM.size() != ROOM_HEIGHT:
		errors.append("room height mismatch")
	for row in ROOM:
		if row.length() != ROOM_WIDTH:
			errors.append("room row width mismatch: %s" % row)
	if not _is_walkable(Vector2i(2, 3)):
		errors.append("starting floor tile should be walkable")
	if _is_walkable(Vector2i(0, 0)):
		errors.append("wall should block movement")
	if _is_walkable(Vector2i(12, 5)):
		errors.append("water should block movement")
	player_cell = Vector2i(1, 1)
	if not _try_step(Vector2i(1, 0)) or player_cell != Vector2i(2, 1):
		errors.append("movement should enter adjacent floor")
	player_cell = Vector2i(1, 1)
	if _try_step(Vector2i(-1, 0)) or player_cell != Vector2i(1, 1):
		errors.append("wall collision should preserve player position")
	player_cell = Vector2i(3, 3)
	chest_open = false
	_interact()
	if chest_open:
		errors.append("distant chest interaction should not open chest")
	player_cell = CHEST_CELL + Vector2i(0, 1)
	chest_open = false
	_interact()
	if not chest_open:
		errors.append("adjacent chest interaction should open chest")
	_save_state()
	chest_open = false
	_load_state()
	if not chest_open:
		errors.append("chest state should survive save/load")
	var save_file := ProjectSettings.globalize_path(SAVE_PATH)
	if FileAccess.file_exists(SAVE_PATH):
		DirAccess.remove_absolute(save_file)
	if errors.is_empty():
		print("[ages-lab] SMOKE PASS: grid, collision, interaction, save/load")
		get_tree().quit(0)
	else:
		for error in errors:
			push_error(error)
		get_tree().quit(1)
