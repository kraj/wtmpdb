/* SPDX-License-Identifier: BSD-2-Clause

  Copyright (c) 2023, Thorsten Kukuk <kukuk@suse.com>

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice,
     this list of conditions and the following disclaimer.

  2. Redistributions in binary form must reproduce the above copyright
     notice, this list of conditions and the following disclaimer in the
     documentation and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
  POSSIBILITY OF SUCH DAMAGE.
*/

#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <getopt.h>
#include <sys/utsname.h>

#include "wtmpdb.h"

static char *wtmpdb_path = _PATH_WTMPDB;

#define TIMEFMT_CTIME 1
#define TIMEFMT_SHORT 2
#define TIMEFMT_HHMM  3

static int64_t wtmp_start = INT64_MAX;
static int after_reboot = 0;

static void
format_time (int fmt, char *dst, size_t dstlen, time_t t)
{
  switch (fmt)
    {
    case TIMEFMT_CTIME:
      snprintf (dst, dstlen, "%s", ctime (&t));
      dst[strlen (dst)-1] = '\0'; /* Remove trailing '\n' */
      break;
    case TIMEFMT_SHORT:
      {
	struct tm *tm = localtime (&t);
	strftime (dst, dstlen, "%a %b %e %H:%M", tm);
	break;
      }
    case TIMEFMT_HHMM:
      {
	struct tm *tm = localtime (&t);
	strftime (dst, dstlen, "%H:%M", tm);
	break;
      }
    default:
      abort ();
    }
}

static int
print_entry (void *unused __attribute__((__unused__)),
	     int argc, char **argv, char **azColName)
{
  const int name_len = 8; /* LAST_LOGIN_LEN */
  const int login_len = 16; /* 16 = short, 24 = full */
  const int logout_len = 5; /* 5 = short, 24 = full */
  const int host_len = 16; /* LAST_DOMAIN_LEN */
  char logintime[32]; /* LAST_TIMESTAMP_LEN */
  char logouttime[32]; /* LAST_TIMESTAMP_LEN */
  char length[32]; /* LAST_TIMESTAMP_LEN */
  char *line;
  char *endptr;

  /* ID, Type, User, LoginTime, LogoutTime, TTY, RemoteHost, Service */
  if (argc != 8)
    {
      fprintf (stderr, "Mangled entry:");
      for (int i = 0; i < argc; i++)
        fprintf (stderr, " %s=%s", azColName[i], argv[i] ? argv[i] : "NULL");
      fprintf (stderr, "\n");
      exit (EXIT_FAILURE);
    }

  const int type = atoi (argv[1]);
  const char *user = argv[2];
  const char *tty = argv[5];
  const char *host = argv[6]?argv[6]:"";

  int64_t login_t = strtoll(argv[3], &endptr, 10);
  if ((errno == ERANGE && (login_t == INT64_MAX || login_t == INT64_MIN))
      || (endptr == argv[1]) || (*endptr != '\0'))
    fprintf (stderr, "Invalid numeric time entry for 'login': '%s'\n",
	     argv[3]);
  format_time (TIMEFMT_SHORT, logintime, sizeof (logintime),
	       login_t/USEC_PER_SEC);

  if (argv[4])
    {
      int64_t logout_t = strtoll(argv[4], &endptr, 10);
      if ((errno == ERANGE && (logout_t == INT64_MAX || logout_t == INT64_MIN))
	  || (endptr == argv[1]) || (*endptr != '\0'))
	fprintf (stderr, "Invalid numeric time entry for 'logout': '%s'\n",
		 argv[4]);
      format_time (TIMEFMT_HHMM, logouttime, sizeof (logouttime),
		   logout_t/USEC_PER_SEC);

      int64_t secs = (logout_t - login_t)/USEC_PER_SEC;
      int mins  = (secs / 60) % 60;
      int hours = (secs / 3600) % 24;
      int days  = secs / 86400;

      if (days)
	snprintf (length, sizeof(length), "(%d+%02d:%02d)", days, hours, mins);
      else if (hours)
	snprintf (length, sizeof(length), " (%02d:%02d)", hours, mins);
      else
	snprintf (length, sizeof(length), " (00:%02d)", mins);
    }
  else /* login but no logout */
    {
      if (after_reboot)
	{
	  snprintf (logouttime, sizeof (logouttime), "crash");
	  length[0] = '\0';
	}
      else
	{
	  switch (type)
	    {
	    case USER_PROCESS:
	      snprintf (logouttime, sizeof (logouttime), "still");
	      snprintf(length, sizeof(length), "logged in");
	      break;
	    case BOOT_TIME:
	      snprintf (logouttime, sizeof (logouttime), "still");
	      snprintf(length, sizeof(length), "running");
	      break;
	    default:
	      snprintf (logouttime, sizeof (logouttime), "ERROR");
	      snprintf(length, sizeof(length), "Unknown: %d", type);
	      break;
	    }
	}
    }

  if (type == BOOT_TIME)
    {
      tty = "system boot";
      after_reboot = 1;
    }

  if (asprintf (&line, "%-8.*s %-12.12s %-16.*s %-*.*s - %-*.*s %s\n",
		name_len, user, tty,
		host_len, host,
		login_len, login_len, logintime,
		logout_len, logout_len, logouttime,
		length) < 0)
    {
      fprintf (stderr, "Out f memory");
      exit (EXIT_FAILURE);
    }

  printf ("%s", line);
  free (line);

  if (login_t < wtmp_start)
    wtmp_start = login_t;

  return 0;
}

static void
usage (int retval)
{
  FILE *output = (retval != EXIT_SUCCESS) ? stderr : stdout;

  fprintf (output, "Usage: wtmpdb [command] [options]\n");
  fputs ("Commands: last, boot, shutdown\n\n", output);
  fputs ("Options for last:\n", output);
  fputs ("  -d, --database FILE   Use FILE as wtmpdb database\n", output);
  fputs ("\n", output);
  fputs ("Options for boot (writes boot entry to wtmpdb):\n", output);
  fputs ("  -d, --database FILE   Use FILE as wtmpdb database\n", output);
  fputs ("\n", output);
  fputs ("Options for shutdown (writes shutdown time to wtmpdb):\n", output);
  fputs ("  -d, --database FILE   Use FILE as wtmpdb database\n", output);
  fputs ("\n", output);
  fputs ("Generic options:\n", output);
  fputs ("  -h, --help            Display this help message and exit\n", output);
  fputs ("  -v, --version         Print version number and exit\n", output);
  fputs ("\n", output);
  exit (retval);
}

static int
main_last (int argc, char **argv)
{
  struct option const longopts[] = {
    {"database", required_argument, NULL, 'd'},
    {NULL, 0, NULL, '\0'}
  };
  char *error = NULL;
  int c;

  while ((c = getopt_long (argc, argv, "d:", longopts, NULL)) != -1)
    {
      switch (c)
        {
        case 'd':
          wtmpdb_path = optarg;
          break;
        default:
          usage (EXIT_FAILURE);
          break;
        }
    }

  if (argc > optind)
    {
      fprintf (stderr, "Unexpected argument: %s\n", argv[optind]);
      usage (EXIT_FAILURE);
    }

  if (wtmpdb_read_all (wtmpdb_path, print_entry, &error) != 0)
    {
      if (error)
        {
          fprintf (stderr, "%s\n", error);
          free (error);
        }
      else
        fprintf (stderr, "Couldn't read all wtmp entries\n");

      exit (EXIT_FAILURE);
    }

  char wtmptime[32];
  format_time (TIMEFMT_CTIME, wtmptime, sizeof (wtmptime),
	       wtmp_start/USEC_PER_SEC);
  printf ("\n%s begins %s\n", wtmpdb_path, wtmptime);

  return EXIT_SUCCESS;
}

static int
main_boot (int argc, char **argv)
{
  struct option const longopts[] = {
    {"database", required_argument, NULL, 'd'},
    {NULL, 0, NULL, '\0'}
  };
  char *error = NULL;
  int c;

  while ((c = getopt_long (argc, argv, "d:", longopts, NULL)) != -1)
    {
      switch (c)
        {
        case 'd':
          wtmpdb_path = optarg;
          break;
        default:
          usage (EXIT_FAILURE);
          break;
        }
    }

  if (argc > optind)
    {
      fprintf (stderr, "Unexpected argument: %s\n", argv[optind]);
      usage (EXIT_FAILURE);
    }

  struct utsname uts;
  uname(&uts);

  struct timespec ts;
  clock_gettime (CLOCK_REALTIME, &ts);
  int64_t time = wtmpdb_timespec2usec (ts);

  if (wtmpdb_login (wtmpdb_path, BOOT_TIME, "reboot", time, "~", uts.release,
		    NULL, &error) < 0)
    {
      if (error)
        {
          fprintf (stderr, "%s\n", error);
          free (error);
        }
      else
        fprintf (stderr, "Couldn't write boot entry\n");

      exit (EXIT_FAILURE);
    }

  return EXIT_SUCCESS;
}

static int
main_shutdown (int argc, char **argv)
{
  struct option const longopts[] = {
    {"database", required_argument, NULL, 'd'},
    {NULL, 0, NULL, '\0'}
  };
  char *error = NULL;
  int c;

  while ((c = getopt_long (argc, argv, "d:", longopts, NULL)) != -1)
    {
      switch (c)
        {
        case 'd':
          wtmpdb_path = optarg;
          break;
        default:
          usage (EXIT_FAILURE);
          break;
        }
    }

  if (argc > optind)
    {
      fprintf (stderr, "Unexpected argument: %s\n", argv[optind]);
      usage (EXIT_FAILURE);
    }

  int64_t id = wtmpdb_get_id (wtmpdb_path, "~", &error);
  if (id < 0)
    {
      if (error)
        {
          fprintf (stderr, "%s\n", error);
          free (error);
        }
      else
        fprintf (stderr, "Couldn't get ID for reboot entry\n");

      exit (EXIT_FAILURE);
    }

  struct timespec ts;
  clock_gettime (CLOCK_REALTIME, &ts);
  int64_t time = wtmpdb_timespec2usec (ts);

  if (wtmpdb_logout (wtmpdb_path, id, time, &error) < 0)
    {
      if (error)
        {
          fprintf (stderr, "%s\n", error);
          free (error);
        }
      else
        fprintf (stderr, "Couldn't write shutdown entry\n");

      exit (EXIT_FAILURE);
    }

  return EXIT_SUCCESS;
}

int
main (int argc, char **argv)
{
  struct option const longopts[] = {
    {"help",     no_argument,       NULL, 'h'},
    {"version",  no_argument,       NULL, 'v'},
    {NULL, 0, NULL, '\0'}
  };
  int c;

  if (argc == 1)
    usage (EXIT_SUCCESS);
  else if (strcmp (argv[1], "last") == 0)
    return main_last (--argc, ++argv);
  else if (strcmp (argv[1], "reboot") == 0)
    return main_boot (--argc, ++argv);
  else if (strcmp (argv[1], "shutdown") == 0)
    return main_shutdown (--argc, ++argv);

  while ((c = getopt_long (argc, argv, "hv", longopts, NULL)) != -1)
    {
      switch (c)
	{
	case 'h':
	  usage (EXIT_SUCCESS);
	  break;
	case 'v':
	  printf ("wtmpdb %s\n", PROJECT_VERSION);
	  break;
	default:
	  usage (EXIT_FAILURE);
	  break;
	}
    }

  if (argc > optind)
    {
      fprintf (stderr, "Unexpected argument: %s\n", argv[optind]);
      usage (EXIT_FAILURE);
    }

  exit (EXIT_SUCCESS);
}