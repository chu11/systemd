/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <getopt.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "sd-bus.h"
#include "sd-event.h"

#include "alloc-util.h"
#include "bus-error.h"
#include "bus-locator.h"
#include "bus-map-properties.h"
#include "bus-unit-util.h"
#include "bus-wait-for-jobs.h"
#include "calendarspec.h"
#include "env-util.h"
#include "exit-status.h"
#include "fd-util.h"
#include "format-util.h"
#include "main-func.h"
#include "parse-argument.h"
#include "parse-util.h"
#include "path-util.h"
#include "pretty-print.h"
#include "process-util.h"
#include "ptyfwd.h"
#include "signal-util.h"
#include "spawn-polkit-agent.h"
#include "strv.h"
#include "terminal-util.h"
#include "unit-def.h"
#include "unit-name.h"
#include "user-util.h"

static bool arg_scope = false;
static bool arg_remain_after_exit = false;
static bool arg_no_block = false;
static bool arg_wait = false;
static const char *arg_unit = NULL;
static const char *arg_description = NULL;
static BusTransport arg_transport = BUS_TRANSPORT_LOCAL;
static bool arg_user = false;
static const char *arg_service_type = NULL;
static const char *arg_exec_user = NULL;
static const char *arg_exec_group = NULL;
static char **arg_environment = NULL;
static char **arg_property = NULL;
static enum {
        ARG_STDIO_NONE,      /* The default, as it is for normal services, stdin connected to /dev/null, and stdout+stderr to the journal */
        ARG_STDIO_PTY,       /* Interactive behaviour, requested by --pty: we allocate a pty and connect it to the TTY we are invoked from */
        ARG_STDIO_DIRECT,    /* Directly pass our stdin/stdout/stderr to the activated service, useful for usage in shell pipelines, requested by --pipe */
        ARG_STDIO_AUTO,      /* If --pipe and --pty are used together we use --pty when invoked on a TTY, and --pipe otherwise */
} arg_stdio = ARG_STDIO_NONE;
static bool arg_quiet = false;
static bool arg_aggressive_gc = false;
static char *arg_working_directory = NULL;
static char **arg_cmdline = NULL;

STATIC_DESTRUCTOR_REGISTER(arg_environment, strv_freep);
STATIC_DESTRUCTOR_REGISTER(arg_property, strv_freep);
STATIC_DESTRUCTOR_REGISTER(arg_working_directory, freep);
STATIC_DESTRUCTOR_REGISTER(arg_cmdline, strv_freep);

static int help(void) {
        _cleanup_free_ char *link = NULL;
        int r;

        r = terminal_urlify_man("systemd-run", "1", &link);
        if (r < 0)
                return log_oom();

        printf("%s [OPTIONS...] COMMAND [ARGUMENTS...]\n"
               "\n%sRun the specified command in a transient scope or service.%s\n\n"
               "  -h --help                       Show this help\n"
               "     --version                    Show package version\n"
               "     --user                       Run as user unit\n"
               "  -H --host=[USER@]HOST           Operate on remote host\n"
               "  -M --machine=CONTAINER          Operate on local container\n"
               "     --scope                      Run this as scope rather than service\n"
               "  -u --unit=UNIT                  Run under the specified unit name\n"
               "  -p --property=NAME=VALUE        Set service or scope unit property\n"
               "     --description=TEXT           Description for unit\n"
               "     --no-block                   Do not wait until operation finished\n"
               "  -r --remain-after-exit          Leave service around until explicitly stopped\n"
               "     --wait                       Wait until service stopped again\n"
               "     --service-type=TYPE          Service type\n"
               "     --uid=USER                   Run as system user\n"
               "     --gid=GROUP                  Run as system group\n"
               "     --working-directory=PATH     Set working directory\n"
               "  -d --same-dir                   Inherit working directory from caller\n"
               "  -E --setenv=NAME[=VALUE]        Set environment variable\n"
               "  -t --pty                        Run service on pseudo TTY as STDIN/STDOUT/\n"
               "                                  STDERR\n"
               "  -P --pipe                       Pass STDIN/STDOUT/STDERR directly to service\n"
               "  -q --quiet                      Suppress information messages during runtime\n"
               "  -G --collect                    Unload unit after it ran, even when failed\n"
               "\nSee the %s for details.\n",
               program_invocation_short_name,
               ansi_highlight(),
               ansi_normal(),
               link);

        return 0;
}

static int parse_argv(int argc, char *argv[]) {

        enum {
                ARG_VERSION = 0x100,
                ARG_USER,
                ARG_SYSTEM,
                ARG_SCOPE,
                ARG_DESCRIPTION,
                ARG_SERVICE_TYPE,
                ARG_EXEC_USER,
                ARG_EXEC_GROUP,
                ARG_NO_BLOCK,
                ARG_WAIT,
                ARG_WORKING_DIRECTORY,
        };

        static const struct option options[] = {
                { "help",              no_argument,       NULL, 'h'                   },
                { "version",           no_argument,       NULL, ARG_VERSION           },
                { "user",              no_argument,       NULL, ARG_USER              },
                { "system",            no_argument,       NULL, ARG_SYSTEM            },
                { "scope",             no_argument,       NULL, ARG_SCOPE             },
                { "unit",              required_argument, NULL, 'u'                   },
                { "description",       required_argument, NULL, ARG_DESCRIPTION       },
                { "remain-after-exit", no_argument,       NULL, 'r'                   },
                { "service-type",      required_argument, NULL, ARG_SERVICE_TYPE      },
                { "wait",              no_argument,       NULL, ARG_WAIT              },
                { "uid",               required_argument, NULL, ARG_EXEC_USER         },
                { "gid",               required_argument, NULL, ARG_EXEC_GROUP        },
                { "setenv",            required_argument, NULL, 'E'                   },
                { "property",          required_argument, NULL, 'p'                   },
                { "tty",               no_argument,       NULL, 't'                   }, /* deprecated alias */
                { "pty",               no_argument,       NULL, 't'                   },
                { "pipe",              no_argument,       NULL, 'P'                   },
                { "quiet",             no_argument,       NULL, 'q'                   },
                { "no-block",          no_argument,       NULL, ARG_NO_BLOCK          },
                { "collect",           no_argument,       NULL, 'G'                   },
                { "working-directory", required_argument, NULL, ARG_WORKING_DIRECTORY },
                { "same-dir",          no_argument,       NULL, 'd'                   },
                { "shell",             no_argument,       NULL, 'S'                   },
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
                        return version();

                case ARG_USER:
                        arg_user = true;
                        break;

                case ARG_SYSTEM:
                        arg_user = false;
                        break;

                case ARG_SCOPE:
                        arg_scope = true;
                        break;

                case 'u':
                        arg_unit = optarg;
                        break;

                case ARG_DESCRIPTION:
                        arg_description = optarg;
                        break;

                case 'r':
                        arg_remain_after_exit = true;
                        break;

                case ARG_SERVICE_TYPE:
                        arg_service_type = optarg;
                        break;

                case ARG_EXEC_USER:
                        arg_exec_user = optarg;
                        break;

                case ARG_EXEC_GROUP:
                        arg_exec_group = optarg;
                        break;

                case 'E':
                        r = strv_env_replace_strdup_passthrough(&arg_environment, optarg);
                        if (r < 0)
                                return log_error_errno(r, "Cannot assign environment variable %s: %m", optarg);

                        break;

                case 'p':
                        if (strv_extend(&arg_property, optarg) < 0)
                                return log_oom();

                        break;

                case 't': /* --pty */
                        if (IN_SET(arg_stdio, ARG_STDIO_DIRECT, ARG_STDIO_AUTO)) /* if --pipe is already used, upgrade to auto mode */
                                arg_stdio = ARG_STDIO_AUTO;
                        else
                                arg_stdio = ARG_STDIO_PTY;
                        break;

                case 'P': /* --pipe */
                        if (IN_SET(arg_stdio, ARG_STDIO_PTY, ARG_STDIO_AUTO)) /* If --pty is already used, upgrade to auto mode */
                                arg_stdio = ARG_STDIO_AUTO;
                        else
                                arg_stdio = ARG_STDIO_DIRECT;
                        break;

                case 'q':
                        arg_quiet = true;
                        break;

                case ARG_NO_BLOCK:
                        arg_no_block = true;
                        break;

                case ARG_WAIT:
                        arg_wait = true;
                        break;

                case ARG_WORKING_DIRECTORY:
                        r = parse_path_argument(optarg, true, &arg_working_directory);
                        if (r < 0)
                                return r;

                        break;

                case 'd': {
                        _cleanup_free_ char *p = NULL;

                        r = safe_getcwd(&p);
                        if (r < 0)
                                return log_error_errno(r, "Failed to get current working directory: %m");

                        if (empty_or_root(p))
                                arg_working_directory = mfree(arg_working_directory);
                        else
                                free_and_replace(arg_working_directory, p);
                        break;
                }

                case 'G':
                        arg_aggressive_gc = true;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached();
                }

        if (arg_stdio == ARG_STDIO_AUTO)
                /* If we both --pty and --pipe are specified we'll automatically pick --pty if we are connected fully
                 * to a TTY and pick direct fd passing otherwise. This way, we automatically adapt to usage in a shell
                 * pipeline, but we are neatly interactive with tty-level isolation otherwise. */
                arg_stdio = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) && isatty(STDERR_FILENO) ?
                        ARG_STDIO_PTY :
                        ARG_STDIO_DIRECT;

        if (argc > optind) {
                char **l;

                l = strv_copy(argv + optind);
                if (!l)
                        return log_oom();

                strv_free_and_replace(arg_cmdline, l);

        } else if (!arg_unit)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Command line to execute required.");

        if (arg_scope && (arg_remain_after_exit || arg_service_type))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "--remain-after-exit and --service-type= are not supported in --scope mode.");

        if (arg_stdio != ARG_STDIO_NONE && arg_scope)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "--pty/--pipe is not compatible in timer or --scope mode.");

        if (arg_stdio != ARG_STDIO_NONE && arg_no_block)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "--pty/--pipe is not compatible with --no-block.");

        if (arg_wait) {
                if (arg_no_block)
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "--wait may not be combined with --no-block.");

                if (arg_scope)
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "--wait may not be combined with --scope.");
        }

        return 1;
}

static int transient_service_set_properties(sd_bus_message *m, const char *pty_path) {
        bool send_term = false;
        int r;

        assert(m);

        r = sd_bus_message_append(m, "(sv)", "Description", "s", arg_description);
        if (r < 0)
                return bus_log_create_error(r);

        if (arg_aggressive_gc) {
                r = sd_bus_message_append(m, "(sv)", "CollectMode", "s", "inactive-or-failed");
                if (r < 0)
                        return bus_log_create_error(r);
        }

        r = bus_append_unit_property_assignment_many(m, UNIT_SERVICE, arg_property);
        if (r < 0)
                return r;

        if (arg_wait || arg_stdio != ARG_STDIO_NONE) {
                r = sd_bus_message_append(m, "(sv)", "AddRef", "b", 1);
                if (r < 0)
                        return bus_log_create_error(r);
        }

        if (arg_remain_after_exit) {
                r = sd_bus_message_append(m, "(sv)", "RemainAfterExit", "b", arg_remain_after_exit);
                if (r < 0)
                        return bus_log_create_error(r);
        }

        if (arg_service_type) {
                r = sd_bus_message_append(m, "(sv)", "Type", "s", arg_service_type);
                if (r < 0)
                        return bus_log_create_error(r);
        }

        if (arg_exec_user) {
                r = sd_bus_message_append(m, "(sv)", "User", "s", arg_exec_user);
                if (r < 0)
                        return bus_log_create_error(r);
        }

        if (arg_exec_group) {
                r = sd_bus_message_append(m, "(sv)", "Group", "s", arg_exec_group);
                if (r < 0)
                        return bus_log_create_error(r);
        }

        if (arg_working_directory) {
                r = sd_bus_message_append(m, "(sv)", "WorkingDirectory", "s", arg_working_directory);
                if (r < 0)
                        return bus_log_create_error(r);
        }

        if (pty_path) {
                r = sd_bus_message_append(m,
                                          "(sv)(sv)(sv)(sv)",
                                          "StandardInput", "s", "tty",
                                          "StandardOutput", "s", "tty",
                                          "StandardError", "s", "tty",
                                          "TTYPath", "s", pty_path);
                if (r < 0)
                        return bus_log_create_error(r);

                send_term = true;

        } else if (arg_stdio == ARG_STDIO_DIRECT) {
                r = sd_bus_message_append(m,
                                          "(sv)(sv)(sv)",
                                          "StandardInputFileDescriptor", "h", STDIN_FILENO,
                                          "StandardOutputFileDescriptor", "h", STDOUT_FILENO,
                                          "StandardErrorFileDescriptor", "h", STDERR_FILENO);
                if (r < 0)
                        return bus_log_create_error(r);

                send_term = isatty(STDIN_FILENO) || isatty(STDOUT_FILENO) || isatty(STDERR_FILENO);
        }

        if (send_term) {
                const char *e;

                e = getenv("TERM");
                if (e) {
                        char *n;

                        n = strjoina("TERM=", e);
                        r = sd_bus_message_append(m,
                                                  "(sv)",
                                                  "Environment", "as", 1, n);
                        if (r < 0)
                                return bus_log_create_error(r);
                }
        }

        if (!strv_isempty(arg_environment)) {
                r = sd_bus_message_open_container(m, 'r', "sv");
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_append(m, "s", "Environment");
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_open_container(m, 'v', "as");
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_append_strv(m, arg_environment);
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_close_container(m);
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_close_container(m);
                if (r < 0)
                        return bus_log_create_error(r);
        }

        /* Exec container */
        if (!strv_isempty(arg_cmdline)) {
                r = sd_bus_message_open_container(m, 'r', "sv");
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_append(m, "s", "ExecStart");
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_open_container(m, 'v', "a(sasb)");
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_open_container(m, 'a', "(sasb)");
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_open_container(m, 'r', "sasb");
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_append(m, "s", arg_cmdline[0]);
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_append_strv(m, arg_cmdline);
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_append(m, "b", false);
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_close_container(m);
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_close_container(m);
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_close_container(m);
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_close_container(m);
                if (r < 0)
                        return bus_log_create_error(r);
        }

        return 0;
}

static int start_transient_service(
                sd_bus *bus) {

        _cleanup_(sd_bus_message_unrefp) sd_bus_message *m = NULL, *reply = NULL;
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_free_ char *service = NULL, *pty_path = NULL;
        int r;

        assert(bus);

        if (arg_unit) {
                r = unit_name_mangle_with_suffix(arg_unit, "as unit",
                                                 arg_quiet ? 0 : UNIT_NAME_MANGLE_WARN,
                                                 ".service", &service);
                if (r < 0)
                        return log_error_errno(r, "Failed to mangle unit name: %m");
                printf ("unit name service is %s\n", service);
        }
        // r = bus_message_new_method_call(bus, &m, bus_systemd_mgr, "StartTransientUnit");
        /* int bus_message_new_method_call( */
        /*         sd_bus *bus, */
        /*         sd_bus_message **m, */
        /*         const BusLocator *locator, */
        /*         const char *member) { */

        /* assert(locator); */

        /* const BusLocator* const bus_systemd_mgr = &(BusLocator){ */
        /*         .destination = "org.freedesktop.systemd1", */
        /*         .path = "/org/freedesktop/systemd1", */
        /*         .interface = "org.freedesktop.systemd1.Manager" */
        /* }; */

        r = sd_bus_message_new_method_call(bus,
                                           &m,
                                           "org.freedesktop.systemd1",
                                           "/org/freedesktop/systemd1",
                                           "org.freedesktop.systemd1.Manager",
                                           "StartTransientUnit");
        if (r < 0)
                return bus_log_create_error(r);

        /* for tty? not sure */
        /* r = sd_bus_message_set_allow_interactive_authorization(m, true); */
        /* if (r < 0) */
        /*         return bus_log_create_error(r); */

        /* Name and mode */
        r = sd_bus_message_append(m, "ss", service, "fail");
        if (r < 0)
                return bus_log_create_error(r);

        /* Properties */
        r = sd_bus_message_open_container(m, 'a', "(sv)");
        if (r < 0)
                return bus_log_create_error(r);

        r = transient_service_set_properties(m, pty_path);
        if (r < 0)
                return r;

        r = sd_bus_message_close_container(m);
        if (r < 0)
                return bus_log_create_error(r);

        /* Auxiliary units */
        r = sd_bus_message_append(m, "a(sa(sv))", 0);
        if (r < 0)
                return bus_log_create_error(r);

        /* achu: tty stuff, ignore for now */
        // polkit_agent_open_if_enabled(arg_transport, true);

        r = sd_bus_call(bus, m, 0, &error, &reply);
        if (r < 0)
                return log_error_errno(r, "Failed to start transient service unit: %s", bus_error_message(&error, r));

        if (!arg_quiet)
                log_info("Running as unit: %s", service);

        return 0;
}


typedef struct RunContext {
        sd_bus *bus;
        sd_event *event;
        PTYForward *forward;
        sd_bus_slot *match;

        /* Current state of the unit */
        char *active_state;
        /* The exit data of the unit */
        char *result;
        uint32_t exit_status;
        bool done;
} RunContext;

static void run_context_free(RunContext *c) {
        assert(c);

        c->forward = pty_forward_free(c->forward);
        c->match = sd_bus_slot_unref(c->match);
        c->bus = sd_bus_unref(c->bus);
        c->event = sd_event_unref(c->event);

        free(c->active_state);
        free(c->result);
}

static void run_context_check_done(RunContext *c) {
        bool done;

        assert(c);

        if (c->match) {
                printf ("check active state = active_state -> %s\n", c->active_state);
                done = STRPTR_IN_SET(c->active_state, "inactive", "failed");
        }
        else
                done = true;

        if (c->forward && done) /* If the service is gone, it's time to drain the output */
                done = pty_forward_drain(c->forward);

        /* if (done) */
        /*         sd_event_exit(c->event, EXIT_SUCCESS); */
        c->done = done;
}

static int run_context_update(RunContext *c, const char *path) {

        _cleanup_(sd_bus_message_unrefp) sd_bus_message *m = NULL;
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        int r;
#if 0
        char *str = NULL;
        uint32_t tmp;
#endif

#if 1
        /* r = bus_map_all_properties(c->bus, */
        /*                            "org.freedesktop.systemd1", */
        /*                            path, */
        /*                            map, */
        /*                            BUS_MAP_STRDUP, */
        /*                            &error, */
        /*                            NULL, */
        /*                            c); */

        r = sd_bus_call_method(
                        c->bus,
                        "org.freedesktop.systemd1",
                        path,
                        "org.freedesktop.DBus.Properties",
                        "GetAll",
                        &error,
                        &m,
                        "s", "");
        if (r < 0)
            return r;

        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sv}");
        if (r < 0)
                return r;

        while ((r = sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv")) > 0) {
                const char *member;
                const char *contents;

                r = sd_bus_message_read_basic(m, SD_BUS_TYPE_STRING, &member);
                if (r < 0)
                        return r;

                //printf ("member = %s\n", member);
                if (!strcmp (member, "ActiveState")) {
                        const char *s;
                        char type;

                        r = sd_bus_message_peek_type(m, NULL, &contents);
                        if (r < 0)
                                return r;

                        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, contents);
                        if (r < 0)
                                return r;

                        /* must call again now that we've called enter container */
                        r = sd_bus_message_peek_type(m, &type, NULL);
                        if (r < 0)
                                return r;

                        if (type != SD_BUS_TYPE_STRING)
                                return -1;

                        r = sd_bus_message_read_basic(m, type, &s);
                        if (r < 0)
                                return r;

                        r = sd_bus_message_exit_container(m);
                        if (r < 0)
                                return r;

                        free (c->active_state);
                        if (!(c->active_state = strdup (s))) {
                                printf ("STRDUP FAILED\n");
                                return -1;
                        }
                        // printf ("new active state = %s\n", c->active_state);
                }
                else if (!strcmp (member, "Result")) {
                        const char *s;
                        char type;

                        r = sd_bus_message_peek_type(m, NULL, &contents);
                        if (r < 0)
                                return r;

                        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, contents);
                        if (r < 0)
                                return r;

                        /* must call again now that we've called enter container */
                        r = sd_bus_message_peek_type(m, &type, NULL);
                        if (r < 0)
                                return r;

                        if (type != SD_BUS_TYPE_STRING)
                                return -1;

                        r = sd_bus_message_read_basic(m, type, &s);
                        if (r < 0)
                                return r;

                        r = sd_bus_message_exit_container(m);
                        if (r < 0)
                                return r;

                        free (c->result);
                        if (!(c->result = strdup (s))) {
                                printf ("STRDUP FAILED\n");
                                return -1;
                        }
                        //printf ("new result = %s\n", c->result);
                }
                else if (!strcmp (member, "ExecMainStatus")) {
                        uint32_t tmp;
                        char type;

                        r = sd_bus_message_peek_type(m, NULL, &contents);
                        if (r < 0)
                                return r;

                        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, contents);
                        if (r < 0)
                                return r;

                        /* must call again now that we've called enter container */
                        r = sd_bus_message_peek_type(m, &type, NULL);
                        if (r < 0)
                                return r;

                        if (type != SD_BUS_TYPE_INT32)
                                return -1;

                        r = sd_bus_message_read_basic(m, type, &tmp);
                        if (r < 0)
                                return r;

                        r = sd_bus_message_exit_container(m);
                        if (r < 0)
                                return r;

                        c->exit_status = tmp;
                        //printf ("exit status = %d\n", tmp);
                }
                else {
                        r = sd_bus_message_skip(m, "v");
                        if (r < 0)
                                return r;
                }

                r = sd_bus_message_exit_container(m);
                if (r < 0)
                        return r;
        }
        if (r < 0)
                return r;

        r = sd_bus_message_exit_container(m);
        if (r < 0)
                return r;
#endif

#if 0
        /* r = sd_bus_call_method(bus, destination, path, "org.freedesktop.DBus.Properties", "Get", error, &reply, "ss", strempty(interface), member); */
        /* if (r < 0) */
        /*         return r; */

        /* r = sd_bus_message_enter_container(reply, 'v', "s"); */
        /* if (r < 0) */
        /*         goto fail; */

        /* r = sd_bus_message_read_basic(reply, 's', &s); */
        /* if (r < 0) */
        /*         goto fail; */

        r = sd_bus_get_property_string (c->bus,
                                        "org.freedesktop.systemd1",
                                        path,
                                        "org.freedesktop.systemd1.Unit",
                                        "ActiveState",
                                        &error,
                                        &str);
        if (r < 0)
                log_error_errno (r, "sd_bus_get_property_string: %s", bus_error_message (&error, r));
        else {
                //printf ("ActiveState str = %s\n", str);
                free (c->active_state);
                c->active_state = str;
        }

        r = sd_bus_get_property_trivial (c->bus,
                                         "org.freedesktop.systemd1",
                                         path,
                                         "org.freedesktop.systemd1.Service",
                                         "ExecMainStatus",
                                         &error,
                                         'i',
                                         &tmp);
        if (r < 0)
                log_error_errno (r, "sd_bus_get_property_trivial: %s", bus_error_message (&error, r));
        else {
                printf ("exit status = %d\n", tmp);
                c->exit_status = tmp;
        }

        str = NULL;
        r = sd_bus_get_property_string (c->bus,
                                        "org.freedesktop.systemd1",
                                        path,
                                        "org.freedesktop.systemd1.Service",
                                        "Result",
                                        &error,
                                        &str);
        if (r < 0)
                log_error_errno (r, "sd_bus_get_property_string: %s", bus_error_message (&error, r));
        else {
                //printf ("Result str = %s\n", str);
                free (c->result);
                c->result = str;
        }
#endif

        run_context_check_done(c);
        return 0;
}

static int on_properties_changed(sd_bus_message *m, void *userdata, sd_bus_error *error) {
        RunContext *c = userdata;

        assert(m);
        assert(c);

        return run_context_update(c, sd_bus_message_get_path(m));
}

static int wait_transient_service(
                sd_bus *bus,
                int *retval) {
        _cleanup_free_ char *service = NULL;
        int r;

        if (arg_unit) {
                r = unit_name_mangle_with_suffix(arg_unit, "as unit",
                                                 arg_quiet ? 0 : UNIT_NAME_MANGLE_WARN,
                                                 ".service", &service);
                if (r < 0)
                        return log_error_errno(r, "Failed to mangle unit name: %m");
                printf ("unit name service is %s\n", service);
        }

        printf ("wait path? %d %d\n", arg_wait, arg_stdio != ARG_STDIO_NONE);
        if (arg_wait || arg_stdio != ARG_STDIO_NONE) {
                _cleanup_(run_context_free) RunContext c = {0};
                _cleanup_free_ char *path = NULL;

                c.bus = sd_bus_ref(bus);

                r = sd_event_default(&c.event);
                if (r < 0)
                        return log_error_errno(r, "Failed to get event loop: %m");

                path = unit_dbus_path_from_name(service);
                if (!path)
                        return log_oom();
                printf ("unit_dbus_path_from_name = %s -> %s\n", service, path);
                // /org/freedesktop/systemd1/unit/unitnamefoo_2eservice
                // dash converts to _2d

                // HOW DO I LIMIT THE MATCH?
                // perhaps need to GetAll just like systemd did already and filter as needed.

                r = sd_bus_match_signal_async(
                                bus,
                                &c.match,
                                "org.freedesktop.systemd1",
                                path,
                                "org.freedesktop.DBus.Properties",
                                "PropertiesChanged",
                                on_properties_changed, NULL, &c);
                if (r < 0)
                        return log_error_errno(r, "Failed to request properties changed signal match: %m");

#if 0
                r = sd_bus_attach_event(bus, c.event, SD_EVENT_PRIORITY_NORMAL);
                if (r < 0)
                        return log_error_errno(r, "Failed to attach bus to event loop: %m");

                r = run_context_update(&c, path);
                if (r < 0)
                        return r;

                r = sd_event_loop(c.event);
                if (r < 0)
                        return log_error_errno(r, "Failed to run event loop: %m");
#endif
#if 0
                r = run_context_update(&c, path);
                if (r < 0)
                        return r;

                while (!c.done) {
                        time_t t_start = time(NULL);
                        printf ("start wait time = %ld\n", t_start);
                        if (sd_bus_wait (bus, UINT64_MAX) < 0)
                                fprintf (stderr, "sd_bus_wait ...");
                        printf ("time passed = %ld\n", time (NULL) - t_start);
                        while ( sd_bus_process(bus, NULL) ) {  }
                }
#endif
#if 1
#include <poll.h>
#define USEC_TO_MSEC(usec) ((unsigned int)((usec) / 1000))
                {
                        while (!c.done) {
                                int n;
                                int timeout;
                                uint64_t usec = 0;
                                struct pollfd fd = {0};
                                time_t t_start;

                                fd.fd = sd_bus_get_fd (bus);
                                fd.events = sd_bus_get_events (bus);

                                if (sd_bus_get_timeout(bus, &usec) < 0)
                                        timeout = -1;
                                else {
                                        timeout = USEC_TO_MSEC (usec);
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
                                        return log_error_errno (n, "failed poll");
                                }
                                printf ("time passed = %ld\n", time (NULL) - t_start);
                                if (!n) {
                                        printf ("continuing\n");
                                        continue;
                                }
                                printf ("revents = %X, n = %d\n", fd.revents, n);
                                while ( sd_bus_process(bus, NULL) ) {  }

                                r = run_context_update(&c, path);
                                if (r < 0)
                                        return r;
                        }
                }

#endif

                // pty stuff i guess
                if (c.forward) {
                        char last_char = 0;
                        r = pty_forward_get_last_char(c.forward, &last_char);
                        if (r >= 0 && !arg_quiet && last_char != '\n')
                                fputc('\n', stdout);
                }

                /* Try to propagate the service's return value. But if the service defines
                 * e.g. SuccessExitStatus, honour this, and return 0 to mean "success". */
                if (streq_ptr(c.result, "success"))
                        *retval = 0;
                else if (streq_ptr(c.result, "exit-code") && c.exit_status > 0)
                        *retval = c.exit_status;
                else if (streq_ptr(c.result, "signal"))
                        *retval = EXIT_EXCEPTION;
                else
                        *retval = EXIT_FAILURE;
        }
        return 0;
}
/* build/systemd-run --user --pipe --collect --property=CPUAccounting=1 --unit="test-$RANDOM" sleep 5 */

static int run(int argc, char* argv[]) {
        _cleanup_(sd_bus_flush_close_unrefp) sd_bus *bus = NULL;
        _cleanup_free_ char *description = NULL;
        int r, retval = EXIT_SUCCESS;

        log_show_color(true);
        log_parse_environment();
        log_open();

        r = parse_argv(argc, argv);
        if (r <= 0)
                return r;

        if (!strv_isempty(arg_cmdline) &&
            arg_transport == BUS_TRANSPORT_LOCAL &&
            !strv_find_startswith(arg_property, "RootDirectory=") &&
            !strv_find_startswith(arg_property, "RootImage=")) {
                /* Patch in an absolute path to fail early for user convenience, but only when we can do it
                 * (i.e. we will be running from the same file system). This also uses the user's $PATH,
                 * while we use a fixed search path in the manager. */

                _cleanup_free_ char *command = NULL;
                r = find_executable(arg_cmdline[0], &command);
                if (r < 0)
                        return log_error_errno(r, "Failed to find executable %s: %m", arg_cmdline[0]);

                printf ("command = %s\n", command);
                free_and_replace(arg_cmdline[0], command);
        }

        if (!arg_description) {
                description = strv_join(arg_cmdline, " ");
                if (!description)
                        return log_oom();

                if (arg_unit && isempty(description)) {
                        r = free_and_strdup(&description, arg_unit);
                        if (r < 0)
                                return log_oom();
                }

                arg_description = description;
        }

        /* If --wait is used connect via the bus, unconditionally, as ref/unref is not supported via the limited direct
         * connection */
        if (arg_wait || arg_stdio != ARG_STDIO_NONE) {
                //printf ("%s:%d\n", __FUNCTION__, __LINE__);
                //printf ("%d %d %d\n", arg_wait, arg_stdio != ARG_STDIO_NONE, (arg_user && arg_transport != BUS_TRANSPORT_LOCAL));
                // r = bus_connect_transport(arg_transport, arg_host, arg_user, &bus);

                r = sd_bus_default_user(&bus);
                if (r < 0) {
                        fprintf (stderr, "sd_bus_default_user\n");
                        exit (1);
                }

                r = sd_bus_set_exit_on_disconnect(bus, true);
                if (r < 0) {
                        fprintf (stderr, "sd_bus_set_exit_on_disconnect\n");
                        exit (1);
                }
        }
        else {
                // r = bus_connect_transport_systemd(arg_transport, arg_host, arg_user, &bus);

                // more complex w/ XDG_RUNTIME_DIR, ignore this for now

#if 0
                const char *e;
                char *ee;
                e = secure_getenv("XDG_RUNTIME_DIR");
                if (!e)
                        return sd_bus_default_user(&bus);

                ee = bus_address_escape(e);
                if (!ee)
                        return -ENOMEM;

                r = sd_bus_new(&bus);
                if (r < 0)
                        return r;

                bus->address = strjoin("unix:path=", ee, "/systemd/private");
                if (!bus->address)
                        return -ENOMEM;

                r = sd_bus_start(bus);
                if (r < 0)
                        return sd_bus_default_user(_bus);

                r = bus_check_peercred(bus);
                if (r < 0)
                        return r;

                free (ee);
#else
                r = sd_bus_default_user (&bus);
                if (r < 0)
                        return r;
#endif
        }

        if (r < 0)
                return bus_log_connect_error(r);

        r = start_transient_service(bus);
        if (r < 0)
                return r;

        r = wait_transient_service(bus, &retval);
        if (r < 0)
                return r;

        return retval;
}

#if 0
#define _DEFINE_MAIN_FUNCTION(intro, impl, ret)                         \
        int main(int argc, char *argv[]) {                              \
                int r;                                                  \
                save_argc_argv(argc, argv);                             \
                intro;                                                  \
                r = impl;                                               \
                if (r < 0)                                              \
                        (void) sd_notifyf(0, "ERRNO=%i", -r);           \
                ask_password_agent_close();                             \
                polkit_agent_close();                                   \
                pager_close();                                          \
                mac_selinux_finish();                                   \
                static_destruct();                                      \
                return ret;                                             \
        }

/* Negative return values from impl are mapped to EXIT_FAILURE, and
 * everything else means success! */
#define DEFINE_MAIN_FUNCTION(impl)                                      \
        _DEFINE_MAIN_FUNCTION(,impl(argc, argv), r < 0 ? EXIT_FAILURE : EXIT_SUCCESS)

/* Zero is mapped to EXIT_SUCCESS, negative values are mapped to EXIT_FAILURE,
 * and positive values are propagated.
 * Note: "true" means failure! */
#define DEFINE_MAIN_FUNCTION_WITH_POSITIVE_FAILURE(impl)                \
        _DEFINE_MAIN_FUNCTION(,impl(argc, argv), r < 0 ? EXIT_FAILURE : r)
#endif

DEFINE_MAIN_FUNCTION_WITH_POSITIVE_FAILURE(run);
