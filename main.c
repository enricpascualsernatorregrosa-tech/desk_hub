#include "desk_hub.h"
#include <stddef.h>

void desk_hub_run(void) {
    DeskHubSystem *sys = desk_hub_create();
    if (sys != NULL) {
        while (1) {
            desk_hub_step(sys);
        }
        desk_hub_destroy(sys);
    }
    return;
}
