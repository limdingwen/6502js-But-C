// This file is part of 6502js But C.
// 
// 6502js But C is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// 6502js But C is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
// 
// You should have received a copy of the GNU Affero General Public License
// along with 6502js But C.  If not, see <https://www.gnu.org/licenses/>.

// Decouples main.c with various OS layers.

#include <stdbool.h>

enum event_type { ET_IGNORE, ET_KEYPRESS, ET_EXPOSE };
struct event { enum event_type type; char kp_key; };

int our_main(int argc, char **argv);

void os_create_window(const char*, int, int);
void os_create_colormap(const float*, int);
bool os_choose_bin(char*, int);
bool os_should_exit(void);
bool os_poll_event(struct event*);
void os_draw_rect(int, int, int, int, const float*, int);
void os_present(void);
void os_close(void);
