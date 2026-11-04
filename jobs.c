/*
 * Minimal background-process table.  No job control (fg/bg/^Z) yet:
 * background commands are tracked only so `wait` and $! work and so
 * zombies are reaped at each prompt.
 */
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>

#include "shell.h"

#define MAXJOBS 64

struct job {
	pid_t pid;
	int status;
	int done;
	int orphan;             /* internal helper; not visible to `wait` */
};

static struct job jobs[MAXJOBS];
static int njobs;

int
wait_status(int st)
{
	if (WIFEXITED(st))
		return WEXITSTATUS(st);
	if (WIFSIGNALED(st))
		return 128 + WTERMSIG(st);
	return 1;
}

static void
add(pid_t pid, int orphan)
{
	int i;

	for (i = 0; i < njobs; i++)
		if (jobs[i].pid == 0)
			break;
	if (i == njobs) {
		if (njobs == MAXJOBS) {
			job_reap();
			for (i = 0; i < njobs && jobs[i].pid; i++)
				;
			if (i == MAXJOBS)
				return; /* table full; leaves a zombie */
		}
		if (i == njobs)
			njobs++;
	}
	jobs[i].pid = pid;
	jobs[i].done = 0;
	jobs[i].status = 0;
	jobs[i].orphan = orphan;
}

void
job_add(pid_t pid)
{
	add(pid, 0);
}

void
job_orphan(pid_t pid)
{
	add(pid, 1);
}

void
job_reap(void)
{
	int i, st;

	for (i = 0; i < njobs; i++) {
		if (jobs[i].pid == 0 || jobs[i].done)
			continue;
		if (waitpid(jobs[i].pid, &st, WNOHANG) == jobs[i].pid) {
			if (jobs[i].orphan)
				jobs[i].pid = 0;
			else {
				jobs[i].done = 1;
				jobs[i].status = wait_status(st);
			}
		}
	}
}

static int
waitone(pid_t pid)
{
	int st;

	while (waitpid(pid, &st, 0) < 0) {
		if (errno != EINTR)
			return 127;
	}
	if (WIFSIGNALED(st) && WTERMSIG(st) == SIGINT && sh.interactive)
		sh.sigint = 0;  /* child took the ^C; don't double-handle */
	return wait_status(st);
}

/* Wait for a foreground or background child. */
int
job_wait_pid(pid_t pid)
{
	int i, st;

	for (i = 0; i < njobs; i++) {
		if (jobs[i].pid != pid)
			continue;
		st = jobs[i].done ? jobs[i].status : waitone(pid);
		jobs[i].pid = 0;
		return st;
	}
	return waitone(pid);
}

int
job_count(void)
{
	int i, n = 0;

	for (i = 0; i < njobs; i++)
		if (jobs[i].pid && !jobs[i].orphan)
			n++;
	return n;
}

int
job_wait_all(void)
{
	int i, st = 0;

	for (i = 0; i < njobs; i++) {
		if (jobs[i].pid == 0)
			continue;
		st = jobs[i].done ? jobs[i].status : waitone(jobs[i].pid);
		jobs[i].pid = 0;
	}
	njobs = 0;
	return st;
}
