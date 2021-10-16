// Decouples main.c with various OS layers.

#include <stdbool.h>

enum event_type { ET_IGNORE, ET_KEYPRESS };
struct event { enum event_type type; char kp_key; };

void os_create_window(const char*, int, int);
bool os_choose_bin(char*);
bool os_should_exit(void);
bool os_poll_event(struct event*);
void os_draw_rect(int, int, int, int, const float*, int);
void os_present(void);
