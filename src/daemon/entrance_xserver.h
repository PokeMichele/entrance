#ifndef ENTRANCE_XLIB_H_
#define ENTRANCE_XLIB_H_
typedef void (*Entrance_X_Cb)(const char *data);
int entrance_xserver_init(Entrance_X_Cb start, const char *dname);
void entrance_xserver_end_wait(pid_t pid);
void entrance_xserver_shutdown(void);
#endif /* ENTRANCE_XLIB_H_ */
