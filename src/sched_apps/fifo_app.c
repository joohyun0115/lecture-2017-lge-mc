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

#define SCHED_FIFO 1
#define JOBS 10000000

static volatile int done;

void *run_fifo(void *data) {
	pid_t pids, my_pid;
	int x = 0;
	int ret;
	unsigned int flags = 0;
	struct sched_param sched;

	memset(&sched, 0, sizeof(sched));
	sched.sched_priority = 8;        /* set priority */

	printf("fifo thread started [%ld]\n", gettid());
	if (sched_setscheduler(gettid(), SCHED_FIFO, &sched) < 0 )
		fprintf(stderr, "SETSCHEDULER failed - err = %s\n", strerror(errno));
	else
		printf("Priority set to \"%d\"\n", sched.sched_priority);

	for (x = 0; x < JOBS; x++) {
		done++;
	}

	printf("deadline thread dies [%ld]\n", gettid());
	return NULL;
}

#define THREAD_COUNT 15

int main(int argc, char **argv)
{
	int i;
	pthread_t thread[THREAD_COUNT];

	printf("main thread [%ld]\n", gettid());

	for (i = 0; i < THREAD_COUNT; i++) {
		pthread_create(&thread[i], NULL, run_fifo, NULL);
	}

	sleep(10);

	done = 1;
	for (i = 0; i < THREAD_COUNT; i++) {
		pthread_join(thread[i], NULL);
	}

	printf("main dies [%ld]\n", gettid());
	return 0;
}

