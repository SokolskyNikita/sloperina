/* Commands from a file (remote.h): the script's lines run on the game's thread from the frame hook, through the
 * same code the controller reaches: binds_fire for the commands, the menu items' own callbacks for the rest. */
#include "remote.h"

#include <stdarg.h>
#include <stdint.h>

#include "binds.h"
#include "gamedata.h"
#include "input.h"
#include "link.h"
#include "menu.h"
#include "menus.h"
#include "sdfs.h"
#include "settings.h"
#include "sys.h"
#include "warp.h"

#define REMOTE_DIR SD_DIR "/remote"
#define CMD_FILE REMOTE_DIR "/cmd.txt"
#define OUT_FILE REMOTE_DIR "/out.txt"

#define SCRIPT_MAX 16384  /* a script's text */
#define OUT_MAX 8192      /* results waiting to be written */
#define POLL_CALLS 6      /* look for cmd.txt every this many calls (10 times a second) */
#define LINES_PER_CALL 64 /* at most this many lines a frame: a long script doesn't hold the game up */
#define STICK_MAX 32767

bool remote_enabled;

static char script[SCRIPT_MAX];
static size_t script_len, cursor;
static int line_no;
static bool running;
static char script_id[24] = "-";

/* what the line being run waits for */
static int wait_left;               /* wait: frames still to go */
static bool pause_wanted;           /* pause: for frame advance to take */
static bool pause_waiting;          /* ... and until it holds the game */
static int steps_left;              /* step: frames still to let through */
static bool stepping;               /* ... and until the game is held again after them */
static bool run_wanted, run_waiting; /* run: the same two */

static uint16_t fire_pending[CMD_COUNT]; /* fire while the game is held: given on the next frame (1 + frames held) */
static int input_frames;                 /* frames the script's input still lasts (0: until changed) */

/* --- text ------------------------------------------------------------------------------------ */

static char lowercase(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c; }
static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

static char *skip_spaces(char *p)
{
  while (is_space(*p))
    p++;
  return p;
}

static void trim_end(char *s)
{
  size_t n = strlen(s);
  while (n && is_space(s[n - 1]))
    s[--n] = 0;
}

/* The next word of *p (ended in place), or "" at the end; *p moves past it. */
static char *take_word(char **p)
{
  char *start = skip_spaces(*p);
  char *end = start;
  while (*end && !is_space(*end))
    end++;
  if (*end)
    *end++ = 0;
  *p = end;
  return start;
}

static bool same(const char *a, const char *b)
{
  while (*a && lowercase(*a) == lowercase(*b)) {
    a++;
    b++;
  }
  return !*a && !*b;
}

/* "12", "-3", "0x1f" */
static bool parse_int(const char *s, int32_t *out)
{
  bool negative = *s == '-';
  if (negative || *s == '+')
    s++;
  int64_t value = 0;
  int digits = 0;
  if (s[0] == '0' && lowercase(s[1]) == 'x') {
    for (s += 2; *s; s++, digits++) {
      char c = lowercase(*s);
      int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
      if (digit < 0 || value > UINT32_MAX)
        return false;
      value = value * 16 + digit;
    }
  } else {
    for (; *s; s++, digits++) {
      if (*s < '0' || *s > '9' || value > UINT32_MAX)
        return false;
      value = value * 10 + (*s - '0');
    }
  }
  if (!digits || value > UINT32_MAX)
    return false;
  *out = (int32_t)(negative ? -value : value);
  return true;
}

/* A name as a script gives it: lower case, single spaces, without the "(... {bind} ...)" some labels end in
 * ("save position ({savepos})" is "save position"), and only what prints. */
static void plain_name(const char *in, char *out, size_t cap)
{
  size_t o = 0;
  bool space = false;
  for (size_t i = 0; in && in[i]; i++) {
    char c = in[i];
    if (c == '(') {
      size_t end = i;
      bool has_bind = false;
      while (in[end] && in[end] != ')') {
        if (in[end] == '{')
          has_bind = true;
        end++;
      }
      if (has_bind && in[end]) {
        i = end;
        continue;
      }
    }
    if (is_space(c) || c == '\n') {
      space = o > 0;
      continue;
    }
    if ((unsigned char)c < 0x20 || (unsigned char)c >= 0x7F) /* the menu's own glyphs */
      continue;
    if (space && o + 1 < cap)
      out[o++] = ' ';
    space = false;
    if (o + 1 < cap)
      out[o++] = lowercase(c);
  }
  out[o] = 0;
}

/* --- results --------------------------------------------------------------------------------- */

static char out[OUT_MAX];
static size_t out_len;

static void out_flush(void)
{
  if (!out_len)
    return;
  fs_file_t file;
  if (fs_open(&file, OUT_FILE, FS_WRITE | FS_ALLOW_APPEND) != 0) {
    fs_create_dir(REMOTE_DIR); /* fails harmlessly if it exists */
    fs_create_file(OUT_FILE, 0);
    if (fs_open(&file, OUT_FILE, FS_WRITE | FS_ALLOW_APPEND) != 0) {
      out_len = 0;
      return;
    }
  }
  int64_t size = 0;
  fs_size(&size, file);
  fs_write_option_t flush = { FS_WRITE_FLUSH };
  fs_write(file, size, out, out_len, &flush);
  fs_close(file);
  out_len = 0;
}

/* one line of out.txt: "<frame> <id> <text>" */
static void out_line(const char *text)
{
  char line[320];
  int n = snprintf(line, sizeof(line), "%u %s %s\n", (unsigned)frame_counter(), script_id, text);
  if (n < 0)
    return;
  if ((size_t)n >= sizeof(line)) {
    n = (int)sizeof(line) - 1;
    line[n - 1] = '\n';
  }
  if (out_len + (size_t)n > sizeof(out))
    out_flush();
  memcpy(out + out_len, line, (size_t)n);
  out_len += (size_t)n;
}

/* the result of the line being run: "<line number> ok|err <text>", to out.txt and the log */
static void result(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void result(bool ok, const char *fmt, ...)
{
  char text[256];
  int n = snprintf(text, sizeof(text), "%d %s ", line_no, ok ? "ok" : "err");
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(text + n, sizeof(text) - (size_t)n, fmt, ap);
  va_end(ap);
  trim_end(text);
  out_line(text);
  log_printf("remote: %s %s", script_id, text);
}

/* --- menu items by name ---------------------------------------------------------------------- */

static const char *const kind_names[] = { "menu", "action", "check", "option", "number", "text", "back" };

static bool has_name(const menu_item_t *item) { return item->label && item->label[0] && item->kind != MI_BACK; }

/* After a part's name in a path: an optional "#n" (which of that name), then the "/" before the next part, or the
 * end. Gives n and how much was read; false if something else follows (the name was only the start of a longer one). */
static bool part_end(const char *after, int *nth, size_t *used)
{
  const char *p = after;
  *nth = 1;
  if (*p == '#') {
    int n = 0;
    for (p++; *p >= '0' && *p <= '9' && n < 10000; p++)
      n = n * 10 + (*p - '0');
    if (n < 1)
      return false;
    *nth = n;
  }
  while (*p == ' ')
    p++;
  if (*p == '/')
    p++;
  else if (*p)
    return false;
  while (*p == ' ')
    p++;
  *used = (size_t)(p - after);
  return true;
}

/* The item of `menu` that the start of `rest` names (a path from here on, as plain_name leaves it): "@7" is item
 * 7; otherwise the longest name that fits ("start/stop timer" before "start", as names may have a "/" in them),
 * "name#2" the second item of that name. Gives how much of `rest` it took, with the "/" after it. */
static menu_item_t *match_item(menu_t *menu, const char *rest, size_t *used)
{
  int nth = 1;
  size_t tail = 0;
  if (rest[0] == '@') {
    int index = 0;
    const char *p = rest + 1;
    if (*p < '0' || *p > '9')
      return 0;
    for (; *p >= '0' && *p <= '9' && index < 100000; p++)
      index = index * 10 + (*p - '0');
    if (index >= menu->n_items || !part_end(p, &nth, &tail) || nth != 1)
      return 0;
    *used = (size_t)(p - rest) + tail;
    return &menu->items[index];
  }
  char name[96];
  size_t best = 0;
  int best_nth = 1;
  for (int i = 0; i < menu->n_items; i++) {
    if (!has_name(&menu->items[i]))
      continue;
    plain_name(menu->items[i].label, name, sizeof(name));
    size_t len = strlen(name);
    bool fits = len > best;
    for (size_t k = 0; fits && k < len; k++)
      fits = rest[k] == name[k];
    if (fits && part_end(rest + len, &nth, &tail)) {
      best = len;
      best_nth = nth;
      *used = len + tail;
    }
  }
  for (int i = 0; best && i < menu->n_items; i++) {
    if (!has_name(&menu->items[i]))
      continue;
    plain_name(menu->items[i].label, name, sizeof(name));
    bool is_it = strlen(name) == best;
    for (size_t k = 0; is_it && k < best; k++)
      is_it = rest[k] == name[k];
    if (is_it && --best_nth == 0)
      return &menu->items[i];
  }
  return 0;
}

/* Follow a path from the top menu. Gives the menu the last part is in and its item (0 for the empty path: the
 * top menu itself); returns what's wrong, or 0. */
static const char *resolve(const char *path, menu_t **menu_out, menu_item_t **item_out)
{
  static char problem[200];
  char plain[200];
  plain_name(path, plain, sizeof(plain));
  const char *rest = plain;
  menu_t *menu = &menu_root;
  menu_item_t *item = 0;
  while (*rest) {
    if (item) { /* the part before this one has to be a menu */
      if (item->kind != MI_SUBMENU || !item->submenu) {
        snprintf(problem, sizeof(problem), "\"%s\" isn't a menu", item->label ? item->label : "?");
        return problem;
      }
      menu = item->submenu;
    }
    size_t used = 0;
    item = match_item(menu, rest, &used);
    if (!item) {
      snprintf(problem, sizeof(problem), "no \"%.120s\" in %.40s", rest, menu->title ? menu->title : "the menu");
      return problem;
    }
    rest += used;
  }
  *menu_out = menu;
  *item_out = item;
  return 0;
}

static void choice_name(menu_item_t *item, int choice, char *buf, size_t cap)
{
  char raw[96] = "";
  if (choice >= 0 && choice < item->n_choices) {
    if (item->choices)
      snprintf(raw, sizeof(raw), "%s", item->choices[choice]);
    else if (item->choice_text)
      item->choice_text(item, choice, raw, sizeof(raw));
  }
  plain_name(raw, buf, cap);
}

/* an item's value as text: "on", "2 (medium)", "150", a read-only line's text; "" for an action or a menu */
static void item_value(menu_item_t *item, char *buf, size_t cap)
{
  char text[160] = "";
  buf[0] = 0;
  switch (item->kind) {
    case MI_CHECK:
      if (item->get_check)
        snprintf(buf, cap, "%s", item->get_check(item) ? "on" : "off");
      break;
    case MI_OPTION:
      if (item->get_option) {
        int choice = item->get_option(item);
        choice_name(item, choice, text, sizeof(text));
        snprintf(buf, cap, "%d (%s)", choice, text);
      }
      break;
    case MI_NUMBER:
      if (item->get_num) {
        int32_t number = item->get_num(item);
        if (number == NUM_UNKNOWN)
          snprintf(buf, cap, "unknown");
        else
          snprintf(buf, cap, "%d", (int)number);
      }
      break;
    case MI_TEXT:
      if (item->text) {
        item->text(item, text, sizeof(text));
        plain_name(text, buf, cap);
      }
      break;
    default:
      break;
  }
}

/* Run an item's callback as the menu would on L, with the menu closed. An item that opens something to type
 * or pick in (a keyboard, a list) needs the menu: what it opened is closed again. */
static const char *choose(menu_item_t *item)
{
  bool was_open = menu_current != 0;
  switch (item->kind) {
    case MI_ACTION:
    case MI_NUMBER:
      if (!item->action)
        return "nothing to do on it";
      item->action(item);
      break;
    case MI_CHECK:
      if (!item->get_check || !item->set_check)
        return "can't be changed";
      item->set_check(item, !item->get_check(item));
      break;
    case MI_OPTION: {
      if (!item->get_option || !item->set_option || item->n_choices < 1)
        return "can't be changed";
      int choice = item->get_option(item);
      item->set_option(item, choice < 0 ? 0 : (choice + 1) % item->n_choices);
      break;
    }
    case MI_SUBMENU:
      return "a menu: list it, or name an item in it";
    default:
      return "not something to choose";
  }
  if (!was_open && (menu_current || menu_editing)) {
    menu_close();
    return "it opened something to type or pick in: that needs the menu";
  }
  return 0;
}

static const char *assign(menu_item_t *item, char *value)
{
  int32_t number = 0;
  bool is_number = parse_int(value, &number);
  switch (item->kind) {
    case MI_CHECK: {
      bool on = same(value, "on") || same(value, "true") || same(value, "yes") || (is_number && number == 1);
      bool off = same(value, "off") || same(value, "false") || same(value, "no") || (is_number && number == 0);
      if (!item->set_check || (!on && !off))
        return "a checkbox: on or off";
      item->set_check(item, on);
      return 0;
    }
    case MI_OPTION: {
      if (!item->set_option)
        return "can't be changed";
      int choice = is_number ? number : -1;
      char wanted[96], name[96];
      plain_name(value, wanted, sizeof(wanted));
      for (int i = 0; choice < 0 && i < item->n_choices; i++) {
        choice_name(item, i, name, sizeof(name));
        if (same(name, wanted))
          choice = i;
      }
      if (choice < 0 || choice >= item->n_choices)
        return "not one of its choices (get shows the current one, by number and name)";
      item->set_option(item, choice);
      return 0;
    }
    case MI_NUMBER: {
      if (!item->set_num || !is_number)
        return "a number";
      int32_t max = item->num_max ? item->num_max(item) : INT32_MAX;
      if (number < item->num_min || (max != NUM_UNKNOWN && number > max))
        return "out of its range";
      item->set_num(item, number);
      return 0;
    }
    default:
      return "not something with a value: do chooses it";
  }
}

static void list_menu(menu_t *menu)
{
  char text[320], name[96], value[160];
  for (int i = 0; i < menu->n_items; i++) {
    menu_item_t *item = &menu->items[i];
    plain_name(item->label, name, sizeof(name));
    item_value(item, value, sizeof(value));
    snprintf(text, sizeof(text), "%d | @%d %s %s%s%s", line_no, i, kind_names[item->kind], name,
             value[0] ? " = " : "", value);
    out_line(text);
  }
  result(true, "%d items in %s", menu->n_items, menu->title ? menu->title : "the menu");
}

/* --- the commands ---------------------------------------------------------------------------- */

static int command_by_key(const char *key)
{
  for (int c = 0; c < CMD_COUNT; c++)
    if (same(key, command_keys[c]))
      return c;
  return -1;
}

static void input_off(void)
{
  input_remote_active = false;
  input_remote_buttons = 0;
  input_remote_sticks[0] = input_remote_sticks[1] = false;
  input_frames = 0;
}

/* "input <buttons> [ls <x> <y>] [rs <x> <y>] [for <frames>]" */
static const char *run_input(char *args)
{
  char *buttons = take_word(&args);
  if (same(buttons, "off")) {
    input_off();
    return 0;
  }
  uint32_t mask = 0, bind;
  int32_t number;
  if (same(buttons, "-") || same(buttons, "none"))
    mask = 0;
  else if (parse_int(buttons, &number))
    mask = (uint32_t)number;
  else if (bind_parse(buttons, &bind))
    mask = bind_mask(bind);
  else
    return "buttons: names with + between (A+ZL, at most four), a number (0x41) or -";
  bool sticks[2] = { false, false };
  int32_t axes[4] = { 0, 0, 0, 0 };
  int32_t frames = 1;
  for (char *word = take_word(&args); *word; word = take_word(&args)) {
    int stick = same(word, "ls") ? 0 : same(word, "rs") ? 1 : -1;
    if (stick >= 0) {
      for (int axis = 0; axis < 2; axis++)
        if (!parse_int(take_word(&args), &axes[2 * stick + axis]) || axes[2 * stick + axis] < -STICK_MAX ||
            axes[2 * stick + axis] > STICK_MAX)
          return "a stick: ls or rs, then x and y from -32767 to 32767";
      sticks[stick] = true;
    } else if (same(word, "for")) {
      if (!parse_int(take_word(&args), &frames) || frames < 0)
        return "for: a number of frames (0: until the next input)";
    } else {
      return "after the buttons: ls <x> <y>, rs <x> <y>, for <frames>";
    }
  }
  input_remote_buttons = mask & 0xFFFF; /* the buttons; the sticks' direction bits come from the sticks */
  memcpy(input_remote_sticks, sticks, sizeof(sticks));
  memcpy(input_remote_stick, axes, sizeof(axes));
  input_remote_active = true;
  input_frames = frames;
  return 0;
}

static void run_status(bool held)
{
  vec3f_t pos = { 0, 0, 0 };
  bool has_pos = link_drawn_pos(&pos);
  const char *map = gd_ready() ? gd_get_str(GD_STR_MAP) : 0;
  const char *warp = warp_state();
  result(true, "frame=%u map=%s link=%s x=%.3f y=%.3f z=%.3f held=%d menu=%d game_paused=%d warp=%s",
         (unsigned)frame_counter(), map && map[0] ? map : "?", has_pos ? "yes" : "no", (double)pos.x, (double)pos.y,
         (double)pos.z, (int)held, menu_current != 0, (int)(gd_ready() && gd_game_paused(0, 0)),
         warp && warp[0] ? warp : "-");
}

/* Run one line. What it waits for, if anything, is left in the state above. */
static void run_line(char *line, bool held)
{
  char *args = line;
  char *verb = take_word(&args);
  args = skip_spaces(args);
  trim_end(args);
  int32_t number = 1;
  const char *problem = 0;
  menu_t *menu = 0;
  menu_item_t *item = 0;

  if (same(verb, "id")) {
    snprintf(script_id, sizeof(script_id), "%s", *args ? take_word(&args) : "-");
  } else if (same(verb, "echo")) {
    result(true, "%s", args);
  } else if (same(verb, "status")) {
    run_status(held);
  } else if (same(verb, "wait")) {
    if (!parse_int(args, &number) || number < 0)
      result(false, "wait: a number of frames");
    else if (held && number > 0)
      result(false, "wait: the game is held, no frames pass (step lets them through)");
    else {
      wait_left = number;
      result(true, "wait %d", (int)number);
    }
  } else if (same(verb, "fire")) {
    int cmd = command_by_key(take_word(&args));
    char *frames = take_word(&args);
    number = 0;
    if (cmd < 0)
      result(false, "fire: not a command (settings.ini's bind_<key> names: savepos, loadpos, ragdoll...)");
    else if (*frames && (!parse_int(frames, &number) || number < 0))
      result(false, "fire: a number of frames to hold it");
    else {
      if (held)
        fire_pending[cmd] = (uint16_t)(1 + (number > 60000 ? 60000 : number)); /* on the next frame let through */
      else
        binds_fire((command_t)cmd, number);
      result(true, "fire %s", command_keys[cmd]);
    }
  } else if (same(verb, "do")) {
    problem = resolve(args, &menu, &item);
    if (!problem && !item)
      problem = "which item?";
    if (!problem)
      problem = choose(item);
    if (problem)
      result(false, "do: %s", problem);
    else {
      char value[160];
      item_value(item, value, sizeof(value));
      result(true, "%s%s%s", kind_names[item->kind], value[0] ? " = " : "", value);
    }
  } else if (same(verb, "set")) {
    char *equals = args;
    while (*equals && *equals != '=')
      equals++;
    if (!*equals) {
      result(false, "set: <menu path> = <value>");
      return;
    }
    *equals = 0;
    char *value = skip_spaces(equals + 1);
    trim_end(args);
    problem = resolve(args, &menu, &item);
    if (!problem && !item)
      problem = "which item?";
    if (!problem)
      problem = assign(item, value);
    if (problem)
      result(false, "set: %s", problem);
    else {
      char now[160];
      item_value(item, now, sizeof(now));
      result(true, "%s = %s", kind_names[item->kind], now);
    }
  } else if (same(verb, "get")) {
    problem = resolve(args, &menu, &item);
    if (!problem && !item)
      problem = "which item?";
    if (problem)
      result(false, "get: %s", problem);
    else {
      char value[160];
      item_value(item, value, sizeof(value));
      result(true, "%s = %s", kind_names[item->kind], value);
    }
  } else if (same(verb, "list")) {
    problem = resolve(args, &menu, &item);
    if (!problem && item && (item->kind != MI_SUBMENU || !item->submenu))
      problem = "not a menu";
    if (problem)
      result(false, "list: %s", problem);
    else
      list_menu(item ? item->submenu : menu);
  } else if (same(verb, "pause")) {
    if (held)
      result(true, "paused");
    else {
      pause_wanted = true;
      pause_waiting = true; /* its result once the game is held */
    }
  } else if (same(verb, "step")) {
    if (*args && (!parse_int(args, &number) || number < 1))
      result(false, "step: a number of frames");
    else {
      if (!held)
        pause_wanted = true;
      steps_left = number;
      stepping = true; /* its result once they've gone through and the game is held again */
    }
  } else if (same(verb, "run")) {
    if (!held)
      result(true, "running");
    else {
      run_wanted = true;
      run_waiting = true;
    }
  } else if (same(verb, "input")) {
    problem = run_input(args);
    if (problem)
      result(false, "input: %s", problem);
    else
      result(true, "input 0x%x for %d", (unsigned)input_remote_buttons, input_frames);
  } else {
    result(false, "not a command: %s", verb);
  }
}

/* --- the script ------------------------------------------------------------------------------ */

/* The script's next line with something on it (ended in place), or 0 at its end. */
static char *next_line(void)
{
  while (cursor < script_len) {
    char *line = script + cursor;
    size_t end = cursor;
    while (end < script_len && script[end] != '\n')
      end++;
    script[end] = 0;
    cursor = end < script_len ? end + 1 : end;
    line_no++;
    line = skip_spaces(line);
    trim_end(line);
    if (*line && *line != '#')
      return line;
  }
  return 0;
}

/* Take cmd.txt if there is one: read it, delete it (so the next can be written), start running it. */
static void poll(void)
{
  static int calls;
  static bool stuck; /* cmd.txt couldn't be deleted: said once, not run again and again */
  if (++calls < POLL_CALLS)
    return;
  calls = 0;
  fs_file_t file;
  if (fs_open(&file, CMD_FILE, FS_READ) != 0) {
    stuck = false;
    return;
  }
  int64_t size = 0;
  size_t got = 0;
  fs_size(&size, file);
  bool fits = size >= 0 && size < SCRIPT_MAX;
  if (fits && size && fs_read(&got, file, 0, script, (size_t)size) != 0)
    got = 0;
  fs_close(file);
  if (stuck)
    return;
  line_no = 0;
  snprintf(script_id, sizeof(script_id), "-");
  if (fs_delete_file(CMD_FILE) != 0) {
    stuck = true;
    result(false, "cmd.txt can't be deleted: not run");
    return;
  }
  if (!fits) {
    result(false, "cmd.txt is over %d bytes: not run", SCRIPT_MAX - 1);
    return;
  }
  script_len = got;
  script[got] = 0;
  cursor = 0;
  running = true;
}

/* Whether the line last run is still waiting; says so (its result) when the wait is over. */
static bool waiting(bool held)
{
  if (wait_left > 0)
    return true;
  if (pause_waiting) {
    if (!held)
      return true;
    pause_waiting = false;
    result(true, "paused");
  }
  if (stepping) {
    if (steps_left > 0 || !held)
      return true;
    stepping = false;
    result(true, "stepped");
  }
  if (run_waiting) {
    if (held)
      return true;
    run_waiting = false;
    result(true, "running");
  }
  return false;
}

void remote_frame(bool held)
{
  if (!remote_enabled || !settings_sd())
    return;
  if (!held) { /* a frame of the game */
    for (int c = 0; c < CMD_COUNT; c++)
      if (fire_pending[c]) {
        binds_fire((command_t)c, fire_pending[c] - 1);
        fire_pending[c] = 0;
      }
    if (input_frames > 0 && --input_frames == 0)
      input_off();
    if (wait_left > 0)
      wait_left--;
  }
  if (!running)
    poll();
  for (int lines = 0; running && lines < LINES_PER_CALL && !waiting(held); lines++) {
    char *line = next_line();
    if (!line) {
      running = false;
      out_line("end");
      break;
    }
    run_line(line, held);
  }
  out_flush();
}

bool remote_take_pause(void)
{
  bool wanted = pause_wanted;
  pause_wanted = false;
  return wanted;
}

bool remote_take_step(void)
{
  if (steps_left <= 0)
    return false;
  steps_left--;
  return true;
}

bool remote_take_run(void)
{
  bool wanted = run_wanted;
  run_wanted = false;
  return wanted;
}
