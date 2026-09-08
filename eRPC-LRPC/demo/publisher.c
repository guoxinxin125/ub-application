#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <lrpc/lrpc.h>
#include "service_image.h"

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/ub_lrpc0";
	int fd = open(dev, O_RDWR | O_CLOEXEC);
	uint64_t *service_data;
	const uint32_t procedures[] = {1, 2, 3, 4};

	if (fd < 0) {
		fprintf(stderr, "publisher: open %s: %s\n", dev, strerror(errno));
		return 1;
	}
	if (lrpc_publish_services(fd, lrpc_add_service_image,
				  lrpc_add_service_image_size, procedures,
				  sizeof(procedures) / sizeof(procedures[0]), 1)) {
		fprintf(stderr, "publisher: publish: %s\n", strerror(errno));
		return 1;
	}
	service_data = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
			    fd, UB_LRPC_DATA_OFFSET);
	if (service_data == MAP_FAILED) {
		fprintf(stderr, "publisher: data mmap: %s\n", strerror(errno));
		return 1;
	}
	*service_data = 100;
	__sync_synchronize();
	munmap(service_data, 4096);
	printf("LRPC_PUBLISHED epoch=1 proc=1 bytes=%zu pid=%d\n",
	       lrpc_add_service_image_size, getpid());
	close(fd);
	return 0;
}
