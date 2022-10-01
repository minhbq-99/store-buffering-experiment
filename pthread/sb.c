#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <pthread.h>
#include <unistd.h>

struct test_context {
	int num_of_tests;
	int *x;
	int *y;
	int *eax0;
	int *eax1;
	volatile int *wait;
};

struct test_input {
	struct test_context *ctx;
	int current_test;
};

struct test_context* init(int number_of_tests)
{
	struct test_context *ctx = malloc(sizeof(struct test_context));

	if (ctx == NULL)
		return NULL;

	ctx->num_of_tests = number_of_tests;
	ctx->x = calloc(number_of_tests, sizeof(*(ctx->x)));
	if (ctx->x == NULL)
		goto free_ctx;

	ctx->y = calloc(number_of_tests, sizeof(*(ctx->y)));
	if (ctx->y == NULL)
		goto free_x;

	ctx->eax0 = calloc(number_of_tests, sizeof(*(ctx->eax0)));
	if (ctx->eax0 == NULL)
		goto free_y;

	ctx->eax1 = calloc(number_of_tests, sizeof(*(ctx->eax1)));
	if (ctx->eax1 == NULL)
		goto free_eax0;

	ctx->wait = calloc(number_of_tests, sizeof(*(ctx->wait)));
	if (ctx->wait == NULL)
		goto free_eax1;

	return ctx;

free_eax1:
	free(ctx->eax1);
free_eax0:
	free(ctx->eax0);
free_y:
	free(ctx->y);
free_x:
	free(ctx->x);
free_ctx:
	free(ctx);
	return NULL;
}

/*
 * Litmus test
 *
 * { x=0; y=0; }
 * P0          | P1          ;
 * MOV [x],$1  | MOV [y],$1  ;
 * MOV EAX,[y] | MOV EAX,[x] ;
 */
void *p0(void *arg)
{
	struct test_input *input = (struct test_input*) arg;
	int ret;
	cpu_set_t cpumask;

	CPU_ZERO(&cpumask);
	CPU_SET(0, &cpumask);
	ret = sched_setaffinity(gettid(), sizeof(cpumask), &cpumask);
	if (ret < 0)
		return (void *)-1;

	/* Wait for p1 */
	while (input->ctx->wait[input->current_test] == 0);

	__asm__ volatile (
		"movl $1, %[x]\t\n"
		"movl %[y], %[eax]\t\n"
		: [x] "=m" (input->ctx->x[input->current_test]),
		  [y] "=m" (input->ctx->y[input->current_test]),
		  [eax] "=&a" (input->ctx->eax0[input->current_test])
		:
		: "cc", "memory"
	);

	__asm__ volatile ("":::"memory");
	return NULL;
}

void *p1(void *arg)
{
	struct test_input *input = (struct test_input*) arg;
	int ret;
	cpu_set_t cpumask;

	CPU_ZERO(&cpumask);
	CPU_SET(7, &cpumask);
	ret = sched_setaffinity(gettid(), sizeof(cpumask), &cpumask);
	if (ret < 0)
		return (void *)-1;

	/* Signal p0 */
	input->ctx->wait[input->current_test] = 1;

	__asm__ volatile (
		"movl $1, %[y]\t\n"
		"movl %[x], %[eax]\t\n"
		: [x] "=m" (input->ctx->x[input->current_test]),
		  [y] "=m" (input->ctx->y[input->current_test]),
		  [eax] "=&a" (input->ctx->eax1[input->current_test])
		:
		: "cc", "memory"
	);
	__asm__ volatile ("":::"memory");
}

void check_result(struct test_context *context)
{
	int i, count = 0;

	for (i = 0; i < context->num_of_tests; i++) {
		if (context->eax0[i] == 0 && context->eax1[i] == 0)
			count++;
	}
	
	printf("Non-SC: %d\n", count);
}

int main(int argc, char **argv)
{
	int i, num_of_tests;

	if (argc < 2) {
		printf("Usage: sb num_of_test");
		return 1;
	}
	num_of_tests = strtol(argv[1], NULL, 10);
	struct test_context *context = init(num_of_tests);
	for (i = 0; i < num_of_tests; i++) {
		pthread_t thread0, thread1;
		struct test_input input = {
			.ctx = context,
			.current_test = i,
		};

		pthread_create(&thread0, NULL, p0, &input);
		pthread_create(&thread1, NULL, p1, &input);
		pthread_join(thread0, NULL);
		pthread_join(thread1, NULL);
	}
	check_result(context);

	return 0;
}
