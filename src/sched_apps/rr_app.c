#define _GNU_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <linux/unistd.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <sched.h>


#define gettid() syscall(__NR_gettid)

#define SCHED_RR 2

static volatile int done;

void *run_rr(void *data) {
	pid_t pids, my_pid;
	int x = 0;
	int ret;
	unsigned int flags = 0;
	struct sched_param sched;

	memset(&sched, 0, sizeof(sched));
	sched.sched_priority = 8;        /* set priority */

	printf("rr thread started [%ld]\n", gettid());
	if (sched_setscheduler(gettid(), SCHED_RR, &sched) < 0 )
		fprintf(stderr, "SETSCHEDULER failed - err = %s\n", strerror(errno));
	else
		printf("Priority set to \"%d\"\n", sched.sched_priority);

	sleep(1);
	for (;;) {
		done++;
	}

	return NULL;
}

#define THREAD_COUNT 15

int main(int argc, char **argv)
{
	int i;
	pthread_t thread[THREAD_COUNT];
	struct sched_param sched;

	memset(&sched, 0, sizeof(sched));
	sched.sched_priority = 10;        /* set priority */
	printf("main thread [%ld]\n", gettid());
	if (sched_setscheduler(gettid(), SCHED_FIFO, &sched) < 0 )
		fprintf(stderr, "SETSCHEDULER failed - err = %s\n", strerror(errno));
	else
		printf("Priority set to \"%d\"\n", sched.sched_priority);


	for (i = 0; i < THREAD_COUNT; i++) {
		pthread_create(&thread[i], NULL, run_rr, NULL);
	}

	sleep(10);
	exit(1);
}

