#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <unistd.h>
#include <string.h>

#define SOCK_PATH "/tmp/qua-socket.sock"

int main(int argc, char **argv)
{
	if (argc < 2)
		return 1;

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return 1;

	struct sockaddr_un addr = {
		.sun_family = AF_UNIX,
		.sun_path = SOCK_PATH
	};

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)))
		return 1;

	struct iovec iov[2] = {
		{ .iov_base = "play", .iov_len = 5 },
		{ .iov_base = argv[1], .iov_len = strlen(argv[1]) + 1 }
	};
	writev(fd, iov, 2);
	close(fd);
}
