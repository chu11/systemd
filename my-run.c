/* SPDX-License-Identifier: LGPL-2.1-or-later */

#define _GNU_SOURCE
#include <unistd.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include <systemd/sd-bus.h>

static bool arg_remain_after_exit = false;
static bool arg_no_block = false;
static const char *arg_unit = NULL;
static const char *arg_description = NULL;
static const char *arg_exec_user = NULL;
static const char *arg_exec_group = NULL;
static char **arg_environment = NULL;
static enum {
        ARG_STDIO_NONE,      /* The default, as it is for normal services, stdin connected to /dev/null, and stdout+stderr to the journal */
        ARG_STDIO_PTY,       /* Interactive behaviour, requested by --pty: we allocate a pty and connect it to the TTY we are invoked from */
        ARG_STDIO_DIRECT,    /* Directly pass our stdin/stdout/stderr to the activated service, useful for usage in shell pipelines, requested by --pipe */
        ARG_STDIO_AUTO,      /* If --pipe and --pty are used together we use --pty when invoked on a TTY, and --pipe otherwise */
} arg_stdio = ARG_STDIO_NONE;
static bool arg_aggressive_gc = false;
static char *arg_working_directory = NULL;
static char **arg_cmdline = NULL;

static int help(void) {
        printf("my-run [OPTIONS...] COMMAND [ARGUMENTS...]\n"
               "\nRun the specified command in a transient scope or service.\n\n"
               "  -h --help                       Show this help\n"
               "     --version                    Show package version\n"
               "  -u --unit=UNIT                  Run under the specified unit name\n"
               "     --description=TEXT           Description for unit\n"
               "     --no-block                   Do not wait until operation finished\n"
               "  -r --remain-after-exit          Leave service around until explicitly stopped\n"
               "     --wait                       Wait until service stopped again\n"
               "     --service-type=TYPE          Service type\n"
               "     --uid=USER                   Run as system user\n"
               "     --gid=GROUP                  Run as system group\n"
               "     --working-directory=PATH     Set working directory\n"
               "  -d --same-dir                   Inherit working directory from caller\n"
               "  -G --collect                    Unload unit after it ran, even when failed\n");

        exit (1);
}

static int parse_argv(int argc, char *argv[]) {

        enum {
                ARG_VERSION = 0x100,
                ARG_USER,
                ARG_SYSTEM,
                ARG_DESCRIPTION,
                ARG_EXEC_USER,
                ARG_EXEC_GROUP,
                ARG_NO_BLOCK,
                ARG_WORKING_DIRECTORY,
        };

        static const struct option options[] = {
                { "help",              no_argument,       NULL, 'h'                   },
                { "version",           no_argument,       NULL, ARG_VERSION           },
                { "unit",              required_argument, NULL, 'u'                   },
                { "description",       required_argument, NULL, ARG_DESCRIPTION       },
                { "remain-after-exit", no_argument,       NULL, 'r'                   },
                { "uid",               required_argument, NULL, ARG_EXEC_USER         },
                { "gid",               required_argument, NULL, ARG_EXEC_GROUP        },
                { "no-block",          no_argument,       NULL, ARG_NO_BLOCK          },
                { "collect",           no_argument,       NULL, 'G'                   },
                { "working-directory", required_argument, NULL, ARG_WORKING_DIRECTORY },
                {},
        };

        int r, c;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "+hrH:M:E:p:tPqGdSu:", options, NULL)) >= 0)

                switch (c) {

                case 'h':
                        return help();

                case ARG_VERSION:
                        fprintf (stderr, "version foo\n");
                        return 0;

                case 'u':
                        arg_unit = optarg;
                        break;

                case ARG_DESCRIPTION:
                        arg_description = optarg;
                        break;

                case 'r':
                        arg_remain_after_exit = true;
                        break;

                case ARG_EXEC_USER:
                        arg_exec_user = optarg;
                        break;

                case ARG_EXEC_GROUP:
                        arg_exec_group = optarg;
                        break;

                case ARG_NO_BLOCK:
                        arg_no_block = true;
                        break;

                case ARG_WORKING_DIRECTORY:
                        /* assume user input absolute path for working dir */
                        if (!(arg_working_directory = strdup (optarg))) {
                                perror ("strdup");
                                return -EINVAL;
                        }
                        break;

                case 'G':
                        arg_aggressive_gc = true;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        /* NOT REACHED */
                        return -EINVAL;
                }

        if (!arg_unit) {
                fprintf (stderr, "--unit required\n");
                return -1;
        }

        if (argc > optind) {
                char **ptr;
                ptr = arg_cmdline = calloc (1, sizeof (char *) * ((argc - optind) + 1));
                if (!arg_cmdline) {
                        perror ("calloc");
                        return -1;
                }
                while (optind < argc) {
                        if (!(*ptr = strdup (argv[optind]))) {
                                perror ("strdup");
                                return -1;
                        }
                        optind++;
                        ptr++;
                }
                *ptr = NULL;
        }
        else {
                fprintf (stderr, "need a command\n");
                return -1;
        }

        if (arg_stdio != ARG_STDIO_NONE && arg_no_block) {
                fprintf (stderr, "--pipe is not compatible with --no-block.");
                return -EINVAL;
        }

        return 0;
}

static int transient_service_set_properties(sd_bus_message *m) {
        bool send_term = false;
        int r;

        assert(m);

        r = sd_bus_message_append(m, "(sv)", "Description", "s", arg_description);
        if (r < 0) {
                perror ("Failed to append bus message");
                return -1;
        }

        if (arg_aggressive_gc) {
                r = sd_bus_message_append(m, "(sv)", "CollectMode", "s", "inactive-or-failed");
                if (r < 0) {
                        perror ("Failed to append bus message");
                        return -1;
                }
        }

        /* do not assume any property assignments for time being, a lot of parsing and a lot of logic to
         * setup */

        if (arg_stdio != ARG_STDIO_NONE) {
                r = sd_bus_message_append(m, "(sv)", "AddRef", "b", 1);
                if (r < 0) {
                        perror ("Failed to append bus message");
                        return -1;
                }
        }

        if (arg_remain_after_exit) {
                r = sd_bus_message_append(m, "(sv)", "RemainAfterExit", "b", arg_remain_after_exit);
                if (r < 0) {
                        perror ("Failed to append bus message");
                        return -1;
                }
        }

        /* if (arg_service_type) { */
        /*         r = sd_bus_message_append(m, "(sv)", "Type", "s", arg_service_type); */
        /*         if (r < 0) { */
        /*                 perror ("Failed to append bus message"); */
        /*                 return -1; */
        /*         } */
        /* } */

        if (arg_exec_user) {
                r = sd_bus_message_append(m, "(sv)", "User", "s", arg_exec_user);
                if (r < 0) {
                        perror ("Failed to append bus message");
                        return -1;
                }
        }

        if (arg_exec_group) {
                r = sd_bus_message_append(m, "(sv)", "Group", "s", arg_exec_group);
                if (r < 0) {
                        perror ("Failed to append bus message");
                        return -1;
                }
        }

        if (arg_working_directory) {
                r = sd_bus_message_append(m, "(sv)", "WorkingDirectory", "s", arg_working_directory);
                if (r < 0) {
                        perror ("Failed to append bus message");
                        return -1;
                }
        }

        /* assume no extra environment for now, gotta deal with this parsing and data structure */
        /* if (!strv_isempty(arg_environment)) { */
        /*         r = sd_bus_message_open_container(m, 'r', "sv"); */
        /*         if (r < 0) { */
        /*                 perror ("Failed to open container"); */
        /*                 goto cleanup; */
        /*         } */

        /*         r = sd_bus_message_append(m, "s", "Environment"); */
        /*         if (r < 0) { */
        /*                 perror ("Failed to append bus message"); */
        /*                 return -1; */
        /*         } */

        /*         r = sd_bus_message_open_container(m, 'v', "as"); */
        /*         if (r < 0) { */
        /*                 perror ("Failed to open container"); */
        /*                 goto cleanup; */
        /*         } */

        /*         r = sd_bus_message_append_strv(m, arg_environment); */
        /*         if (r < 0) { */
        /*                 perror ("Failed to append bus message strv"); */
        /*                 return -1; */
        /*         } */

        /*         r = sd_bus_message_close_container(m); */
        /*         if (r < 0) { */
        /*                 perror ("Failed to close container"); */
        /*                 goto cleanup; */
        /*         } */

        /*         r = sd_bus_message_close_container(m); */
        /*         if (r < 0) { */
        /*                 perror ("Failed to close container"); */
        /*                 goto cleanup; */
        /*         } */
        /* } */

        /* Exec container */
        if (arg_cmdline) {
                r = sd_bus_message_open_container(m, 'r', "sv");
                if (r < 0) {
                        perror ("Failed to open container");
                        goto cleanup;
                }

                r = sd_bus_message_append(m, "s", "ExecStart");
                if (r < 0) {
                        perror ("Failed to append bus message");
                        return -1;
                }

                r = sd_bus_message_open_container(m, 'v', "a(sasb)");
                if (r < 0) {
                        perror ("Failed to open container");
                        goto cleanup;
                }

                r = sd_bus_message_open_container(m, 'a', "(sasb)");
                if (r < 0) {
                        perror ("Failed to open container");
                        goto cleanup;
                }

                r = sd_bus_message_open_container(m, 'r', "sasb");
                if (r < 0) {
                        perror ("Failed to open container");
                        goto cleanup;
                }

                r = sd_bus_message_append(m, "s", arg_cmdline[0]);
                if (r < 0) {
                        perror ("Failed to append bus message");
                        return -1;
                }

                r = sd_bus_message_append_strv(m, arg_cmdline);
                if (r < 0) {
                        perror ("Failed to append bus message strv");
                        return -1;
                }

                r = sd_bus_message_append(m, "b", false);
                if (r < 0) {
                        perror ("Failed to append bus message");
                        return -1;
                }

                r = sd_bus_message_close_container(m);
                if (r < 0) {
                        perror ("Failed to close container");
                        goto cleanup;
                }

                r = sd_bus_message_close_container(m);
                if (r < 0) {
                        perror ("Failed to close container");
                        goto cleanup;
                }

                r = sd_bus_message_close_container(m);
                if (r < 0) {
                        perror ("Failed to close container");
                        goto cleanup;
                }

                r = sd_bus_message_close_container(m);
                if (r < 0) {
                        perror ("Failed to close container");
                        goto cleanup;
                }
        }

cleanup:
        return 0;
}

static int start_transient_service(
                sd_bus *bus) {

        sd_bus_message *m = NULL, *reply = NULL;
        sd_bus_error error = SD_BUS_ERROR_NULL;
        char *service = NULL;
        int r;

        assert(bus);

        if (arg_unit) {
                /* assume user input a single word, so just add .service */
                if (asprintf (&service, "%s.service", arg_unit) < 0) {
                        perror ("asprintf");
                        goto cleanup;
                }
                printf ("unit name service is %s\n", service);
        }

        r = sd_bus_message_new_method_call(bus,
                                           &m,
                                           "org.freedesktop.systemd1",
                                           "/org/freedesktop/systemd1",
                                           "org.freedesktop.systemd1.Manager",
                                           "StartTransientUnit");
        if (r < 0) {
                perror ("Failed to call method");
                goto cleanup;
        }

        /* Name and mode */
        r = sd_bus_message_append(m, "ss", service, "fail");
        if (r < 0) {
                perror ("Failed to append bus message");
                goto cleanup;
        }

        /* Properties */
        r = sd_bus_message_open_container(m, 'a', "(sv)");
        if (r < 0) {
                perror ("Failed to open container");
                goto cleanup;
        }

        r = transient_service_set_properties(m);
        if (r < 0)
                goto cleanup;

        r = sd_bus_message_close_container(m);
        if (r < 0) {
                perror ("Failed to close container");
                goto cleanup;
        }

        /* Auxiliary units */
        r = sd_bus_message_append(m, "a(sa(sv))", 0);
        if (r < 0) {
                perror ("Failed to append bus message");
                goto cleanup;
        }

        r = sd_bus_call(bus, m, 0, &error, &reply);
        if (r < 0) {
                fprintf (stderr,
                         "Failed to start transient service unit: %s\n",
                         error.message ? error.message : strerror (errno));
                goto cleanup;
        }

        fprintf (stderr, "Running as unit: %s\n", service);

        r = 0;
cleanup:
        sd_bus_message_unref (m);
        sd_bus_message_unref (reply);
        sd_bus_error_free (&error);
        free (service);
        return r;
}

/* build/systemd-run --user --pipe --collect --property=CPUAccounting=1 --unit="test-$RANDOM" sleep 5 */
static int run(int argc, char* argv[]) {
        sd_bus *bus = NULL;
        char *description = NULL;
        int r;

        r = parse_argv(argc, argv);
        if (r < 0)
                return r;

        if (!arg_description) {
                // use argz lib later
                //description = strv_join(arg_cmdline, " ");
                description = strdup ("running a job test");
                if (!description) {
                        perror ("strdup");
                        return -1;
                }
                arg_description = description;
        }

        r = sd_bus_default_user (&bus);
        if (r < 0)
                return r;

        r = start_transient_service(bus);
        if (r < 0)
                return r;

        r = 0;
cleanup:
        sd_bus_flush_close_unref (bus);
        free (description);
        free (arg_working_directory);
        if (arg_cmdline) {
                char **tmp = arg_cmdline;
                while (*tmp) {
                        free (*tmp);
                        tmp++;
                }
                free (arg_cmdline);
        }
        return r;
}

int main (int argc, char *argv[])
{
        int r;
        r = run (argc, argv);
        /* if (r < 0) { */
        /*         (void) sd_notifyf(0, "ERRNO=%i", -r); */
        /* } */
        return (r < 0 ? EXIT_FAILURE : r);
}
