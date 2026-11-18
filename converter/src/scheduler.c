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
    rfc_cond cancel_cond;

    atomic_size_t internal_refs;
    atomic_uint_fast64_t submit_alloc_count;

    rfc_scheduler_config cfg;
    rfc_scheduler_stats stats;

    rfc_job_id next_job_id;
    int destroying;
    uint32_t active_callbacks;
    uint64_t exec_reserved_bytes;
    uint64_t retained_reserved_bytes;
    size_t pending_admits;

    rfc_job **queue;
    size_t queue_cap;
    size_t queue_len;

    rfc_job *cancel_head;
    rfc_job *cancel_tail;
    uint32_t active_cancel_dispatches;
    rfc_thread cancel_thread;
    int cancel_thread_spawned;

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

static int add_u64_sched(uint64_t a, uint64_t b, uint64_t *out) {
    if (a > UINT64_MAX - b) {
        return 0;
    }
    *out = a + b;
    return 1;
}

static int scheduler_is_drained_locked(const rfc_scheduler *sched) {
    return sched->active_callbacks == 0 && sched->stats.active_workers == 0 &&
           sched->queue_len == 0 && sched->cancel_head == NULL &&
           sched->active_cancel_dispatches == 0;
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
        rfc_cond_destroy(&sched->cancel_cond);
        rfc_cond_destroy(&sched->drain_cond);
        rfc_cond_destroy(&sched->work_cond);
        rfc_mutex_destroy(&sched->lock);
        free(sched);
    }
}

void rfc_scheduler_internal_note_submit_alloc(rfc_scheduler *sched) {
    if (sched) {
        atomic_fetch_add_explicit(&sched->submit_alloc_count, 1u,
                                  memory_order_relaxed);
    }
}

void rfc_scheduler_internal_unreserve_retained(rfc_scheduler *sched,
                                               uint64_t bytes) {
    if (!sched || bytes == 0) {
        return;
    }
    rfc_mutex_lock(&sched->lock);
    if (sched->retained_reserved_bytes >= bytes) {
        sched->retained_reserved_bytes -= bytes;
    } else {
        sched->retained_reserved_bytes = 0;
    }
    rfc_cond_broadcast(&sched->work_cond);
    rfc_mutex_unlock(&sched->lock);
}

void rfc_scheduler_internal_lock(rfc_scheduler *sched) {
    if (sched) {
        rfc_mutex_lock(&sched->lock);
    }
}

void rfc_scheduler_internal_unlock(rfc_scheduler *sched) {
    if (sched) {
        rfc_mutex_unlock(&sched->lock);
    }
}

int rfc_scheduler_internal_is_destroying_locked(rfc_scheduler *sched) {
    return sched ? sched->destroying : 1;
}

void rfc_scheduler_internal_callback_enter_locked(rfc_scheduler *sched) {
    if (sched) {
        sched->active_callbacks++;
    }
}

void rfc_scheduler_internal_callback_leave(rfc_scheduler *sched) {
    if (!sched) {
        return;
    }
    rfc_mutex_lock(&sched->lock);
    if (sched->active_callbacks > 0) {
        sched->active_callbacks--;
    }
    if (scheduler_is_drained_locked(sched)) {
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

void rfc_scheduler_internal_adjust_live_partials_locked(rfc_scheduler *sched,
                                                        int64_t delta_bytes) {
    if (!sched || delta_bytes == 0) {
        return;
    }
    if (delta_bytes > 0) {
        sched->stats.live_partial_bytes += (uint64_t)delta_bytes;
    } else {
        uint64_t sub = (uint64_t)(-(delta_bytes + 1)) + 1u;
        if (sched->stats.live_partial_bytes >= sub) {
            sched->stats.live_partial_bytes -= sub;
        } else {
            sched->stats.live_partial_bytes = 0u;
        }
    }
}

void rfc_scheduler_internal_adjust_live_partials(rfc_scheduler *sched,
                                                 int64_t delta_bytes) {
    if (!sched || delta_bytes == 0) {
        return;
    }
    rfc_mutex_lock(&sched->lock);
    rfc_scheduler_internal_adjust_live_partials_locked(sched, delta_bytes);
    rfc_mutex_unlock(&sched->lock);
}

static void wait_job_callbacks_drained(rfc_job *job, int same_cb_thread) {
    if (!job || same_cb_thread) {
        return;
    }
    rfc_mutex_lock(&job->cb_lock);
    rfc_mutex_unlock(&job->cb_lock);
}

static int enqueue_active_cancel_locked(rfc_scheduler *sched, rfc_job *job) {
    if (!sched || !job || !job->active_session) {
        return 0;
    }
    int has_sess = (job->active_sess_adapter &&
                    job->active_sess_adapter->cancel_session);
    int has_jrec = (job->active_jrec_adapter &&
                    job->active_jrec_adapter->cancel_session);
    int has_jtrans = (job->active_jtrans_adapter &&
                      job->active_jtrans_adapter->cancel_session);
    if (!has_sess && !has_jrec && !has_jtrans) {
        return 0;
    }
    if (job->cancel_queued &&
        job->pending_cancel.session == job->active_session) {
        return 0;
    }
    if (job->cancel_queued) {
        return 0;
    }
    job->pending_cancel.sess_adapter = job->active_sess_adapter;
    job->pending_cancel.jrec_adapter = job->active_jrec_adapter;
    job->pending_cancel.jtrans_adapter = job->active_jtrans_adapter;
    job->pending_cancel.session = job->active_session;
    job->cancel_queued = 1;
    job->cancel_next = NULL;
    job->active_cancel_calls++;
    rfc_job_retain(job);

    if (sched->cancel_tail) {
        sched->cancel_tail->cancel_next = job;
    } else {
        sched->cancel_head = job;
    }
    sched->cancel_tail = job;
    rfc_cond_signal(&sched->cancel_cond);
    return 1;
}

static void cancel_worker_main(void *arg) {
    rfc_scheduler *sched = (rfc_scheduler *)arg;
    rfc_mutex_lock(&sched->lock);
    for (;;) {
        while (!sched->cancel_head && !sched->destroying) {
            rfc_cond_wait(&sched->cancel_cond, &sched->lock);
        }
        if (!sched->cancel_head && sched->destroying) {
            break;
        }
        rfc_job *j = sched->cancel_head;
        sched->cancel_head = j->cancel_next;
        if (!sched->cancel_head) {
            sched->cancel_tail = NULL;
        }
        j->cancel_next = NULL;
        j->cancel_queued = 0;
        rfc_cancel_snapshot snap = j->pending_cancel;
        memset(&j->pending_cancel, 0, sizeof(j->pending_cancel));
        sched->active_cancel_dispatches++;
        rfc_mutex_unlock(&sched->lock);

        if (snap.sess_adapter && snap.sess_adapter->cancel_session &&
            snap.session) {
            snap.sess_adapter->cancel_session(snap.session);
        } else if (snap.jrec_adapter && snap.jrec_adapter->cancel_session &&
                   snap.session) {
            snap.jrec_adapter->cancel_session(snap.session);
        } else if (snap.jtrans_adapter &&
                   snap.jtrans_adapter->cancel_session && snap.session) {
            snap.jtrans_adapter->cancel_session(snap.session);
        }

        rfc_mutex_lock(&sched->lock);
        rfc_mutex_lock(&j->lock);
        if (j->active_cancel_calls > 0) {
            j->active_cancel_calls--;
        }
        rfc_cond_broadcast(&j->done_cond);
        rfc_mutex_unlock(&j->lock);

        if (sched->active_cancel_dispatches > 0) {
            sched->active_cancel_dispatches--;
        }
        if (scheduler_is_drained_locked(sched)) {
            rfc_cond_broadcast(&sched->drain_cond);
        }
        rfc_mutex_unlock(&sched->lock);

        rfc_job_release(j);
        rfc_mutex_lock(&sched->lock);
    }
    rfc_mutex_unlock(&sched->lock);
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
    rfc_job *drop_job = NULL;
    int same_cb_thread = 0;

    rfc_mutex_lock(&sched->lock);
    rfc_mutex_lock(&job->lock);

    if (job->published || job->publishing || job->publish_decided ||
        job->state == RFC_JOB_SUCCEEDED || job->state == RFC_JOB_FAILED ||
        job->state == RFC_JOB_CANCELLED || job->state == RFC_JOB_SUPERSEDED) {
        rfc_mutex_unlock(&job->lock);
        rfc_mutex_unlock(&sched->lock);
        return RFC_OK;
    }

    job->cancel_requested = 1;
    job->suppress_callbacks = 1;
    same_cb_thread =
        job->cb_owner_valid &&
        rfc_thread_id_equal(job->cb_owner, rfc_thread_id_current());

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
        if (scheduler_is_drained_locked(sched)) {
            rfc_cond_broadcast(&sched->drain_cond);
        }
        rfc_mutex_unlock(&job->lock);
        rfc_mutex_unlock(&sched->lock);

        wait_job_callbacks_drained(job, same_cb_thread);
        rfc_job_release_partials(job);
        if (drop_job) {
            rfc_job_release(drop_job);
        }
        return RFC_OK;
    }

    enqueue_active_cancel_locked(sched, job);
    rfc_mutex_unlock(&job->lock);
    rfc_mutex_unlock(&sched->lock);

    wait_job_callbacks_drained(job, same_cb_thread);
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
                head->suppress_callbacks = 1;
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
        uint64_t needed_exec = 0;

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
            needed_exec = head->exec_working_bytes;

            int cpu_ok = (sched->stats.active_cpu_threads + needed_threads <=
                          sched->cfg.total_cpu_threads);
            uint64_t next_exec = 0;
            uint64_t total_mem = 0;
            int mem_ok =
                add_u64_sched(sched->exec_reserved_bytes, needed_exec,
                              &next_exec) &&
                add_u64_sched(next_exec, sched->retained_reserved_bytes,
                              &total_mem) &&
                total_mem <= sched->cfg.memory_budget_bytes;
            if (cpu_ok && mem_ok) {
                can_run = 1;
            }
        }

        if (!can_run) {
            if (scheduler_is_drained_locked(sched)) {
                rfc_cond_broadcast(&sched->drain_cond);
            }
            rfc_cond_wait(&sched->work_cond, &sched->lock);
            continue;
        }

        rfc_job *job = sched->queue[0];
        sched->active_jobs[w->index] = job;
        pop_queue_at(sched, 0);
        sched->stats.active_workers++;
        if (sched->stats.active_workers > sched->stats.peak_workers) {
            sched->stats.peak_workers = sched->stats.active_workers;
        }
        sched->stats.active_cpu_threads += needed_threads;
        if (sched->stats.active_cpu_threads > sched->stats.peak_cpu_threads) {
            sched->stats.peak_cpu_threads = sched->stats.active_cpu_threads;
        }
        sched->exec_reserved_bytes += needed_exec;
        sched->stats.active_reserved_bytes = sched->exec_reserved_bytes;
        uint64_t total_live =
            sched->exec_reserved_bytes + sched->retained_reserved_bytes;
        if (total_live > sched->stats.peak_reserved_bytes) {
            sched->stats.peak_reserved_bytes = total_live;
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
        if (job->publish_decided) {
            if (job->published && exec_res.st == RFC_OK) {
                job->stage = RFC_STAGE_DONE;
                job->state = RFC_JOB_SUCCEEDED;
                job->status = RFC_OK;
                job->failing_stage = RFC_STAGE_NONE;
                if (job->detail[0] == '\0') {
                    rfc_job_internal_set_detail(job->detail, "ok");
                }
            } else {
                job->state = RFC_JOB_FAILED;
                job->status =
                    (exec_res.st != RFC_OK) ? exec_res.st : RFC_ERR_IO;
                job->failing_stage = (exec_res.stage_at != RFC_STAGE_NONE)
                                         ? exec_res.stage_at
                                         : RFC_STAGE_EXPORT;
                if (job->detail[0] == '\0') {
                    rfc_job_internal_set_detail(
                        job->detail, exec_res.err_detail
                                         ? exec_res.err_detail
                                         : rfc_status_name(job->status));
                }
            }
        } else if (job->published && exec_res.st == RFC_OK) {
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
        sched->exec_reserved_bytes -= needed_exec;
        sched->stats.active_reserved_bytes = sched->exec_reserved_bytes;
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
        if (scheduler_is_drained_locked(sched)) {
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
    if (!rfc_cond_init(&sched->cancel_cond)) {
        rfc_cond_destroy(&sched->drain_cond);
        rfc_cond_destroy(&sched->work_cond);
        rfc_mutex_destroy(&sched->lock);
        free(sched);
        return NULL;
    }

    sched->queue = (rfc_job **)calloc(eff.max_queue_depth, sizeof(sched->queue[0]));
    if (!sched->queue) {
        rfc_cond_destroy(&sched->cancel_cond);
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
    atomic_init(&sched->submit_alloc_count, 0u);

    rfc_mutex_lock(&sched->lock);
    if (!rfc_thread_start_fn(&sched->cancel_thread, cancel_worker_main,
                             sched)) {
        sched->destroying = 1;
        rfc_mutex_unlock(&sched->lock);
        rfc_scheduler_internal_release(sched);
        return NULL;
    }
    sched->cancel_thread_spawned = 1;

    if (!ensure_workers_started_locked(sched, eff.max_workers)) {
        sched->destroying = 1;
        rfc_cond_broadcast(&sched->work_cond);
        rfc_cond_broadcast(&sched->cancel_cond);
        rfc_mutex_unlock(&sched->lock);
        for (uint32_t i = 0; i < RFC_SCHED_MAX_WORKERS; i++) {
            if (sched->workers[i].spawned) {
                rfc_thread_join(&sched->workers[i].thread);
                sched->workers[i].spawned = 0;
            }
        }
        if (sched->cancel_thread_spawned) {
            rfc_thread_join(&sched->cancel_thread);
            sched->cancel_thread_spawned = 0;
        }
        rfc_scheduler_internal_release(sched);
        return NULL;
    }
    rfc_mutex_unlock(&sched->lock);
    return sched;
}

static int limits_tighter(const rfc_limits *next, const rfc_limits *prev) {
    return next->max_width < prev->max_width ||
           next->max_height < prev->max_height ||
           next->max_pixels < prev->max_pixels ||
           next->max_frames < prev->max_frames ||
           next->max_total_pixels < prev->max_total_pixels ||
           next->max_decoded_bytes < prev->max_decoded_bytes ||
           next->max_metadata_bytes < prev->max_metadata_bytes ||
           next->max_compressed_bytes < prev->max_compressed_bytes ||
           next->max_expansion_ratio < prev->max_expansion_ratio;
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
    if (sched->stats.active_workers > cfg->max_workers ||
        sched->stats.active_cpu_threads > cfg->total_cpu_threads ||
        sched->queue_len > cfg->max_queue_depth ||
        sched->pending_admits > cfg->max_queue_depth - sched->queue_len) {
        rfc_mutex_unlock(&sched->lock);
        return RFC_ERR_BUSY;
    }
    uint64_t live_mem = 0;
    if (!add_u64_sched(sched->exec_reserved_bytes, sched->retained_reserved_bytes,
                       &live_mem) ||
        live_mem > cfg->memory_budget_bytes) {
        rfc_mutex_unlock(&sched->lock);
        return RFC_ERR_BUSY;
    }
    for (size_t i = 0; i < sched->queue_len; i++) {
        const rfc_job *qj = sched->queue[i];
        uint64_t peak_q = 0;
        if (qj->budget_limit_bytes > cfg->memory_budget_bytes ||
            !add_u64_sched(qj->exec_working_bytes,
                           sched->retained_reserved_bytes, &peak_q) ||
            peak_q > cfg->memory_budget_bytes) {
            rfc_mutex_unlock(&sched->lock);
            return RFC_ERR_BUSY;
        }
    }
    if (limits_tighter(&cfg->default_limits, &sched->cfg.default_limits)) {
        if (sched->pending_admits > 0) {
            rfc_mutex_unlock(&sched->lock);
            return RFC_ERR_BUSY;
        }
        for (uint32_t i = 0; i < RFC_SCHED_MAX_WORKERS; i++) {
            const rfc_job *aj = sched->active_jobs[i];
            if (aj && !aj->has_limits_override) {
                rfc_mutex_unlock(&sched->lock);
                return RFC_ERR_BUSY;
            }
        }
        for (size_t i = 0; i < sched->queue_len; i++) {
            const rfc_job *qj = sched->queue[i];
            if (qj && !qj->has_limits_override) {
                rfc_mutex_unlock(&sched->lock);
                return RFC_ERR_BUSY;
            }
        }
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
    out->submission_alloc_count =
        atomic_load_explicit(&m->submit_alloc_count, memory_order_relaxed);
    rfc_mutex_unlock(&m->lock);
}

void rfc_scheduler_destroy(rfc_scheduler *sched) {
    if (!sched) {
        return;
    }
    rfc_job *to_release[RFC_SCHED_MAX_QUEUE_DEPTH];
    size_t release_count = 0;

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
            if (!aj->published && !aj->publishing && !aj->publish_decided) {
                aj->cancel_requested = 1;
                aj->suppress_callbacks = 1;
                enqueue_active_cancel_locked(sched, aj);
            }
            rfc_mutex_unlock(&aj->lock);
        }
    }

    rfc_cond_broadcast(&sched->work_cond);
    rfc_cond_broadcast(&sched->cancel_cond);
    rfc_mutex_unlock(&sched->lock);

    for (size_t i = 0; i < release_count; i++) {
        rfc_job_release_partials(to_release[i]);
        rfc_job_release(to_release[i]);
    }

    rfc_mutex_lock(&sched->lock);
    while (!scheduler_is_drained_locked(sched)) {
        rfc_cond_wait(&sched->drain_cond, &sched->lock);
    }
    rfc_cond_broadcast(&sched->work_cond);
    rfc_cond_broadcast(&sched->cancel_cond);
    rfc_mutex_unlock(&sched->lock);

    for (uint32_t i = 0; i < RFC_SCHED_MAX_WORKERS; i++) {
        if (sched->workers[i].spawned) {
            rfc_thread_join(&sched->workers[i].thread);
            sched->workers[i].spawned = 0;
        }
    }
    if (sched->cancel_thread_spawned) {
        rfc_thread_join(&sched->cancel_thread);
        sched->cancel_thread_spawned = 0;
    }

    rfc_scheduler_internal_release(sched);
}

static rfc_status parse_verify_settings_sched(rfc_format_id target_format,
                                              const rfc_setting_value *settings,
                                              size_t setting_count,
                                              rfc_exactness_class *out_exact,
                                              rfc_metadata_policy *out_meta);


/* PNG as a source (decode only): the frame, the shared loader's IDAT copy (up
 * to twice a stored-deflate source), its raw filter rows and RGB buffer.
 * The provider contract's 8x is the target's VERIFY peak (limits.c). */
#define RFC_PNG_DECODE_WORKING_SET_MULTIPLIER 5u

static rfc_status compute_jxl_stage_work_sched(uint64_t decoded_bytes,
                                               uint64_t pixels,
                                               uint32_t eff_req_threads,
                                               int is_encode,
                                               uint64_t *out_work) {
    uint64_t dec_scaled = 0u;
    uint64_t pix_scaled = 0u;
    uint64_t thr_bytes = 0u;
    uint64_t base_fixed =
        is_encode ? (16ull * 1024ull * 1024ull) : (8ull * 1024ull * 1024ull);
    uint64_t sum = 0u;
    if (!mul_u64_sched(decoded_bytes, is_encode ? 4ull : 3ull, &dec_scaled) ||
        !mul_u64_sched(pixels, is_encode ? 36ull : 16ull, &pix_scaled) ||
        !mul_u64_sched((uint64_t)eff_req_threads, 4ull * 1024ull * 1024ull,
                       &thr_bytes) ||
        !add_u64_sched(dec_scaled, pix_scaled, &sum) ||
        !add_u64_sched(sum, thr_bytes, &sum) ||
        !add_u64_sched(sum, base_fixed, out_work)) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    return RFC_OK;
}

static rfc_status compute_avif_stage_work_sched(uint64_t decoded_bytes,
                                                uint64_t pixels,
                                                uint32_t eff_req_threads,
                                                int is_encode,
                                                uint64_t *out_work) {
    uint64_t dec_scaled = 0u;
    uint64_t pix_scaled = 0u;
    uint64_t thr_bytes = 0u;
    uint64_t per_pix =
        is_encode ? 44ull : (16ull + 8ull * (uint64_t)eff_req_threads);
    uint64_t base_fixed = 16ull * 1024ull * 1024ull;
    uint64_t sum = 0u;
    if (!mul_u64_sched(decoded_bytes, 4ull, &dec_scaled) ||
        !mul_u64_sched(pixels, per_pix, &pix_scaled) ||
        !mul_u64_sched((uint64_t)eff_req_threads, 8ull * 1024ull * 1024ull,
                       &thr_bytes) ||
        !add_u64_sched(dec_scaled, pix_scaled, &sum) ||
        !add_u64_sched(sum, thr_bytes, &sum) ||
        !add_u64_sched(sum, base_fixed, out_work)) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    return RFC_OK;
}

static rfc_status compute_endpoint_stage_work_sched(
    const rfc_provider_memory_info *mem,
    uint64_t decoded_bytes,
    uint64_t pixels,
    uint32_t eff_req_threads,
    int is_encode,
    uint64_t *out_work) {
    if (mem->format == RFC_FORMAT_JPEG_XL) {
        return compute_jxl_stage_work_sched(decoded_bytes, pixels,
                                            eff_req_threads, is_encode,
                                            out_work);
    }
    if (mem->format == RFC_FORMAT_AVIF) {
        return compute_avif_stage_work_sched(decoded_bytes, pixels,
                                             eff_req_threads, is_encode,
                                             out_work);
    }
    uint32_t mult = mem->working_set_multiplier;
    uint64_t overhead = mem->fixed_overhead_bytes;
    if (mem->format == RFC_FORMAT_PNG && !is_encode) {
        mult = RFC_PNG_DECODE_WORKING_SET_MULTIPLIER;
    }
    if (mem->format == RFC_FORMAT_WEBP) {
        if (!is_encode) {
            mult = 5u;
            overhead = 2ull * 1024ull * 1024ull;
        } else if (eff_req_threads <= 1u) {
            mult = 16u;
            overhead = 28ull * 1024ull * 1024ull;
        }
    }
    uint64_t scaled = 0u;
    if (!mul_u64_sched(decoded_bytes, (uint64_t)mult, &scaled) ||
        !add_u64_sched(overhead, scaled, out_work)) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    return RFC_OK;
}

static rfc_status compute_preflight_with_contracts(
    const rfc_scheduler_config *cfg,
    const rfc_provider_memory_info *src_mem,
    const rfc_provider_memory_info *dst_mem,
    rfc_exactness_class exactness,
    uint32_t requested_codec_threads,
    uint64_t source_size,
    const rfc_layout *source_layout,
    const rfc_metadata *source_meta,
    uint64_t retained_bytes,
    uint64_t expected_working_set_bytes,
    uint64_t job_memory_limit_bytes,
    const rfc_limits *limits_override,
    rfc_limits *out_limits,
    uint64_t *out_reservation,
    uint64_t *out_exec_working,
    uint64_t *out_budget_limit) {
    rfc_limits eff = limits_override ? *limits_override : cfg->default_limits;
    rfc_status st = rfc_limits_validate(&eff);
    if (st != RFC_OK) {
        return st;
    }

    st = rfc_limits_check_compressed(&eff, (uint64_t)source_size);
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

    /* Up to five copies of the carried metadata coexist at VERIFY: the
     * decoded source's, the encoder's staging copy, the encoded output's,
     * and the target provider's parsed input and output (measured: a 10 MiB
     * XMP packet TIFF -> AVIF peaks near five copies beyond the source). */
    uint64_t meta_allowance = 0u;
    if (source_meta) {
        uint64_t m_sum = 0u;
        if (!add_u64_sched((uint64_t)source_meta->icc_len,
                           (uint64_t)source_meta->exif_len, &m_sum) ||
            !add_u64_sched(m_sum, (uint64_t)source_meta->xmp_len, &m_sum) ||
            !mul_u64_sched(m_sum, 5ull, &meta_allowance)) {
            return RFC_ERR_LIMIT_EXCEEDED;
        }
    } else if (source_size > 0u) {
        /* Unknown until IMPORT, but the carried metadata can be no larger
         * than the source bytes or the metadata limit. */
        uint64_t m_bound = (uint64_t)source_size < eff.max_metadata_bytes
                               ? (uint64_t)source_size
                               : eff.max_metadata_bytes;
        if (!mul_u64_sched(m_bound, 5ull, &meta_allowance)) {
            return RFC_ERR_LIMIT_EXCEEDED;
        }
    }

    uint64_t base_reservation = 0;
    if (expected_working_set_bytes > 0) {
        base_reservation = expected_working_set_bytes;
    } else if (src_mem->format == RFC_FORMAT_JPEG &&
               dst_mem->format == RFC_FORMAT_JPEG &&
               exactness == RFC_EXACT_COEFFICIENT_EXACT && source_size > 0u) {
        /* Coefficient-exact optimization holds the source bytes, the output
         * (bounded by source + 64 KiB), and at peak (reading the output's
         * coefficients at VERIFY) the source coefficient image, the
         * provider's whole-image arrays and the output image: three images of
         * at most 2x the decoded bytes each, plus MCU padding. Submission
         * derives the layout from the SOF when the caller gives none; only a
         * stream whose SOF could not be sniffed falls back to the source. */
        uint64_t core = 0u;
        uint64_t src2 = 0u;
        if (source_layout) {
            if (!mul_u64_sched(decoded_bytes, 8ull, &core)) {
                return RFC_ERR_LIMIT_EXCEEDED;
            }
        } else if (!mul_u64_sched(source_size, 32ull, &core)) {
            return RFC_ERR_LIMIT_EXCEEDED;
        }
        if (core < 64ull * 1024ull * 1024ull) {
            core = 64ull * 1024ull * 1024ull;
        }
        if (!mul_u64_sched(source_size, 3ull, &src2) ||
            !add_u64_sched(core, src2, &core) ||
            !add_u64_sched(core, 16ull * 1024ull * 1024ull, &core) ||
            !add_u64_sched(core, meta_allowance, &base_reservation)) {
            return RFC_ERR_LIMIT_EXCEEDED;
        }
    } else if (source_layout) {
        uint32_t eff_req_threads =
            requested_codec_threads ? requested_codec_threads : 1u;
        /* The imported source bytes stay in the job's budget for the whole
         * job, beside every stage's working set. TIFF adds terms
         * proportional to the source, whatever the frame size: libtiff's
         * read cap is twice the source, and a TIFF target's output buffer
         * and VERIFY read cap each carry the source's metadata twice. */
        uint64_t src_factor = 1u;
        if (src_mem->format == RFC_FORMAT_TIFF) {
            src_factor += 2u;
        }
        if (dst_mem->format == RFC_FORMAT_TIFF) {
            src_factor += 4u;
        }
        uint64_t src_terms = 0u;
        if (!mul_u64_sched(source_size, src_factor, &src_terms)) {
            return RFC_ERR_LIMIT_EXCEEDED;
        }
        if (src_mem->format == RFC_FORMAT_JPEG_XL ||
            dst_mem->format == RFC_FORMAT_JPEG_XL ||
            src_mem->format == RFC_FORMAT_AVIF ||
            dst_mem->format == RFC_FORMAT_AVIF) {
            uint64_t pixels = 0u;
            uint64_t src_work = 0u;
            uint64_t dst_work = 0u;
            if (!mul_u64_sched((uint64_t)source_layout->width,
                               (uint64_t)source_layout->height, &pixels)) {
                return RFC_ERR_LIMIT_EXCEEDED;
            }
            st = compute_endpoint_stage_work_sched(
                src_mem, decoded_bytes, pixels, eff_req_threads, 0, &src_work);
            if (st != RFC_OK) {
                return st;
            }
            st = compute_endpoint_stage_work_sched(
                dst_mem, decoded_bytes, pixels, eff_req_threads, 1, &dst_work);
            if (st != RFC_OK) {
                return st;
            }
            uint64_t peak_work = (src_work > dst_work) ? src_work : dst_work;
            if (!add_u64_sched(peak_work, meta_allowance, &base_reservation) ||
                !add_u64_sched(base_reservation, src_terms,
                               &base_reservation)) {
                return RFC_ERR_LIMIT_EXCEEDED;
            }
        } else {
            uint32_t src_mult = src_mem->working_set_multiplier;
            uint64_t src_overhead = src_mem->fixed_overhead_bytes;
            if (src_mem->format == RFC_FORMAT_PNG) {
                src_mult = RFC_PNG_DECODE_WORKING_SET_MULTIPLIER;
            }
            if (src_mem->format == RFC_FORMAT_WEBP) {
                src_mult = 5u;
                src_overhead = 2ull * 1024ull * 1024ull;
            }
            uint32_t dst_mult = dst_mem->working_set_multiplier;
            uint64_t dst_overhead = dst_mem->fixed_overhead_bytes;
            if (dst_mem->format == RFC_FORMAT_WEBP && eff_req_threads <= 1u) {
                dst_mult = 16u;
                dst_overhead = 28ull * 1024ull * 1024ull;
            }
            uint32_t mult = src_mult > dst_mult ? src_mult : dst_mult;
            uint64_t overhead =
                src_overhead > dst_overhead ? src_overhead : dst_overhead;
            if (!add_u64_sched(overhead, meta_allowance, &overhead)) {
                return RFC_ERR_LIMIT_EXCEEDED;
            }
            uint64_t work = 0;
            if (!mul_u64_sched(decoded_bytes, (uint64_t)mult, &work) ||
                work > UINT64_MAX - overhead) {
                return RFC_ERR_LIMIT_EXCEEDED;
            }
            if (!add_u64_sched(overhead + work, src_terms,
                               &base_reservation)) {
                return RFC_ERR_LIMIT_EXCEEDED;
            }
        }
    } else if (src_mem->format == RFC_FORMAT_JPEG &&
               dst_mem->format == RFC_FORMAT_JPEG_XL &&
               exactness == RFC_EXACT_ORIGINAL_BYTE_RECONSTRUCTION &&
               source_size > 0u) {
        uint32_t eff_req_threads =
            requested_codec_threads ? requested_codec_threads : 1u;
        uint64_t scaled_src = 0u;
        uint64_t thr_bytes = 0u;
        uint64_t sum = 0u;
        if (!mul_u64_sched(source_size, 16ull, &scaled_src) ||
            !mul_u64_sched((uint64_t)eff_req_threads, 4ull * 1024ull * 1024ull,
                           &thr_bytes)) {
            return RFC_ERR_LIMIT_EXCEEDED;
        }
        uint64_t core_est =
            (scaled_src > 32ull * 1024ull * 1024ull)
                ? scaled_src
                : (32ull * 1024ull * 1024ull);
        if (!add_u64_sched(core_est, thr_bytes, &sum) ||
            !add_u64_sched(sum, 16ull * 1024ull * 1024ull, &sum) ||
            !add_u64_sched(sum, meta_allowance, &base_reservation)) {
            return RFC_ERR_LIMIT_EXCEEDED;
        }
    } else if (src_mem->format == dst_mem->format &&
               exactness == RFC_EXACT_BYTE_IDENTICAL &&
               source_size > 0u) {
        uint64_t scaled_src = 0u;
        uint64_t core_est = 0u;
        if (!mul_u64_sched(source_size, 16ull, &scaled_src)) {
            return RFC_ERR_LIMIT_EXCEEDED;
        }
        core_est = (scaled_src > 32ull * 1024ull * 1024ull)
                       ? scaled_src
                       : (32ull * 1024ull * 1024ull);
        if (!add_u64_sched(core_est, meta_allowance, &base_reservation)) {
            return RFC_ERR_LIMIT_EXCEEDED;
        }
    } else {
        return RFC_ERR_LIMIT_EXCEEDED;
    }

    uint64_t budget_limit = job_memory_limit_bytes > 0
                                ? job_memory_limit_bytes
                                : base_reservation;
    uint64_t exec_working =
        (base_reservation > retained_bytes &&
         base_reservation - retained_bytes > budget_limit)
            ? (base_reservation - retained_bytes)
            : budget_limit;
    uint64_t reservation = 0;
    if (!add_u64_sched(exec_working, retained_bytes, &reservation)) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }

    if (reservation > cfg->memory_budget_bytes ||
        budget_limit > cfg->memory_budget_bytes) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }

    *out_limits = eff;
    *out_reservation = reservation;
    *out_exec_working = exec_working;
    *out_budget_limit = budget_limit;
    return RFC_OK;
}

static rfc_status parse_verify_settings_sched(rfc_format_id target_format,
                                              const rfc_setting_value *settings,
                                              size_t setting_count,
                                              rfc_exactness_class *out_exact,
                                              rfc_metadata_policy *out_meta) {
    return rfc_job_internal_resolve_exactness(target_format, settings,
                                              setting_count, out_exact,
                                              out_meta);
}

static rfc_status compute_preflight(const rfc_scheduler_config *cfg,
                                    rfc_format_id source_format,
                                    rfc_format_id target_format,
                                    uint32_t requested_codec_threads,
                                    uint64_t source_size,
                                    const rfc_layout *source_layout,
                                    const rfc_metadata *source_meta,
                                    const rfc_setting_value *settings,
                                    size_t setting_count,
                                    uint32_t acknowledged_unsupported_metadata,
                                    const rfc_pipeline_stage_ops *stage_ops,
                                    uint64_t retained_bytes,
                                    uint64_t expected_working_set_bytes,
                                    uint64_t job_memory_limit_bytes,
                                    const rfc_limits *limits_override,
                                    rfc_provider_memory_info *out_src_mem,
                                    rfc_provider_memory_info *out_dst_mem,
                                    rfc_limits *out_limits,
                                    uint64_t *out_reservation,
                                    uint64_t *out_exec_working,
                                    uint64_t *out_budget_limit) {
    if ((acknowledged_unsupported_metadata & ~RFC_META_ITEM_ALL) != 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_status st = rfc_job_internal_validate_settings(
        source_format, target_format, settings, setting_count, stage_ops, NULL);
    if (st != RFC_OK) {
        return st;
    }
    rfc_exactness_class ex = RFC_EXACT_PIXEL_EXACT;
    rfc_metadata_policy pol = RFC_META_POLICY_PRESERVE;
    st = parse_verify_settings_sched(target_format, settings, setting_count,
                                     &ex, &pol);
    if (st != RFC_OK) {
        return st;
    }
    if ((!stage_ops || !stage_ops->on_verify) && source_meta != NULL) {
        st = rfc_metadata_check_source_preflight(
            target_format, pol, ex, source_meta, NULL, 0u,
            acknowledged_unsupported_metadata, NULL);
        if (st != RFC_OK) {
            return st;
        }
    }
    rfc_provider_memory_info src_mem;
    rfc_provider_memory_info dst_mem;
    st = rfc_query_provider_memory_contract(source_format, &src_mem);
    if (st != RFC_OK) {
        return st;
    }
    st = rfc_query_provider_memory_contract(target_format, &dst_mem);
    if (st != RFC_OK) {
        return st;
    }
    st = compute_preflight_with_contracts(
        cfg, &src_mem, &dst_mem, ex, requested_codec_threads, source_size,
        source_layout, source_meta, retained_bytes, expected_working_set_bytes,
        job_memory_limit_bytes, limits_override, out_limits, out_reservation,
        out_exec_working, out_budget_limit);
    if (st == RFC_OK) {
        if (out_src_mem) {
            *out_src_mem = src_mem;
        }
        if (out_dst_mem) {
            *out_dst_mem = dst_mem;
        }
    }
    return st;
}

static int scheduler_capacity_ok_locked(const rfc_scheduler *sched,
                                        const rfc_job *ignore_queued,
                                        size_t excluding_pending,
                                        uint64_t next_retained,
                                        uint64_t new_exec_working) {
    size_t queued_eff = sched->queue_len;
    if (ignore_queued) {
        for (size_t i = 0; i < sched->queue_len; i++) {
            if (sched->queue[i] == ignore_queued) {
                queued_eff--;
                break;
            }
        }
    }
    size_t pending_eff = sched->pending_admits > excluding_pending
                             ? (sched->pending_admits - excluding_pending)
                             : 0u;
    if (queued_eff >= sched->cfg.max_queue_depth ||
        queued_eff >= sched->queue_cap ||
        pending_eff > sched->cfg.max_queue_depth - queued_eff) {
        return 0;
    }
    /* Reserve exact saturated queue capacity across pending_admits as well. */
    if (queued_eff + pending_eff >= sched->cfg.max_queue_depth ||
        queued_eff + pending_eff >= sched->queue_cap) {
        return 0;
    }

    uint64_t total_live = 0;
    if (!add_u64_sched(sched->exec_reserved_bytes, next_retained, &total_live) ||
        total_live > sched->cfg.memory_budget_bytes) {
        return 0;
    }
    uint64_t new_peak = 0;
    if (!add_u64_sched(new_exec_working, next_retained, &new_peak) ||
        new_peak > sched->cfg.memory_budget_bytes) {
        return 0;
    }
    for (size_t i = 0; i < sched->queue_len; i++) {
        const rfc_job *qj = sched->queue[i];
        if (!qj || qj == ignore_queued) {
            continue;
        }
        uint64_t peak_q = 0;
        if (!add_u64_sched(qj->exec_working_bytes, next_retained, &peak_q) ||
            peak_q > sched->cfg.memory_budget_bytes) {
            return 0;
        }
    }
    return 1;
}

static rfc_status pre_reserve_submit_inputs(
    rfc_scheduler *sched,
    rfc_job_slot *slot,
    const rfc_provider_memory_info *src_mem,
    const rfc_provider_memory_info *dst_mem,
    const rfc_job_spec *spec,
    uint64_t effective_source_size,
    uint64_t retained_bytes,
    uint64_t expected_budget_limit,
    int *out_pending_admit,
    uint64_t *out_reserved_bytes) {
    *out_pending_admit = 0;
    *out_reserved_bytes = 0;

    if (slot) {
        rfc_mutex_lock(&slot->lock);
    }
    rfc_mutex_lock(&sched->lock);
    if (sched->destroying) {
        rfc_mutex_unlock(&sched->lock);
        if (slot) {
            rfc_mutex_unlock(&slot->lock);
        }
        return RFC_ERR_CANCELLED;
    }

    rfc_exactness_class ex = RFC_EXACT_PIXEL_EXACT;
    rfc_metadata_policy pol = RFC_META_POLICY_PRESERVE;
    (void)parse_verify_settings_sched(spec->target_format, spec->settings,
                                      spec->setting_count, &ex, &pol);
    rfc_limits live_limits;
    uint64_t live_reservation = 0;
    uint64_t live_exec_working = 0;
    uint64_t live_budget = 0;
    rfc_status st = compute_preflight_with_contracts(
        &sched->cfg, src_mem, dst_mem, ex, spec->requested_codec_threads,
        effective_source_size, spec->source_layout, spec->source_meta,
        retained_bytes, spec->expected_working_set_bytes,
        spec->job_memory_limit_bytes, spec->limits_override, &live_limits,
        &live_reservation, &live_exec_working, &live_budget);
    if (st != RFC_OK) {
        rfc_mutex_unlock(&sched->lock);
        if (slot) {
            rfc_mutex_unlock(&slot->lock);
        }
        return st;
    }
    if (live_budget != expected_budget_limit) {
        rfc_mutex_unlock(&sched->lock);
        if (slot) {
            rfc_mutex_unlock(&slot->lock);
        }
        return RFC_ERR_BUSY;
    }

    uint64_t next_retained = 0;
    if (!add_u64_sched(sched->retained_reserved_bytes, retained_bytes,
                       &next_retained)) {
        rfc_mutex_unlock(&sched->lock);
        if (slot) {
            rfc_mutex_unlock(&slot->lock);
        }
        return RFC_ERR_BUSY;
    }
    const rfc_job *prev = slot ? slot->current_job : NULL;
    if (!scheduler_capacity_ok_locked(sched, prev, 0u, next_retained,
                                      live_exec_working)) {
        rfc_mutex_unlock(&sched->lock);
        if (slot) {
            rfc_mutex_unlock(&slot->lock);
        }
        return RFC_ERR_BUSY;
    }

    sched->pending_admits++;
    sched->retained_reserved_bytes = next_retained;
    uint64_t total_live =
        sched->exec_reserved_bytes + sched->retained_reserved_bytes;
    if (total_live > sched->stats.peak_reserved_bytes) {
        sched->stats.peak_reserved_bytes = total_live;
    }
    *out_pending_admit = 1;
    *out_reserved_bytes = retained_bytes;

    rfc_mutex_unlock(&sched->lock);
    if (slot) {
        rfc_mutex_unlock(&slot->lock);
    }
    return RFC_OK;
}

static void release_pending_submit(rfc_scheduler *sched,
                                   int pending_admit,
                                   uint64_t reserved_bytes) {
    if (!sched || (!pending_admit && reserved_bytes == 0)) {
        return;
    }
    rfc_mutex_lock(&sched->lock);
    if (pending_admit && sched->pending_admits > 0) {
        sched->pending_admits--;
    }
    if (reserved_bytes > 0) {
        if (sched->retained_reserved_bytes >= reserved_bytes) {
            sched->retained_reserved_bytes -= reserved_bytes;
        } else {
            sched->retained_reserved_bytes = 0;
        }
        rfc_cond_broadcast(&sched->work_cond);
    }
    rfc_mutex_unlock(&sched->lock);
}

static rfc_status admit_prepared_job(rfc_scheduler *sched,
                                     rfc_job_slot *slot,
                                     int update_slot_spec,
                                     int *pending_admit_inout,
                                     const rfc_provider_memory_info *src_mem,
                                     const rfc_provider_memory_info *dst_mem,
                                     rfc_format_id source_format,
                                     rfc_format_id target_format,
                                     rfc_job_inputs *inputs,
                                     uint32_t requested_codec_threads,
                                     uint64_t expected_working_set_bytes,
                                     uint64_t job_memory_limit_bytes,
                                     const rfc_limits *limits_override,
                                     int has_stage_ops,
                                     const rfc_pipeline_stage_ops *stage_ops,
                                     rfc_user_ctx_box *ctx_box,
                                     rfc_progress_fn progress_fn,
                                     void *progress_user,
                                     const rfc_limits *eff_limits,
                                     uint64_t reservation_bytes,
                                     uint64_t exec_working_bytes,
                                     uint64_t budget_limit_bytes,
                                     rfc_job **out_job) {
    rfc_scheduler_internal_note_submit_alloc(sched);
    rfc_job *job = rfc_job_internal_create();
    if (!job) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    rfc_scheduler_internal_note_submit_alloc(sched);
    job->budget = rfc_budget_create(budget_limit_bytes);
    if (!job->budget) {
        rfc_job_release(job);
        return RFC_ERR_OUT_OF_MEMORY;
    }
    rfc_job_internal_bind_budget(job, job->budget);

    job->source_format = source_format;
    job->target_format = target_format;
    rfc_job_inputs_retain(inputs);
    job->inputs = inputs;
    job->limits = *eff_limits;
    job->has_limits_override = (limits_override != NULL);
    job->requested_threads =
        requested_codec_threads ? requested_codec_threads : 1u;
    job->reserved_memory_bytes = reservation_bytes;
    job->exec_working_bytes = exec_working_bytes;
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

    if (slot) {
        rfc_mutex_lock(&slot->lock);
        if (!update_slot_spec && !slot->has_last_spec) {
            rfc_mutex_unlock(&slot->lock);
            rfc_job_release(job);
            return RFC_ERR_INVALID_ARGUMENT;
        }
        job->slot_state = slot->state;
        rfc_slot_state_retain(job->slot_state);
    }

    rfc_mutex_lock(&sched->lock);
    if (sched->destroying) {
        rfc_mutex_unlock(&sched->lock);
        if (slot) {
            rfc_mutex_unlock(&slot->lock);
        }
        rfc_job_release(job);
        return RFC_ERR_CANCELLED;
    }

    rfc_exactness_class ex = RFC_EXACT_PIXEL_EXACT;
    rfc_metadata_policy pol = RFC_META_POLICY_PRESERVE;
    (void)parse_verify_settings_sched(job->target_format, inputs->settings,
                                      inputs->setting_count, &ex, &pol);
    rfc_limits live_limits;
    uint64_t live_reservation = 0;
    uint64_t live_exec_working = 0;
    uint64_t live_budget = 0;
    rfc_status st = compute_preflight_with_contracts(
        &sched->cfg, src_mem, dst_mem, ex, requested_codec_threads,
        inputs->effective_source_size,
        inputs->has_layout ? &inputs->source_layout : NULL,
        inputs->has_meta ? &inputs->source_meta : NULL, inputs->retained_bytes,
        expected_working_set_bytes, job_memory_limit_bytes, limits_override,
        &live_limits, &live_reservation, &live_exec_working, &live_budget);
    if (st != RFC_OK) {
        rfc_mutex_unlock(&sched->lock);
        if (slot) {
            rfc_mutex_unlock(&slot->lock);
        }
        rfc_job_release(job);
        return st;
    }
    job->limits = live_limits;
    job->reserved_memory_bytes = live_reservation;
    job->exec_working_bytes = live_exec_working;
    job->budget_limit_bytes = live_budget;
    if (live_budget != budget_limit_bytes) {
        /* Concurrent reconfigure changed limit; rebuild job budget outside lock or fail conservatively. */
        rfc_mutex_unlock(&sched->lock);
        if (slot) {
            rfc_mutex_unlock(&slot->lock);
        }
        rfc_job_release(job);
        return RFC_ERR_BUSY;
    }

    rfc_job *prev = slot ? slot->current_job : NULL;
    size_t prev_q_idx = SIZE_MAX;
    if (prev) {
        rfc_mutex_lock(&prev->lock);
        for (size_t i = 0; i < sched->queue_len; i++) {
            if (sched->queue[i] == prev) {
                prev_q_idx = i;
                break;
            }
        }
    }

    size_t self_pending =
        (pending_admit_inout && *pending_admit_inout) ? 1u : 0u;
    if (!scheduler_capacity_ok_locked(sched, prev, self_pending,
                                      sched->retained_reserved_bytes,
                                      job->exec_working_bytes)) {
        if (prev) {
            rfc_mutex_unlock(&prev->lock);
        }
        rfc_mutex_unlock(&sched->lock);
        if (slot) {
            rfc_mutex_unlock(&slot->lock);
        }
        rfc_job_release(job);
        return RFC_ERR_BUSY;
    }

    rfc_job_id new_id = sched->next_job_id++;
    job->job_id = new_id;
    job->sched = sched;
    rfc_scheduler_internal_retain(sched);

    rfc_job *drop_queue_prev = NULL;
    rfc_job *drop_slot_prev = NULL;
    rfc_job_inputs *old_slot_inputs = NULL;
    rfc_user_ctx_box *old_slot_box = NULL;
    int same_cb_thread = 0;

    if (slot) {
        if (update_slot_spec) {
            old_slot_inputs = slot->inputs;
            old_slot_box = slot->user_ctx_box;

            rfc_job_inputs_retain(inputs);
            slot->inputs = inputs;
            if (ctx_box) {
                rfc_user_ctx_box_retain(ctx_box);
            }
            slot->user_ctx_box = ctx_box;
            slot->has_last_spec = 1;
            slot->source_format = source_format;
            slot->target_format = target_format;
            slot->requested_codec_threads = requested_codec_threads;
            slot->expected_working_set_bytes = expected_working_set_bytes;
            slot->job_memory_limit_bytes = job_memory_limit_bytes;
            if (limits_override) {
                slot->owned_limits = *limits_override;
                slot->has_limits = 1;
            } else {
                slot->has_limits = 0;
            }
            slot->has_stage_ops = has_stage_ops;
            if (stage_ops) {
                slot->stage_ops = *stage_ops;
            } else {
                memset(&slot->stage_ops, 0, sizeof(slot->stage_ops));
            }
            slot->progress_fn = progress_fn;
            slot->progress_user = progress_user;
        }

        slot->attempt_count++;
        job->attempt_number = slot->attempt_count;
        rfc_slot_state_set_id(slot->state, new_id);

        if (prev) {
            int prev_publish_in_flight =
                prev->published || prev->state == RFC_JOB_SUCCEEDED ||
                (prev->state == RFC_JOB_RUNNING &&
                 (prev->publishing || prev->publish_decided));
            if (!prev_publish_in_flight) {
                prev->superseded_by_id = new_id;
                prev->suppress_callbacks = 1;
            }
            same_cb_thread =
                prev->cb_owner_valid &&
                rfc_thread_id_equal(prev->cb_owner, rfc_thread_id_current());
            if (prev_q_idx < sched->queue_len) {
                prev->superseded = 1;
                prev->cancel_requested = 1;
                if (prev->state == RFC_JOB_QUEUED) {
                    prev->state = RFC_JOB_SUPERSEDED;
                    prev->status = RFC_ERR_STALE_JOB;
                    prev->failing_stage = prev->stage;
                    rfc_job_internal_set_detail(prev->detail,
                                                "superseded by retry");
                    rfc_cond_broadcast(&prev->done_cond);
                    sched->stats.total_superseded++;
                }
                drop_queue_prev = pop_queue_at(sched, prev_q_idx);
            } else if (prev->state == RFC_JOB_RUNNING && !prev->published &&
                       !prev->publishing && !prev->publish_decided) {
                prev->superseded = 1;
                prev->cancel_requested = 1;
                enqueue_active_cancel_locked(sched, prev);
            }
            rfc_mutex_unlock(&prev->lock);
            drop_slot_prev = prev;
            slot->current_job = NULL;
        }
        rfc_job_retain(job);
        slot->current_job = job;
    } else {
        job->attempt_number = 1u;
    }

    if (out_job) {
        rfc_job_retain(job);
        *out_job = job;
    }

    if (pending_admit_inout && *pending_admit_inout) {
        if (sched->pending_admits > 0) {
            sched->pending_admits--;
        }
        *pending_admit_inout = 0;
    }
    rfc_job_inputs_arm_io_ownership(inputs);
    sched->queue[sched->queue_len++] = job;
    sched->stats.queued_jobs = (uint32_t)sched->queue_len;
    sched->stats.total_submitted++;
    uint64_t total_live =
        sched->exec_reserved_bytes + sched->retained_reserved_bytes;
    if (total_live > sched->stats.peak_reserved_bytes) {
        sched->stats.peak_reserved_bytes = total_live;
    }
    rfc_cond_broadcast(&sched->work_cond);
    rfc_mutex_unlock(&sched->lock);
    if (slot) {
        rfc_mutex_unlock(&slot->lock);
    }

    if (drop_slot_prev) {
        wait_job_callbacks_drained(drop_slot_prev, same_cb_thread);
    }
    if (drop_queue_prev) {
        rfc_job_release_partials(drop_queue_prev);
        rfc_job_release(drop_queue_prev);
    }
    if (drop_slot_prev) {
        rfc_job_release(drop_slot_prev);
    }
    rfc_job_inputs_release(old_slot_inputs);
    rfc_user_ctx_box_release(old_slot_box);
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
    if (spec->io_source &&
        (spec->source_bytes != NULL || spec->source_size > 0u ||
         (spec->has_stage_ops && spec->stage_ops.on_import != NULL))) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (spec->io_sink &&
        (spec->has_stage_ops && spec->stage_ops.on_export != NULL)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    rfc_scheduler_config cur_cfg;
    rfc_status st = rfc_scheduler_get_config(sched, &cur_cfg);
    if (st != RFC_OK) {
        return st;
    }

    rfc_format_id eff_source_format = spec->source_format;
    uint64_t eff_source_size = (uint64_t)spec->source_size;
    rfc_file_identity sniff_src_id;
    memset(&sniff_src_id, 0, sizeof(sniff_src_id));
    rfc_sniff_geometry pre_geo;
    memset(&pre_geo, 0, sizeof(pre_geo));
    if (spec->io_source) {
        const rfc_limits *pre_lim =
            spec->limits_override ? spec->limits_override
                                  : &cur_cfg.default_limits;
        st = rfc_limits_validate(pre_lim);
        if (st != RFC_OK) {
            return st;
        }
        rfc_sniff_result pre_sniff;
        uint64_t sniff_sz = 0u;
        st = rfc_input_sniff_source_preflight(
            pre_lim, spec->io_source, spec->source_format, &pre_sniff,
            &sniff_sz, &sniff_src_id, &pre_geo);
        if (st != RFC_OK) {
            return st;
        }
        eff_source_format = pre_sniff.content_format;
        eff_source_size = sniff_sz;
    }

    /* Decoding work scales with the frame, not the compressed size: when the
     * caller gave neither a layout nor a working-set estimate, size a PNG,
     * JPEG, TIFF, GIF, HEIF or HEIC job from the geometry the sniff read (byte-identical
     * copies and JPEG XL original-byte reconstruction keep their source-size
     * reservations; an explicit estimate keeps its caller's sizing). */
    rfc_job_spec geo_spec;
    rfc_layout geo_layout;
    if (!spec->source_layout && spec->expected_working_set_bytes == 0u &&
        (eff_source_format == RFC_FORMAT_PNG ||
         eff_source_format == RFC_FORMAT_JPEG ||
         eff_source_format == RFC_FORMAT_TIFF ||
         eff_source_format == RFC_FORMAT_GIF ||
         eff_source_format == RFC_FORMAT_HEIF ||
         eff_source_format == RFC_FORMAT_HEIC)) {
        rfc_sniff_geometry geo;
        memset(&geo, 0, sizeof(geo));
        if (spec->io_source) {
            geo = pre_geo;
        } else if (spec->source_bytes && spec->source_size > 0u) {
            rfc_sniff_result geo_sniff;
            const rfc_limits *geo_lim = spec->limits_override
                                            ? spec->limits_override
                                            : &cur_cfg.default_limits;
            (void)rfc_input_internal_sniff_bytes_geometry(
                spec->source_bytes, spec->source_size, NULL, geo_lim,
                &geo_sniff, &geo);
        }
        rfc_exactness_class geo_ex = RFC_EXACT_PIXEL_EXACT;
        rfc_metadata_policy geo_pol = RFC_META_POLICY_PRESERVE;
        if (rfc_job_internal_resolve_exactness(
                spec->target_format, spec->settings, spec->setting_count,
                &geo_ex, &geo_pol) == RFC_OK &&
            (geo_ex == RFC_EXACT_PIXEL_EXACT ||
             geo_ex == RFC_EXACT_COEFFICIENT_EXACT) &&
            geo.has_geometry) {
            memset(&geo_layout, 0, sizeof(geo_layout));
            geo_layout.format = geo.format;
            geo_layout.alpha = (geo.format == RFC_SAMPLE_RGBA8 ||
                                geo.format == RFC_SAMPLE_RGBA16)
                                   ? RFC_ALPHA_STRAIGHT
                                   : RFC_ALPHA_NONE;
            geo_layout.width = geo.width;
            geo_layout.height = geo.height;
            geo_layout.bits_per_sample =
                (geo.format == RFC_SAMPLE_GRAY16 ||
                 geo.format == RFC_SAMPLE_RGB16 ||
                 geo.format == RFC_SAMPLE_RGBA16)
                    ? 16u
                    : 8u;
            if (rfc_layout_normalize_stride(&geo_layout, &geo_layout) !=
                RFC_OK) {
                geo_layout.stride_bytes = 0u;
            }
            if (geo_layout.stride_bytes > 0u) {
                geo_spec = *spec;
                geo_spec.source_layout = &geo_layout;
                spec = &geo_spec;
            }
        }
    }
    if (spec->io_sink) {
        st = rfc_output_validate_sink_preflight(
            spec->io_sink, spec->io_source,
            sniff_src_id.valid ? &sniff_src_id : NULL);
        if (st != RFC_OK) {
            return st;
        }
    }

    uint64_t retained_bytes = 0;
    st = rfc_job_internal_compute_retained_bytes(
        spec->source_bytes, spec->source_size, spec->source_meta,
        spec->settings, spec->setting_count, spec->io_source, spec->io_sink,
        &retained_bytes);
    if (st != RFC_OK) {
        return st;
    }

    rfc_provider_memory_info src_mem;
    rfc_provider_memory_info dst_mem;
    rfc_limits eff_limits;
    uint64_t reservation_bytes = 0;
    uint64_t exec_working_bytes = 0;
    uint64_t budget_limit_bytes = 0;
    const rfc_pipeline_stage_ops *stage_ops =
        spec->has_stage_ops ? &spec->stage_ops : NULL;
    st = compute_preflight(
        &cur_cfg, eff_source_format, spec->target_format,
        spec->requested_codec_threads, eff_source_size, spec->source_layout,
        spec->source_meta, spec->settings, spec->setting_count,
        spec->acknowledged_unsupported_metadata, stage_ops, retained_bytes,
        spec->expected_working_set_bytes, spec->job_memory_limit_bytes,
        spec->limits_override, &src_mem, &dst_mem, &eff_limits,
        &reservation_bytes, &exec_working_bytes, &budget_limit_bytes);
    if (st != RFC_OK) {
        return st;
    }

    int pending_admit = 0;
    uint64_t reserved_remaining = 0;
    st = pre_reserve_submit_inputs(
        sched, slot, &src_mem, &dst_mem, spec, eff_source_size, retained_bytes,
        budget_limit_bytes, &pending_admit, &reserved_remaining);
    if (st != RFC_OK) {
        return st;
    }

    rfc_job_inputs *inputs = rfc_job_inputs_create(
        sched, &reserved_remaining, spec->source_bytes, spec->source_size,
        spec->source_layout, spec->source_meta, spec->settings,
        spec->setting_count, spec->acknowledged_unsupported_metadata,
        spec->io_source, spec->io_sink, eff_source_size);
    if (!inputs) {
        release_pending_submit(sched, pending_admit, reserved_remaining);
        return RFC_ERR_OUT_OF_MEMORY;
    }

    rfc_user_ctx_box *ctx_box = NULL;
    if (spec->stage_ops.user_ctx || spec->stage_ops.destroy_user_ctx) {
        rfc_scheduler_internal_note_submit_alloc(sched);
        ctx_box = rfc_user_ctx_box_create(spec->stage_ops.user_ctx,
                                          spec->stage_ops.destroy_user_ctx);
        if (!ctx_box) {
            release_pending_submit(sched, pending_admit, 0u);
            rfc_job_inputs_release(inputs);
            return RFC_ERR_OUT_OF_MEMORY;
        }
    }

    st = admit_prepared_job(
        sched, slot, 1, &pending_admit, &src_mem, &dst_mem, eff_source_format,
        spec->target_format, inputs, spec->requested_codec_threads,
        spec->expected_working_set_bytes, spec->job_memory_limit_bytes,
        spec->limits_override, spec->has_stage_ops, &spec->stage_ops, ctx_box,
        spec->progress_fn, spec->progress_user, &eff_limits, reservation_bytes,
        exec_working_bytes, budget_limit_bytes, out_job);

    release_pending_submit(sched, pending_admit, 0u);
    rfc_job_inputs_release(inputs);
    rfc_user_ctx_box_release(ctx_box);
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

    rfc_mutex_lock(&slot->lock);
    if (!slot->has_last_spec || !slot->inputs) {
        rfc_mutex_unlock(&slot->lock);
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (slot->inputs->sched && slot->inputs->sched != sched) {
        rfc_mutex_unlock(&slot->lock);
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_format_id source_format = slot->source_format;
    rfc_format_id target_format = slot->target_format;
    rfc_job_inputs *inputs = slot->inputs;
    rfc_job_inputs_retain(inputs);
    rfc_user_ctx_box *ctx_box = slot->user_ctx_box;
    rfc_user_ctx_box_retain(ctx_box);
    uint32_t requested_threads = slot->requested_codec_threads;
    uint64_t expected_working_set = slot->expected_working_set_bytes;
    uint64_t job_memory_limit = slot->job_memory_limit_bytes;
    rfc_limits owned_limits = slot->owned_limits;
    int has_limits = slot->has_limits;
    int has_stage_ops = slot->has_stage_ops;
    rfc_pipeline_stage_ops stage_ops = slot->stage_ops;
    rfc_progress_fn progress_fn = slot->progress_fn;
    void *progress_user = slot->progress_user;
    rfc_mutex_unlock(&slot->lock);

    rfc_scheduler_config cur_cfg;
    rfc_status st = rfc_scheduler_get_config(sched, &cur_cfg);
    if (st != RFC_OK) {
        rfc_job_inputs_release(inputs);
        rfc_user_ctx_box_release(ctx_box);
        return st;
    }

    rfc_provider_memory_info src_mem;
    rfc_provider_memory_info dst_mem;
    rfc_limits eff_limits;
    uint64_t reservation_bytes = 0;
    uint64_t exec_working_bytes = 0;
    uint64_t budget_limit_bytes = 0;
    st = compute_preflight(
        &cur_cfg, source_format, target_format, requested_threads,
        inputs->effective_source_size,
        inputs->has_layout ? &inputs->source_layout : NULL,
        inputs->has_meta ? &inputs->source_meta : NULL, inputs->settings,
        inputs->setting_count, inputs->acknowledged_unsupported_metadata,
        has_stage_ops ? &stage_ops : NULL, inputs->retained_bytes,
        expected_working_set, job_memory_limit, has_limits ? &owned_limits : NULL,
        &src_mem, &dst_mem, &eff_limits, &reservation_bytes, &exec_working_bytes,
        &budget_limit_bytes);
    if (st != RFC_OK) {
        rfc_job_inputs_release(inputs);
        rfc_user_ctx_box_release(ctx_box);
        return st;
    }

    st = admit_prepared_job(
        sched, slot, 0, NULL, &src_mem, &dst_mem, source_format, target_format,
        inputs, requested_threads, expected_working_set, job_memory_limit,
        has_limits ? &owned_limits : NULL, has_stage_ops, &stage_ops, ctx_box,
        progress_fn, progress_user, &eff_limits, reservation_bytes,
        exec_working_bytes, budget_limit_bytes, out_job);

    rfc_job_inputs_release(inputs);
    rfc_user_ctx_box_release(ctx_box);
    return st;
}
