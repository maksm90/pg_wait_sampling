/*
 * collector.c
 *		Collector of wait event history and profile.
 *
 * Copyright (c) 2015-2016, Postgres Professional
 *
 * IDENTIFICATION
 *	  contrib/pg_wait_sampling/collector.c
 */
#include "postgres.h"

#if PG_VERSION_NUM >= 130000
#include "common/hashfn.h"
#endif
#include "pgstat.h"
#include "postmaster/bgworker.h"
#if PG_VERSION_NUM >= 130000
#include "postmaster/interrupt.h"
#endif
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "utils/guc.h"

#include "compat.h"
#include "pg_wait_sampling.h"

static const double USAGE_INIT = 1.0;
static const double USAGE_INCREASE = 1.0;
static const double USAGE_DECREASE_FACTOR = 0.99;
static const int USAGE_DEALLOC_PERCENT = 5;
static const int USAGE_DEALLOC_MIN_NUM = 10;

#if PG_VERSION_NUM < 130000
static volatile sig_atomic_t ShutdownRequestPending = false;
static void
SignalHandlerForShutdownRequest(SIGNAL_ARGS)
{
	int save_errno = errno;
	ShutdownRequestPending = true;
	if (MyProc)
		SetLatch(&MyProc->procLatch);
	errno = save_errno;
}
#endif

/*
 * qsort comparator for sorting into increasing usage order
 */
static int
entry_cmp(const void *lhs, const void *rhs)
{
	double l_usage = (*(ProfileHashEntry *const *) lhs)->usage;
	double r_usage = (*(ProfileHashEntry *const *) rhs)->usage;

	if (l_usage < r_usage)
		return -1;
	else if (l_usage > r_usage)
		return +1;
	else
		return 0;
}

/*
 * Deallocate least used entries in profile hashtable.
 * Caller must hold an exclusive lock.
 */
static void
pgws_entry_dealloc()
{
	HASH_SEQ_STATUS hash_seq;
	ProfileHashEntry **entries;
	ProfileHashEntry  *entry;
	int			nvictims;
	int			i;

	/*
	 * Sort entries by usage and deallocate USAGE_DEALLOC_PERCENT of them.
	 * While we're scanning the table, apply the decay factor to the usage
	 * values.
	 */
	entries = palloc(
		hash_get_num_entries(pgws_profile_hash) * sizeof(ProfileHashEntry *)
	);

	i = 0;
	hash_seq_init(&hash_seq, pgws_profile_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		entries[i++] = entry;
		entry->usage *= USAGE_DECREASE_FACTOR;
	}

	qsort(entries, i, sizeof(ProfileHashEntry *), entry_cmp);

	/*
	 * We remove USAGE_DEALLOC_PERCENT number of entries or at least
	 * USAGE_DEALLOC_MIN_NUM entries if full number of existing entries is not
	 * less
	 */
	nvictims = Max(USAGE_DEALLOC_MIN_NUM, i * USAGE_DEALLOC_PERCENT / 100);
	nvictims = Min(nvictims, i);

	for (i = 0; i < nvictims; i++)
	{
		hash_search(pgws_profile_hash, &entries[i]->key, HASH_REMOVE, NULL);
	}

	pfree(entries);
}

/*
 * Read current waits from backends and write them to shared structures
 */
static void
probe_waits(const bool write_history, const bool write_profile)
{
	if (write_profile)
		LWLockAcquire(pgws_profile_lock, LW_EXCLUSIVE);
	if (write_history)
		LWLockAcquire(pgws_history_lock, LW_EXCLUSIVE);

	/*
	 * Iterate PGPROCs
	 */
	for (int i = 0; i < ProcGlobal->allProcCount; i++)
	{
		PGPROC	   *proc = GetPGProcByNumber(i);
		pgwsQueryId queryId =
			pgwsWhetherProfileQueryId ? pgws_proc_queryids[i] : 0;
		uint32 	 	wait_event_info = proc->wait_event_info;
		pid_t		pid = proc->pid;

		/*
		 * FIXME:
		 * Non-zero pid actually doesn't imply that process slot is not freed.
		 * After process termination this field becomes unchanged and thereby
		 * stores the pid of previous process. The possible indicator of process
		 * termination might be a condition `proc->procLatch->owner_pid == 0`.
		 * However, there is no any way to protect `owner_pid` field from
		 * concurrent modifications. So its using might result in race
		 * conditions.
		 *
		 * Another option is to use the lists of freed PGPROCs from ProcGlocal:
		 * freeProcs, walsenderFreeProcs, bgworkerFreeProcs and autovacFreeProcs
		 * to define indexes of all freed slots in allProcs. But this requires
		 * acquiring ProcStructLock spinlock that is impractical for iteration
		 * over so long lists.
		 *
		 * The most appropriate solution here is to iterate over ProcArray items
		 * under ProcArrayLock and over AuxiliaryProcs under ProcStructLock
		 * spinlock (AuxiliaryProcs contains just NUM_AUXILIARY_PROCS=5 slots)
		 * or without any locks as it's done in pg_stat_get_activity() function.
		 * These arrays are not accessible externally and require to add some
		 * iterator object into corresponding containing modules.
		 */
		if (pid == 0)
			continue;

		// TODO: take into account the state without waiting as CPU time
		if (wait_event_info == 0)
			continue;

		/* Write to the history if needed */
		if (write_history)
		{
			int index = pgws_history_ring->index % pgwsHistoryBufferSize;

			pgws_history_ring->items[index] = (HistoryItem) {
				pid, wait_event_info, queryId, GetCurrentTimestamp()
			};
			pgws_history_ring->index++;
		}

		/* Write to the profile if needed */
		if (write_profile)
		{
			ProfileHashKey		key;
			ProfileHashEntry   *entry;

			/* Set up key for hashtable search */
			key.pid = pgwsWhetherProfilePid ? pid : 0;
			key.wait_event_info = wait_event_info;
			key.queryid = queryId;

			/* Lookup the hash table entry with exclusive lock */
			entry = (ProfileHashEntry *)
				hash_search(pgws_profile_hash, &key, HASH_FIND, NULL);

			/* Create new entry, if not present */
			if (!entry)
			{

				/* Make space if needed */
				while (hash_get_num_entries(pgws_profile_hash) >= pgwsMaxProfileEntries)
					pgws_entry_dealloc();

				entry = (ProfileHashEntry *)
					hash_search(pgws_profile_hash, &key, HASH_ENTER_NULL, NULL);
				Assert(entry);

				entry->counter = 1;
				entry->usage = USAGE_INIT;
			}
			else
			{
				entry->counter++;
				entry->usage += USAGE_INCREASE;
			}
		}
	}

	if (write_history)
		LWLockRelease(pgws_history_lock);
	if (write_profile)
		LWLockRelease(pgws_profile_lock);
}

/*
 * Delta between two timestamps in milliseconds.
 */
static int64
millisecs_diff(TimestampTz tz1, TimestampTz tz2)
{
	long	secs;
	int		microsecs;

	TimestampDifference(tz1, tz2, &secs, &microsecs);

	return secs * 1000 + microsecs / 1000;

}

/*
 * Main routine of wait history collector.
 */
void
pgws_collector_main(Datum main_arg)
{
	TimestampTz		current_ts,
					history_ts,
					profile_ts;

	/*
	 * Establish signal handlers.
	 *
	 * We want to respond to the ProcSignal notifications.  This is done in the
	 * upstream provided procsignal_sigusr1_handler, which is automatically used
	 * if a bgworker connects to a database. But since our worker doesn't
	 * connect to any database even though it calls InitPostgres, which will
	 * still initializze a new backend and thus partitipate to the ProcSignal
	 * infrastructure.
	 */
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	BackgroundWorkerUnblockSignals();
	InitPostgresCompat(NULL, InvalidOid, NULL, InvalidOid, false, false, NULL);
	SetProcessingMode(NormalProcessing);

	/* Make pg_wait_sampling recognisable in pg_stat_activity */
	pgstat_report_appname("pg_wait_sampling collector");

	ereport(LOG, (errmsg("pg_wait_sampling collector started")));

	/* Start counting time for history and profile samples */
	profile_ts = history_ts = GetCurrentTimestamp();

	while (1)
	{
		int		rc;
		int64	history_diff,
				profile_diff;
		bool	write_history,
				write_profile;
		int		history_timeout,
				profile_timeout,
				actual_timeout;

		/* Clear any already-pending wakeups */
		ResetLatch(MyLatch);

		/* We need an explicit call for at least ProcSignal notifications. */
		CHECK_FOR_INTERRUPTS();

		/* Process any requests or signals received recently */
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		/* Shutdown if requested */
		if (ShutdownRequestPending)
			break;

		/* Calculate time for the next sample of history or profile */
		current_ts = GetCurrentTimestamp();
		history_diff = millisecs_diff(history_ts, current_ts);
		profile_diff = millisecs_diff(profile_ts, current_ts);

		/* Write profile or history */
		write_history = pgwsHistoryPeriod &&
			(history_diff >= (int64) pgwsHistoryPeriod);
		write_profile = pgwsProfilePeriod &&
			(profile_diff >= (int64) pgwsProfilePeriod);
		if (write_history || write_profile)
		{
			probe_waits(write_history, write_profile);

			if (write_history)
			{
				history_ts = current_ts;
				history_diff = 0;
			}

			if (write_profile)
			{
				profile_ts = current_ts;
				profile_diff = 0;
			}
		}

		/* Wait until next sample time */
		history_timeout = pgwsHistoryPeriod >= (int) history_diff ?
			pgwsHistoryPeriod - (int) history_diff : 0;
		profile_timeout = pgwsProfilePeriod >= (int) profile_diff ?
			pgwsProfilePeriod - (int) profile_diff : 0;

		actual_timeout = 0;
		if (pgwsProfilePeriod && !pgwsHistoryPeriod)
			actual_timeout = profile_timeout;
		else if (pgwsHistoryPeriod && !pgwsProfilePeriod)
			actual_timeout = history_timeout;
		else if (pgwsHistoryPeriod && pgwsProfilePeriod)
			actual_timeout = Min(history_timeout, profile_timeout);

		rc = WaitLatchCompat(MyLatch,
				WL_LATCH_SET | WL_POSTMASTER_DEATH |
					(pgwsHistoryPeriod || pgwsProfilePeriod ? WL_TIMEOUT : 0),
				actual_timeout, PG_WAIT_EXTENSION);

		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);
	}

	/*
	 * We're done.  Explicitly detach the shared memory segment so that we
	 * don't get a resource leak warning at commit time.  This will fire any
	 * on_dsm_detach callbacks we've registered, as well.  Once that's done,
	 * we can go ahead and exit.
	 */
	ereport(LOG, (errmsg("pg_wait_sampling collector shutting down")));
	proc_exit(0);
}
