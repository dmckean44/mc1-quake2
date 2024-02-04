/*
 * Copyright (C) 1997-2001 Id Software, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * =======================================================================
 *
 * This file implements all system dependent generic functions.
 *
 * =======================================================================
 */

#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/select.h> /* for fd_set */
#ifndef FNDELAY
#define FNDELAY O_NDELAY
#endif

#include "../../common/header/common.h"
#include "../../common/header/glob.h"

extern void *GetGameAPI(void *);

// Evil hack to determine if stdin is available
qboolean stdin_active = true;

// Config dir
char cfgdir[MAX_OSPATH] = CFGDIR;

// Console logfile
extern FILE	*logfile;

/* ================================================================ */

void
Sys_Error(const char *error, ...)
{
	va_list argptr;
	char string[1024];

	/* change stdin to non blocking */
	fcntl(0, F_SETFL, fcntl(0, F_GETFL, 0) & ~FNDELAY);

#ifndef DEDICATED_ONLY
	CL_Shutdown();
#endif
	Qcommon_Shutdown();

	va_start(argptr, error);
	vsnprintf(string, 1024, error, argptr);
	va_end(argptr);
	fprintf(stderr, "Error: %s\n", string);

	exit(1);
}

void
Sys_Quit(void)
{
#ifndef DEDICATED_ONLY
	CL_Shutdown();
#endif

	if (logfile)
	{
		fclose(logfile);
		logfile = NULL;
	}

	Qcommon_Shutdown();
	fcntl(0, F_SETFL, fcntl(0, F_GETFL, 0) & ~FNDELAY);

	printf("------------------------------------\n");

	exit(0);
}

void
Sys_Init(void)
{
	Sys_ConsoleOutput("Terminal has no colors support.\n");
}

/* ================================================================ */

char *
Sys_ConsoleInput(void)
{
	static char text[256];
	int len;

	if (!dedicated || !dedicated->value)
	{
		return NULL;
	}

	if (!stdin_active)
	{
		return NULL;
	}

	len = read(0, text, sizeof(text));

	if (len == 0)   /* eof! */
	{
		stdin_active = false;
		return NULL;
	}

	if (len < 1)
	{
		return NULL;
	}

	text[len - 1] = 0; /* rip off the /n and terminate */

	return text;
}

void
Sys_ConsoleOutput(char *string)
{
	fputs(string, stdout);
}

/* ================================================================ */

long long
Sys_Microseconds(void)
{
	static struct timeval s_t0;
	static int s_initialized;
	struct timeval t;

	// Get current time of day.
	if (gettimeofday(&t, NULL) == -1) {
		Sys_Error("gettimeofday failed");
	}
	if (!s_initialized) {
		s_t0 = t;
		s_initialized = 1;
	}

	// Calculate delta to start of application.
	time_t secs = t.tv_sec - s_t0.tv_sec;
	suseconds_t usecs = t.tv_usec - s_t0.tv_usec;
	if (usecs < 0L) {
		usecs += 1000000L;
		secs -= 1L;
	}

	// Convert to microseconds.
	return (1000000LL * (long long)(secs)) + (long long)usecs;
}

int
Sys_Milliseconds(void)
{
	return (int)(Sys_Microseconds()/1000ll);
}

void
Sys_Nanosleep(int nanosec)
{
	if (nanosec <= 1000) {
		return;
	}
	int microsec = nanosec / 1000;
	long long sleep_until = Sys_Microseconds() + (long long)microsec;
	while (Sys_Microseconds() < sleep_until)
		;
}

/* ================================================================ */

/* The musthave and canhave arguments are unused in MC1Q2. We
   can't remove them since Sys_FindFirst() and Sys_FindNext()
   are defined in shared.h and may be used in custom game DLLs. */

static char findbase[MAX_OSPATH];
static char findpath[MAX_OSPATH];
static char findpattern[MAX_OSPATH];
static DIR *fdir;

char *
Sys_FindFirst(char *path, unsigned musthave, unsigned canhave)
{
	struct dirent *d;
	char *p;

	if (fdir)
	{
		Sys_Error("Sys_BeginFind without close");
	}

	strcpy(findbase, path);

	if ((p = strrchr(findbase, '/')) != NULL)
	{
		*p = 0;
		strcpy(findpattern, p + 1);
	}
	else
	{
		strcpy(findpattern, "*");
	}

	if (strcmp(findpattern, "*.*") == 0)
	{
		strcpy(findpattern, "*");
	}

	if ((fdir = opendir(findbase)) == NULL)
	{
		return NULL;
	}

	while ((d = readdir(fdir)) != NULL)
	{
		if (!*findpattern || glob_match(findpattern, d->d_name))
		{
			if ((strcmp(d->d_name, ".") != 0) || (strcmp(d->d_name, "..") != 0))
			{
				snprintf(findpath, sizeof(findpath), "%s/%s", findbase, d->d_name);
				return findpath;
			}
		}
	}

	return NULL;
}

char *
Sys_FindNext(unsigned musthave, unsigned canhave)
{
	struct dirent *d;

	if (fdir == NULL)
	{
		return NULL;
	}

	while ((d = readdir(fdir)) != NULL)
	{
		if (!*findpattern || glob_match(findpattern, d->d_name))
		{
			if ((strcmp(d->d_name, ".") != 0) || (strcmp(d->d_name, "..") != 0))
			{
				snprintf(findpath, sizeof(findpath), "%s/%s", findbase, d->d_name);
				return findpath;
			}
		}
	}

	return NULL;
}

void
Sys_FindClose(void)
{
	if (fdir != NULL)
	{
		closedir(fdir);
	}

	fdir = NULL;
}

/* ================================================================ */

void
Sys_UnloadGame(void)
{
}

void *
Sys_GetGameAPI(void *parms)
{
	return GetGameAPI(parms);
}

/* ================================================================ */

void
Sys_Mkdir(const char *path)
{
	if (!Sys_IsDir(path))
	{
		if (mkdir(path, 0755) != 0)
		{
			Com_Error(ERR_FATAL, "Couldn't create dir %s\n", path);
		}
	}
}

qboolean
Sys_IsDir(const char *path)
{
	struct stat sb;

	if (stat(path, &sb) != -1)
	{
		if (S_ISDIR(sb.st_mode))
		{
			return true;
		}
	}

	return false;
}

qboolean
Sys_IsFile(const char *path)
{
	struct stat sb;

	if (stat(path, &sb) != -1)
	{
		if (S_ISREG(sb.st_mode))
		{
			return true;
		}
	}

	return false;
}

char *
Sys_GetHomeDir(void)
{
	static char gdir[MAX_OSPATH];
	snprintf(gdir, sizeof(gdir), "./%s/", cfgdir);
	Sys_Mkdir(gdir);
	return gdir;
}

void
Sys_Remove(const char *path)
{
	remove(path);
}

int
Sys_Rename(const char *from, const char *to)
{
	return rename(from, to);
}

void
Sys_RemoveDir(const char *path)
{
	char filepath[MAX_OSPATH];
	struct dirent *file;
	DIR *directory;

	if (Sys_IsDir(path))
	{
		directory = opendir(path);
		if (directory)
		{
			while ((file = readdir(directory)) != NULL)
			{
				snprintf(filepath, MAX_OSPATH, "%s/%s", path, file->d_name);
				Sys_Remove(filepath);
			}

			closedir(directory);
			Sys_Remove(path);
		}
	}
}

qboolean
Sys_Realpath(const char *in, char *out, size_t size)
{
	/* TODO(m): Implement me! */
	Q_strlcpy(out, in, size);
	return true;
}

/* ================================================================ */

void *
Sys_GetProcAddress(void *handle, const char *sym)
{
	(void)handle;
	(void)sym;
	return NULL;
}

void
Sys_FreeLibrary(void *handle)
{
	(void)handle;
}

void *
Sys_LoadLibrary(const char *path, const char *sym, void **handle)
{
	(void)path;
	(void)sym;
	(void)handle;
	return NULL;
}

/* ================================================================ */

void
Sys_GetWorkDir(char *buffer, size_t len)
{
	buffer[0] = '\0';
}

qboolean
Sys_SetWorkDir(char *path)
{
	return false;
}
