/* SPDX-License-Identifier: LGPL-2.1-or-later */

#define _GNU_SOURCE
#include <unistd.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <poll.h>

#include <systemd/sd-bus.h>

static const char *arg_unit = NULL;

static int help(void) {
        printf("my-wait [OPTIONS...] COMMAND [ARGUMENTS...]\n"
               "\nWait the specified command in a transient scope or service.\n\n"
               "  -h --help                       Show this help\n"
               "     --version                    Show package version\n"
               "  -u --unit=UNIT                  Run under the specified unit name\n");

        exit (1);
}

static int parse_argv(int argc, char *argv[]) {

        enum {
                ARG_VERSION = 0x100,
        };

        static const struct option options[] = {
                { "help",              no_argument,       NULL, 'h'                   },
                { "version",           no_argument,       NULL, ARG_VERSION           },
                { "unit",              required_argument, NULL, 'u'                   },
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

        return 0;
}

static int cleanupunit(int argc, char* argv[]) {
        sd_bus *bus = NULL;
        char *service_path = NULL;
        sd_bus_error error = SD_BUS_ERROR_NULL;
        int r;
        char *active_state = NULL;
        char *load_state = NULL;
        uint64_t timestamp;

        r = parse_argv(argc, argv);
        if (r < 0)
                return r;

        r = sd_bus_default_user (&bus);
        if (r < 0)
                return r;

        /* . escapes to _2e */
        if (asprintf (&service_path,
                      "/org/freedesktop/systemd1/unit/%s_2eservice",
                      arg_unit) < 0) {
                perror ("asprintf");
                goto cleanup;
        }
        printf ("unit name service path is %s\n", service_path);

        r = sd_bus_get_property_string (bus,
                                        "org.freedesktop.systemd1",
                                        service_path,
                                        "org.freedesktop.systemd1.Unit",
                                        "ActiveState",
                                        &error,
                                        &active_state);
        if (r < 0) {
                fprintf (stderr, "sd_bus_get_property_string: %s\n", error.message);
                goto cleanup;
        }
        printf ("initial active state = %s\n", active_state);

        /* make sure exited */

        r = sd_bus_get_property_trivial (bus,
                                         "org.freedesktop.systemd1",
                                         service_path,
                                         "org.freedesktop.systemd1.Service",
                                         "ExecMainExitTimestamp",
                                         &error,
                                         't',
                                         &timestamp);
        if (r < 0) {
                fprintf (stderr, "sd_bus_get_property_trivial: %s\n", error.message);
                goto cleanup;
        }

        if (timestamp == 0) {
                fprintf (stderr, "job hasn't exitted\n");
                goto cleanup;
        }

        if (!strcmp (active_state, "active")) {
                sd_bus_message *reply = NULL;
                char *service_name = NULL;
                const char* response;

                /* assume user input a single word, so just add .service */
                if (asprintf (&service_name,
                              "%s.service",
                              arg_unit) < 0) {
                        perror ("asprintf");
                        goto cleanup;
                }
                printf ("unit name service name is %s\n", service_name);

                /* XXX sd_bus_call_method_async? */

                /* apparently "name" here is name.service, no escaping, i have no idea why */

                r = sd_bus_call_method (bus,
                                        "org.freedesktop.systemd1",
                                        "/org/freedesktop/systemd1",
                                        "org.freedesktop.systemd1.Manager",
                                        "StopUnit",
                                        &error,
                                        &reply,
                                        "ss",
                                        service_name, "fail");
                if (r < 0) {
                        fprintf (stderr, "sd_bus_call_method: %s\n", error.message);
                        goto cleanup;
                }

                /* response unnecessary? */
                /* r = sd_bus_message_read(reply, "o", &response); */
                /* if (r < 0) */
                /* { */
                /*         perror ("sd_bus_message_read"); */
                /*         goto cleanup_active; */
                /* } */

                fprintf (stderr, "stopped unit %s\n", arg_unit);
        cleanup_active:
                sd_bus_message_unref (reply);
                free (service_name);
        }
        /* achu: generally speaking i think can only be failed if it is not active?
         * Perhaps there are some weird corner cases I don't yet know of.
         */
        else if (!strcmp (active_state, "failed")
                 || !strcmp (active_state, "inactive")) {
                sd_bus_message *reply = NULL;
                char *service_name = NULL;
                const char* response;

                /* assume user input a single word, so just add .service */
                if (asprintf (&service_name,
                              "%s.service",
                              arg_unit) < 0) {
                        perror ("asprintf");
                        goto cleanup;
                }
                printf ("unit name service name is %s\n", service_name);

                /* XXX sd_bus_call_method_async? */

                /* apparently "name" here is name.service, no escaping, i have no idea why */

                r = sd_bus_call_method (bus,
                                        "org.freedesktop.systemd1",
                                        "/org/freedesktop/systemd1",
                                        "org.freedesktop.systemd1.Manager",
                                        "ResetFailedUnit",
                                        &error,
                                        &reply,
                                        "s",
                                        service_name);
                if (r < 0) {
                        fprintf (stderr, "sd_bus_call_method: %s\n", error.message);
                        goto cleanup;
                }

                /* achu: this always return < 0, is there no response? */
                /* r = sd_bus_message_read(reply, "o", &response); */
                /* if (r < 0) */
                /* { */
                /*         fprintf (stderr, "sd_bus_message_read: %s\n", response); */
                /*         goto cleanup_failed; */
                /* } */

                fprintf (stderr, "reset failed unit %s\n", arg_unit);
        cleanup_failed:
                sd_bus_message_unref (reply);
                free (service_name);
        }

        r = 0;
cleanup:
        sd_bus_flush_close_unref (bus);
        sd_bus_error_free (&error);
        free (service_path);
        free (active_state);
        free (load_state);
        return r;
}

int main (int argc, char *argv[])
{
        int r;
        r = cleanupunit (argc, argv);
        /* if (r < 0) { */
        /*         (void) sd_notifyf(0, "ERRNO=%i", -r); */
        /* } */
        return (r < 0 ? EXIT_FAILURE : r);
}
