/* Copyright (c) 2014, Sourish Mazumder (sourish.mazumder@gmail.com) */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/ioccom.h>

#include <sys/nvram_ioctl.h>

#define NVRAM_DEV	"/dev/"NVRAM_DEV_NAME

static void
get_usage(void)
{
	fprintf(stdout, "nvram_ctl create geom|devfs\n");
	fprintf(stdout, "nvram_ctl destroy\n");
	fprintf(stdout, "nvram_ctl help\n");

	return;
}

static int
nvram_dev_ioctl(int fd, unsigned long request)
{
	int error;

	error = ioctl(fd, request);

	return error;
}

int
main(int argc, char **argv)
{
	int error = 0;
	int fd = 0;
	char *cmdname;

	if (argc < 2) {
		get_usage();
		error = 1;
		goto out;
	}

	cmdname = argv[1];
	if (strcmp(cmdname, "help") == 0) {
		get_usage();
		goto out;
	}

	fd = open(NVRAM_DEV, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Failed to open device file %s\n", NVRAM_DEV);
		error = 1;
		goto out;
	}

	if (strcmp(cmdname, "create") == 0) {
		if (argc < 3) {
			get_usage();
			error = 1;
			goto out;
		}
		cmdname = argv[2];
		if (strcmp(cmdname, "geom") == 0) {
			error = nvram_dev_ioctl(fd, NVRAM_GEOM_CREATE);
			goto out;
		}
		if (strcmp(cmdname, "devfs") == 0) {
			error = nvram_dev_ioctl(fd, NVRAM_DEVFS_CREATE);
			goto out;
		}
	}
	if (strcmp(cmdname, "destroy") == 0) {
		error = nvram_dev_ioctl(fd, NVRAM_DESTROY);
		goto out;
	}

out:
	close(fd);
	return error;
}
