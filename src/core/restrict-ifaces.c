/* SPDX-License-Identifier: LGPL-2.1+ */

#include "fd-util.h"
#include "restrict-ifaces.h"
#include "netlink-util.h"

#if BPF_FRAMEWORK
/* libbpf, clang and llc compile time dependencies are satisfied */

#include "bpf-dlopen.h"
#include "bpf-link.h"

#include "bpf/restrict_ifaces/restrict-ifaces.skel.h"

static struct restrict_ifaces_bpf *restrict_ifaces_bpf_free(struct restrict_ifaces_bpf *obj) {
        restrict_ifaces_bpf__destroy(obj);
        return NULL;
}

DEFINE_TRIVIAL_CLEANUP_FUNC(struct restrict_ifaces_bpf *, restrict_ifaces_bpf_free);

static int prepare_restrict_ifaces_bpf(Unit* u, bool is_allow_list,
                const Set *restrict_network_interfaces,
                struct restrict_ifaces_bpf **ret_object) {
        _cleanup_(restrict_ifaces_bpf_freep) struct restrict_ifaces_bpf *obj = NULL;
        _cleanup_(sd_netlink_unrefp) sd_netlink *rtnl = NULL;
        char *iface;
        int r, map_fd;

        assert(ret_object);

        obj = restrict_ifaces_bpf__open();
        if (!obj)
                return log_unit_error_errno(u, SYNTHETIC_ERRNO(ENOMEM), "Failed to open BPF object");

        r = sym_bpf_map__resize(obj->maps.sd_restrictif, MAX(set_size(restrict_network_interfaces), 1u));
        if (r != 0)
                return log_unit_error_errno(u, r,
                                "Failed to resize BPF map '%s': %m",
                                sym_bpf_map__name(obj->maps.sd_restrictif));

        obj->rodata->is_allow_list = is_allow_list;

        r = restrict_ifaces_bpf__load(obj);
        if (r != 0)
                return log_unit_error_errno(u, r, "Failed to load BPF object: %m");

        map_fd = sym_bpf_map__fd(obj->maps.sd_restrictif);

        SET_FOREACH(iface, restrict_network_interfaces) {
                uint8_t dummy = 0;
                int ifindex;
                ifindex = rtnl_resolve_interface(&rtnl, iface);
                if (ifindex < 0) {
                        log_unit_warning_errno(u, ifindex, "Couldn't find index of network interface: %m. Ignoring '%s'", iface);
                        continue;
                }

                if (sym_bpf_map_update_elem(map_fd, &ifindex, &dummy, BPF_ANY))
                        return log_unit_error_errno(u, errno, "Failed to update BPF map '%s' fd: %m", sym_bpf_map__name(obj->maps.sd_restrictif));
        }

        *ret_object = TAKE_PTR(obj);
        return 0;
}

int restrict_network_interfaces_supported(void) {
        _cleanup_(restrict_ifaces_bpf_freep) struct restrict_ifaces_bpf *obj = NULL;
        int r;
        static int supported = -1;

        if (supported >= 0)
                return supported;

        r = cg_unified_controller(SYSTEMD_CGROUP_CONTROLLER);
        if (r < 0) {
                log_warning_errno(r, "Can't determine whether the unified hierarchy is used: %m");
                supported = 0;
                return supported;
        }
        if (r == 0) {
                log_debug_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                                "Not running with unified cgroup hierarchy, BPF is not supported");
                supported = 0;
                return supported;
        }

        if (dlopen_bpf() < 0)
                return false;

        if (!sym_bpf_probe_prog_type(BPF_PROG_TYPE_CGROUP_SKB, /*ifindex=*/0)) {
                log_debug_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                                "BPF program type cgroup_skb is not supported");
                supported = 0;
                return supported;
        }

        r = prepare_restrict_ifaces_bpf(NULL, true, NULL, &obj);
        if (r < 0)
                return log_debug_errno(r, "Failed to load BPF object: %m");

        supported = bpf_can_link_program(obj->progs.sd_restrictif_i);
        return supported;
}

static int restrict_network_interfaces_install_impl(Unit *u) {
        _cleanup_(bpf_link_freep) struct bpf_link *egress_link = NULL, *ingress_link = NULL;
        _cleanup_(restrict_ifaces_bpf_freep) struct restrict_ifaces_bpf *obj = NULL;
        _cleanup_free_ char *cgroup_path = NULL;
        _cleanup_close_ int cgroup_fd = -1;
        CGroupContext *cc;
        int r;

        cc = unit_get_cgroup_context(u);
        if (!cc)
                return 0;

        r = cg_get_path(SYSTEMD_CGROUP_CONTROLLER, u->cgroup_path, NULL, &cgroup_path);
        if (r < 0)
                return log_unit_error_errno(u, r, "Failed to get cgroup path: %m");

        if (!cc->restrict_network_interfaces)
                return 0;

        r = prepare_restrict_ifaces_bpf(u,
                cc->restrict_network_interfaces_is_allow_list,
                cc->restrict_network_interfaces,
                &obj);
        if (r < 0)
                return r;

        cgroup_fd = open(cgroup_path, O_RDONLY | O_CLOEXEC | O_DIRECTORY, 0);
        if (cgroup_fd < 0)
                return -errno;

        ingress_link = sym_bpf_program__attach_cgroup(obj->progs.sd_restrictif_i, cgroup_fd);
        r = sym_libbpf_get_error(ingress_link);
        if (r != 0)
                return log_unit_error_errno(u, r, "Failed to create ingress cgroup link: %m");

        egress_link = sym_bpf_program__attach_cgroup(obj->progs.sd_restrictif_e, cgroup_fd);
        r = sym_libbpf_get_error(egress_link);
        if (r != 0)
                return log_unit_error_errno(u, r, "Failed to create egress cgroup link: %m");

        u->restrict_ifaces_ingress_bpf_link = TAKE_PTR(ingress_link);
        u->restrict_ifaces_egress_bpf_link = TAKE_PTR(egress_link);

        return 0;
}

int restrict_network_interfaces_install(Unit *u) {
        int r = restrict_network_interfaces_install_impl(u);
        fdset_close(u->initial_restric_ifaces_link_fds);
        return r;
}

int serialize_restrict_network_interfaces(Unit *u, FILE *f, FDSet *fds) {
        int r;

        assert(u);

        r = bpf_serialize_link(f, fds, "restrict-ifaces-bpf-fd", u->restrict_ifaces_ingress_bpf_link);
        if (r < 0)
                return r;

        return bpf_serialize_link(f, fds, "restrict-ifaces-bpf-fd", u->restrict_ifaces_egress_bpf_link);
}

int restrict_network_interfaces_add_initial_link_fd(Unit *u, int fd) {
        int r;

        assert(u);

        if (!u->initial_restric_ifaces_link_fds) {
                u->initial_restric_ifaces_link_fds = fdset_new();
                if (!u->initial_restric_ifaces_link_fds)
                        return log_oom();
        }

        r = fdset_put(u->initial_restric_ifaces_link_fds, fd);
        if (r < 0)
                return log_unit_error_errno(u, r, "Failed to put restrict-ifaces-bpf-fd %d to restored fdset: %m", fd);

        return 0;
}

#else /* ! BPF_FRAMEWORK */
int restrict_network_interfaces_supported(void) {
        return 0;
}

int restrict_network_interfaces_install(Unit *u) {
        return log_unit_debug_errno(u, SYNTHETIC_ERRNO(EOPNOTSUPP),
                        "Failed to install RestrictInterfaces: BPF programs built from source code are not supported: %m");
}

int serialize_restrict_network_interfaces(Unit *u, FILE *f, FDSet *fds) {
        return 0;
}

int restrict_network_interfaces_add_initial_link_fd(Unit *u, int fd) {
        return 0;
}
#endif
