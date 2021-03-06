// port specific settings

#ifndef PORT_CONFIG_H
#define PORT_CONFIG_H

// draw2.c
#define START_ROW  0 // which row of tiles to start rendering at?
#define END_ROW   28 // ..end

// pico.c
#define CAN_HANDLE_240_LINES	0 // fow now

//#define dprintf(f,...) printf(f"\n",##__VA_ARGS__)
#ifdef __DEBUG_PRINT
#ifdef __cplusplus
extern "C"
#endif
void dprintf(char *format, ...);
#else
#define dprintf(x...)
#endif

#endif //PORT_CONFIG_H
