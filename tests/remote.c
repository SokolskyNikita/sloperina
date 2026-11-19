/* The command file (remote.c): reading a script, its lines, menu items by name, waiting, pausing and stepping,
 * and what out.txt gets; no game. The SD card, the menu tree and the game's state are stand-ins. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../src/ootgz/binds.c"
#include "../src/ootgz/remote.c"

/* --- stand-ins ------------------------------------------------------------------------------- */

static uint32_t now; /* the game's frame counter */
uint32_t frame_counter(void) { return now; }
uint32_t input_held(void) { return 0; }
uint32_t input_pressed(void) { return 0; }
const char *T(const char *english) { return english; }
void gfx_logf(const char *fmt, ...) {}
void log_printf(const char *fmt, ...) {}
bool settings_sd(void) { return true; }
bool link_drawn_pos(vec3f_t *out)
{
  *out = (vec3f_t){ 1.5f, 2, -3 };
  return true;
}
bool gd_ready(void) { return true; }
const char *gd_get_str(uint32_t hash) { return "KokiriForest"; }
bool gd_game_paused(bool *is_pause, int32_t *pause_requests) { return false; }
const char *warp_state(void) { return ""; }
/* Link's actor and the player object, as status and trace read them: his velocity at +0x388, the item on ZR and
 * in hand at the player object's +0x7D0 and +0x7D8 (the jump height's address is that object + 0x18E8) */
static _Alignas(8) uint8_t actor_mem[0x400], player_mem[0x1D00];
static float facing_x = 1, facing_z = 0;
uintptr_t link_actor(void) { return (uintptr_t)actor_mem; }
/* the player object's lock state and on-ground flag are read as words; its action state isn't there (-1) */
uintptr_t sys_ptr(uintptr_t addr) { return 0; }
bool sys_read32(uintptr_t addr, uint32_t *out)
{
  memcpy(out, (const void *)addr, 4);
  return true;
}
bool sys_mapped(uintptr_t addr, size_t size, uint32_t perm) { return true; }
bool link_facing(double *fx, double *fz)
{
  *fx = facing_x, *fz = facing_z;
  return true;
}
bool js_height_address(uintptr_t *addr)
{
  *addr = (uintptr_t)player_mem + 0x18E8;
  return true;
}
/* load position's side: whether Link's record is known, and where a place put the saved position */
static link_record_status_t record = REC_READY;
static int record_requests, position_loads;
static double placed[4];
link_record_status_t link_record_status(void) { return record; }
void link_record_request(void) { record_requests++; }
void position_load(void) { position_loads++; }
void position_set(double x, double y, double z, double facing)
{
  placed[0] = x, placed[1] = y, placed[2] = z, placed[3] = facing;
}
static actor_near_t actors_there[3] = { { "Npc_Kokiri", 2.5f }, { "Obj_Sign", 4.25f }, { "Npc_Far", 30 } };
int actors_near(float radius, actor_near_t *out, int max)
{
  int n = 0;
  for (int i = 0; i < 3 && n < max; i++)
    if (actors_there[i].distance <= radius)
      out[n++] = actors_there[i];
  return n;
}

bool input_remote_active;
uint32_t input_remote_buttons;
bool input_remote_sticks[2];
int32_t input_remote_stick[4];

/* the SD card: cmd.txt (there or not) and out.txt */
static char cmd_file[SCRIPT_MAX * 2];
static size_t cmd_size;
static bool cmd_there, cmd_locked;
static char out_file[1 << 16];
static size_t out_size;
static int opened;
enum { H_CMD = 1, H_OUT = 2 };

nn_result_t fs_create_dir(const char *path) { return 0; }
nn_result_t fs_create_file(const char *path, int64_t size) { return 0; }
nn_result_t fs_delete_file(const char *path)
{
  if (strcmp(path, CMD_FILE) || cmd_locked)
    return 1;
  cmd_there = false;
  return 0;
}
nn_result_t fs_open(fs_file_t *out, const char *path, int mode)
{
  bool is_cmd = !strcmp(path, CMD_FILE);
  if (is_cmd && !cmd_there)
    return 1;
  out->handle = (void *)(intptr_t)(is_cmd ? H_CMD : H_OUT);
  opened++;
  return 0;
}
nn_result_t fs_size(int64_t *out, fs_file_t f)
{
  *out = (intptr_t)f.handle == H_CMD ? (int64_t)cmd_size : (int64_t)out_size;
  return 0;
}
nn_result_t fs_read(size_t *read, fs_file_t f, int64_t offset, void *buf, size_t size)
{
  memcpy(buf, cmd_file + offset, size);
  *read = size;
  return 0;
}
nn_result_t fs_write(fs_file_t f, int64_t offset, const void *buf, size_t size, const fs_write_option_t *opt)
{
  assert((intptr_t)f.handle == H_OUT && (size_t)offset == out_size && out_size + size < sizeof(out_file));
  memcpy(out_file + out_size, buf, size);
  out_size += size;
  out_file[out_size] = 0;
  return 0;
}
void fs_close(fs_file_t f) { opened--; }

/* the menu: root > cheats (a checkbox, an option, a number, two actions of one name) and a line of text */
menu_t menu_root, *menu_current;
menu_item_t *menu_editing;
static menu_t m_cheats;
static menu_item_t root_items[4], cheat_items[9];
static bool magic;
static int speed = 1, rupees = 10, reloads, closes;
static const char *const speeds[] = { "slow", "normal", "fast" };

void menu_close(void)
{
  menu_current = 0;
  menu_editing = 0;
  closes++;
}
static bool get_magic(menu_item_t *it) { return magic; }
static void set_magic(menu_item_t *it, bool on) { magic = on; }
static int get_speed(menu_item_t *it) { return speed; }
static void set_speed(menu_item_t *it, int choice) { speed = choice; }
static int32_t get_rupees(menu_item_t *it) { return rupees; }
static void set_rupees(menu_item_t *it, int32_t v) { rupees = v; }
static int32_t max_rupees(menu_item_t *it) { return 999; }
static void act_reload(menu_item_t *it) { reloads += (int)it->data; }
static void act_keyboard(menu_item_t *it) { menu_editing = it; } /* opens something to type in */
static void text_where(menu_item_t *it, char *buf, int len) { snprintf(buf, len, "\x01 Link  at  12.5"); }

static void build_menu(void)
{
  menu_root = (menu_t){ .title = "sloperinaGZ", .items = root_items, .n_items = 3 };
  root_items[0] = (menu_item_t){ .kind = MI_SUBMENU, .label = "cheats", .submenu = &m_cheats };
  root_items[1] = (menu_item_t){ .kind = MI_TEXT, .text = text_where };
  root_items[2] = (menu_item_t){ .kind = MI_BACK, .label = "close" };
  m_cheats = (menu_t){ .title = "cheats", .items = cheat_items, .n_items = 9, .parent = &menu_root };
  cheat_items[0] = (menu_item_t){ .kind = MI_CHECK, .label = "Infinite  Magic", .get_check = get_magic,
                                  .set_check = set_magic };
  cheat_items[1] = (menu_item_t){ .kind = MI_OPTION, .label = "speed", .n_choices = 3, .choices = speeds,
                                  .get_option = get_speed, .set_option = set_speed };
  cheat_items[2] = (menu_item_t){ .kind = MI_NUMBER, .label = "rupees", .get_num = get_rupees,
                                  .set_num = set_rupees, .num_max = max_rupees };
  cheat_items[3] = (menu_item_t){ .kind = MI_ACTION, .label = "reload map ({savepos})", .action = act_reload,
                                  .data = 1 };
  cheat_items[4] = (menu_item_t){ .kind = MI_ACTION, .label = "reload map", .action = act_reload, .data = 10 };
  cheat_items[5] = (menu_item_t){ .kind = MI_ACTION, .label = "name it", .action = act_keyboard };
  cheat_items[6] = (menu_item_t){ .kind = MI_ACTION, .label = "start/stop timer", .action = act_reload, .data = 100 };
  cheat_items[7] = (menu_item_t){ .kind = MI_ACTION, .label = "start", .action = act_reload, .data = 1000 };
  cheat_items[8] = (menu_item_t){ .kind = MI_BACK, .label = "return" };
}

/* --- the test's own ------------------------------------------------------------------------- */

static void send(const char *text)
{
  cmd_size = strlen(text);
  memcpy(cmd_file, text, cmd_size);
  cmd_there = true;
}

/* one frame of the game: the controller's read (nothing held), then the script */
static void frame(void)
{
  now++;
  binds_update(0, 0, 0);
  remote_frame(false);
}

/* frames until the script is taken and done (not past a wait it's still in) */
static void run_script(const char *text)
{
  out_size = 0;
  out_file[0] = 0;
  send(text);
  for (int i = 0; i < 4 * POLL_CALLS && (cmd_there || running); i++)
    frame();
}

static void out_has(const char *want)
{
  if (!strstr(out_file, want)) {
    printf("out.txt has no \"%s\":\n%s", want, out_file);
    assert(0);
  }
}

int main(void)
{
  binds_reset();
  build_menu();
  remote_enabled = false;
  send("echo no\n");
  for (int i = 0; i < 20; i++)
    frame();
  assert(cmd_there && !out_size); /* off: the file is left alone */
  remote_enabled = true;

  /* a script is taken (deleted), its lines run in order, each with a result, then "end" */
  run_script("# a comment\n\nid t1\necho hello  there \r\nnonsense 1\n");
  assert(!cmd_there && !running && !opened);
  out_has(" t1 4 ok hello  there\n");
  out_has(" t1 5 err not a command: nonsense\n");
  out_has(" t1 end\n");
  assert(!strncmp(out_file, "26 t1 4 ok", 10)); /* the frame it ran on: the sixth call since it was on */

  /* fire: the command reads as pressed that frame, as held for the frames asked, then not */
  out_size = 0;
  send("fire savepos\nwait 2\nfire rise 3\n");
  for (int i = 0; i < POLL_CALLS - 1; i++)
    frame();
  assert(!cmd_pressed(CMD_SAVEPOS));
  frame();
  assert(cmd_pressed(CMD_SAVEPOS) && running && !cmd_there);
  frame();
  assert(!cmd_pressed(CMD_SAVEPOS) && !cmd_active(CMD_SAVEPOS) && !cmd_pressed(CMD_RISE));
  frame(); /* the wait's second frame: the next line runs */
  assert(cmd_pressed(CMD_RISE) && cmd_held(CMD_RISE) && !running);
  for (int i = 0; i < 3; i++) {
    frame();
    assert(!cmd_pressed(CMD_RISE) && cmd_held(CMD_RISE));
  }
  frame();
  assert(!cmd_held(CMD_RISE));
  out_has(" 1 ok fire savepos\n");
  out_has(" 3 ok fire rise\n");
  run_script("fire nothing\nfire rise x\n");
  out_has("1 err fire: not a command");
  out_has("2 err fire: a number of frames");

  /* menu items by name: any case and spacing, a label's bind left out, #n and @n */
  run_script("set cheats/infinite magic = on\nget CHEATS / Infinite Magic\ndo cheats/infinite magic\n");
  out_has(" 1 ok check = on\n");
  out_has(" 2 ok check = on\n");
  out_has(" 3 ok check = off\n");
  assert(!magic);
  run_script("set cheats/speed = fast\nget cheats/speed\nset cheats/speed = 0\ndo cheats/speed\nset cheats/speed = warp\n");
  out_has(" 1 ok option = 2 (fast)\n");
  out_has(" 3 ok option = 0 (slow)\n");
  out_has(" 4 ok option = 1 (normal)\n");
  out_has(" 5 err set: not one of its choices");
  assert(speed == 1);
  run_script("set cheats/rupees = 250\nset cheats/rupees = 1000\nset cheats/rupees = lots\nget cheats/@2\n");
  out_has(" 1 ok number = 250\n");
  out_has(" 2 err set: out of its range\n");
  out_has(" 3 err set: a number\n");
  out_has(" 4 ok number = 250\n");
  assert(rupees == 250);
  run_script("do cheats/reload map\ndo cheats/reload map#2\ndo cheats/reload map#3\n");
  assert(reloads == 11);
  out_has(" 1 ok action\n");
  out_has(" 3 err do: no \"reload map#3\" in cheats\n");
  run_script("do cheats\ndo cheats/speed/slow\ndo nowhere/x\nget @1\ndo cheats/name it\nset cheats/rupees\n");
  out_has(" 1 err do: a menu");
  out_has(" 2 err do: \"speed\" isn't a menu\n");
  out_has(" 3 err do: no \"nowhere/x\" in sloperinaGZ\n");
  out_has(" 4 ok text = link at 12.5\n");
  out_has(" 5 err do: it opened something to type or pick in");
  out_has(" 6 err set: <menu path> = <value>\n");
  assert(!menu_editing && closes == 1);
  /* names with a "/" in them: the longest that fits, with or without spaces round the path's own "/" */
  reloads = 0;
  run_script("do cheats/start/stop timer\ndo cheats / start\ndo cheats/start/stop\ndo cheats/@3\ndo cheats/@30\n");
  assert(reloads == 1101);
  out_has(" 3 err do: \"start\" isn't a menu\n");
  out_has(" 5 err do: no \"@30\" in cheats\n");
  run_script("list\nlist cheats\nlist cheats/speed\n");
  out_has(" 1 | @0 menu cheats\n");
  out_has(" 1 | @1 text  = link at 12.5\n");
  out_has(" 1 ok 3 items in sloperinaGZ\n");
  out_has(" 2 | @1 option speed = 1 (normal)\n");
  out_has(" 2 | @3 action reload map\n");
  out_has(" 3 err list: not a menu\n");

  /* pause, step and run, as frame advance takes them (ootgz.c), with input and a fire for the frames let through */
  out_size = 0;
  send("pause\ninput A+ZL ls 100 -200 for 2\nfire loadpos\nwait 1\nstep 3\nstatus\nrun\necho after\n");
  for (int i = 0; i < POLL_CALLS; i++)
    frame();
  assert(remote_take_pause() && !remote_take_pause() && running);
  frame();
  assert(!strstr(out_file, "paused") && !input_remote_active); /* not held yet: the pause waits */
  remote_frame(true);                                           /* held: on to the lines after it */
  out_has(" 1 ok paused\n");
  out_has(" 2 ok input 0x101 for 2\n");
  out_has(" 4 err wait: the game is held");
  assert(input_remote_active && input_remote_buttons == (BTN_A | BTN_ZL) && input_remote_sticks[0] &&
         !input_remote_sticks[1] && input_remote_stick[0] == 100 && input_remote_stick[1] == -200);
  assert(!cmd_pressed(CMD_LOADPOS)); /* fired while held: on the first frame let through */
  assert(!strstr(out_file, "status") && !remote_take_run());
  assert(remote_take_step());
  frame();
  assert(cmd_pressed(CMD_LOADPOS) && input_remote_active);
  assert(remote_take_step());
  frame();
  assert(!cmd_pressed(CMD_LOADPOS) && !input_remote_active); /* the input's two frames are over */
  assert(remote_take_step() && !remote_take_step());
  frame();
  assert(!strstr(out_file, "stepped")); /* the third frame ran; held again before the step is done */
  remote_frame(true);
  out_has(" 5 ok stepped\n");
  out_has(" 6 ok frame=");
  out_has(" map=KokiriForest link=yes x=1.500 y=2.000 z=-3.000 held=1 menu=0 game_paused=0 vy=0.000 hs=0.000 ");
  assert(remote_take_run() && !remote_take_run() && running);
  remote_frame(true);
  assert(!strstr(out_file, "after")); /* run waits until the game runs */
  frame();
  out_has(" 7 ok running\n");
  out_has(" 8 ok after\n");
  assert(!running);
  run_script("step\n"); /* a step with the game running pauses first */
  assert(remote_take_pause() && remote_take_step() && !remote_take_step() && running);
  remote_frame(true);
  out_has(" 1 ok stepped\n");
  run_script("input off\ninput 0x3 for 0\ninput A+Q\ninput A ls 1\ninput - rs 0 40000\n");
  out_has(" 2 ok input 0x3 for 0\n");
  out_has(" 3 err input: buttons");
  out_has(" 4 err input: a stick");
  out_has(" 5 err input: a stick");
  for (int i = 0; i < 5; i++)
    frame();
  assert(input_remote_active && input_remote_buttons == 3); /* for 0: until the next input */
  run_script("input off\n");
  assert(!input_remote_active);

  /* status has how Link moves and what he holds; trace gives a line a frame, without holding the script up;
   * actors lists who is near */
  float velocity[3] = { 3, 6.125f, -4 };
  int32_t zr_item = 0, held = -1;
  memcpy(actor_mem + 0x388, velocity, sizeof(velocity));
  memcpy(player_mem + 0x7D0, &zr_item, 4);
  memcpy(player_mem + 0x7D8, &held, 4);
  uint32_t lock_state = 2, on_ground = 0x101;
  memcpy(player_mem + 0x1C74, &lock_state, 4);
  memcpy(player_mem + 0x13D0, &on_ground, 4);
  run_script("status\n");
  out_has(" vy=6.125 hs=5.000 facing=90.0 hand=-1 zr=0 lock=2 ground=1 act=-1 wpn=-1 warp=-\n");
  out_size = 0;
  send("trace 3\necho now\nwait 5\necho later\n");
  for (int i = 0; i < POLL_CALLS; i++)
    frame();
  out_has(" 1 ok trace 3\n");
  out_has(" 2 ok now\n");
  assert(!strstr(out_file, " t y=")); /* from the next frame on */
  held = 3;
  memcpy(player_mem + 0x7D8, &held, 4);
  for (int i = 0; i < 6; i++)
    frame();
  out_has(" 4 ok later\n");
  int traced = 0;
  for (const char *p = out_file; (p = strstr(p, " t y=2.000 vy=6.125 hs=5.000 x=1.500 z=-3.000 hand=3 zr=0 paused=0 lock=2 ground=1 act=-1 wpn=-1\n")); p++)
    traced++;
  assert(traced == 3);
  run_script("trace 100\ntrace off\nwait 2\ntrace x\n");
  assert(!strstr(out_file, " t y="));
  out_has(" 4 err trace: a number of frames, or off\n");
  run_script("actors\nactors 3\nactors far\n");
  out_has(" 1 | 2.50 Npc_Kokiri\n");
  out_has(" 1 | 4.25 Obj_Sign\n");
  out_has(" 1 ok 2 actors within 6 m\n");
  out_has(" 2 ok 1 actors within 3 m\n");
  out_has(" 3 err actors: a distance in metres\n");

  /* place: the saved position set and loaded; facing as now unless given; refused until Link's record is known */
  run_script("place -12.45 110.36 4.66 250\nplace 1 2.5 -3\nplace 1 2\nplace 1 2 x\n");
  out_has(" 1 ok place -12.450 110.360 4.660 facing 250.0\n");
  out_has(" 2 ok place 1.000 2.500 -3.000 facing 90.0\n");
  out_has(" 3 err place: x y z");
  out_has(" 4 err place: x y z");
  assert(position_loads == 2 && placed[0] == 1 && placed[1] == 2.5 && placed[2] == -3 && placed[3] > 89.9);
  record = REC_FAILED;
  run_script("place 1 2 3\n");
  out_has(" 1 err place: finding Link's position");
  assert(position_loads == 2 && record_requests == 1);
  record = REC_READY;

  /* a file too long isn't run; one that can't be deleted isn't run, and is said once */
  out_size = 0;
  memset(cmd_file, 'x', SCRIPT_MAX);
  cmd_size = SCRIPT_MAX;
  cmd_there = true;
  for (int i = 0; i < 2 * POLL_CALLS; i++)
    frame();
  assert(!cmd_there && !running);
  out_has(" - 0 err cmd.txt is over 16383 bytes: not run\n");
  out_size = 0;
  cmd_locked = true;
  send("echo twice\n");
  for (int i = 0; i < 5 * POLL_CALLS; i++)
    frame();
  out_has("err cmd.txt can't be deleted: not run\n");
  assert(!strstr(strstr(out_file, "err") + 1, "err")); /* once */
  assert(!strstr(out_file, "twice") && !opened);
  cmd_locked = false;
  cmd_there = false; /* the sender takes it away: the next is run again */
  for (int i = 0; i < 2 * POLL_CALLS; i++)
    frame();

  /* a long script: at most LINES_PER_CALL lines a frame, all of them in the end */
  static char many[SCRIPT_MAX];
  size_t len = 0;
  for (int i = 0; i < 200; i++)
    len += (size_t)snprintf(many + len, sizeof(many) - len, "echo line %d\n", i);
  cmd_there = false;
  run_script(many);
  out_has(" 200 ok line 199\n");
  out_has(" - end\n");

  printf("remote: ok\n");
  return 0;
}
