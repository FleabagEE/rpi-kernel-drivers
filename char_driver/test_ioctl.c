#include <stdio.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MYDEVICE_MAGIC 'M'
#define MYDEVICE_CLEAR   _IO(MYDEVICE_MAGIC, 0)
#define MYDEVICE_GET_LEN _IOR(MYDEVICE_MAGIC, 1, int)

int main() {
    int fd = open("/dev/mydevice", O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    int len;
    ioctl(fd, MYDEVICE_GET_LEN, &len);
    printf("buffer_len = %d\n", len);

    ioctl(fd, MYDEVICE_CLEAR);
    printf("cleared\n");

    ioctl(fd, MYDEVICE_GET_LEN, &len);
    printf("buffer_len after clear = %d\n", len);

    close(fd);
    return 0;
}
