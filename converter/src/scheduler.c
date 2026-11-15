#include "job_internal.h"

typedef struct rfc_worker_slot {
    rfc_thread thread;
    rfc_scheduler *sched;
    uint32_t index;
    int spawned;
} rfc_worker_slot;

struct rfc_scheduler {
    rfc_mutex lock;
    rfc_cond work_cond;
    rfc_cond drain_cond;

    atomic_size_t internal_refs;

    rfc_scheduler_config cfg;
    rfc_scheduler_stats stats;

    rfc_job_id next_job_id;
    int destroying;
    uint32_t active_callbacks;

    rfc_job **queue;
    size_t queue_cap;
    size_t queue_len;

    rfc_job *active_jobs[RFC_SCHED_MAX_WORKERS];
    rfc_worker_slot workers[RFC_SCHED_MAX_WORKERS];
};

static int mul_u64_sched(uint64_t a, uint64_t b, uint64_t *out) {
    if (a != 0 && b > UINT64_MAX / a) {
        return 0;
    }
    *out = a * b;
    return 1;
}

void rfc_scheduler_internal_retain(rfc_scheduler *sched) {
    if (sched) {
        atomic_fetch_add_explicit(&sched->internal_refs, 1u,
                                  memory_order_relaxed);
    }
}

void rfc_scheduler_internal_release(rfc_scheduler *sched) {
    if (!sched) {
        return;
    }
    if (atomic_fetch_sub_explicit(&sched->internal_refs, 1u,
                                  memory_order_acq_rel) == 1u) {
        free(sched->queue);
        rfc_cond_destroy(&sched->drain_cond);
        rfc_cond_destroy(&sched->work_cond);
        rfc_mutex_destroy(&sched->lock);
        free(sched);
    }
}

int rfc_scheduler_internal_is_destroying(rfc_scheduler *sched) {
    if (!sched) {
        return 1;
    }
    rfc_mutex_lock(&sched->lock);
    int d = sched->destroying;
    rfc_mutex_unlock(&sched->lock);
    return d;
}

void rfc_scheduler_internal_callback_enter(rfc_scheduler *sched,
                                           int *allowed_out) {
    if (!sched || !allowed_out) {
        return;
    }
    rfc_mutex_lock(&sched->lock);
    if (sched->destroying) {
        *allowed_out = 0;
    } else {
        *allowed_out = 1;
        sched->active_callbacks++;
    }
    rfc_mutex_unlock(&sched->lock);
}

void rfc_scheduler_internal_callback_leave(rfc_scheduler *sched) {
    if (!sched) {
        return;
    }
    rfc_mutex_lock(&sched->lock);
    if (sched->active_callbacks > 0) {
        sched->active_callbacks--;
    }
    if (sched->active_callbacks == 0 && sched->stats.active_workers == 0 &&
        sched->queue_len == 0) {
        rfc_cond_broadcast(&sched->drain_cond);
    }
    rfc_mutex_unlock(&sched->lock);
}

void rfc_scheduler_internal_on_cancel_count(rfc_scheduler *sched) {
    (void)sched;
}

void rfc_scheduler_internal_on_publish_cancelled(rfc_scheduler *sched) {
    if (!sched) {
        return;
    }
    rfc_mutex_lock(&sched->lock);
    sched->stats.published_cancelled_count++;
    rfc_mutex_unlock(&sched->lock);
}

void rfc_scheduler_internal_adjust_live_partials(rfc_scheduler *sched,
                                                 int64_t delta_bytes) {
    if (!sched || delta_bytes == 0) {
        return;
    }
    rfc_mutex_lock(&sched->lock);
    if (delta_bytes > 0) {
        sched->stats.live_partial_bytes += (uint64_t)delta_bytes;
    } else {
        uint64_t sub = (uint64_t)(-delta_bytes);
        if (sched->stats.live_partial_bytes >= sub) {
            sched->stats.live_partial_bytes -= sub;
        } else {
            sched->stats.live_partial_bytes = 0;
        }
    }
    rfc_mutex_unlock(&sched->lock);
}

static int collect_active_cancel_locked(rfc_job *job,
                                        rfc_cancel_snapshot *out) {
    memset(out, 0, sizeof(*out));
    int has_sess = (job->active_sess_adapter &&
                    job->active_sess_adapter->cancel_session &&
                    job->active_session);
    int has_leg = (job->active_legacy_adapter &&
                   job->active_legacy_adapter->cancel);
    if (!has_sess && !has_leg) {
        return 0;
    }
    out->job = job;
    out->sess_adapter = has_sess ? job->active_sess_adapter : NULL;
    out->session = has_sess ? job->active_session : NULL;
    out->legacy_adapter = has_leg ? job->active_legacy_adapter : NULL;
    job->active_cancel_calls++;
    rfc_job_retain(job);
    return 1;
}

static void finish_active_cancel_unlocked(rfc_cancel_snapshot *snap) {
    if (!snap || !snap->job) {
        return;
    }
    if (snap->sess_adapter && snap->sess_adapter->cancel_session &&
        snap->session) {
        snap->sess_adapter->cancel_session(snap->session);
    }
    if (snap->legacy_adapter && snap->legacy_adapter->cancel) {
        snap->legacy_adapter->cancel(snap->legacy_adapter->ctx);
    }
    rfc_mutex_lock(&snap->job->lock);
    if (snap->job->active_cancel_calls > 0) {
        snap->job->active_cancel_calls--;
    }
    rfc_cond_broadcast(&snap->job->done_cond);
    rfc_mutex_unlock(&snap->job->lock);
    rfc_job_release(snap->job);
    memset(snap, 0, sizeof(*snap));
}

static rfc_job *pop_queue_at(rfc_scheduler *sched, size_t idx) {
    rfc_job *j = sched->queue[idx];
    if (idx + 1u < sched->queue_len) {
        memmove(&sched->queue[idx], &sched->queue[idx + 1u],
                (sched->queue_len - idx - 1u) * sizeof(sched->queue[0]));
    }
    sched->queue_len--;
    sched->stats.queued_jobs = (uint32_t)sched->queue_len;
    return j;
}

rfc_status rfc_scheduler_internal_cancel_job(rfc_scheduler *sched, rfc_job *job) {
    if (!sched || !job) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_cancel_snapshot csnap;
    memset(&csnap, 0, sizeof(csnap));
    rfc_job *drop_job = NULL;

    rfc_mutex_lock(&sched->lock);
    rfc_mutex_lock(&job->lock);

    if (job->published || job->state == RFC_JOB_SUCCEEDED ||
        job->state == RFC_JOB_FAILED || job->state == RFC_JOB_CANCELLED ||
        job->state == RFC_JOB_SUPERSEDED) {
        rfc_mutex_unlock(&job->lock);
        rfc_mutex_unlock(&sched->lock);
        return RFC_OK;
    }

    job->cancel_requested = 1;
    job->suppress_callbacks = 1;

    if (job->state == RFC_JOB_QUEUED) {
        job->state = RFC_JOB_CANCELLED;
        job->status = RFC_ERR_CANCELLED;
        job->failing_stage = job->stage;
        rfc_job_internal_set_detail(job->detail, "cancelled before execution");
        rfc_cond_broadcast(&job->done_cond);

        for (size_t i = 0; i < sched->queue_len; i++) {
            if (sched->queue[i] == job) {
                drop_job = pop_queue_at(sched, i);
                break;
            }
        }
        sched->stats.total_cancelled++;
        if (sched->stats.active_workers == 0 && sched->queue_len == 0 &&
            sched->active_callbacks == 0) {
            rfc_cond_broadcast(&sched->drain_cond);
        }
        rfc_mutex_unlock(&job->lock);
        rfc_mutex_unlock(&sched->lock);
        rfc_job_release_partials(job);
        if (drop_job) {
            rfc_job_release(drop_job);
        }
        return RFC_OK;
    }

    collect_active_cancel_locked(job, &csnap);
    rfc_mutex_unlock(&job->lock);
    rfc_mutex_unlock(&sched->lock);

    finish_active_cancel_unlocked(&csnap);
    return RFC_OK;
}

static void worker_main(void *arg) {
    rfc_worker_slot *w = (rfc_worker_slot *)arg;
    rfc_scheduler *sched = w->sched;

    rfc_mutex_lock(&sched->lock);
    for (;;) {
        if (sched->destroying) {
            break;
        }

        while (sched->queue_len > 0) {
            rfc_job *head = sched->queue[0];
            rfc_mutex_lock(&head->lock);
            int dead = head->cancel_requested || head->superseded ||
                       head->superseded_by_id != RFC_JOB_ID_NONE ||
                       head->state == RFC_JOB_CANCELLED ||
                       head->state == RFC_JOB_SUPERSEDED;
            if (dead && head->state == RFC_JOB_QUEUED) {
                head->state = head->superseded ? RFC_JOB_SUPERSEDED
                                               : RFC_JOB_CANCELLED;
                head->status = head->superseded ? RFC_ERR_STALE_JOB
                                                : RFC_ERR_CANCELLED;
                if (head->superseded) {
                    sched->stats.total_superseded++;
                } else {
                    sched->stats.total_cancelled++;
                }
                rfc_cond_broadcast(&head->done_cond);
            }
            rfc_mutex_unlock(&head->lock);
            if (!dead) {
                break;
            }
            pop_queue_at(sched, 0);
            rfc_mutex_unlock(&sched->lock);
            rfc_job_release_partials(head);
            rfc_job_release(head);
            rfc_mutex_lock(&sched->lock);
        }

        int can_run = 0;
        uint32_t needed_threads = 0;
        uint64_t needed_mem = 0;

        if (!sched->destroying &&
            w->index < sched->cfg.max_workers &&
            sched->queue_len > 0 &&
            sched->stats.active_workers < sched->cfg.max_workers) {
            rfc_job *head = sched->queue[0];
            uint32_t req = head->requested_threads ? head->requested_threads : 1u;
            needed_threads = req > sched->cfg.total_cpu_threads
                                 ? sched->cfg.total_cpu_threads
                                 : req;
            if (needed_threads == 0) {
                needed_threads = 1u;
            }
            needed_mem = head->reserved_memory_bytes;

            int cpu_ok = (sched->stats.active_cpu_threads + needed_threads <=
                          sched->cfg.total_cpu_threads);
            int mem_ok =
                (sched->stats.active_reserved_bytes + needed_mem <=
                 sched->cfg.memory_budget_bytes) ||
                (needed_mem > sched->cfg.memory_budget_bytes &&
                 sched->stats.active_reserved_bytes == 0);
            if (cpu_ok && mem_ok) {
                can_run = 1;
            }
        }

        if (!can_run) {
            if (sched->stats.active_workers == 0 && sched->queue_len == 0 &&
                sched->active_callbacks == 0) {
                rfc_cond_broadcast(&sched->drain_cond);
            }
            rfc_cond_wait(&sched->work_cond, &sched->lock);
            continue;
        }

        rfc_job *job = pop_queue_at(sched, 0);
        sched->active_jobs[w->index] = job;
        sched->stats.active_workers++;
        if (sched->stats.active_workers > sched->stats.peak_workers) {
            sched->stats.peak_workers = sched->stats.active_workers;
        }
        sched->stats.active_cpu_threads += needed_threads;
        if (sched->stats.active_cpu_threads > sched->stats.peak_cpu_threads) {
            sched->stats.peak_cpu_threads = sched->stats.active_cpu_threads;
        }
        sched->stats.active_reserved_bytes += needed_mem;
        if (sched->stats.active_reserved_bytes > sched->stats.peak_reserved_bytes) {
            sched->stats.peak_reserved_bytes = sched->stats.active_reserved_bytes;
        }

        rfc_mutex_lock(&job->lock);
        job->admitted_threads = needed_threads;
        job->state = RFC_JOB_RUNNING;
        rfc_mutex_unlock(&job->lock);
        rfc_mutex_unlock(&sched->lock);

        rfc_job_exec_result exec_res;
        memset(&exec_res, 0, sizeof(exec_res));
        rfc_job_internal_execute(job, &exec_res);

        /* Release scheduler reservations AND commit job terminal state atomically before waking waiters. */
        rfc_mutex_lock(&sched->lock);
        rfc_mutex_lock(&job->lock);

        int abort_sup =
            job->superseded || job->superseded_by_id != RFC_JOB_ID_NONE;
        int abort_can =
            job->cancel_requested || job->state == RFC_JOB_CANCELLED;
        if (job->published && exec_res.st == RFC_OK) {
            job->stage = RFC_STAGE_DONE;
            job->state = RFC_JOB_SUCCEEDED;
            job->status = RFC_OK;
            job->failing_stage = RFC_STAGE_NONE;
            if (job->detail[0] == '\0') {
                rfc_job_internal_set_detail(job->detail, "ok");
            }
        } else if (abort_sup) {
            job->state = RFC_JOB_SUPERSEDED;
            job->status = RFC_ERR_STALE_JOB;
            job->failing_stage = exec_res.stage_at;
            rfc_job_internal_set_detail(job->detail, "superseded by retry");
        } else if (abort_can || exec_res.st == RFC_ERR_CANCELLED) {
            job->state = RFC_JOB_CANCELLED;
            job->status = RFC_ERR_CANCELLED;
            job->failing_stage = exec_res.stage_at;
            rfc_job_internal_set_detail(job->detail, "cancelled");
            if (job->published) {
                sched->stats.published_cancelled_count++;
            }
        } else if (exec_res.st == RFC_OK) {
            job->stage = RFC_STAGE_DONE;
            job->state = RFC_JOB_SUCCEEDED;
            job->status = RFC_OK;
            job->failing_stage = RFC_STAGE_NONE;
            if (job->detail[0] == '\0') {
                rfc_job_internal_set_detail(job->detail, "ok");
            }
        } else {
            job->state = RFC_JOB_FAILED;
            job->status = exec_res.st;
            job->failing_stage = exec_res.stage_at;
            if (job->detail[0] == '\0') {
                rfc_job_internal_set_detail(job->detail,
                                            exec_res.err_detail
                                                ? exec_res.err_detail
                                                : rfc_status_name(exec_res.st));
            }
        }
        job->suppress_callbacks = 1;
        rfc_job_state term_state = job->state;

        sched->active_jobs[w->index] = NULL;
        sched->stats.active_workers--;
        sched->stats.active_cpu_threads -= needed_threads;
        sched->stats.active_reserved_bytes -= needed_mem;
        if (term_state == RFC_JOB_SUCCEEDED) {
            sched->stats.total_completed++;
        } else if (term_state == RFC_JOB_CANCELLED) {
            sched->stats.total_cancelled++;
        } else if (term_state == RFC_JOB_SUPERSEDED) {
            sched->stats.total_superseded++;
        } else {
            sched->stats.total_failed++;
        }

        rfc_cond_broadcast(&job->done_cond);
        rfc_cond_broadcast(&sched->work_cond);
        if (sched->stats.active_workers == 0 && sched->queue_len == 0 &&
            sched->active_callbacks == 0) {
            rfc_cond_broadcast(&sched->drain_cond);
        }
        rfc_mutex_unlock(&job->lock);
        rfc_mutex_unlock(&sched->lock);

        rfc_job_release(job);
        rfc_mutex_lock(&sched->lock);
    }
    rfc_mutex_unlock(&sched->lock);
}

void rfc_scheduler_config_default(rfc_scheduler_config *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->max_workers = RFC_SCHED_DEFAULT_WORKERS;
    out->total_cpu_threads = RFC_SCHED_DEFAULT_CPU_THREADS;
    out->max_queue_depth = RFC_SCHED_DEFAULT_QUEUE_DEPTH;
    out->memory_budget_bytes = RFC_SCHED_DEFAULT_MEMORY_BUDGET;
    rfc_limits_default(&out->default_limits);
}

rfc_status rfc_scheduler_config_validate(const rfc_scheduler_config *cfg) {
    if (!cfg) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (cfg->max_workers < RFC_SCHED_MIN_WORKERS ||
        cfg->max_workers > RFC_SCHED_MAX_WORKERS ||
        cfg->total_cpu_threads < RFC_SCHED_MIN_CPU_THREADS ||
        cfg->total_cpu_threads > RFC_SCHED_MAX_CPU_THREADS ||
        cfg->max_queue_depth < RFC_SCHED_MIN_QUEUE_DEPTH ||
        cfg->max_queue_depth > RFC_SCHED_MAX_QUEUE_DEPTH ||
        cfg->memory_budget_bytes < RFC_SCHED_MIN_MEMORY_BUDGET ||
        cfg->memory_budget_bytes > RFC_SCHED_MAX_MEMORY_BUDGET) {
        return RFC_ERR_SETTING_VALUE;
    }
    return rfc_limits_validate(&cfg->default_limits);
}

static int ensure_workers_started_locked(rfc_scheduler *sched, uint32_t count) {
    for (uint32_t i = 0; i < count && i < RFC_SCHED_MAX_WORKERS; i++) {
        if (!sched->workers[i].spawned) {
            sched->workers[i].sched = sched;
            sched->workers[i].index = i;
            if (!rfc_thread_start_fn(&sched->workers[i].thread, worker_main,
                                     &sched->workers[i])) {
                return 0;
            }
            sched->workers[i].spawned = 1;
        }
    }
    return 1;
}

rfc_scheduler *rfc_scheduler_create(const rfc_scheduler_config *cfg) {
    rfc_scheduler_config eff;
    if (cfg) {
        eff = *cfg;
    } else {
        rfc_scheduler_config_default(&eff);
    }
    if (rfc_scheduler_config_validate(&eff) != RFC_OK) {
        return NULL;
    }

    rfc_scheduler *sched = (rfc_scheduler *)calloc(1u, sizeof(*sched));
    if (!sched) {
        return NULL;
    }
    if (!rfc_mutex_init(&sched->lock)) {
        free(sched);
        return NULL;
    }
    if (!rfc_cond_init(&sched->work_cond)) {
        rfc_mutex_destroy(&sched->lock);
        free(sched);
        return NULL;
    }
    if (!rfc_cond_init(&sched->drain_cond)) {
        rfc_cond_destroy(&sched->work_cond);
        rfc_mutex_destroy(&sched->lock);
        free(sched);
        return NULL;
    }

    sched->queue = (rfc_job **)calloc(eff.max_queue_depth, sizeof(sched->queue[0]));
    if (!sched->queue) {
        rfc_cond_destroy(&sched->drain_cond);
        rfc_cond_destroy(&sched->work_cond);
        rfc_mutex_destroy(&sched->lock);
        free(sched);
        return NULL;
    }
    sched->queue_cap = eff.max_queue_depth;
    sched->cfg = eff;
    sched->next_job_id = 1u;
    atomic_init(&sched->internal_refs, 1u);

    rfc_mutex_lock(&sched->lock);
    if (!ensure_workers_started_locked(sched, eff.max_workers)) {
        sched->destroying = 1;
        rfc_cond_broadcast(&sched->work_cond);
        rfc_mutex_unlock(&sched->lock);
        for (uint32_t i = 0; i < RFC_SCHED_MAX_WORKERS; i++) {
            if (sched->workers[i].spawned) {
                rfc_thread_join(&sched->workers[i].thread);
                sched->workers[i].spawned = 0;
            }
        }
        rfc_scheduler_internal_release(sched);
        return NULL;
    }
    rfc_mutex_unlock(&sched->lock);
    return sched;
}

rfc_status rfc_scheduler_reconfigure(rfc_scheduler *sched,
                                     const rfc_scheduler_config *cfg) {
    if (!sched || !cfg) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_status st = rfc_scheduler_config_validate(cfg);
    if (st != RFC_OK) {
        return st;
    }

    rfc_mutex_lock(&sched->lock);
    if (sched->destroying) {
        rfc_mutex_unlock(&sched->lock);
        return RFC_ERR_CANCELLED;
    }
    if (cfg->max_queue_depth > sched->queue_cap) {
        rfc_job **grown = (rfc_job **)realloc(
            sched->queue, cfg->max_queue_depth * sizeof(sched->queue[0]));
        if (!grown) {
            rfc_mutex_unlock(&sched->lock);
            return RFC_ERR_OUT_OF_MEMORY;
        }
        sched->queue = grown;
        sched->queue_cap = cfg->max_queue_depth;
    }
    if (!ensure_workers_started_locked(sched, cfg->max_workers)) {
        rfc_mutex_unlock(&sched->lock);
        return RFC_ERR_OUT_OF_MEMORY;
    }
    sched->cfg = *cfg;
    rfc_cond_broadcast(&sched->work_cond);
    rfc_mutex_unlock(&sched->lock);
    return RFC_OK;
}

rfc_status rfc_scheduler_get_config(const rfc_scheduler *sched,
                                    rfc_scheduler_config *out) {
    if (!sched || !out) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_scheduler *m = (rfc_scheduler *)sched;
    rfc_mutex_lock(&m->lock);
    *out = m->cfg;
    rfc_mutex_unlock(&m->lock);
    return RFC_OK;
}

void rfc_scheduler_stats_get(const rfc_scheduler *sched,
                             rfc_scheduler_stats *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!sched) {
        return;
    }
    rfc_scheduler *m = (rfc_scheduler *)sched;
    rfc_mutex_lock(&m->lock);
    *out = m->stats;
    rfc_mutex_unlock(&m->lock);
}

void rfc_scheduler_destroy(rfc_scheduler *sched) {
    if (!sched) {
        return;
    }
    rfc_job *to_release[RFC_SCHED_MAX_QUEUE_DEPTH];
    size_t release_count = 0;
    rfc_cancel_snapshot cancels[RFC_SCHED_MAX_WORKERS];
    size_t cancel_count = 0;

    rfc_mutex_lock(&sched->lock);
    sched->destroying = 1;

    while (sched->queue_len > 0) {
        rfc_job *j = pop_queue_at(sched, 0);
        rfc_mutex_lock(&j->lock);
        j->cancel_requested = 1;
        j->suppress_callbacks = 1;
        if (j->state == RFC_JOB_QUEUED) {
            j->state = RFC_JOB_CANCELLED;
            j->status = RFC_ERR_CANCELLED;
            j->failing_stage = j->stage;
            rfc_job_internal_set_detail(j->detail, "cancelled during shutdown");
            rfc_cond_broadcast(&j->done_cond);
            sched->stats.total_cancelled++;
        }
        rfc_mutex_unlock(&j->lock);
        if (release_count < RFC_SCHED_MAX_QUEUE_DEPTH) {
            to_release[release_count++] = j;
        }
    }

    for (uint32_t i = 0; i < RFC_SCHED_MAX_WORKERS; i++) {
        rfc_job *aj = sched->active_jobs[i];
        if (aj) {
            rfc_mutex_lock(&aj->lock);
            if (!aj->published) {
                aj->cancel_requested = 1;
                aj->suppress_callbacks = 1;
                if (cancel_count < RFC_SCHED_MAX_WORKERS &&
                    collect_active_cancel_locked(aj, &cancels[cancel_count])) {
                    cancel_count++;
                }
            }
            rfc_mutex_unlock(&aj->lock);
        }
    }

    rfc_cond_broadcast(&sched->work_cond);
    rfc_mutex_unlock(&sched->lock);

    for (size_t i = 0; i < cancel_count; i++) {
        finish_active_cancel_unlocked(&cancels[i]);
    }
    for (size_t i = 0; i < release_count; i++) {
        rfc_job_release_partials(to_release[i]);
        rfc_job_release(to_release[i]);
    }

    rfc_mutex_lock(&sched->lock);
    while (sched->stats.active_workers > 0 || sched->active_callbacks > 0) {
        rfc_cond_wait(&sched->drain_cond, &sched->lock);
    }
    rfc_cond_broadcast(&sched->work_cond);
    rfc_mutex_unlock(&sched->lock);

    for (uint32_t i = 0; i < RFC_SCHED_MAX_WORKERS; i++) {
        if (sched->workers[i].spawned) {
            rfc_thread_join(&sched->workers[i].thread);
            sched->workers[i].spawned = 0;
        }
    }

    rfc_scheduler_internal_release(sched);
}

static rfc_status compute_preflight(const rfc_scheduler_config *cfg,
                                    rfc_format_id source_format,
                                    rfc_format_id target_format,
                                    size_t source_size,
                                    const rfc_layout *source_layout,
                                    const rfc_metadata *source_meta,
                                    uint64_t expected_working_set_bytes,
                                    uint64_t job_memory_limit_bytes,
                                    const rfc_limits *limits_override,
                                    rfc_limits *out_limits,
                                    uint64_t *out_reservation,
                                    uint64_t *out_budget_limit) {
    rfc_provider_memory_info src_mem;
    rfc_provider_memory_info dst_mem;
    rfc_status st =
        rfc_query_provider_memory_contract(source_format, &src_mem);
    if (st != RFC_OK) {
        return st;
    }
    st = rfc_query_provider_memory_contract(target_format, &dst_mem);
    if (st != RFC_OK) {
        return st;
    }

    rfc_limits eff = limits_override ? *limits_override : cfg->default_limits;
    st = rfc_limits_validate(&eff);
    if (st != RFC_OK) {
        return st;
    }

    uint64_t decoded_bytes = 0;
    if (source_layout) {
        st = rfc_limits_check_image(&eff, source_layout, 1u);
        if (st != RFC_OK) {
            return st;
        }
        st = rfc_limits_compute_frame_bytes(source_layout, 1u, &decoded_bytes);
        if (st != RFC_OK) {
            return st;
        }
        if (source_size > 0) {
            st = rfc_limits_check_decompression(&eff, (uint64_t)source_size,
                                                decoded_bytes);
            if (st != RFC_OK) {
                return st;
            }
        }
    }
    if (source_meta) {
        st = rfc_limits_check_metadata(&eff, source_meta);
        if (st != RFC_OK) {
            return st;
        }
    }

    uint64_t reservation = 0;
    if (expected_working_set_bytes > 0) {
        reservation = expected_working_set_bytes;
    } else if (source_layout) {
        uint32_t mult = src_mem.working_set_multiplier > dst_mem.working_set_multiplier
                            ? src_mem.working_set_multiplier
                            : dst_mem.working_set_multiplier;
        uint64_t overhead = src_mem.fixed_overhead_bytes > dst_mem.fixed_overhead_bytes
                                ? src_mem.fixed_overhead_bytes
                                : dst_mem.fixed_overhead_bytes;
        uint64_t work = 0;
        if (!mul_u64_sched(decoded_bytes, (uint64_t)mult, &work) ||
            work > UINT64_MAX - overhead) {
            return RFC_ERR_LIMIT_EXCEEDED;
        }
        reservation = overhead + work;
    } else {
        return RFC_ERR_LIMIT_EXCEEDED;
    }

    if (reservation > cfg->memory_budget_bytes) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    if (job_memory_limit_bytes > cfg->memory_budget_bytes) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }

    *out_limits = eff;
    *out_reservation = reservation;
    *out_budget_limit =
        job_memory_limit_bytes > 0 ? job_memory_limit_bytes : reservation;
    return RFC_OK;
}

static rfc_status enqueue_new_job(rfc_scheduler *sched,
                                  rfc_job_slot *slot,
                                  rfc_format_id source_format,
                                  rfc_format_id target_format,
                                  const uint8_t *source_bytes,
                                  size_t source_size,
                                  const rfc_layout *source_layout,
                                  const rfc_metadata *source_meta,
                                  const rfc_setting_value *settings,
                                  size_t setting_count,
                                  uint32_t requested_codec_threads,
                                  uint64_t reservation_bytes,
                                  uint64_t budget_limit_bytes,
                                  const rfc_limits *eff_limits,
                                  int has_stage_ops,
                                  const rfc_pipeline_stage_ops *stage_ops,
                                  rfc_user_ctx_box *ctx_box,
                                  rfc_progress_fn progress_fn,
                                  void *progress_user,
                                  rfc_job **out_job) {
    rfc_job *job = rfc_job_internal_create();
    if (!job) {
        return RFC_ERR_OUT_OF_MEMORY;
    }

    if (!rfc_job_internal_clone_inputs(source_bytes, source_size, source_layout,
                                       source_meta, settings, setting_count,
                                       &job->source_bytes, &job->source_layout,
                                       &job->has_layout, &job->source_meta,
                                       &job->has_meta, &job->owned_icc,
                                       &job->settings)) {
        rfc_job_release(job);
        return RFC_ERR_OUT_OF_MEMORY;
    }

    job->budget = rfc_budget_create(budget_limit_bytes);
    if (!job->budget) {
        rfc_job_release(job);
        return RFC_ERR_OUT_OF_MEMORY;
    }
    rfc_budget_allocator(job->budget, &job->allocator);

    job->source_format = source_format;
    job->target_format = target_format;
    job->source_size = source_size;
    job->setting_count = setting_count;
    job->limits = *eff_limits;
    job->requested_threads = requested_codec_threads ? requested_codec_threads : 1u;
    job->reserved_memory_bytes = reservation_bytes;
    job->budget_limit_bytes = budget_limit_bytes;
    job->has_stage_ops = has_stage_ops;
    if (stage_ops) {
        job->stage_ops = *stage_ops;
    }
    if (ctx_box) {
        rfc_user_ctx_box_retain(ctx_box);
        job->user_ctx_box = ctx_box;
    }
    job->progress_fn = progress_fn;
    job->progress_user = progress_user;
    job->slot = slot;

    rfc_mutex_lock(&sched->lock);
    if (sched->destroying) {
        rfc_mutex_unlock(&sched->lock);
        rfc_job_release(job);
        return RFC_ERR_CANCELLED;
    }
    if (sched->queue_len >= sched->cfg.max_queue_depth ||
        sched->queue_len >= sched->queue_cap) {
        rfc_mutex_unlock(&sched->lock);
        rfc_job_release(job);
        return RFC_ERR_BUSY;
    }

    rfc_job_id new_id = sched->next_job_id++;
    job->job_id = new_id;
    job->sched = sched;
    rfc_scheduler_internal_retain(sched);

    rfc_job *drop_queue_prev = NULL;
    rfc_job *drop_slot_prev = NULL;
    rfc_cancel_snapshot csnap;
    memset(&csnap, 0, sizeof(csnap));

    if (slot) {
        slot->attempt_count++;
        job->attempt_number = slot->attempt_count;
        rfc_job *prev = slot->current_job;
        if (prev) {
            rfc_mutex_lock(&prev->lock);
            prev->superseded_by_id = new_id;
            prev->suppress_callbacks = 1;
            if (prev->state == RFC_JOB_QUEUED) {
                prev->superseded = 1;
                prev->cancel_requested = 1;
                prev->state = RFC_JOB_SUPERSEDED;
                prev->status = RFC_ERR_STALE_JOB;
                prev->failing_stage = prev->stage;
                rfc_job_internal_set_detail(prev->detail, "superseded by retry");
                rfc_cond_broadcast(&prev->done_cond);
                sched->stats.total_superseded++;
                for (size_t i = 0; i < sched->queue_len; i++) {
                    if (sched->queue[i] == prev) {
                        drop_queue_prev = pop_queue_at(sched, i);
                        break;
                    }
                }
            } else if (prev->state == RFC_JOB_RUNNING && !prev->published) {
                prev->superseded = 1;
                prev->cancel_requested = 1;
                collect_active_cancel_locked(prev, &csnap);
            }
            rfc_mutex_unlock(&prev->lock);
            drop_slot_prev = prev;
            slot->current_job = NULL;
        }
        rfc_job_retain(job);
        slot->current_job = job;
        slot->current_job_id = new_id;
    } else {
        job->attempt_number = 1u;
    }

    if (out_job) {
        rfc_job_retain(job);
        *out_job = job;
    }

    /* Queue holds initial reference on job (1). */
    sched->queue[sched->queue_len++] = job;
    sched->stats.queued_jobs = (uint32_t)sched->queue_len;
    sched->stats.total_submitted++;
    rfc_cond_broadcast(&sched->work_cond);
    rfc_mutex_unlock(&sched->lock);

    finish_active_cancel_unlocked(&csnap);
    if (drop_queue_prev) {
        rfc_job_release_partials(drop_queue_prev);
        rfc_job_release(drop_queue_prev);
    }
    if (drop_slot_prev) {
        rfc_job_release(drop_slot_prev);
    }
    return RFC_OK;
}

rfc_status rfc_scheduler_submit(rfc_scheduler *sched,
                                rfc_job_slot *slot,
                                const rfc_job_spec *spec,
                                rfc_job **out_job) {
    if (out_job) {
        *out_job = NULL;
    }
    if (!sched || !spec) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if ((spec->source_size > 0 && !spec->source_bytes) ||
        (spec->setting_count > 0 && !spec->settings)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    rfc_scheduler_config cur_cfg;
    rfc_status st = rfc_scheduler_get_config(sched, &cur_cfg);
    if (st != RFC_OK) {
        return st;
    }

    rfc_limits eff_limits;
    uint64_t reservation_bytes = 0;
    uint64_t budget_limit_bytes = 0;
    st = compute_preflight(&cur_cfg, spec->source_format, spec->target_format,
                           spec->source_size, spec->source_layout,
                           spec->source_meta, spec->expected_working_set_bytes,
                           spec->job_memory_limit_bytes, spec->limits_override,
                           &eff_limits, &reservation_bytes, &budget_limit_bytes);
    if (st != RFC_OK) {
        return st;
    }

    if (slot) {
        uint8_t *slot_bytes = NULL;
        rfc_layout slot_layout;
        int slot_has_layout = 0;
        rfc_metadata slot_meta;
        int slot_has_meta = 0;
        uint8_t *slot_icc = NULL;
        rfc_setting_value *slot_settings = NULL;

        if (!rfc_job_internal_clone_inputs(
                spec->source_bytes, spec->source_size, spec->source_layout,
                spec->source_meta, spec->settings, spec->setting_count,
                &slot_bytes, &slot_layout, &slot_has_layout, &slot_meta,
                &slot_has_meta, &slot_icc, &slot_settings)) {
            return RFC_ERR_OUT_OF_MEMORY;
        }

        rfc_user_ctx_box *new_box = NULL;
        if (spec->stage_ops.user_ctx || spec->stage_ops.destroy_user_ctx) {
            new_box = rfc_user_ctx_box_create(spec->stage_ops.user_ctx,
                                              spec->stage_ops.destroy_user_ctx);
            if (!new_box) {
                rfc_job_internal_copy_spec_data_free(slot_bytes, slot_icc,
                                                     slot_settings,
                                                     spec->setting_count);
                return RFC_ERR_OUT_OF_MEMORY;
            }
        }

        rfc_mutex_lock(&slot->lock);
        uint8_t *old_bytes = slot->owned_source_bytes;
        uint8_t *old_icc = slot->owned_icc;
        rfc_setting_value *old_settings = slot->owned_settings;
        size_t old_scount = slot->owned_setting_count;
        rfc_user_ctx_box *old_box = slot->user_ctx_box;

        slot->has_last_spec = 1;
        slot->source_format = spec->source_format;
        slot->target_format = spec->target_format;
        slot->owned_source_bytes = slot_bytes;
        slot->owned_source_size = spec->source_size;
        slot->owned_layout = slot_layout;
        slot->has_layout = slot_has_layout;
        slot->owned_meta = slot_meta;
        slot->has_meta = slot_has_meta;
        slot->owned_icc = slot_icc;
        slot->owned_settings = slot_settings;
        slot->owned_setting_count = spec->setting_count;
        slot->requested_codec_threads = spec->requested_codec_threads;
        slot->expected_working_set_bytes = spec->expected_working_set_bytes;
        slot->job_memory_limit_bytes = spec->job_memory_limit_bytes;
        if (spec->limits_override) {
            slot->owned_limits = *spec->limits_override;
            slot->has_limits = 1;
        } else {
            slot->has_limits = 0;
        }
        slot->has_stage_ops = spec->has_stage_ops;
        slot->stage_ops = spec->stage_ops;
        slot->user_ctx_box = new_box;
        slot->progress_fn = spec->progress_fn;
        slot->progress_user = spec->progress_user;

        st = enqueue_new_job(
            sched, slot, spec->source_format, spec->target_format,
            slot->owned_source_bytes, slot->owned_source_size,
            slot->has_layout ? &slot->owned_layout : NULL,
            slot->has_meta ? &slot->owned_meta : NULL, slot->owned_settings,
            slot->owned_setting_count, slot->requested_codec_threads,
            reservation_bytes, budget_limit_bytes, &eff_limits,
            slot->has_stage_ops, &slot->stage_ops, slot->user_ctx_box,
            slot->progress_fn, slot->progress_user, out_job);
        rfc_mutex_unlock(&slot->lock);

        rfc_job_internal_copy_spec_data_free(old_bytes, old_icc, old_settings,
                                             old_scount);
        rfc_user_ctx_box_release(old_box);
        return st;
    }

    rfc_user_ctx_box *tmp_box = NULL;
    if (spec->stage_ops.user_ctx || spec->stage_ops.destroy_user_ctx) {
        tmp_box = rfc_user_ctx_box_create(spec->stage_ops.user_ctx,
                                          spec->stage_ops.destroy_user_ctx);
        if (!tmp_box) {
            return RFC_ERR_OUT_OF_MEMORY;
        }
    }
    st = enqueue_new_job(
        sched, NULL, spec->source_format, spec->target_format,
        spec->source_bytes, spec->source_size, spec->source_layout,
        spec->source_meta, spec->settings, spec->setting_count,
        spec->requested_codec_threads, reservation_bytes, budget_limit_bytes,
        &eff_limits, spec->has_stage_ops, &spec->stage_ops, tmp_box,
        spec->progress_fn, spec->progress_user, out_job);
    rfc_user_ctx_box_release(tmp_box);
    return st;
}

rfc_status rfc_scheduler_retry(rfc_scheduler *sched,
                               rfc_job_slot *slot,
                               rfc_job **out_job) {
    if (out_job) {
        *out_job = NULL;
    }
    if (!sched || !slot) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    rfc_scheduler_config cur_cfg;
    rfc_status st = rfc_scheduler_get_config(sched, &cur_cfg);
    if (st != RFC_OK) {
        return st;
    }

    rfc_mutex_lock(&slot->lock);
    if (!slot->has_last_spec) {
        rfc_mutex_unlock(&slot->lock);
        return RFC_ERR_INVALID_ARGUMENT;
    }

    rfc_limits eff_limits;
    uint64_t reservation_bytes = 0;
    uint64_t budget_limit_bytes = 0;
    st = compute_preflight(
        &cur_cfg, slot->source_format, slot->target_format,
        slot->owned_source_size,
        slot->has_layout ? &slot->owned_layout : NULL,
        slot->has_meta ? &slot->owned_meta : NULL,
        slot->expected_working_set_bytes, slot->job_memory_limit_bytes,
        slot->has_limits ? &slot->owned_limits : NULL, &eff_limits,
        &reservation_bytes, &budget_limit_bytes);
    if (st != RFC_OK) {
        rfc_mutex_unlock(&slot->lock);
        return st;
    }

    st = enqueue_new_job(
        sched, slot, slot->source_format, slot->target_format,
        slot->owned_source_bytes, slot->owned_source_size,
        slot->has_layout ? &slot->owned_layout : NULL,
        slot->has_meta ? &slot->owned_meta : NULL, slot->owned_settings,
        slot->owned_setting_count, slot->requested_codec_threads,
        reservation_bytes, budget_limit_bytes, &eff_limits, slot->has_stage_ops,
        &slot->stage_ops, slot->user_ctx_box, slot->progress_fn,
        slot->progress_user, out_job);
    rfc_mutex_unlock(&slot->lock);
    return st;
}
