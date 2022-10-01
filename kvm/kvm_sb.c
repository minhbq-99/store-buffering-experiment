#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/kvm.h>
#include <sys/mman.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>

struct vcpu {
	int vcpu_fd;
	struct kvm_run *vcpu_run;
};

struct vm {
	int vm_fd;
	void *map_addr;
};

long vcpu_mmap_size;
volatile int p0_ready;
volatile int p1_ready;

/* 16-bit assembly */
char p0[] = "\xe6\x01"			// out 1, al
	    "\xc6\x06\x01\x00\x01"	// mov byte [1], 1
	    "\xa0\x00\x00"		// mov al, [0]
	    "\xa2\x02\x00"		// mov byte [2], al
	    "\x0f\xae\xf0"		// mfence
	    "\xf4";			// hlt

char p1[] = "\xe6\x01"			// out 1, al
	    "\xc6\x06\x00\x00\x01"	// mov byte [0], 1
	    "\xa0\x01\x00"		// mov al, [1]
	    "\xa2\x03\x00"		// mov byte [3], al
	    "\x0f\xae\xf0"		// mfence
	    "\xf4";			// hlt

#define P0_CODE 0x800
#define P1_CODE 0x900

struct vm* create_vm(void)
{
	int kvm_fd, ret, kvm_vm, bytes_needed;
	void *userspace_addr;
	struct kvm_userspace_memory_region mem;
	struct vm *vm;

	vm = malloc(sizeof(struct vm));
	if (!vm)
		return NULL;

	kvm_fd = open("/dev/kvm", O_RDONLY);
	if (kvm_fd < 0) {
		perror("open /dev/kvm");
		goto free_vm;
	}

	vcpu_mmap_size = ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, NULL);
	if (vcpu_mmap_size < 0) {
		perror("ioctl KVM_GET_VCPU_MMAP_SIZE");
		goto close_dev;
	}
	
	ret = ioctl(kvm_fd, KVM_GET_API_VERSION, NULL);
	if (ret != KVM_API_VERSION) {
		perror("ioctl KVM_GET_API_VERSION");
		goto close_dev;
	}

	kvm_vm = ioctl(kvm_fd, KVM_CREATE_VM, NULL);
	if (kvm_vm < 0) {
		perror("ioctl KVM_CREATE_VM");
		goto close_dev;
	}

	userspace_addr = mmap(0, 0x1000, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
	if (userspace_addr == MAP_FAILED) {
		perror("mmap");
		goto close_vm;
	}

	memcpy(userspace_addr + P0_CODE, p0, sizeof(p0));
	memcpy(userspace_addr + P1_CODE, p1, sizeof(p1));

	mem.userspace_addr = (unsigned long long) userspace_addr;
	mem.guest_phys_addr = 0;
	mem.memory_size = 0x1000;
	mem.slot = 0;
	mem.flags = 0;

	ret = ioctl(kvm_vm, KVM_SET_USER_MEMORY_REGION, &mem);
	if (ret < 0) {
		perror("ioctl KVM_SET_USER_MEMORY_REGION");
		ret = -1;
		goto unmap;
	}

	vm->vm_fd = kvm_vm;
	vm->map_addr = userspace_addr;
	close(kvm_fd);
	return vm;

unmap:
	munmap(userspace_addr, 0x1000);
close_vm:
	close(kvm_vm);
close_dev:
	close(kvm_fd);
free_vm:
	free(vm);
	return NULL;
}

struct vcpu* setup_vcpu(int kvm_vm, int id)
{
	int kvm_vcpu, ret;
	struct kvm_regs regs = {};
	struct kvm_sregs sregs;
	struct vcpu *vcpu;

	vcpu = malloc(sizeof(struct vcpu));
	if (!vcpu) {
		fprintf(stderr, "Failed to malloc\n");
		return NULL;
	}

	kvm_vcpu = ioctl(kvm_vm, KVM_CREATE_VCPU, id);
	if (kvm_vcpu < 0) {
		perror("ioctl KVM_CREATE_VCPU");
		goto free_vcpu;
	}
	vcpu->vcpu_fd = kvm_vcpu;

	vcpu->vcpu_run = mmap(0, vcpu_mmap_size, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE, kvm_vcpu, 0);
	if (vcpu->vcpu_run == MAP_FAILED) {
		perror("mmap");
		goto close_vcpu;
	}

	if (id == 0)
		regs.rip = P0_CODE;
	else
		regs.rip = P1_CODE;
	regs.rflags = 0x2;
	ret = ioctl(kvm_vcpu, KVM_SET_REGS, &regs);
	if (ret < 0) {
		perror("ioctl KVM_SET_REGS");
		goto unmap_vcpu_run;
	}

	ret = ioctl(kvm_vcpu, KVM_GET_SREGS, &sregs);
	if (ret < 0) {
		perror("ioctl KVM_SET_SREGS");
		goto unmap_vcpu_run;
	}

	sregs.cs.base = sregs.cs.selector = 0;
	ret = ioctl(kvm_vcpu, KVM_SET_SREGS, &sregs);
	if (ret < 0) {
		perror("ioctl KVM_SET_SREGS");
		goto unmap_vcpu_run;
	}

	return vcpu;

unmap_vcpu_run:
	munmap(vcpu->vcpu_run, vcpu_mmap_size);
close_vcpu:
	close(kvm_vcpu);
free_vcpu:
	free(vcpu);
	return NULL;
}

int reset_vcpu(int vcpu_fd, int id)
{
	int ret;
	struct kvm_regs regs = {};

	if (id == 0)
		regs.rip = P0_CODE;
	else
		regs.rip = P1_CODE;
	regs.rflags = 0x2;
	ret = ioctl(vcpu_fd, KVM_SET_REGS, &regs);
	if (ret < 0) {
		perror("ioctl KVM_SET_REGS");
		return -1;
	}

	return 0;
}

struct vcpu_handler {
	struct vcpu *vcpu;
	int cpu_id;
};

void *handle_cpu(void *arg)
{
	struct vcpu_handler *handler = (struct vcpu_handler *) arg;
	struct vcpu *vcpu = handler->vcpu;
	int ret;
	
	while (1) {
		ret = ioctl(vcpu->vcpu_fd, KVM_RUN, NULL);
		if (ret < 0) {
			perror("ioctl KVM_RUN");
			return (void *)-1;
		}
		switch(vcpu->vcpu_run->exit_reason) {
		case KVM_EXIT_HLT:
			return NULL;
		case KVM_EXIT_IO:
			if (handler->cpu_id == 0) {
				p0_ready = 1;
				while (!p1_ready);
			} else {
				p1_ready = 1;
				while (!p0_ready);
			}
			continue;
		default:
			printf("Unhandled exit: %d\n", vcpu->vcpu_run->exit_reason);
			return (void *)-1;
		}
	}
}

int main(int argc, char **argv)
{
	int number_of_tests, i, count = 0;
	struct vm *vm;
	struct vcpu *vcpu0, *vcpu1;
	pthread_t thread0, thread1;
	struct vcpu_handler handler0, handler1;
	int err;

	if (argc < 2) {
		printf("Usage: kvm_sb number_of_tests\n");
		return 1;
	}
	number_of_tests = strtol(argv[1], NULL, 10);

	vm = create_vm();
	if (!vm) {
		fprintf(stderr, "Failed to create KVM VM\n");
		return 1;
	}

	vcpu0 = setup_vcpu(vm->vm_fd, 0);
	if (!vcpu0) {
		fprintf(stderr, "Failed to create KVM vcpu 0\n");
		return 1;
	}

	vcpu1 = setup_vcpu(vm->vm_fd, 1);
	if (!vcpu1) {
		fprintf(stderr, "Failed to create KVM vcpu 1\n");
		return 1;
	}

	for (i = 0; i < number_of_tests; i++) {
		handler0.cpu_id = 0;
		handler0.vcpu = vcpu0;
		pthread_create(&thread0, NULL, handle_cpu, &handler0);
		if (err < 0) {
			errno = err;
			perror("pthread_create");
			return 1;
		}

		handler1.cpu_id = 1;
		handler1.vcpu = vcpu1;
		pthread_create(&thread1, NULL, handle_cpu, &handler1);
		if (err < 0) {
			errno = err;
			perror("pthread_create");
			return 1;
		}

		pthread_join(thread0, NULL);
		pthread_join(thread1, NULL);

		if ((*((char *) vm->map_addr + 2) == '\x00') &&
		    (*((char *) vm->map_addr + 3) == '\x00'))
			count++;
		reset_vcpu(vcpu0->vcpu_fd, 0);
		reset_vcpu(vcpu1->vcpu_fd, 1);
		p0_ready = 0;
		p1_ready = 0;
		memset(vm->map_addr, 0, 20);
	}

	printf("Non SC: %d\n", count);

	return 0;
}
