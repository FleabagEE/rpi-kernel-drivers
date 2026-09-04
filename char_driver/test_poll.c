#include <stdio.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

int main() {
    int fd = open("/dev/mydevice", O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;

    printf("waiting for data...\n");
    poll(&pfd, 1, -1);

    if (pfd.revents & POLLIN) {
        char buf[256];
        int n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            buf[n] = '\0';
            printf("got data: %s\n", buf);
        }
    }
    close(fd);
    return 0;
}
