/* The command binds (binds.c): gz's bind evaluation, names, parsing, menu text and recording; no game. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../src/ootgz/binds.c"

static uint32_t held, pressed;
static char logged[256];
uint32_t input_held(void) { return held; }
uint32_t input_pressed(void) { return pressed; }
const char *T(const char *english) { return english; }
void gfx_logf(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(logged, sizeof(logged), fmt, ap);
  va_end(ap);
}

/* one frame with these buttons held */
static void frame(uint32_t now)
{
  static uint32_t before;
  held = now;
  pressed = now & ~before;
  binds_update(now, pressed, before & ~now);
  before = now;
}

static void name_is(uint32_t bind, const char *want)
{
  char buf[64];
  bind_name(bind, buf, sizeof(buf));
  if (strcmp(buf, want)) {
    printf("bind name: %s, wanted %s\n", buf, want);
    assert(0);
  }
}

static void expands(const char *in, const char *want)
{
  char buf[256];
  const char *out = binds_expand(in, buf, sizeof(buf));
  if (strcmp(out, want)) {
  /* binds_fire (remote.h): pressed this frame without its buttons, held for the frames asked, even while the
   * command doesn't apply; the controller's own press of it still works meanwhile */
  binds_reset();
  frame(0);
  binds_fire(CMD_SAVEPOS, 0);
  assert(cmd_pressed(CMD_SAVEPOS) && cmd_repeat(CMD_SAVEPOS) && cmd_active(CMD_SAVEPOS));
  frame(0);
  assert(!cmd_pressed(CMD_SAVEPOS) && !cmd_active(CMD_SAVEPOS) && !cmd_held(CMD_SAVEPOS));
  binds_set_enabled(CMD_RISE, false);
  binds_fire(CMD_RISE, 2);
  assert(cmd_pressed(CMD_RISE) && cmd_held(CMD_RISE));
  frame(0);
  assert(!cmd_pressed(CMD_RISE) && cmd_held(CMD_RISE) && cmd_active(CMD_RISE));
  frame(0);
  assert(cmd_held(CMD_RISE));
  frame(0);
  assert(!cmd_held(CMD_RISE) && !cmd_active(CMD_RISE));
  binds_set_enabled(CMD_RISE, true);
  binds_fire(CMD_COUNT, 0); /* not a command: nothing */
  frame(BTN_R);
  frame(BTN_R | BTN_DUP);
  assert(cmd_pressed(CMD_SAVEPOS));
  frame(0);

    printf("expand \"%s\": \"%s\", wanted \"%s\"\n", in, out, want);
    assert(0);
  }
}

int main(void)
{
  binds_reset();
  for (int c = 0; c < CMD_COUNT; c++) /* the menu's apply with the menu open, as ootgz.c's command_context */
    binds_set_enabled((command_t)c, true);

  /* names and parsing */
  name_is(binds[CMD_MENU], "R + rs-down");
  name_is(binds[CMD_TIMER], "R + A + d-left");
  name_is(BIND_NONE, "unbound");
  uint32_t b;
  assert(bind_parse("r + RS-DOWN", &b) && b == binds[CMD_MENU]);
  assert(bind_parse("unbound", &b) && b == BIND_NONE);
  assert(bind_parse("plus", &b) && bind_mask(b) == BTN_PLUS);
  assert(bind_parse("ZL + minus + d-up", &b) && bind_mask(b) == (BTN_ZL | BTN_MINUS | BTN_DUP) && bind_length(b) == 3);
  assert(!bind_parse("R + Q", &b));
  assert(!bind_parse("A + B + X + Y + L", &b)); /* five */

  /* in order: R then right stick down opens the menu, the other way round doesn't */
  frame(0);
  frame(BTN_R);
  assert(!cmd_active(CMD_MENU));
  frame(BTN_R | BTN_RS_DOWN);
  assert(cmd_pressed(CMD_MENU));
  frame(BTN_R | BTN_RS_DOWN);
  assert(cmd_active(CMD_MENU) && !cmd_pressed(CMD_MENU));
  frame(0);
  frame(BTN_RS_DOWN);
  frame(BTN_RS_DOWN | BTN_R);
  assert(!cmd_active(CMD_MENU));
  frame(0);
  /* both in one frame */
  frame(BTN_R | BTN_RS_DOWN);
  assert(cmd_pressed(CMD_MENU));
  frame(0);

  /* R + d-left (return) holds d-left (menu left): only return */
  frame(BTN_R);
  frame(BTN_R | BTN_DLEFT);
  assert(cmd_pressed(CMD_RETURN) && !cmd_active(CMD_LEFT));
  frame(0);
  frame(BTN_DLEFT);
  assert(cmd_pressed(CMD_LEFT) && cmd_repeat(CMD_LEFT));
  for (int i = 0; i < 11; i++) {
    frame(BTN_DLEFT);
    assert(!cmd_repeat(CMD_LEFT));
  }
  frame(BTN_DLEFT);
  assert(cmd_repeat(CMD_LEFT)); /* the 13th frame: repeating */
  frame(BTN_DLEFT);
  frame(BTN_DLEFT);
  assert(!cmd_repeat(CMD_LEFT));
  frame(BTN_DLEFT);
  assert(cmd_repeat(CMD_LEFT)); /* then every 3 */
  frame(0);

  /* R + A + d-right: reset jump height, not show/hide watches (R + d-right) */
  frame(BTN_R);
  frame(BTN_R | BTN_A);
  frame(BTN_R | BTN_A | BTN_DRIGHT);
  assert(cmd_pressed(CMD_RESET_JS) && !cmd_active(CMD_WATCHES));
  frame(0);
  frame(BTN_R);
  frame(BTN_R | BTN_DRIGHT);
  assert(cmd_pressed(CMD_WATCHES) && !cmd_active(CMD_RESET_JS));
  frame(0);

  /* frame advance's tap and its cancels: L alone, let go; L then L3 is run, and L was cancelled */
  frame(BTN_L);
  assert(cmd_pressed(CMD_ADVANCE));
  frame(0);
  assert(!cmd_active(CMD_ADVANCE) && !cmd_cancelled(CMD_ADVANCE));
  frame(BTN_L);
  frame(BTN_L | BTN_LSTICK);
  assert(cmd_pressed(CMD_RUN) && cmd_cancelled(CMD_ADVANCE) && !cmd_active(CMD_ADVANCE));
  assert(!cmd_active(CMD_LEVITATE)); /* L3 inside L + L3 */
  frame(0);
  /* run again in either order; L3 then L: L (select, with the menu open) gives way to it */
  frame(BTN_LSTICK);
  frame(BTN_LSTICK | BTN_L);
  assert(cmd_pressed(CMD_RUN) && !cmd_active(CMD_SELECT) && !cmd_active(CMD_ADVANCE));
  frame(BTN_LSTICK | BTN_L);
  assert(cmd_active(CMD_RUN) && !cmd_pressed(CMD_RUN));
  frame(0);

  /* an active bind stays so when another button is pressed (only a bind holding it ends it, as R + d-up does R) */
  frame(BTN_L);
  frame(BTN_L | BTN_A);
  assert(cmd_active(CMD_ADVANCE) && !cmd_cancelled(CMD_ADVANCE));
  frame(0);
  /* one in progress: another button cancels it; a stick doesn't */
  frame(BTN_R);
  frame(BTN_R | BTN_LS_LEFT);
  frame(BTN_R | BTN_LS_LEFT | BTN_RS_DOWN);
  assert(cmd_pressed(CMD_MENU));
  frame(0);
  frame(BTN_R);
  frame(BTN_R | BTN_A);
  assert(cmd_cancelled(CMD_MENU));
  frame(BTN_R | BTN_A | BTN_RS_DOWN);
  assert(!cmd_active(CMD_MENU));
  frame(0);

  /* a command that doesn't apply: L + L3 off, so L3 toggles levitate */
  binds_set_enabled(CMD_RUN, false);
  frame(BTN_L);
  frame(BTN_L | BTN_LSTICK);
  assert(cmd_pressed(CMD_LEVITATE) && !cmd_active(CMD_RUN) && !cmd_held(CMD_RUN));
  binds_set_enabled(CMD_RUN, true);
  frame(0);

  /* steady controls: any order */
  frame(BTN_X);
  assert(cmd_held(CMD_RISE));
  frame(BTN_X | BTN_RSTICK);
  assert(cmd_held(CMD_RISE) && cmd_held(CMD_HOVER));
  frame(0);

  /* menu text: {key}, and the menu's words only once they're rebound */
  expands("save position ({savepos})", "save position (R + d-up)");
  expands("L: fill   d-left/right: change", "L: fill   d-left/right: change");
  binds[CMD_SELECT] = B1(B_A);
  binds[CMD_LEFT] = B1(B_Y);
  expands("L: fill   d-left/right: change (L held: by 10)", "A: fill   Y/d-right: change (A held: by 10)");
  expands("L/R and L3 and (L", "L/R and L3 and (A");
  binds[CMD_RETURN] = B2(B_ZL, B_B);
  expands("R + d-left: return", "ZL + B: return");
  binds_reset();
  expands("no change", "no change");

  /* recording: let go, then ZL, d-up, let go */
  frame(BTN_L); /* the row chosen with L */
  binds_record_start(CMD_SAVEPOS);
  assert(binds_record_frame());
  frame(0);
  assert(binds_record_frame());
  frame(BTN_ZL);
  assert(binds_record_frame());
  frame(BTN_ZL | BTN_DUP);
  assert(binds_record_frame());
  assert(!cmd_active(CMD_UP)); /* nothing is a command meanwhile */
  frame(0);
  assert(binds_record_frame()); /* recorded: asking */
  assert(!strcmp(binds_record_text(), "ZL + d-up"));
  name_is(binds[CMD_SAVEPOS], "R + d-up"); /* not yet */
  frame(BTN_A); /* neither keep nor cancel: still asking */
  assert(binds_record_frame());
  frame(0);
  assert(binds_record_frame());
  binds_changed = false;
  frame(BTN_L); /* select: keep */
  assert(!binds_record_frame());
  name_is(binds[CMD_SAVEPOS], "ZL + d-up");
  assert(binds_enabled && binds_changed);
  frame(BTN_L);
  assert(!cmd_pressed(CMD_SELECT)); /* the L that kept it doesn't select as well */
  frame(0);
  /* cancelled with back: no change */
  frame(BTN_L);
  binds_record_start(CMD_SAVEPOS);
  frame(0);
  binds_record_frame();
  frame(BTN_Y);
  binds_record_frame();
  frame(0);
  assert(binds_record_frame());
  frame(BTN_B);
  assert(!binds_record_frame());
  name_is(binds[CMD_SAVEPOS], "ZL + d-up");
  frame(0);
  /* rebinding select itself: confirmed with the old select */
  frame(BTN_L);
  binds_record_start(CMD_SELECT);
  frame(0);
  binds_record_frame();
  frame(BTN_A);
  binds_record_frame();
  frame(0);
  binds_record_frame();
  frame(BTN_L);
  assert(!binds_record_frame());
  name_is(binds[CMD_SELECT], "A");
  binds[CMD_SELECT] = bind_default(CMD_SELECT);
  frame(0);
  /* holding the chooser unbinds; a needed one stays */
  frame(BTN_L);
  binds_record_start(CMD_RAGDOLL);
  for (int i = 0; i < CLEAR_FRAMES - 1; i++) {
    frame(BTN_L);
    assert(binds_record_frame());
  }
  frame(BTN_L);
  assert(!binds_record_frame());
  name_is(binds[CMD_RAGDOLL], "unbound");
  frame(BTN_L);
  binds_record_start(CMD_SELECT);
  for (int i = 0; i < CLEAR_FRAMES; i++) {
    frame(BTN_L);
    binds_record_frame();
  }
  name_is(binds[CMD_SELECT], "L");
  frame(0);
  /* nothing pressed: given up, kept */
  binds_record_start(CMD_LOADPOS);
  for (int i = 0; i <= GIVE_UP_FRAMES + 1 && binds_record_frame(); i++)
    frame(0);
  name_is(binds[CMD_LOADPOS], "R + d-down");
  assert(binds_enabled);

  puts("binds: all passed");
  return 0;
}
