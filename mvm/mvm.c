/*
 * Copyright (C) 2018 Min Le (lemin9538@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <sys/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <inttypes.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <netinet/ether.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/epoll.h>
#include <linux/netlink.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <getopt.h>
#include <sys/mount.h>

#include <mvm.h>

struct vm *mvm_vm = NULL;

int verbose;

extern struct vm_os os_other;
extern struct vm_os os_linux;

static struct vm_os *vm_oses[] = {
	&os_other,
	&os_linux,
	NULL,
};

void *map_vm_memory(struct vm *vm)
{
	uint64_t args[2];
	void *addr = NULL;

	args[0] = 0;
	args[1] = vm->vm_info.mem_size;

	addr = mmap(NULL, args[1], PROT_READ | PROT_WRITE,
			MAP_SHARED, vm->vm_fd, args[0]);
	if (addr == (void *)-1)
		return NULL;

	printv("mmap addr is 0x%lx\n", (unsigned long)addr);

	if (ioctl(vm->vm_fd, IOCTL_VM_MMAP, args)) {
		printf("* error - mmap memory failed 0x%lx 0x%lx\n",
				args[0], vm->vm_info.mem_size);
		munmap(addr, vm->vm_info.mem_size);
		addr = 0;
	}

	return addr;
}

static int create_new_vm(struct vm_info *vminfo)
{
	int fd, vmid = -1;

	fd = open("/dev/mvm/mvm0", O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		perror("/dev/mvm/mvm0");
		return -EIO;
	}

	printv("* create new vm *\n");
	printv("        -name       : %s\n", vminfo->name);
	printv("        -os_type    : %s\n", vminfo->os_type);
	printv("        -nr_vcpus   : %d\n", vminfo->nr_vcpus);
	printv("        -bit64      : %d\n", vminfo->bit64);
	printv("        -mem_size   : 0x%lx\n", vminfo->mem_size);
	printv("        -mem_start  : 0x%lx\n", vminfo->mem_start);
	printv("        -entry      : 0x%lx\n", vminfo->entry);
	printv("        -setup_data : 0x%lx\n", vminfo->setup_data);

	vmid = ioctl(fd, IOCTL_CREATE_VM, vminfo);
	if (vmid <= 0) {
		perror("vmid");
		return vmid;
	}

	close(fd);

	return vmid;
}

static int release_vm(int vmid)
{
	int fd, ret;

	fd = open("/dev/mvm/mvm0", O_RDWR);
	if (fd < 0)
		return -ENODEV;

	ret = ioctl(fd, IOCTL_DESTROY_VM, vmid);
	close(fd);

	return ret;
}

int destroy_vm(struct vm *vm)
{
	if (!vm)
		return -EINVAL;

	if (vm->mmap) {
		if (vm->vm_fd)
			ioctl(vm->vm_fd, IOCTL_VM_UNMAP, 0);
		munmap(vm->mmap, vm->vm_info.mem_size);
	}

	if (vm->vm_fd > 0)
		close(vm->vm_fd);

	if (vm->vmid > 0)
		release_vm(vm->vmid);

	if (vm->image_fd > 0)
		close(vm->image_fd);

	if (vm->os_data > 0)
		free(vm->os_data);

	free(vm);

	return 0;
}

void print_usage(void)
{
	fprintf(stderr, "\nUsage: mvm [options] \n\n");
	fprintf(stderr, "    -c <vcpu_count>            (set the vcpu numbers of the vm)\n");
	fprintf(stderr, "    -m <mem_size_in_MB>        (set the memsize of the vm - 2M align)\n");
	fprintf(stderr, "    -i <boot or kernel image>  (the kernel or bootimage to use)\n");
	fprintf(stderr, "    -s <mem_start>             (set the membase of the vm if not a boot.img)\n");
	fprintf(stderr, "    -n <vm name>               (the name of the vm)\n");
	fprintf(stderr, "    -t <vm type>               (the os type of the vm )\n");
	fprintf(stderr, "    -b <32 or 64>              (32bit or 64 bit )\n");
	fprintf(stderr, "    -r                         (do not load ramdisk image)\n");
	fprintf(stderr, "    -v                         (verbose print debug information)\n");
	fprintf(stderr, "    -d                         (run as a daemon process)\n");
	fprintf(stderr, "\n");
	exit(EXIT_FAILURE);
}

static int create_and_init_vm(struct vm *vm)
{
	int ret = 0;
	char path[32];
	struct vm_info *info = &vm->vm_info;

	/* set the default value of vm_info if not give */
	if (info->entry == 0)
		info->entry = VM_MEM_START;
	if (info->mem_start == 0)
		info->mem_start = VM_MEM_START;
	if (info->mem_size == 0)
		info->mem_size = VM_MIN_MEM_SIZE;
	if (info->nr_vcpus == 0)
		info->nr_vcpus = 1;

	vm->vmid = create_new_vm(info);
	if (vm->vmid <= 0)
		return (vm->vmid);

	memset(path, 0, 32);
	sprintf(path, "/dev/mvm/mvm%d", vm->vmid);
	vm->vm_fd = open(path, O_RDWR | O_NONBLOCK);
	if (vm->vm_fd < 0) {
		perror(path);
		return -EIO;
	}

	/*
	 * map a fix region for this vm, need to call ioctl
	 * to informe hypervisor to map the really physical
	 * memory
	 */
	vm->mmap = map_vm_memory(vm);
	if (!vm->mmap)
		return -EAGAIN;

	*(unsigned long *)vm->mmap = 0xdeadbeef;

	/* load the image into the vm memory */
	ret = vm->os->load_image(vm);
	if (ret)
		return ret;

	ret = vm->os->setup_vm_env(vm);
	if (ret)
		return ret;

	return 0;
}

static struct vm_os *get_vm_os(char *os_type)
{
	int i;
	struct vm_os *os = NULL;
	struct vm_os *default_os = NULL;

	for (i = 0; ; i++) {
		os = vm_oses[i];
		if (!os)
			break;

		if (strcmp(os_type, os->name) == 0)
			return os;

		if (strcmp("default", os->name) == 0)
			default_os = os;
	}

	return default_os;
}

static int mvm_main(struct vm_info *info, char *image_path, unsigned long flags)
{
	int image_fd, ret;
	struct vm_os *os;

	if (info->name[0] == 0)
		strcpy(info->name, "unknown");
	if (info->os_type[0] == 0)
		strcpy(info->os_type, "unknown");

	os = get_vm_os(info->os_type);
	if (!os)
		return -EINVAL;

	/* read the image to get the entry and other args */
	image_fd = open(image_path, O_RDWR | O_NONBLOCK);
	if (image_fd < 0) {
		perror(image_path);
		return -ENOENT;
	}

	mvm_vm = malloc(sizeof(struct vm));
	if (!mvm_vm) {
		close(image_fd);
		return -ENOMEM;
	}

	memset(mvm_vm, 0, sizeof(struct vm));
	mvm_vm->os = os;
	mvm_vm->image_fd = image_fd;
	mvm_vm->flags = flags;
	info->mmap_base = 0;
	memcpy(&mvm_vm->vm_info, info, sizeof(struct vm_info));

	/* free the unused memory */
	free(info);
	free(image_path);

	ret = os->early_init(mvm_vm);
	if (ret) {
		printf("* error - os early init faild %d\n", ret);
		goto release_vm;
	}

	/* ensure the below field is not modified */
	mvm_vm->vmid = 0;
	mvm_vm->vm_fd = -1;

	ret = create_and_init_vm(mvm_vm);
	if (ret)
		goto release_vm;

	/*
	 * now start the vm
	 */
	ret = ioctl(mvm_vm->vm_fd, IOCTL_POWER_UP_VM, NULL);
	if (ret)
		goto release_vm;

	/* do loop */
	while (1) {

	}

release_vm:
	destroy_vm(mvm_vm);
	return ret;
}

static struct option options[] = {
	{"vcpu_number", required_argument, NULL, 'c'},
	{"mem_size",	required_argument, NULL, 'm'},
	{"image",	required_argument, NULL, 'i'},
	{"mem_start",	required_argument, NULL, 's'},
	{"name",	required_argument, NULL, 'n'},
	{"os_type",	required_argument, NULL, 't'},
	{"bit",		required_argument, NULL, 'b'},
	{"no_ramdisk",	no_argument,	   NULL, 'r'},
	{"help",	no_argument,	   NULL, 'h'},
	{NULL,		0,		   NULL,  0}
};

static int parse_vm_memsize(char *buf, uint64_t *size)
{
	int len = 0;

	if (!buf)
		return -EINVAL;

	len = strlen(buf) - 1;

	if ((buf[len] != 'm') && (buf[len] != 'M'))
		return -EINVAL;

	buf[len] = '\0';
	*size = atol(buf) * 1024 * 1024;

	return 0;
}

static int parse_vm_membase(char *buf, unsigned long *value)
{
	if (strlen(buf) < 3)
		return -EINVAL;

	if ((buf[0] == '0') && (buf[1] == 'x')) {
		sscanf(buf, "0x%lx", value);
		return 0;
	}

	return -EINVAL;
}

static void signal_handler(int signum)
{
	int vmid = 0xfff;

	switch (signum) {
	case SIGALRM:
	case SIGINT:
	case SIGTERM:
		if (mvm_vm)
			vmid = mvm_vm->vmid;

		printf("* received signal %i vm-%d\n", signum, vmid);
		destroy_vm(mvm_vm);
		exit(0);
	default:
		break;
	}
}

int main(int argc, char **argv)
{
	int ret, opt, idx;
	struct vm_info *info = NULL;
	char *image_path = NULL;
	char *optstr = "c:m:i:s:n:t:b:rv?hd";
	unsigned long flags = 0;
	int run_as_daemon = 0;

	info = (struct vm_info *)malloc(sizeof(struct vm_info));
	if (!info)
		return -ENOMEM;

	/*
	 * default is 64 bit, 1 vcpus and 32M memory
	 */
	memset(info, 0, sizeof(struct vm_info));
	info->bit64 = 1;

	while ((opt = getopt_long(argc, argv, optstr, options, &idx)) != -1) {
		switch(opt) {
		case 'c':
			info->nr_vcpus = atoi(optarg);
			break;
		case 'm':
			ret = parse_vm_memsize(optarg, &info->mem_size);
			if (ret)
				print_usage();
			break;
		case 'i':
			image_path = malloc(256);
			if (!image_path) {
				free(info);
				return -ENOMEM;
			}

			strcpy(image_path, optarg);
			break;
		case 's':
			ret = parse_vm_membase(optarg, (uint64_t *)&info->mem_start);
			if (ret)
				print_usage();
			break;
		case 'n':
			strncpy((char *)info->name, optarg, 31);
			break;
		case 't':
			strncpy((char *)info->os_type, optarg, 31);
			break;
		case 'b':
			ret = atoi(optarg);
			if ((ret != 32) && (ret != 64))
				print_usage();
			info->bit64 = ret == 64 ? 1 : 0;
			break;
		case 'r':
			flags |= MVM_FLAGS_NO_RAMDISK;
			break;
		case 'v':
			verbose = 1;
			break;
		case 'd':
			run_as_daemon = 1;
			break;
		case 'h':
			print_usage();
			break;
		}
	}

	if (!image_path) {
		printf("* error - please point the image for this VM\n");
		return -1;
	}

	if (info->nr_vcpus > VM_MAX_VCPUS) {
		printf("* warning - support max %d vcpus\n", VM_MAX_VCPUS);
		info->nr_vcpus = VM_MAX_VCPUS;
	}

	if (run_as_daemon) {
		if (daemon(1, 1)) {
			printf("failed to run as daemon\n");
			exit(EXIT_FAILURE);
		}
	}

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	return mvm_main(info, image_path, flags);
}
