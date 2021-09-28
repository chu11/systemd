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

static int help(void) {
        printf("my-list [OPTIONS...] COMMAND [ARGUMENTS...]\n"
               "\nWait the specified command in a transient scope or service.\n\n"
               "  -h --help                       Show this help\n"
               "     --version                    Show package version\n");

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

                case '?':
                        return -EINVAL;

                default:
                        /* NOT REACHED */
                        return -EINVAL;
                }

        return 0;
}

struct UnitInfo {
        const char *machine;
        const char *id;
        const char *description;
        const char *load_state;
        const char *active_state;
        const char *sub_state;
        const char *following;
        const char *unit_path;
        uint32_t job_id;
        const char *job_type;
        const char *job_path;
};

static int cleanupunit(int argc, char* argv[]) {
        sd_bus *bus = NULL;
        sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus_message *m = NULL;
        sd_bus_message *reply = NULL;
        char *patterns[] = { "test*", NULL };
        int r;
        struct UnitInfo u;

        r = parse_argv(argc, argv);
        if (r < 0)
                return r;

        r = sd_bus_default_user (&bus);
        if (r < 0)
                return r;

        r = sd_bus_message_new_method_call(bus,
                                           &m,
                                           "org.freedesktop.systemd1",
                                           "/org/freedesktop/systemd1",
                                           "org.freedesktop.systemd1.Manager",
                                           "ListUnitsByPatterns");
        if (r < 0) {
                fprintf (stderr, "sd_bus_message_new_method_call: %s\n", strerror (-r));
                goto cleanup;
        }

        /* states */
        r = sd_bus_message_append_strv(m, NULL);
        if (r < 0) {
                fprintf (stderr, "sd_bus_message_append_strv: %s\n", strerror (-r));
                goto cleanup;
        }

        /* patterns */
        r = sd_bus_message_append_strv(m, patterns);
        if (r < 0) {
                fprintf (stderr, "sd_bus_message_append_strv: %s\n", strerror (-r));
                goto cleanup;
        }

        r = sd_bus_call(bus, m, 0, &error, &reply);
        if (r < 0) {
                fprintf (stderr, "sd_bus_call: %s %s\n", strerror (-r), error.message);
                goto cleanup;
        }

        r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "(ssssssouso)");
        if (r < 0) {
                fprintf (stderr, "sd_bus_message_enter_container: %s\n", strerror (-r));
                goto cleanup;
        }

        while (1) {
                r = sd_bus_message_read(
                                reply,
                                "(ssssssouso)",
                                &u.id,
                                &u.description,
                                &u.load_state,
                                &u.active_state,
                                &u.sub_state,
                                &u.following,
                                &u.unit_path,
                                &u.job_id,
                                &u.job_type,
                                &u.job_path);
                if (r < 0) {
                        fprintf (stderr, "sd_bus_message_read: %s\n", strerror (-r));
                        goto cleanup;
                }

                if (r == 0)
                        break;

                printf ("id = %s\n", u.id);
        }

        r = sd_bus_message_exit_container(reply);
        if (r < 0) {
                fprintf (stderr, "sd_bus_message_exit_container: %s\n", strerror (-r));
                goto cleanup;
        }

        r = 0;
cleanup:
        sd_bus_message_unref (m);
        sd_bus_message_unref (reply);
        sd_bus_flush_close_unref (bus);
        sd_bus_error_free (&error);
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
