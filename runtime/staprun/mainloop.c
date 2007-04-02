/* -*- linux-c -*-
 *
 * mainloop - staprun main loop
 *
 * This file is part of systemtap, and is free software.  You can
 * redistribute it and/or modify it under the terms of the GNU General
 * Public License (GPL); either version 2, or (at your option) any
 * later version.
 *
 * Copyright (C) 2005-2007 Red Hat Inc.
 */

#include "staprun.h"

/* globals */
int control_channel = 0;
int ncpus;
int use_old_transport = 0;

/**
 *	send_request - send request to kernel over control channel
 *	@type: the relay-app command id
 *	@data: pointer to the data to be sent
 *	@len: length of the data to be sent
 *
 *	Returns 0 on success, negative otherwise.
 */
int send_request(int type, void *data, int len)
{
	char buf[1024];
	if (len > (int)sizeof(buf)) {
		err("exceeded maximum send_request size.\n");
		return -1;
	}
	memcpy(buf, &type, 4);
	memcpy(&buf[4],data,len);
	return write(control_channel, buf, len+4);
}

/* 
 * start_cmd forks the command given on the command line
 * with the "-c" option. It will not exec that command
 * until it received signal SIGUSR1. We do it this way because 
 * we must have the pid of the forked command so it can be set to 
 * the module and made available internally as _stp_target.
 * SIGUSR1 is sent from stp_main_loop() below when it receives
 * STP_START from the module.
 */
void start_cmd(void)
{
	pid_t pid;
	sigset_t usrset;
		
	sigemptyset(&usrset);
	sigaddset(&usrset, SIGUSR1);
	sigprocmask(SIG_BLOCK, &usrset, NULL);

	dbug ("execing target_cmd %s\n", target_cmd);
	if ((pid = fork()) < 0) {
		perror ("fork");
		exit(-1);
	} else if (pid == 0) {
		int signum;

		if (setregid(cmd_gid, cmd_gid) < 0) {
			perror("setregid");
		}
		if (setreuid(cmd_uid, cmd_uid) < 0) {
			perror("setreuid");
		}
		/* wait here until signaled */
		sigwait(&usrset, &signum);

		if (execl("/bin/sh", "sh", "-c", target_cmd, NULL) < 0)
			perror(target_cmd);
		_exit(-1);
	}
	target_pid = pid;
}

/** 
 * system_cmd() executes system commands in response
 * to an STP_SYSTEM message from the module. These
 * messages are sent by the system() systemtap function.
 * uid and gid are set because staprun is running as root and 
 * it is best to run commands as the real user.
 */
void system_cmd(char *cmd)
{
	pid_t pid;

	dbug ("system %s\n", cmd);
	if ((pid = fork()) < 0) {
		perror ("fork");
	} else if (pid == 0) {
		if (setregid(cmd_gid, cmd_gid) < 0) {
			perror("setregid");
		}
		if (setreuid(cmd_uid, cmd_uid) < 0) {
			perror("setreuid");
		}
		if (execl("/bin/sh", "sh", "-c", cmd, NULL) < 0)
			perror(cmd);
		_exit(-1);
	}
}


/* stp_check script */
#ifdef PKGLIBDIR
char *stp_check=PKGLIBDIR "/stp_check";
#else
char *stp_check="stp_check";
#endif

static int run_stp_check (void)
{
	int ret ;
	/* run the _stp_check script */
	dbug("executing %s\n", stp_check);
	ret = system(stp_check);
	dbug("DONE\n");
	return ret;
}

/**
 *	init_stp - initialize the app
 *	@print_summary: boolean, print summary or not at end of run
 *
 *	Returns 0 on success, negative otherwise.
 */
int init_staprun(void)
{
	char bufcmd[128];
	int rstatus;
	int pid;

	if (system(VERSION_CMD)) {
		dbug("Using OLD TRANSPORT\n");
		use_old_transport = 1;
	}

	if (attach_mod) {
		if (init_ctl_channel() < 0)
			return -1;
		if (use_old_transport) {
			if (init_oldrelayfs() < 0) {
				close_ctl_channel();
				return -1;
			} 
		} else {
			if (init_relayfs() < 0) {
				close_ctl_channel();
				return -1;
			}
		}
		return 0;
	}

	if (run_stp_check() < 0)
		return -1;

	/* insert module */
	sprintf(bufcmd, "_stp_bufsize=%d", buffer_size);
        modoptions[0] = "insmod";
        modoptions[1] = modpath;
        modoptions[2] = bufcmd;
        /* modoptions[3...N] set by command line parser. */

	if ((pid = fork()) < 0) {
		perror ("fork");
		exit(-1);
	} else if (pid == 0) {
		if (execvp("/sbin/insmod",  modoptions) < 0)
			_exit(-1);
	}
	if (waitpid(pid, &rstatus, 0) < 0) {
		perror("waitpid");
		exit(-1);
	}
	if (WIFEXITED(rstatus) && WEXITSTATUS(rstatus)) {
		fprintf(stderr, "ERROR, couldn't insmod probe module %s\n", modpath);
		return -1;
	}
	
	/* create control channel */
	if (init_ctl_channel() < 0) {
		err("Failed to initialize control channel.\n");
		goto exit1;
	}

	/* fork target_cmd if requested. */
	/* It will not actually exec until signalled. */
	if (target_cmd)
		start_cmd();

	return 0;

exit1:
	snprintf(bufcmd, sizeof(bufcmd), "/sbin/rmmod -w %s", modname);
	if (system(bufcmd))
		fprintf(stderr, "ERROR: couldn't rmmod probe module %s.\n", modname);
	return -1;
}



void cleanup_and_exit (int closed)
{
	char tmpbuf[128];
	pid_t err;
	static int exiting = 0;

	signal(SIGINT, SIG_IGN);
	signal(SIGTERM, SIG_IGN);
	signal(SIGHUP, SIG_IGN);
	signal(SIGQUIT, SIG_IGN);

	if (exiting)
		return;
	exiting = 1;

	dbug("CLEANUP AND EXIT  closed=%d\n", closed);

	/* what about child processes? we will wait for them here. */
	err = waitpid(-1, NULL, WNOHANG);
	if (err >= 0)
		fprintf(stderr,"\nWaititing for processes to exit\n");
	while(wait(NULL) > 0) ;

	if (use_old_transport)
		close_oldrelayfs(closed == 2);
	else
		close_relayfs();

	dbug("closing control channel\n");
	close_ctl_channel();

	if (closed == 0) {
		dbug("removing module\n");
		snprintf(tmpbuf, sizeof(tmpbuf), "/sbin/rmmod -w %s", modname);
		if (system(tmpbuf)) {
			fprintf(stderr, "ERROR: couldn't rmmod probe module %s.\n", modname);
			exit(1);
		}
	} else if (closed == 2) {
		fprintf(stderr, "\nDisconnecting from systemtap module.\n");
		fprintf(stderr, "To reconnect, type \"staprun -A %s\"\n", modname); 
	}

	exit(0);
}

static void sigproc(int signum)
{
	dbug("sigproc %d\n", signum);
	if (signum == SIGCHLD) {
		pid_t pid = waitpid(-1, NULL, WNOHANG);
		if (pid != target_pid)
			return;
	} else if (signum == SIGQUIT)
		cleanup_and_exit(2);
		
	send_request(STP_EXIT, NULL, 0);
}

/**
 *	stp_main_loop - loop forever reading data
 */
static char recvbuf[8192];

int stp_main_loop(void)
{
	int nb;
	void *data;
	int type;
	FILE *ofp = stdout;

	setvbuf(ofp, (char *)NULL, _IOLBF, 0);

	signal(SIGINT, sigproc);
	signal(SIGTERM, sigproc);
	signal(SIGHUP, sigproc);
	signal(SIGCHLD, sigproc);
	signal(SIGQUIT, sigproc);

	dbug("in main loop\n");

	while (1) { /* handle messages from control channel */
		nb = read(control_channel, recvbuf, sizeof(recvbuf));
		if (nb <= 0) {
			perror("recv");
			fprintf(stderr, "WARNING: unexpected EOF. nb=%d\n", nb);
			continue;
		}

		type = *(int *)recvbuf;
		data = (void *)(recvbuf + sizeof(int));

		switch (type) { 
#ifdef STP_OLD_TRANSPORT
		case STP_REALTIME_DATA:
			write(out_fd[0], data, nb - sizeof(int));
                        break;
#endif
		case STP_OOB_DATA:
			fputs ((char *)data, stderr);
			break;
		case STP_EXIT: 
		{
			/* module asks us to unload it and exit */
			int *closed = (int *)data;
			dbug("got STP_EXIT, closed=%d\n", *closed);
			cleanup_and_exit(*closed);
			break;
		}
		case STP_START: 
		{
			struct _stp_msg_start *t = (struct _stp_msg_start *)data;
			dbug("probe_start() returned %d\n", t->res);
			if (t->res < 0) {
				if (target_cmd)
					kill (target_pid, SIGKILL);
				cleanup_and_exit(0);
			} else if (target_cmd)
				kill (target_pid, SIGUSR1);
			break;
		}
		case STP_SYSTEM:
		{
			struct _stp_msg_cmd *c = (struct _stp_msg_cmd *)data;
			system_cmd(c->cmd);
			break;
		}
		case STP_TRANSPORT:
		{
			struct _stp_msg_start ts;
			if (use_old_transport) {
				if (init_oldrelayfs() < 0)
					cleanup_and_exit(0);
			} else {
				if (init_relayfs() < 0)
					cleanup_and_exit(0);
			}
			ts.target = target_pid;
			send_request(STP_START, &ts, sizeof(ts));
			if (load_only)
				cleanup_and_exit(2);
			break;
		}
		case STP_MODULE:
		{
			dbug("STP_MODULES request received\n");
			do_module(data);
			break;
		}		
		case STP_SYMBOLS:
		{
			struct _stp_msg_symbol *req = (struct _stp_msg_symbol *)data;
			dbug("STP_SYMBOLS request received\n");
			if (req->endian != 0x1234) {
				fprintf(stderr,"ERROR: staprun is compiled with different endianess than the kernel!\n");
				cleanup_and_exit(0);
			}
			if (req->ptr_size != sizeof(char *)) {
				fprintf(stderr,"ERROR: staprun is compiled with %d-bit pointers and the kernel uses %d-bit.\n",
					8*(int)sizeof(char *), 8*req->ptr_size);
				cleanup_and_exit(0);
			}
			do_kernel_symbols();
			break;
		}
		default:
			fprintf(stderr, "WARNING: ignored message of type %d\n", (type));
		}
	}
	fclose(ofp);
	return 0;
}
