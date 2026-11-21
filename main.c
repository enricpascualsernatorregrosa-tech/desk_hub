#include "desk_hub.h"

int main(void) {
    int exit_code = 1;
    DeskHubSystem *sys = NULL;

    if (desk_hub_init(&sys) != 0) {
        exit_code = 0;
        for (;;) {
            desk_hub_step(sys);
        }
        desk_hub_destroy(sys);
    }
    return exit_code;
}