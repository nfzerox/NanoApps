#ifndef TR_PLATFORM_H
#define TR_PLATFORM_H
#include <stdint.h>
uint64_t plat_time_us(void);
/* Game data by name ("player.bksb"); the caller frees. save != 0 reads the save folder. */
void *plat_read_file(const char *name, uint32_t *size, int save);
int plat_write_file(const char *name, const void *data, uint32_t size);
void plat_log(const char *fmt, ...);
/* Write the log out now (slow: for rare moments only). */
void plat_log_flush(void);
/* Called many times during a frame, so the platform can note the finger's state more often
 * than once a frame (a quick tap can begin and end within one slow frame). */
void plat_poll(void);
/* Stop the game with a message; does not return. */
void plat_fatal(const char *message);
#endif
