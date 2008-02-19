/*
   librc
   core RC functions
   */

/*
 * Copyright 2007-2008 Roy Marples
 * All rights reserved

 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

const char librc_copyright[] = "Copyright (c) 2007-2008 Roy Marples";

#include "librc.h"
#ifdef __FreeBSD__
#include <sys/sysctl.h>
#endif
#include <signal.h>

#define SOFTLEVEL	RC_SVCDIR "/softlevel"

#ifndef S_IXUGO
# define S_IXUGO (S_IXUSR | S_IXGRP | S_IXOTH)
#endif

/* File stream used for plugins to write environ vars to */
FILE *rc_environ_fd = NULL;

typedef struct rc_service_state_name {
	rc_service_state_t state;
	const char *name;
} rc_service_state_name_t;

/* We MUST list the states below 0x10 first
 * The rest can be in any order */
static const rc_service_state_name_t rc_service_state_names[] = {
	{ RC_SERVICE_STARTED,     "started" },
	{ RC_SERVICE_STOPPED,     "stopped" },
	{ RC_SERVICE_STARTING,    "starting" },
	{ RC_SERVICE_STOPPING,    "stopping" },
	{ RC_SERVICE_INACTIVE,    "inactive" },
	{ RC_SERVICE_WASINACTIVE, "wasinactive" },
	{ RC_SERVICE_COLDPLUGGED, "coldplugged" },
	{ RC_SERVICE_FAILED,      "failed" },
	{ RC_SERVICE_SCHEDULED,   "scheduled"},
	{ 0, NULL}
};

#define LS_INITD	0x01
#define LS_DIR   0x02
static char **ls_dir (const char *dir, int options)
{
	DIR *dp;
	struct dirent *d;
	char **list = NULL;
	struct stat buf;

	if ((dp = opendir (dir)) == NULL)
		return (NULL);

	while (((d = readdir (dp)) != NULL)) {
		if (d->d_name[0] != '.') {
			if (options & LS_INITD) {
				int l = strlen (d->d_name);

				/* Check that our file really exists.
				 * This is important as a service maybe in a runlevel, but
				 * could also have been removed. */
				char *file = rc_strcatpaths (dir, d->d_name, NULL);
				int ok = stat (file, &buf);
				free (file);
				if (ok != 0)
					continue;

				/* .sh files are not init scripts */
				if (l > 2 && d->d_name[l - 3] == '.' &&
				    d->d_name[l - 2] == 's' &&
				    d->d_name[l - 1] == 'h')
					continue;
			}
			if (options & LS_DIR) {
				if (stat (d->d_name, &buf) == 0 && ! S_ISDIR (buf.st_mode))
					continue;
			}
			rc_strlist_addsort (&list, d->d_name);
		}
	}
	closedir (dp);

	return (list);
}

static bool rm_dir (const char *pathname, bool top)
{
	DIR *dp;
	struct dirent *d;

	if ((dp = opendir (pathname)) == NULL)
		return (false);

	errno = 0;
	while (((d = readdir (dp)) != NULL) && errno == 0) {
		if (strcmp (d->d_name, ".") != 0 && strcmp (d->d_name, "..") != 0) {
			char *tmp = rc_strcatpaths (pathname, d->d_name, (char *) NULL);
			if (d->d_type == DT_DIR) {
				if (! rm_dir (tmp, true))
				{
					free (tmp);
					closedir (dp);
					return (false);
				}
			} else {
				if (unlink (tmp)) {
					free (tmp);
					closedir (dp);
					return (false);
				}
			}
			free (tmp);
		}
	}
	closedir (dp);

	if (top && rmdir (pathname) != 0)
		return (false);

	return (true);
}

const char *rc_sys (void)
{
#ifdef __FreeBSD__
	int jailed = 0;
	size_t len = sizeof (jailed);

	if (sysctlbyname ("security.jail.jailed", &jailed, &len, NULL, 0) == 0)
		if (jailed == 1)
			return (RC_SYS_JAIL);
#endif

#ifdef __linux__
	if (exists ("/proc/xen")) {
		if ((fp = fopen ("/proc/xen/capabilities", "r"))) {
			fclose (fp);
			if (file_regex ("/proc/xen/capabilities", "control_d"))
				return (RC_SYS_XEN0);
		}
		if (! sys[0])
			return (RC_SYS_XENU);
	} else if (file_regex ("/proc/cpuinfo", "UML"))
		return (RC_SYS_UML);
	else if (file_regex ("/proc/self/status",
			       "(s_context|VxID|envID):[[:space:]]*[1-9]"))
		return (RC_SYS_VPS);
#endif

	return (NULL);
}

static const char *rc_parse_service_state (rc_service_state_t state)
{
	int i;

	for (i = 0; rc_service_state_names[i].name; i++) {
		if (rc_service_state_names[i].state == state)
			return (rc_service_state_names[i].name);
	}

	return (NULL);
}

bool rc_runlevel_starting (void)
{
	return (exists (RC_STARTING));
}
librc_hidden_def(rc_runlevel_starting)

bool rc_runlevel_stopping (void)
{
	return (exists (RC_STOPPING));
}
librc_hidden_def(rc_runlevel_stopping)

char **rc_runlevel_list (void)
{
	return (ls_dir (RC_RUNLEVELDIR, LS_DIR));
}
librc_hidden_def(rc_runlevel_list)

char *rc_runlevel_get (void)
{
	FILE *fp;
	char *runlevel = NULL;

	if ((fp = fopen (SOFTLEVEL, "r"))) {
		runlevel = xmalloc (sizeof (char) * PATH_MAX);
		if (fgets (runlevel, PATH_MAX, fp)) {
			int i = strlen (runlevel) - 1;
			if (runlevel[i] == '\n')
				runlevel[i] = 0;
		} else
			*runlevel = '\0';
		fclose (fp);
	}

	if (! runlevel || ! *runlevel) {
		free (runlevel);
		runlevel = xstrdup (RC_LEVEL_SYSINIT);
	}

	return (runlevel);
}
librc_hidden_def(rc_runlevel_get)

bool rc_runlevel_set (const char *runlevel)
{
	FILE *fp = fopen (SOFTLEVEL, "w");

	if (! fp)
		return (false);
	fprintf (fp, "%s", runlevel);
	fclose (fp);
	return (true);
}
librc_hidden_def(rc_runlevel_set)

bool rc_runlevel_exists (const char *runlevel)
{
	char *path;
	struct stat buf;
	bool retval = false;

	if (! runlevel)
		return (false);

	path = rc_strcatpaths (RC_RUNLEVELDIR, runlevel, (char *) NULL);
	if (stat (path, &buf) == 0 && S_ISDIR (buf.st_mode))
		retval = true;
	free (path);
	return (retval);
}
librc_hidden_def(rc_runlevel_exists)

	/* Resolve a service name to it's full path */
char *rc_service_resolve (const char *service)
{
	char buffer[PATH_MAX];
	char *file;
	int r = 0;
	struct stat buf;

	if (! service)
		return (NULL);

	if (service[0] == '/')
		return (xstrdup (service));

	file = rc_strcatpaths (RC_SVCDIR, "started", service, (char *) NULL);
	if (lstat (file, &buf) || ! S_ISLNK (buf.st_mode)) {
		free (file);
		file = rc_strcatpaths (RC_SVCDIR, "inactive", service, (char *) NULL);
		if (lstat (file, &buf) || ! S_ISLNK (buf.st_mode)) {
			free (file);
			file = NULL;
		}
	}

	memset (buffer, 0, sizeof (buffer));
	if (file) {
		r = readlink (file, buffer, sizeof (buffer));
		free (file);
		if (r > 0)
			return (xstrdup (buffer));
	}
	snprintf (buffer, sizeof (buffer), RC_INITDIR "/%s", service);

	/* So we don't exist in /etc/init.d - check /usr/local/etc/init.d */
	if (stat (buffer, &buf) != 0) {
		snprintf (buffer, sizeof (buffer), RC_INITDIR_LOCAL "/%s", service);
		if (stat (buffer, &buf) != 0)
			return (NULL);
	}

	return (xstrdup (buffer));
}
librc_hidden_def(rc_service_resolve)

bool rc_service_exists (const char *service)
{
	char *file;
	bool retval = false;
	int len;
	struct stat buf;

	if (! service)
		return (false);

	len = strlen (service);

	/* .sh files are not init scripts */
	if (len > 2 && service[len - 3] == '.' &&
	    service[len - 2] == 's' &&
	    service[len - 1] == 'h')
		return (false);

	if (! (file = rc_service_resolve (service)))
		return (false);

	if (stat (file, &buf) == 0 && buf.st_mode & S_IXUGO)
		retval = true;
	free (file);
	return (retval);
}
librc_hidden_def(rc_service_exists)

#define OPTSTR ". '%s'; echo \"${opts}\""
char **rc_service_extra_commands (const char *service)
{
	char *svc;
	char *cmd = NULL;
	char *buffer = NULL;
	char **commands = NULL;
	char *token;
	char *p;
	FILE *fp;
	size_t l;

	if (! (svc = rc_service_resolve (service)))
		return (NULL);

	l = strlen (OPTSTR) + strlen (svc) + 1;
	cmd = xmalloc (sizeof (char) * l);
	snprintf (cmd, l, OPTSTR, svc);
	free (svc);
	if ((fp = popen (cmd, "r"))) {
		p = buffer = rc_getline (fp);
		while ((token = strsep (&p, " ")))
			rc_strlist_addsort (&commands, token);
		pclose (fp);
		free (buffer);
	}
	free (cmd);
	return (commands);
}
librc_hidden_def(rc_service_extra_commands)

#define DESCSTR ". '%s'; echo \"${description%s%s}\""
char *rc_service_description (const char *service, const char *option)
{
	char *svc;
	char *cmd;
	char *desc = NULL;
	FILE *fp;
	size_t l;

	if (! (svc = rc_service_resolve (service)))
		return (NULL);

	if (! option)
		option = "";

	l = strlen (DESCSTR) + strlen (svc) + strlen (option) + 2;
	cmd = xmalloc (sizeof (char) * l);
	snprintf (cmd, l, DESCSTR, svc, option ? "_" : "", option);
	free (svc);
	if ((fp = popen (cmd, "r"))) {
		desc = rc_getline (fp);
		pclose (fp);
	}
	free (cmd);
	return (desc);
}
librc_hidden_def(rc_service_description)

bool rc_service_in_runlevel (const char *service, const char *runlevel)
{
	char *file;
	bool retval;

	if (! runlevel || ! service)
		return (false);

	file = rc_strcatpaths (RC_RUNLEVELDIR, runlevel, basename_c (service),
			       (char *) NULL);
	retval = exists (file);
	free (file);

	return (retval);
}
librc_hidden_def(rc_service_in_runlevel)

bool rc_service_mark (const char *service, const rc_service_state_t state)
{
	char *file;
	int i = 0;
	int skip_state = -1;
	const char *base;
	char *init = rc_service_resolve (service);
	bool skip_wasinactive = false;

	if (! init)
		return (false);

	base = basename_c (service);

	if (state != RC_SERVICE_STOPPED) {
		if (! exists (init)) {
			free (init);
			return (false);
		}

		file = rc_strcatpaths (RC_SVCDIR, rc_parse_service_state (state), base,
				       (char *) NULL);
		if (exists (file))
			unlink (file);
		i = symlink (init, file);
		if (i != 0)	{
			free (file);
			free (init);
			return (false);
		}

		free (file);
		skip_state = state;
	}

	if (state == RC_SERVICE_COLDPLUGGED || state == RC_SERVICE_FAILED) {
		free (init);
		return (true);
	}

	/* Remove any old states now */
	for (i = 0; rc_service_state_names[i].name; i++) {
		int s = rc_service_state_names[i].state;

		if ((s != skip_state &&
		     s != RC_SERVICE_STOPPED &&
		     s != RC_SERVICE_COLDPLUGGED &&
		     s != RC_SERVICE_SCHEDULED) &&
		    (! skip_wasinactive || s != RC_SERVICE_WASINACTIVE))
		{
			file = rc_strcatpaths (RC_SVCDIR, rc_parse_service_state (s), base,
					       (char *) NULL);
			if (exists (file)) {
				if ((state == RC_SERVICE_STARTING ||
				     state == RC_SERVICE_STOPPING) &&
				    s == RC_SERVICE_INACTIVE)
				{
					char *wasfile = rc_strcatpaths (RC_SVCDIR,
									rc_parse_service_state (RC_SERVICE_WASINACTIVE),
									base, (char *) NULL);

					symlink (init, wasfile);
					skip_wasinactive = true;
					free (wasfile);
				}
				unlink (file);
			}
			free (file);
		}
	}

	/* Remove the exclusive state if we're inactive */
	if (state == RC_SERVICE_STARTED ||
	    state == RC_SERVICE_STOPPED ||
	    state == RC_SERVICE_INACTIVE)
	{
		file = rc_strcatpaths (RC_SVCDIR, "exclusive", base, (char *) NULL);
		unlink (file);
		free (file);
	}

	/* Remove any options and daemons the service may have stored */
	if (state == RC_SERVICE_STOPPED) {
		char *dir = rc_strcatpaths (RC_SVCDIR, "options", base, (char *) NULL);
		rm_dir (dir, true);
		free (dir);

		dir = rc_strcatpaths (RC_SVCDIR, "daemons", base, (char *) NULL);
		rm_dir (dir, true);
		free (dir);

		rc_service_schedule_clear (service);
	}

	/* These are final states, so remove us from scheduled */
	if (state == RC_SERVICE_STARTED || state == RC_SERVICE_STOPPED) {
		char *sdir = rc_strcatpaths (RC_SVCDIR, "scheduled", (char *) NULL);
		char **dirs = ls_dir (sdir, 0);
		char *dir;
		int serrno;

		STRLIST_FOREACH (dirs, dir, i) {
			char *bdir = rc_strcatpaths (sdir, dir, (char *) NULL);
			file = rc_strcatpaths (bdir, base, (char *) NULL);
			unlink (file);
			free (file);

			/* Try and remove the dir - we don't care about errors */
			serrno = errno;
			rmdir (bdir);
			errno = serrno;
			free (bdir);
		}
		rc_strlist_free (dirs);
		free (sdir);
	}

	free (init);
	return (true);
}
librc_hidden_def(rc_service_mark)

rc_service_state_t rc_service_state (const char *service)
{
	int i;
	int state = RC_SERVICE_STOPPED;

	for (i = 0; rc_service_state_names[i].name; i++) {
		char *file = rc_strcatpaths (RC_SVCDIR, rc_service_state_names[i].name,
					     basename_c (service), (char*) NULL);
		if (exists (file)) {
			if (rc_service_state_names[i].state <= 0x10)
				state = rc_service_state_names[i].state;
			else
				state |= rc_service_state_names[i].state;
		}
		free (file);
	}

	if (state & RC_SERVICE_STOPPED) {
		char **services = rc_services_scheduled_by (service);
		if (services) {
			state |= RC_SERVICE_SCHEDULED;
			free (services);
		}
	}

	return (state);
}
librc_hidden_def(rc_service_state)

char *rc_service_value_get (const char *service, const char *option)
{
	FILE *fp;
	char *line = NULL;
	char *file = rc_strcatpaths (RC_SVCDIR, "options", service, option,
				     (char *) NULL);

	if ((fp = fopen (file, "r"))) {
		line = rc_getline (fp);
		fclose (fp);
	}
	free (file);

	return (line);
}
librc_hidden_def(rc_service_value_get)

bool rc_service_value_set (const char *service, const char *option,
			   const char *value)
{
	FILE *fp;
	char *path = rc_strcatpaths (RC_SVCDIR, "options", service, (char *) NULL);
	char *file = rc_strcatpaths (path, option, (char *) NULL);
	bool retval = false;

	if (mkdir (path, 0755) != 0 && errno != EEXIST) {
		free (path);
		free (file);
		return (false);
	}

	if ((fp = fopen (file, "w"))) {
		if (value)
			fprintf (fp, "%s", value);
		fclose (fp);
		retval = true;
	}

	free (path);
	free (file);
	return (retval);
}
librc_hidden_def(rc_service_value_set)

static pid_t _exec_service (const char *service, const char *arg)
{
	char *file;
	char *fifo;
	pid_t pid = -1;
	sigset_t full;
	sigset_t old;
	struct sigaction sa;

	file = rc_service_resolve (service);
	if (! exists (file)) {
		rc_service_mark (service, RC_SERVICE_STOPPED);
		free (file);
		return (0);
	}

	/* We create a fifo so that other services can wait until we complete */
	fifo = rc_strcatpaths (RC_SVCDIR, "exclusive", basename_c (service),
			       (char *) NULL);

	if (mkfifo (fifo, 0600) != 0 && errno != EEXIST) {
		free (fifo);
		free (file);
		return (-1);
	}

	/* We need to block signals until we have forked */
	memset (&sa, 0, sizeof (sa));
	sa.sa_handler = SIG_DFL;
	sigemptyset (&sa.sa_mask);
	sigfillset (&full);
	sigprocmask (SIG_SETMASK, &full, &old);

	if ((pid = fork ()) == 0) {
		/* Restore default handlers */
		sigaction (SIGCHLD, &sa, NULL);
		sigaction (SIGHUP, &sa, NULL);
		sigaction (SIGINT, &sa, NULL);
		sigaction (SIGQUIT, &sa, NULL);
		sigaction (SIGTERM, &sa, NULL);
		sigaction (SIGUSR1, &sa, NULL);
		sigaction (SIGWINCH, &sa, NULL);

		/* Unmask signals */
		sigprocmask (SIG_SETMASK, &old, NULL);

		/* Safe to run now */
		execl (file, file, arg, (char *) NULL);
		fprintf (stderr, "unable to exec `%s': %s\n",
			 file, strerror (errno));
		unlink (fifo);
		_exit (EXIT_FAILURE);
	}

	if (pid == -1)
		fprintf (stderr, "fork: %s\n", strerror (errno));

	sigprocmask (SIG_SETMASK, &old, NULL);

	free (fifo);
	free (file);

	return (pid);
}

pid_t rc_service_stop (const char *service)
{
	rc_service_state_t state = rc_service_state (service);

	if (state & RC_SERVICE_FAILED)
		return (-1);

	if (state & RC_SERVICE_STOPPED)
		return (0);

	return (_exec_service (service, "stop"));
}
librc_hidden_def(rc_service_stop)

pid_t rc_service_start (const char *service)
{
	rc_service_state_t state = rc_service_state (service);

	if (state & RC_SERVICE_FAILED)
		return (-1);

	if (! state & RC_SERVICE_STOPPED)
		return (0);

	return (_exec_service (service, "start"));
}
librc_hidden_def(rc_service_start)

bool rc_service_schedule_start (const char *service,
				const char *service_to_start)
{
	char *dir;
	char *init;
	char *file;
	bool retval;

	/* service may be a provided service, like net */
	if (! service || ! rc_service_exists (service_to_start))
		return (false);

	dir = rc_strcatpaths (RC_SVCDIR, "scheduled", basename_c (service),
			      (char *) NULL);
	if (mkdir (dir, 0755) != 0 && errno != EEXIST) {
		free (dir);
		return (false);
	}

	init = rc_service_resolve (service_to_start);
	file = rc_strcatpaths (dir, basename_c (service_to_start), (char *) NULL);
	retval = (exists (file) || symlink (init, file) == 0);
	free (init);
	free (file);
	free (dir);

	return (retval);
}
librc_hidden_def(rc_service_schedule_start)

bool rc_service_schedule_clear (const char *service)
{
	char *dir = rc_strcatpaths (RC_SVCDIR, "scheduled", basename_c (service),
				    (char *) NULL);
	bool retval;

	if (! (retval = rm_dir (dir, true)) && errno == ENOENT)
		retval = true;
	free (dir);
	return (retval);
}
librc_hidden_def(rc_service_schedule_clear)


char **rc_services_in_runlevel (const char *runlevel)
{
	char *dir;
	char **list = NULL;

	if (! runlevel) {
		int i;
		char **local = ls_dir (RC_INITDIR_LOCAL, LS_INITD);

		list = ls_dir (RC_INITDIR, LS_INITD);
		STRLIST_FOREACH (local, dir, i)
			rc_strlist_addsortu (&list, dir);
		rc_strlist_free (local);
		return (list);
	}

	/* These special levels never contain any services */
	if (strcmp (runlevel, RC_LEVEL_SYSINIT) == 0 ||
	    strcmp (runlevel, RC_LEVEL_SINGLE) == 0)
		return (NULL);

	dir = rc_strcatpaths (RC_RUNLEVELDIR, runlevel, (char *) NULL);
	list = ls_dir (dir, LS_INITD);
	free (dir);
	return (list);
}
librc_hidden_def(rc_services_in_runlevel)

char **rc_services_in_state (rc_service_state_t state)
{
	char *dir = rc_strcatpaths (RC_SVCDIR, rc_parse_service_state (state),
				    (char *) NULL);
	char **list = NULL;

	if (state == RC_SERVICE_SCHEDULED) {
		char **dirs = ls_dir (dir, 0);
		char *d;
		int i;

		STRLIST_FOREACH (dirs, d, i) {
			char *p = rc_strcatpaths (dir, d, (char *) NULL);
			char **entries = ls_dir (p, LS_INITD);
			char *e;
			int j;

			STRLIST_FOREACH (entries, e, j)
				rc_strlist_addsortu (&list, e);

			if (entries)
				free (entries);
		}

		if (dirs)
			free (dirs);
	} else {
		list = ls_dir (dir, LS_INITD);
	}

	free (dir);
	return (list);
}
librc_hidden_def(rc_services_in_state)

bool rc_service_add (const char *runlevel, const char *service)
{
	bool retval;
	char *init;
	char *file;

	if (! rc_runlevel_exists (runlevel)) {
		errno = ENOENT;
		return (false);
	}

	if (rc_service_in_runlevel (service, runlevel)) {
		errno = EEXIST;
		return (false);
	}

	init = rc_service_resolve (service);

	/* We need to ensure that only things in /etc/init.d are added
	 * to the boot runlevel */
	if (strcmp (runlevel, RC_LEVEL_BOOT) == 0) {
		char tmp[MAXPATHLEN] = { '\0' };
		char *p;
		
		p = realpath (dirname (init), tmp);
		free (init);
		if (! *p)
			return (false);

		retval = (strcmp (tmp, RC_INITDIR) == 0);
		if (! retval) {
			errno = EPERM;
			return (false);
		}
		init = rc_strcatpaths (RC_INITDIR, service, (char *) NULL); 
	}

	file = rc_strcatpaths (RC_RUNLEVELDIR, runlevel, basename_c (service),
			       (char *) NULL);
	retval = (symlink (init, file) == 0);
	free (init);
	free (file);
	return (retval);
}
librc_hidden_def(rc_service_add)

bool rc_service_delete (const char *runlevel, const char *service)
{
	char *file;
	bool retval = false;

	if (! runlevel || ! service)
		return (false);

	file = rc_strcatpaths (RC_RUNLEVELDIR, runlevel, basename_c (service),
			       (char *) NULL);
	if (unlink (file) == 0)
		retval = true;

	free (file);
	return (retval);
}
librc_hidden_def(rc_service_delete)

char **rc_services_scheduled_by (const char *service)
{
	char **dirs = ls_dir (RC_SVCDIR "/scheduled", 0);
	char **list = NULL;
	char *dir;
	int i;

	STRLIST_FOREACH (dirs, dir, i) {
		char *file = rc_strcatpaths (RC_SVCDIR, "scheduled", dir, service,
					     (char *) NULL);
		if (exists (file))
			rc_strlist_add (&list, file);
		free (file);
	}
	rc_strlist_free (dirs);

	return (list);
}
librc_hidden_def(rc_services_scheduled_by)

char **rc_services_scheduled (const char *service)
{
	char *dir = rc_strcatpaths (RC_SVCDIR, "scheduled", basename_c (service),
				    (char *) NULL);
	char **list = NULL;

	list = ls_dir (dir, LS_INITD);
	free (dir);
	return (list);
}
librc_hidden_def(rc_services_scheduled)
