#ifndef TEST_BPS_H
#define TEST_BPS_H
#define BPS_SUCCESS 0
#define BPS_FAILURE -1
#define BPS_IO_INPUT 1
#define BPS_IO_EXCEPT 2
int bps_add_fd(int fd, int events, int (*handler)(int, int, void *), void *data);
int bps_remove_fd(int fd);
#endif
