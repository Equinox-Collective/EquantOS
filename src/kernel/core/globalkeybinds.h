#ifndef GLOBALKEYBINDS_H
#define GLOBALKEYBINDS_H

#include "../drivers/input.h"
#include <stdbool.h>

// Process incoming input event. Returns true if key was intercepted globally.
bool globalkeybinds_process(const input_event_t *ev);

#endif // GLOBALKEYBINDS_H