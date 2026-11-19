/* Commands from a file, for scripts and tools: with settings > remote control on, a script written to
 * sd:/sloperinaGZ/remote/cmd.txt is taken (the file is deleted: the next may be written) and run on the game's
 * thread, a line at a time, as if each were pressed or chosen in the menu. What each line did goes to
 * sd:/sloperinaGZ/remote/out.txt and the log. On an emulator the SD card is a folder on the PC, so anything that
 * can write a file can drive the module; tools/remote.py does.
 *
 * A script's lines (blank ones and # comments are skipped):
 *   id <word>                  the word that starts this script's lines in out.txt (otherwise "-")
 *   fire <command> [frames]    a command of settings > rebind commands, by its settings.ini key (savepos,
 *                              loadpos, ragdoll, levitate...), held for that many frames more (rise, sink)
 *   do <menu path>             choose a menu item: an action runs, a checkbox flips, an option moves on
 *   set <menu path> = <value>  a checkbox (on / off), an option (its text or number) or a number
 *   get <menu path>            its value and its line as the menu shows it
 *   list [<menu path>]         a menu's items: number, kind, name, value
 *   wait <frames>              the next line runs that many frames later
 *   pause                      hold the game, as frame advance does (it needn't be on)
 *   step [frames]              let that many frames through (pausing first), then hold again
 *   run                        let the game run again
 *   input <buttons> [ls <x> <y>] [rs <x> <y>] [for <frames>]
 *                              buttons and sticks for the game, on top of the controller: "A+ZL", "0x41" or
 *                              "-" for none; sticks -32767..32767; for 1 frame unless said (for 0: until the
 *                              next input line). "input off" ends it.
 *   status                     the frame, the map, where Link is and how he moves (vy, hs: his speed up and
 *                              across), what he holds and has on ZR, whether the game is held
 *   trace <frames> | off       a line for each of the next frames: "<line> t y= vy= hs= x= z= hand= zr= paused="
 *   actors [metres]            the running actors within that distance of Link (6 unless said), nearest first
 *   echo <text>                the text, into out.txt
 * A menu path is the names from the top menu down, "/" between them, any case ("cheats/infinite magic",
 * "scene/end cutscene"); "name#2" is the second item of that name, "@7" the menu's item number 7 (list shows
 * the numbers), for lines with no name.
 *
 * out.txt gets one line per script line: "<frame> <id> <line number> ok|err <text>", and "<frame> <id> end"
 * when the script is done. */
#ifndef REMOTE_H
#define REMOTE_H

#include <stdbool.h>

extern bool remote_enabled; /* settings > remote control */

/* Once a frame on the game's thread, after the controller's read (held: false), and from frame advance's hold
 * while the game is held (held: true). */
void remote_frame(bool held);

/* For frame advance (ootgz.c): the script asked to hold the game; to let one frame through (true once per
 * frame of a step); to run again. Each is taken by the call. */
bool remote_take_pause(void);
bool remote_take_step(void);
bool remote_take_run(void);

#endif
