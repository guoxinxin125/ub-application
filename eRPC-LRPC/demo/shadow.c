#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <lrpc/lrpc.h>

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/ub_lrpc0";

	if (lrpc_shadow_run(dev, 1, 1)) {
		fprintf(stderr, "shadow: %s: %s\n", dev, strerror(errno));
		return 1;
	}
	return 0;
}
