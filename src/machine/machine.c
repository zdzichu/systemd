/***
  This file is part of systemd.

  Copyright 2011 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "sd-messages.h"

#include "alloc-util.h"
#include "bus-error.h"
#include "bus-util.h"
#include "escape.h"
#include "extract-word.h"
#include "fd-util.h"
#include "fileio.h"
#include "format-util.h"
#include "hashmap.h"
#include "machine-dbus.h"
#include "machine.h"
#include "mkdir.h"
#include "parse-util.h"
#include "process-util.h"
#include "special.h"
#include "string-table.h"
#include "terminal-util.h"
#include "unit-name.h"
#include "util.h"

Machine* machine_new(Manager *manager, MachineClass class, const char *name) {
        Machine *m;

        assert(manager);
        assert(class < _MACHINE_CLASS_MAX);
        assert(name);

        /* Passing class == _MACHINE_CLASS_INVALID here is fine. It
         * means as much as "we don't know yet", and that we'll figure
         * it out later when loading the state file. */

        m = new0(Machine, 1);
        if (!m)
                return NULL;

        m->name = strdup(name);
        if (!m->name)
                goto fail;

        if (class != MACHINE_HOST) {
                m->state_file = strappend("/run/systemd/machines/", m->name);
                if (!m->state_file)
                        goto fail;
        }

        m->class = class;

        if (hashmap_put(manager->machines, m->name, m) < 0)
                goto fail;

        m->manager = manager;

        return m;

fail:
        free(m->state_file);
        free(m->name);
        return mfree(m);
}

void machine_free(Machine *m) {
        assert(m);

        while (m->operations)
                operation_free(m->operations);

        if (m->in_gc_queue)
                LIST_REMOVE(gc_queue, m->manager->machine_gc_queue, m);

        machine_release_unit(m);

        free(m->scope_job);

        (void) hashmap_remove(m->manager->machines, m->name);

        if (m->manager->host_machine == m)
                m->manager->host_machine = NULL;

        if (m->leader > 0)
                (void) hashmap_remove_value(m->manager->machine_leaders, PID_TO_PTR(m->leader), m);

        sd_bus_message_unref(m->create_message);

        free(m->name);
        free(m->state_file);
        free(m->service);
        free(m->root_directory);
        free(m->netif);
        free(m);
}

int machine_save(Machine *m) {
        _cleanup_free_ char *temp_path = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        assert(m);

        if (!m->state_file)
                return 0;

        if (!m->started)
                return 0;

        r = mkdir_safe_label("/run/systemd/machines", 0755, 0, 0);
        if (r < 0)
                goto fail;

        r = fopen_temporary(m->state_file, &f, &temp_path);
        if (r < 0)
                goto fail;

        (void) fchmod(fileno(f), 0644);

        fprintf(f,
                "# This is private data. Do not parse.\n"
                "NAME=%s\n",
                m->name);

        if (m->unit) {
                _cleanup_free_ char *escaped;

                escaped = cescape(m->unit);
                if (!escaped) {
                        r = -ENOMEM;
                        goto fail;
                }

                fprintf(f, "SCOPE=%s\n", escaped); /* We continue to call this "SCOPE=" because it is internal only, and we want to stay compatible with old files */
        }

        if (m->scope_job)
                fprintf(f, "SCOPE_JOB=%s\n", m->scope_job);

        if (m->service) {
                _cleanup_free_ char *escaped;

                escaped = cescape(m->service);
                if (!escaped) {
                        r = -ENOMEM;
                        goto fail;
                }
                fprintf(f, "SERVICE=%s\n", escaped);
        }

        if (m->root_directory) {
                _cleanup_free_ char *escaped;

                escaped = cescape(m->root_directory);
                if (!escaped) {
                        r = -ENOMEM;
                        goto fail;
                }
                fprintf(f, "ROOT=%s\n", escaped);
        }

        if (!sd_id128_is_null(m->id))
                fprintf(f, "ID=" SD_ID128_FORMAT_STR "\n", SD_ID128_FORMAT_VAL(m->id));

        if (m->leader != 0)
                fprintf(f, "LEADER="PID_FMT"\n", m->leader);

        if (m->class != _MACHINE_CLASS_INVALID)
                fprintf(f, "CLASS=%s\n", machine_class_to_string(m->class));

        if (dual_timestamp_is_set(&m->timestamp))
                fprintf(f,
                        "REALTIME="USEC_FMT"\n"
                        "MONOTONIC="USEC_FMT"\n",
                        m->timestamp.realtime,
                        m->timestamp.monotonic);

        if (m->n_netif > 0) {
                unsigned i;

                fputs("NETIF=", f);

                for (i = 0; i < m->n_netif; i++) {
                        if (i != 0)
                                fputc(' ', f);

                        fprintf(f, "%i", m->netif[i]);
                }

                fputc('\n', f);
        }

        r = fflush_and_check(f);
        if (r < 0)
                goto fail;

        if (rename(temp_path, m->state_file) < 0) {
                r = -errno;
                goto fail;
        }

        if (m->unit) {
                char *sl;

                /* Create a symlink from the unit name to the machine
                 * name, so that we can quickly find the machine for
                 * each given unit. Ignore error. */
                sl = strjoina("/run/systemd/machines/unit:", m->unit);
                (void) symlink(m->name, sl);
        }

        return 0;

fail:
        (void) unlink(m->state_file);

        if (temp_path)
                (void) unlink(temp_path);

        return log_error_errno(r, "Failed to save machine data %s: %m", m->state_file);
}

static void machine_unlink(Machine *m) {
        assert(m);

        if (m->unit) {

                char *sl;

                sl = strjoina("/run/systemd/machines/unit:", m->unit);
                (void) unlink(sl);
        }

        if (m->state_file)
                (void) unlink(m->state_file);
}

int machine_load(Machine *m) {
        _cleanup_free_ char *realtime = NULL, *monotonic = NULL, *id = NULL, *leader = NULL, *class = NULL, *netif = NULL;
        int r;

        assert(m);

        if (!m->state_file)
                return 0;

        r = parse_env_file(m->state_file, NEWLINE,
                           "SCOPE",     &m->unit,
                           "SCOPE_JOB", &m->scope_job,
                           "SERVICE",   &m->service,
                           "ROOT",      &m->root_directory,
                           "ID",        &id,
                           "LEADER",    &leader,
                           "CLASS",     &class,
                           "REALTIME",  &realtime,
                           "MONOTONIC", &monotonic,
                           "NETIF",     &netif,
                           NULL);
        if (r < 0) {
                if (r == -ENOENT)
                        return 0;

                return log_error_errno(r, "Failed to read %s: %m", m->state_file);
        }

        if (id)
                sd_id128_from_string(id, &m->id);

        if (leader)
                parse_pid(leader, &m->leader);

        if (class) {
                MachineClass c;

                c = machine_class_from_string(class);
                if (c >= 0)
                        m->class = c;
        }

        if (realtime)
                timestamp_deserialize(realtime, &m->timestamp.realtime);
        if (monotonic)
                timestamp_deserialize(monotonic, &m->timestamp.monotonic);

        if (netif) {
                size_t allocated = 0, nr = 0;
                const char *p;
                int *ni = NULL;

                p = netif;
                for (;;) {
                        _cleanup_free_ char *word = NULL;
                        int ifi;

                        r = extract_first_word(&p, &word, NULL, 0);
                        if (r == 0)
                                break;
                        if (r == -ENOMEM)
                                return log_oom();
                        if (r < 0) {
                                log_warning_errno(r, "Failed to parse NETIF: %s", netif);
                                break;
                        }

                        if (parse_ifindex(word, &ifi) < 0)
                                continue;

                        if (!GREEDY_REALLOC(ni, allocated, nr+1)) {
                                free(ni);
                                return log_oom();
                        }

                        ni[nr++] = ifi;
                }

                free(m->netif);
                m->netif = ni;
                m->n_netif = nr;
        }

        return r;
}

static int machine_start_scope(Machine *m, sd_bus_message *properties, sd_bus_error *error) {
        int r = 0;

        assert(m);
        assert(m->class != MACHINE_HOST);

        if (!m->unit) {
                _cleanup_free_ char *escaped = NULL;
                char *scope, *description, *job = NULL;

                escaped = unit_name_escape(m->name);
                if (!escaped)
                        return log_oom();

                scope = strjoin("machine-", escaped, ".scope");
                if (!scope)
                        return log_oom();

                description = strjoina(m->class == MACHINE_VM ? "Virtual Machine " : "Container ", m->name);

                r = manager_start_scope(m->manager, scope, m->leader, SPECIAL_MACHINE_SLICE, description, properties, error, &job);
                if (r < 0) {
                        log_error("Failed to start machine scope: %s", bus_error_message(error, r));
                        free(scope);
                        return r;
                } else {
                        m->unit = scope;

                        free(m->scope_job);
                        m->scope_job = job;
                }
        }

        if (m->unit)
                hashmap_put(m->manager->machine_units, m->unit, m);

        return r;
}

int machine_start(Machine *m, sd_bus_message *properties, sd_bus_error *error) {
        int r;

        assert(m);

        if (!IN_SET(m->class, MACHINE_CONTAINER, MACHINE_VM))
                return -EOPNOTSUPP;

        if (m->started)
                return 0;

        r = hashmap_put(m->manager->machine_leaders, PID_TO_PTR(m->leader), m);
        if (r < 0)
                return r;

        /* Create cgroup */
        r = machine_start_scope(m, properties, error);
        if (r < 0)
                return r;

        log_struct(LOG_INFO,
                   LOG_MESSAGE_ID(SD_MESSAGE_MACHINE_START),
                   "NAME=%s", m->name,
                   "LEADER="PID_FMT, m->leader,
                   LOG_MESSAGE("New machine %s.", m->name),
                   NULL);

        if (!dual_timestamp_is_set(&m->timestamp))
                dual_timestamp_get(&m->timestamp);

        m->started = true;

        /* Save new machine data */
        machine_save(m);

        machine_send_signal(m, true);

        return 0;
}

static int machine_stop_scope(Machine *m) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        char *job = NULL;
        int r;

        assert(m);
        assert(m->class != MACHINE_HOST);

        if (!m->unit)
                return 0;

        r = manager_stop_unit(m->manager, m->unit, &error, &job);
        if (r < 0) {
                log_error("Failed to stop machine scope: %s", bus_error_message(&error, r));
                return r;
        }

        free(m->scope_job);
        m->scope_job = job;

        return 0;
}

int machine_stop(Machine *m) {
        int r;
        assert(m);

        if (!IN_SET(m->class, MACHINE_CONTAINER, MACHINE_VM))
                return -EOPNOTSUPP;

        r = machine_stop_scope(m);

        m->stopping = true;

        machine_save(m);

        return r;
}

int machine_finalize(Machine *m) {
        assert(m);

        if (m->started)
                log_struct(LOG_INFO,
                           LOG_MESSAGE_ID(SD_MESSAGE_MACHINE_STOP),
                           "NAME=%s", m->name,
                           "LEADER="PID_FMT, m->leader,
                           LOG_MESSAGE("Machine %s terminated.", m->name),
                           NULL);

        machine_unlink(m);
        machine_add_to_gc_queue(m);

        if (m->started) {
                machine_send_signal(m, false);
                m->started = false;
        }

        return 0;
}

bool machine_check_gc(Machine *m, bool drop_not_started) {
        assert(m);

        if (m->class == MACHINE_HOST)
                return true;

        if (drop_not_started && !m->started)
                return false;

        if (m->scope_job && manager_job_is_active(m->manager, m->scope_job))
                return true;

        if (m->unit && manager_unit_is_active(m->manager, m->unit))
                return true;

        return false;
}

void machine_add_to_gc_queue(Machine *m) {
        assert(m);

        if (m->in_gc_queue)
                return;

        LIST_PREPEND(gc_queue, m->manager->machine_gc_queue, m);
        m->in_gc_queue = true;
}

MachineState machine_get_state(Machine *s) {
        assert(s);

        if (s->class == MACHINE_HOST)
                return MACHINE_RUNNING;

        if (s->stopping)
                return MACHINE_CLOSING;

        if (s->scope_job)
                return MACHINE_OPENING;

        return MACHINE_RUNNING;
}

int machine_kill(Machine *m, KillWho who, int signo) {
        assert(m);

        if (!IN_SET(m->class, MACHINE_VM, MACHINE_CONTAINER))
                return -EOPNOTSUPP;

        if (!m->unit)
                return -ESRCH;

        if (who == KILL_LEADER) {
                /* If we shall simply kill the leader, do so directly */

                if (kill(m->leader, signo) < 0)
                        return -errno;

                return 0;
        }

        /* Otherwise, make PID 1 do it for us, for the entire cgroup */
        return manager_kill_unit(m->manager, m->unit, signo, NULL);
}

int machine_openpt(Machine *m, int flags) {
        assert(m);

        switch (m->class) {

        case MACHINE_HOST: {
                int fd;

                fd = posix_openpt(flags);
                if (fd < 0)
                        return -errno;

                if (unlockpt(fd) < 0)
                        return -errno;

                return fd;
        }

        case MACHINE_CONTAINER:
                if (m->leader <= 0)
                        return -EINVAL;

                return openpt_in_namespace(m->leader, flags);

        default:
                return -EOPNOTSUPP;
        }
}

int machine_open_terminal(Machine *m, const char *path, int mode) {
        assert(m);

        switch (m->class) {

        case MACHINE_HOST:
                return open_terminal(path, mode);

        case MACHINE_CONTAINER:
                if (m->leader <= 0)
                        return -EINVAL;

                return open_terminal_in_namespace(m->leader, path, mode);

        default:
                return -EOPNOTSUPP;
        }
}

void machine_release_unit(Machine *m) {
        assert(m);

        if (!m->unit)
                return;

        (void) hashmap_remove(m->manager->machine_units, m->unit);
        m->unit = mfree(m->unit);
}

static const char* const machine_class_table[_MACHINE_CLASS_MAX] = {
        [MACHINE_CONTAINER] = "container",
        [MACHINE_VM] = "vm",
        [MACHINE_HOST] = "host",
};

DEFINE_STRING_TABLE_LOOKUP(machine_class, MachineClass);

static const char* const machine_state_table[_MACHINE_STATE_MAX] = {
        [MACHINE_OPENING] = "opening",
        [MACHINE_RUNNING] = "running",
        [MACHINE_CLOSING] = "closing"
};

DEFINE_STRING_TABLE_LOOKUP(machine_state, MachineState);

static const char* const kill_who_table[_KILL_WHO_MAX] = {
        [KILL_LEADER] = "leader",
        [KILL_ALL] = "all"
};

DEFINE_STRING_TABLE_LOOKUP(kill_who, KillWho);
