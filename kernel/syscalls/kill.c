/*
 * fiwix/kernel/syscalls/kill.c
 *
 * Copyright 2018-2022, Jordi Sanfeliu. All rights reserved.
 * Distributed under the terms of the Fiwix License.
 */

#include <fiwix/types.h>
#include <fiwix/process.h>
#include <fiwix/signal.h>
#include <fiwix/errno.h>

#ifdef __DEBUG__
#include <fiwix/stdio.h>
#endif /*__DEBUG__ */

int sys_kill(__pid_t pid, __sigset_t signum)
{
	int count;
	struct proc *p;

#ifdef __DEBUG__
	printk("(pid %d) sys_kill(%d, %d)\n", current->pid, pid, signum);
#endif /*__DEBUG__ */

	if(signum > NSIG) {
		return -EINVAL;
	}
	if(pid == -1) {
		count = 0;
		FOR_EACH_PROCESS(p) {
			if(p->pid == INIT || p == current || !can_signal(p)) {
				p = p->next;
				continue;
			}
			count++;
			send_sig(p, signum);
			p = p->next;
		}
		return count ? 0 : -ESRCH;
	}
	if(!pid) {
		return kill_pgrp(current->pgid, signum, USER);
	}
	if(pid < 1) {
		return kill_pgrp(-pid, signum, USER);
	}

	return kill_pid(pid, signum, USER);
}
