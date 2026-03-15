#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCK_PATH "/tmp/qua-socket.sock"

#ifndef MSG
#define MSG "next"
#endif

int main(void)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return 1;

	struct sockaddr_un addr = {
		.sun_family = AF_UNIX,
		.sun_path = SOCK_PATH
	};

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)))
		return 1;

	write(fd, MSG, sizeof(MSG));
	close(fd);
}
