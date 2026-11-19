/* gz's save / load position: R + d-up saves where Link is and which way he faces, R + d-down puts him
 * back there (menu closed; also link > save / load position). In the same map only. Link is moved
 * through his physics record, which levitate.c finds and keeps (link_record_*). */
#ifndef POSITION_H
#define POSITION_H

#include <stdbool.h>

void position_save(void);
void position_load(void);
/* the saved position given, not taken from where Link is: this map, facing in degrees (0 = +z); a load then
 * puts him there (remote.h's place) */
void position_set(double x, double y, double z, double facing_degrees);
/* frame hook, after levitate_frame: the shortcuts, and a load in progress */
void position_frame(bool menu_open);

#endif
