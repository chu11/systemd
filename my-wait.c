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

static int get_properties_inactive (sd_bus *bus, const char *path)
{
        sd_bus_error error = SD_BUS_ERROR_NULL;
        uint32_t tmp;
        uint64_t t;
        int r;

        r = sd_bus_get_property_trivial (bus,
                                         "org.freedesktop.systemd1",
                                         path,
                                         "org.freedesktop.systemd1.Service",
                                         "ExecMainStatus",
                                         &error,
                                         'i',
                                         &tmp);
        if (r < 0) {
                fprintf (stderr, "sd_bus_get_property_trivial: %s\n", error.message);
                goto cleanup;
        }
        printf ("exit status = %d\n", tmp);

        r = sd_bus_get_property_trivial (bus,
                                         "org.freedesktop.systemd1",
                                         path,
                                         "org.freedesktop.systemd1.Service",
                                         "ExecMainStartTimestamp",
                                         &error,
                                         't',
                                         &t);
        if (r < 0) {
                fprintf (stderr, "sd_bus_get_property_trivial: %s\n", error.message);
                goto cleanup;
        }
        printf ("start time = %lu\n", t);

        r = sd_bus_get_property_trivial (bus,
                                         "org.freedesktop.systemd1",
                                         path,
                                         "org.freedesktop.systemd1.Service",
                                         "ExecMainExitTimestamp",
                                         &error,
                                         't',
                                         &t);
        if (r < 0) {
                fprintf (stderr, "sd_bus_get_property_trivial: %s\n", error.message);
                goto cleanup;
        }
        printf ("Exit time = %lu\n", t);

        r = 0;
cleanup:
        sd_bus_error_free (&error);
        return r;
}

static int get_properties_done (sd_bus *bus,
                                const char *active_state,
                                const char *path)
{
        if (!strcmp (active_state, "inactive")
            || !strcmp (active_state, "failed"))
                return get_properties_inactive (bus, path);
        return 0;
}

struct wait_data {
        sd_bus *bus;

        char *active_state;
        char *result;
        uint32_t exit_status;
        uint64_t exit_timestamp;
        bool done;
};

static int get_properties_changed (struct wait_data *wd, const char *path)
{
        sd_bus_message *m = NULL;
        sd_bus_error error = SD_BUS_ERROR_NULL;
        int r;

        r = sd_bus_call_method(wd->bus,
                               "org.freedesktop.systemd1",
                               path,
                               "org.freedesktop.DBus.Properties",
                               "GetAll",
                               &error,
                               &m,
                               "s", "");
        if (r < 0) {
                fprintf (stderr, "sd_bus_call_method: %s\n", error.message);
                goto cleanup;
        }

        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sv}");
        if (r < 0) {
                perror ("sd_bus_message_enter_container");
                goto cleanup;
        }

        while ((r = sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv")) > 0) {
                const char *member;
                const char *contents;

                r = sd_bus_message_read_basic(m, SD_BUS_TYPE_STRING, &member);
                if (r < 0) {
                        perror ("sd_bus_message_read_basic");
                        goto cleanup;
                }

                if (!strcmp (member, "ActiveState")) {
                        const char *s;
                        char type;

                        r = sd_bus_message_peek_type(m, NULL, &contents);
                        if (r < 0) {
                                perror ("peek type\n");
                                goto cleanup;
                        }

                        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, contents);
                        if (r < 0) {
                                perror ("sd_bus_message_enter_container");
                                goto cleanup;
                        }

                        /* must call again now that we've called enter container */
                        r = sd_bus_message_peek_type(m, &type, NULL);
                        if (r < 0) {
                                perror ("peek type\n");
                                goto cleanup;
                        }

                        if (type != SD_BUS_TYPE_STRING) {
                                fprintf (stderr, "invalid type\n");
                                goto cleanup;
                        }

                        r = sd_bus_message_read_basic(m, type, &s);
                        if (r < 0) {
                                perror ("sd_bus_message_read_basic");
                                goto cleanup;
                        }

                        r = sd_bus_message_exit_container(m);
                        if (r < 0) {
                                perror ("sd_bus_message_exit_container");
                                goto cleanup;
                        }

                        if (!wd->active_state
                            || strcmp (wd->active_state, s) != 0) {
                                free (wd->active_state);
                                if (!(wd->active_state = strdup (s))) {
                                        perror ("strdup");
                                        goto cleanup;
                                }
                                printf ("new active state = %s\n", wd->active_state);
                        }
                }
                else if (!strcmp (member, "Result")) {
                        const char *s;
                        char type;

                        r = sd_bus_message_peek_type(m, NULL, &contents);
                        if (r < 0) {
                                perror ("peek type\n");
                                goto cleanup;
                        }

                        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, contents);
                        if (r < 0) {
                                perror ("sd_bus_message_enter_container");
                                goto cleanup;
                        }

                        /* must call again now that we've called enter container */
                        r = sd_bus_message_peek_type(m, &type, NULL);
                        if (r < 0) {
                                perror ("peek type\n");
                                goto cleanup;
                        }

                        if (type != SD_BUS_TYPE_STRING) {
                                fprintf (stderr, "invalid type\n");
                                goto cleanup;
                        }

                        r = sd_bus_message_read_basic(m, type, &s);
                        if (r < 0) {
                                perror ("sd_bus_message_read_basic");
                                goto cleanup;
                        }

                        r = sd_bus_message_exit_container(m);
                        if (r < 0) {
                                perror ("sd_bus_message_exit_container");
                                goto cleanup;
                        }

                        if (!wd->result
                            || strcmp (wd->result, s) != 0) {
                                free (wd->result);
                                if (!(wd->result = strdup (s))) {
                                        perror ("strdup");
                                        goto cleanup;
                                }
                                printf ("new result = %s\n", wd->result);
                        }
                }
                else if (!strcmp (member, "ExecMainStatus")) {
                        uint32_t tmp;
                        char type;

                        r = sd_bus_message_peek_type(m, NULL, &contents);
                        if (r < 0) {
                                perror ("peek type\n");
                                goto cleanup;
                        }

                        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, contents);
                        if (r < 0) {
                                perror ("sd_bus_message_enter_container");
                                goto cleanup;
                        }

                        /* must call again now that we've called enter container */
                        r = sd_bus_message_peek_type(m, &type, NULL);
                        if (r < 0) {
                                perror ("peek type\n");
                                goto cleanup;
                        }

                        if (type != SD_BUS_TYPE_INT32) {
                                fprintf (stderr, "invalid type\n");
                                goto cleanup;
                        }

                        r = sd_bus_message_read_basic(m, type, &tmp);
                        if (r < 0) {
                                perror ("sd_bus_message_read_basic");
                                goto cleanup;
                        }

                        r = sd_bus_message_exit_container(m);
                        if (r < 0) {
                                perror ("sd_bus_message_exit_container");
                                goto cleanup;
                        }

                        if (wd->exit_status != tmp) {
                                wd->exit_status = tmp;
                                printf ("exit status = %d\n", tmp);
                        }
                }
                else if (!strcmp (member, "ExecMainExitTimestamp")) {
                        uint64_t tmp;
                        char type;

                        r = sd_bus_message_peek_type(m, NULL, &contents);
                        if (r < 0) {
                                perror ("peek type\n");
                                goto cleanup;
                        }

                        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, contents);
                        if (r < 0) {
                                perror ("sd_bus_message_enter_container");
                                goto cleanup;
                        }

                        /* must call again now that we've called enter container */
                        r = sd_bus_message_peek_type(m, &type, NULL);
                        if (r < 0) {
                                perror ("peek type\n");
                                goto cleanup;
                        }

                        if (type != SD_BUS_TYPE_UINT64) {
                                fprintf (stderr, "invalid type\n");
                                goto cleanup;
                        }

                        r = sd_bus_message_read_basic(m, type, &tmp);
                        if (r < 0) {
                                perror ("sd_bus_message_read_basic");
                                goto cleanup;
                        }

                        r = sd_bus_message_exit_container(m);
                        if (r < 0) {
                                perror ("sd_bus_message_exit_container");
                                goto cleanup;
                        }

                        if (wd->exit_timestamp != tmp) {
                                wd->exit_timestamp = tmp;
                                printf ("exit timestamp= %lu\n", tmp);
                        }
                }
                else {
                        r = sd_bus_message_skip(m, "v");
                        if (r < 0) {
                                perror ("sd_bus_message_skip");
                                goto cleanup;
                        }
                }

                r = sd_bus_message_exit_container(m);
                if (r < 0) {
                        perror ("sd_bus_message_exit_container");
                        goto cleanup;
                }
        }
        if (r < 0) {
                perror ("sd_bus_message_enter_container");
                goto cleanup;
        }

        r = sd_bus_message_exit_container(m);
        if (r < 0) {
                perror ("sd_bus_message_exit_container");
                goto cleanup;
        }


        if (!strcmp (wd->active_state, "inactive")
            || !strcmp (wd->active_state, "failed")
            || wd->exit_timestamp != 0)
                wd->done = true;

        r = 0;
cleanup:
        sd_bus_message_unref (m);
        sd_bus_error_free (&error);
        return r;
}

static int on_properties_changed(sd_bus_message *m, void *userdata, sd_bus_error *error) {
        struct wait_data *wd = userdata;
        const char *path = sd_bus_message_get_path(m);

        printf ("properties changed on path %s\n", path);
        return get_properties_changed (wd, path);
}


static int waitunit(int argc, char* argv[]) {
        sd_bus *bus = NULL;
        char *service_path = NULL;
        sd_bus_error error = SD_BUS_ERROR_NULL;
        struct wait_data wd = {0};
        int r;
        char *active_state = NULL;
        char *load_state = NULL;

        r = parse_argv(argc, argv);
        if (r < 0)
                return r;

        r = sd_bus_default_user (&bus);
        if (r < 0)
                return r;

        /* assume user input a single word, so just add .service */
        /* . escapes to _2e */
        if (asprintf (&service_path,
                      "/org/freedesktop/systemd1/unit/%s_2eservice",
                      arg_unit) < 0) {
                perror ("asprintf");
                goto cleanup;
        }
        printf ("unit name service is %s\n", service_path);

        /* first make sure unit exists */
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

        r = sd_bus_get_property_string (bus,
                                        "org.freedesktop.systemd1",
                                        service_path,
                                        "org.freedesktop.systemd1.Unit",
                                        "LoadState",
                                        &error,
                                        &load_state);
        if (r < 0) {
                fprintf (stderr, "sd_bus_get_property_string: %s\n", error.message);
                goto cleanup;
        }
        printf ("initial load state = %s\n", load_state);

        if (!strcmp (active_state, "inactive")
            && !strcmp (load_state, "not-found")) {
                printf ("unit %s not running\n", arg_unit);
                goto cleanup;
        }

        if (!strcmp (active_state, "inactive")
            || !strcmp (active_state, "failed")) {
                printf ("unit %s is already done\n", arg_unit);
                get_properties_done (bus, active_state, service_path);
                goto done;
        }

        if (!strcmp (active_state, "active")) {
                /* chance unit has exited but is active b/c of remain-after-exit / RemainAfterExit
                 *
                 * check exit timestamp to see if it's still going
                 */
                uint64_t t;
                r = sd_bus_get_property_trivial (bus,
                                                 "org.freedesktop.systemd1",
                                                 service_path,
                                                 "org.freedesktop.systemd1.Service",
                                                 "ExecMainExitTimestamp",
                                                 &error,
                                                 't',
                                                 &t);
                if (r < 0) {
                        fprintf (stderr, "sd_bus_get_property_trivial: %s\n", error.message);
                        goto cleanup;
                }

                if (t != 0) {
                        fprintf (stderr, "unit is completed\n");
                        goto done;
                }

                /* else active and still running, fall through to polling loop */
        }

        wd.bus = sd_bus_ref(bus);

        r = sd_bus_call_method(bus,
                               "org.freedesktop.systemd1",
                               "/org/freedesktop/systemd1",
                               "org.freedesktop.systemd1.Manager",
                               "Subscribe",
                               &error,
                               NULL,
                               NULL);
        if (r < 0) {
                perror ("Failed to call method");
                goto cleanup;
        }

        r = sd_bus_match_signal_async(bus,
                                      NULL,
                                      "org.freedesktop.systemd1",
                                      service_path,
                                      "org.freedesktop.DBus.Properties",
                                      "PropertiesChanged",
                                      on_properties_changed, NULL, &wd);
        if (r < 0) {
                perror ("Failed to setup signal async");
                goto cleanup;
        }

        while (!wd.done) {
                int n;
                int timeout = 0;
                uint64_t usec = 0;
                struct pollfd fd = {0};
                time_t t_start;

                fd.fd = sd_bus_get_fd (bus);
                fd.events = sd_bus_get_events (bus);

                if (sd_bus_get_timeout(bus, &usec) < 0)
                        timeout = -1;
                else {
                        /* convert usec to millisecond, do some hacks to round up
                         */
                        printf ("usec = %lu %lu\n", usec, UINT64_MAX);
                        if (usec) {
                                uint64_t tmp;
                                if (usec >= (UINT64_MAX - 1000))
                                        tmp = INT_MAX;
                                else if ((usec % 1000) != 0)
                                        tmp = (usec + 1000) / 1000;
                                else
                                        tmp = usec / 1000;
                                if (tmp > INT_MAX)
                                        timeout = INT_MAX;
                                else
                                        timeout = tmp;
                        }
                }
                printf ("fd = %d, events = %X, timeout = %d\n", fd.fd, fd.events, timeout);

                /* if no events or no timeout, assume event ready to go right now */
                if (!fd.events || !timeout) {
                        while ( sd_bus_process(bus, NULL) ) {  }
                        continue;
                }

                t_start = time (NULL);
                printf ("start wait time = %ld\n", t_start);
                if ((n = poll (&fd, 1, timeout)) < 0) {
                        perror ("poll");
                        goto cleanup;
                }
                printf ("time passed = %ld\n", time (NULL) - t_start);
                if (!n) {
                        printf ("continuing\n");
                        continue;
                }
                printf ("revents = %X, n = %d\n", fd.revents, n);
                if (fd.revents & POLLIN) {
                        while ( sd_bus_process(bus, NULL) ) {  }
                }
        }
done:
        r = 0;
cleanup:
        wd.bus = sd_bus_unref(wd.bus);
        free (wd.active_state);
        free (wd.result);
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
        r = waitunit (argc, argv);
        /* if (r < 0) { */
        /*         (void) sd_notifyf(0, "ERRNO=%i", -r); */
        /* } */
        return (r < 0 ? EXIT_FAILURE : r);
}
