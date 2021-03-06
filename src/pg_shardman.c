/* -------------------------------------------------------------------------
 *
 * pg_shardman.c
 *		This module contains background worker accepting sharding tasks for
 *		execution, membership commands implementation and common routines for
 *		querying the metadata.
 *
 * Copyright (c) 2017, Postgres Professional
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/time.h> /* For 'select' portability */
#include <sys/select.h>

/* These are always necessary for a bgworker */
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"

#include "catalog/pg_subscription_rel.h"
#include "utils/memutils.h"
#include "utils/guc.h"
#include "utils/snapmgr.h"
#include "utils/builtins.h"
#include "executor/spi.h"
#include "access/xact.h"
#include "commands/extension.h"
#include "commands/sequence.h"
#include "libpq-fe.h"

#include "pg_shardman.h"
#include "shard.h"
#include "shardman_hooks.h"


/* ensure that extension won't load against incompatible version of Postgres */
PG_MODULE_MAGIC;

static Cmd *next_cmd(void);
static PGconn *listen_cmd_log_inserts(void);
static void wait_notify(void);
static void shardlord_sigterm(SIGNAL_ARGS);
static void shardlord_sigusr1(SIGNAL_ARGS);
static bool pg_shardman_installed_local(void);

static void add_node(Cmd *cmd);
static bool node_in_cluster(int id);

static void rm_node(Cmd *cmd);

/* flags set by signal handlers */
volatile sig_atomic_t got_sigterm = false;
volatile sig_atomic_t got_sigusr1 = false;

/* GUC variables */
bool shardman_shardlord;
char *shardman_shardlord_dbname;
char *shardman_shardlord_connstring;
int shardman_cmd_retry_naptime;
int shardman_poll_interval;
int shardman_my_id;
bool shardman_sync_replicas;

/* Just global vars. */
/* Connection to local server for LISTEN notifications. Is is global for easy
 * cleanup after receiving SIGTERM.
 */
static PGconn *conn = NULL;
int32 shardman_my_node_id = -1;

/*
 * Entrypoint of the module. Define variables and register background worker.
 */
void
_PG_init()
{
	BackgroundWorker shardlord_bgw;
	char *desc;

	if (!process_shared_preload_libraries_in_progress)
	{
		ereport(ERROR, (errmsg("pg_shardman can only be loaded via shared_preload_libraries"),
						errhint("Add pg_shardman to shared_preload_libraries.")));
	}

	/* remember & set hooks */
	old_log_hook = emit_log_hook;
	emit_log_hook = shardman_log;

	DefineCustomBoolVariable("shardman.shardlord",
							 "This node is the shardlord?",
							 NULL,
							 &shardman_shardlord,
							 false,
							 PGC_POSTMASTER,
							 0,
							 NULL, NULL, NULL);

	DefineCustomStringVariable(
		"shardman.shardlord_dbname",
		"Active only if shardman.shardlord is on. Name of the database"
		" on shardlord node, shardlord bgw will connect to it",
		NULL,
		&shardman_shardlord_dbname,
		NULL,
		PGC_POSTMASTER,
		0,
		NULL, NULL, NULL
		);

	DefineCustomStringVariable(
		"shardman.shardlord_connstring",
		"Active only if shardman.shardlord is on. Connstring to reach shardlord from"
		"worker nodes to set up logical replication",
		NULL,
		&shardman_shardlord_connstring,
		"",
		PGC_POSTMASTER,
		0,
		NULL, NULL, NULL
		);

	DefineCustomIntVariable("shardman.cmd_retry_naptime",
							"Sleep time in millisec between retrying to execute failing command",
							NULL,
							&shardman_cmd_retry_naptime,
							10000,
							0,
							INT_MAX,
							PGC_SIGHUP,
							0,
							NULL, NULL, NULL);

	desc = "Unfortunately, some actions are not yet implemented using proper"
		"notifications and we need to poll the target node to learn progress."
		"This variable specifies how often (in milliseconds) we do that.";
	DefineCustomIntVariable("shardman.poll_interval",
							desc,
							NULL,
							&shardman_poll_interval,
							10000,
							0,
							INT_MAX,
							PGC_SIGHUP,
							0,
							NULL, NULL, NULL);

	/*
	 * This GUC is used to include node id in log messages. It is set with
	 * alter system, user should never do that. This is ugly, but
	 * - id must be known by all backends
	 * - we must have easy way to update it
	 * - it must be presistent, and we must reread it on server startup
	 * GUC is ugly, because it is stored in conf file, may be reset by user,
	 * global (it is harder to support multiple databases) and alter system
	 * cannot be executed in transaction.
	 * Another idea we considered was to store it in shmem. However, there
	 * seems to be no easy way to execute something (read id from disk and
	 * set it in shmem) on server start. This could be accomplished with
	 * separate bgw, but that's much more code. Besides, we must know in which
	 * database shardman lies to connect to db from bgw, and it seems like
	 * there isn't much space for that but in GUC itself (or any other global
	 * text file), so why bother with that?
	 */
	DefineCustomIntVariable("shardman.my_id",
							"node id to be included in logs",
							NULL,
							&shardman_my_id,
							SHMN_INVALID_NODE_ID,
							SHMN_INVALID_NODE_ID,
							INT_MAX,
							PGC_SIGHUP,
							0,
							NULL, NULL, NULL);

	DefineCustomBoolVariable("shardman.sync_replicas",
							 "Make replicas synchronous?",
							 NULL,
							 &shardman_sync_replicas,
							 false,
							 PGC_POSTMASTER,
							 0,
							 NULL, NULL, NULL);


	if (shardman_shardlord)
	{
		/* register shardlord */
		sprintf(shardlord_bgw.bgw_name, "shardlord");
		shardlord_bgw.bgw_flags = BGWORKER_SHMEM_ACCESS |
			BGWORKER_BACKEND_DATABASE_CONNECTION;
		shardlord_bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
		/* + 999 to round up */
		shardlord_bgw.bgw_restart_time =
			(shardman_cmd_retry_naptime + 999) / 1000L;
		sprintf(shardlord_bgw.bgw_library_name, "pg_shardman");
		sprintf(shardlord_bgw.bgw_function_name, "shardlord_main");
		shardlord_bgw.bgw_notify_pid = 0;
		RegisterBackgroundWorker(&shardlord_bgw);
	}
	/* TODO: clean up publications if we were shardlord before */
}

/*
 * Module unload callback. Currently Postgres never unloads shlibs, but who
 * knows the future...
 */
void
_PG_fini(void)
{
	/* Uninstall hooks. */
	emit_log_hook = old_log_hook;
}

/*
 * shardlord bgw starts here
 */
void
shardlord_main(Datum main_arg)
{
	Cmd *cmd;
	MemoryContext old_ctx;
	MemoryContext cmd_ctx;

	shmn_elog(LOG, "Shardlord started");
	if (shardman_shardlord_dbname == NULL)
		shmn_elog(FATAL, "shardlord_dbname is not specified");
	/* Connect to the database to use SPI*/
	BackgroundWorkerInitializeConnection(shardman_shardlord_dbname, NULL);
	/* sanity check */
	if (!pg_shardman_installed_local())
	{
		/* shardlord won't run without extension */
		shmn_elog(INFO,
				  "Terminating shardlord: pg_shardman lib is preloaded, but ext is not created");
		proc_exit(1);
	}

	/* Establish signal handlers before unblocking signals. */
	pqsignal(SIGTERM, shardlord_sigterm);
	pqsignal(SIGUSR1, shardlord_sigusr1);
	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	void_spi("select shardman.lord_boot();");
	conn = listen_cmd_log_inserts();

	cmd_ctx = AllocSetContextCreate(CurrentMemoryContext,
									"Shardman per-cmd context",
									ALLOCSET_DEFAULT_SIZES);
	/* main loop */
	while (1948)
	{
		old_ctx = MemoryContextSwitchTo(cmd_ctx);
		while ((cmd = next_cmd()) != NULL)
		{
			char **opts;

			update_cmd_status(cmd->id, "in progress");
			shmn_elog(DEBUG1, "Working on command %ld, %s, opts are",
				 cmd->id, cmd->cmd_type);
			for (opts = cmd->opts; *opts; opts++)
				shmn_elog(DEBUG1, "%s", *opts);
			if (strcmp(cmd->cmd_type, "add_node") == 0)
				add_node(cmd);
			else if (strcmp(cmd->cmd_type, "rm_node") == 0)
				rm_node(cmd);
			else if (strcmp(cmd->cmd_type, "create_hash_partitions") == 0)
				create_hash_partitions(cmd);
			else if (strcmp(cmd->cmd_type, "move_part") == 0)
				move_part(cmd);
			else if (strcmp(cmd->cmd_type, "create_replica") == 0)
				create_replica(cmd);
			else if (strcmp(cmd->cmd_type, "rebalance") == 0)
				rebalance(cmd);
			else if (strcmp(cmd->cmd_type, "set_replevel") == 0)
				set_replevel(cmd);
			else
				shmn_elog(FATAL, "Unknown cmd type %s", cmd->cmd_type);
			MemoryContextReset(cmd_ctx);
		}
		MemoryContextSwitchTo(old_ctx);
		wait_notify();
		check_for_sigterm();
	}
}

/*
 * Execute statement via SPI, when we are not particulary interested in the
 * result. Returns the number of rows processed.
 */
uint64
void_spi(char *sql)
{
	uint64 rows_processed;
	SPI_XACT_STATUS;

	SPI_PROLOG;
	if (SPI_exec(sql, 0) < 0)
		shmn_elog(FATAL, "Stmt failed: %s", sql);
	rows_processed = SPI_processed;
	SPI_EPILOG;
	return rows_processed;
}

/*
 * Open libpq connection to our server and start listening to cmd_log inserts
 * notifications.
 */
PGconn *
listen_cmd_log_inserts(void)
{
	PGresult   *res;

	/* TODO: make sure we connect to ourselves, probably cmp ports */
	conn = PQconnectdb(shardman_shardlord_connstring);
	/* Check to see that the backend connection was successfully made */
	if (PQstatus(conn) != CONNECTION_OK)
		shmn_elog(FATAL, "Connection to local database failed: %s",
			 PQerrorMessage(conn));

	res = PQexec(conn, "LISTEN shardman_cmd_log_update");
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		shmn_elog(FATAL, "LISTEN command failed: %s", PQerrorMessage(conn));
	}
    PQclear(res);

	return conn;
}

/*
 * Wait until NOTIFY or signal arrives. If select is alerted, but there are
 * no notifcations, we also return.
 */
void
wait_notify()
{
	int			sock;
	fd_set		input_mask;
    PGnotify   *notify;

	sock = PQsocket(conn);
	if (sock < 0)
		shmn_elog(FATAL, "Couldn't get sock from pgconn");

	FD_ZERO(&input_mask);
	FD_SET(sock, &input_mask);

	if (select(sock + 1, &input_mask, NULL, NULL, NULL) < 0)
	{
		if (errno == EINTR)
			return; /* signal has arrived */
		shmn_elog(FATAL, "select() failed: %s", strerror(errno));
	}

	/* TODO: what if connection broke? */
	PQconsumeInput(conn);
	/* eat all notifications at once */
	while ((notify = PQnotifies(conn)) != NULL)
	{
		shmn_elog(DEBUG1, "NOTIFY %s received from backend PID %d",
			 notify->relname, notify->be_pid);
		PQfreemem(notify);
	}

	return;
}

/*
 * Retrieve next cmd to work on -- uncompleted command with min id.
 * Returns NULL if queue is empty. Memory is allocated in the current cxt.
 */
Cmd *
next_cmd(void)
{
	const char	   *sql;
	Cmd			   *cmd = NULL;
	MemoryContext	oldcxt = CurrentMemoryContext,
					spicxt;
	int				e;
	SPI_XACT_STATUS;

	SPI_PROLOG;

	/* Get the oldest pending task (id + cmd_type) */
	sql = "select id, cmd_type from shardman.cmd_log"
		  " where status in ('waiting', 'in progress')"
		  " order by id asc"
		  " limit 1;";
	e = SPI_execute(sql, true, 0);
	if (e < 0)
		shmn_elog(FATAL, "Stmt failed: %s", sql);

	if (SPI_processed > 0)
	{
		TupleDesc		rowdesc;
		HeapTuple		tuple;
		bool			isnull;
		int				cmd_id_attr,
						cmd_type_attr,
						opt_attr;
		uint64			i;

		/* get tuple and its descriptor */
		rowdesc = SPI_tuptable->tupdesc;
		tuple = SPI_tuptable->vals[0];

		/* get attribute numbers */
		cmd_id_attr = SPI_fnumber(rowdesc, "id");
		Assert(cmd_id_attr != SPI_ERROR_NOATTRIBUTE);
		cmd_type_attr = SPI_fnumber(rowdesc, "cmd_type");
		Assert(cmd_type_attr != SPI_ERROR_NOATTRIBUTE);

		/* copy the command itself to callee context */
		spicxt = MemoryContextSwitchTo(oldcxt);
		cmd = palloc0(sizeof(Cmd));
		cmd->id = DatumGetInt64(SPI_getbinval(tuple, rowdesc,
											  cmd_id_attr,
											  &isnull));
		cmd->cmd_type = SPI_getvalue(tuple, rowdesc, cmd_type_attr);
		MemoryContextSwitchTo(spicxt);

		/* Now get options. sql will be freed by SPI_finish */
		sql = psprintf("select unnest(cmd_opts) opt from shardman.cmd_log"
					   " where id = " INT64_FORMAT, cmd->id);
		e = SPI_execute(sql, true, 0);
		if (e < 0)
			shmn_elog(FATAL, "Stmt failed: %s", sql);

		MemoryContextSwitchTo(oldcxt);
		/* +1 for NULL in the end */
		cmd->opts = palloc0((SPI_processed + 1) * sizeof(char *));
		for (i = 0; i < SPI_processed; i++)
		{
			/* get tuple and its descriptor */
			rowdesc = SPI_tuptable->tupdesc;
			tuple = SPI_tuptable->vals[i];

			/* get attribute numbers */
			opt_attr = SPI_fnumber(rowdesc, "opt");
			Assert(opt_attr != SPI_ERROR_NOATTRIBUTE);

			cmd->opts[i] = SPI_getvalue(tuple, rowdesc, opt_attr);
		}
		MemoryContextSwitchTo(spicxt);
	}

	SPI_EPILOG;
	return cmd;
}

/*
 * Update command status
 */
void
update_cmd_status(int64 id, const char *new_status)
{
	char *sql;
	int e;
	SPI_XACT_STATUS;

	SPI_PROLOG;
	sql = psprintf("update shardman.cmd_log set status = '%s' where id = %ld;",
				   new_status, id);
	e = SPI_exec(sql, 0);
	if (e < 0)
	{
		shmn_elog(FATAL, "Stmt failed: %s", sql);
	}
	SPI_EPILOG;
}

/*
 * Check that extension is installed locally. We must be connected to db at
 * this point
 */
bool
pg_shardman_installed_local(void)
{
	bool installed = true;

	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	if (get_extension_oid("pg_shardman", true) == InvalidOid)
		installed = false;
	PopActiveSnapshot();
	CommitTransactionCommand();

	return installed;
}

/*
 * Signal handler for SIGTERM
 *		Set a flag to let the main loop to terminate.
 */
void
shardlord_sigterm(SIGNAL_ARGS)
{
	got_sigterm = true;
}

/*
 * Signal handler for SIGUSR1
 *		Set a flag to let cancel a command
 */
void
shardlord_sigusr1(SIGNAL_ARGS)
{
	got_sigusr1 = true;
}

bool signal_pending(void) { return got_sigterm || got_sigusr1; }

/*
 * Cleanup and exit in case of SIGTERM
 */
void
check_for_sigterm(void)
{
	if (got_sigterm)
	{
		shmn_elog(LOG, "Shardlord received SIGTERM, exiting");
		if (conn != NULL)
			PQfinish(conn);
		proc_exit(0);
	}
}

/* Command canceled via sigusr1 */
void
cmd_canceled(Cmd *cmd)
{
	got_sigusr1 = false;
	shmn_elog(INFO, "Command %ld canceled", cmd->id);
	update_cmd_status(cmd->id, "canceled");
}

/*
 * Adding node consists of
 * - verifying the node is not in the cluster, i.e. 'nodes' table
 * - reinstalling extenstion
 * - recreating repslot
 * - recreating subscription
 * - setting node id on the node itself
 * - waiting for initial tablesync
 * - add node to the nodes table and cmd as success
 * We do all this stuff to make all actions are idempodent to be able to retry
 * them in case of any failure.
 * TODO: orphan repslot is possible if cmd canceled.
 */
void
add_node(Cmd *cmd)
{
	PGconn *conn = NULL;
	const char *connstr = cmd->opts[0];
    PGresult *res = NULL;
	bool pg_shardman_installed;
	int32 node_id;
	char *sql;
	bool tablesync_done = false;

	shmn_elog(INFO, "Adding node %s", connstr);
	/*
	 * Generate node id beforehand to tell it to the node. This is safe since
	 * shardlord is single-threaded.
	 */
	StartTransactionCommand();
	node_id = DatumGetInt32(
		DirectFunctionCall1Coll(
			nextval, InvalidOid,
			PointerGetDatum(cstring_to_text("shardman.nodes_id_seq"))));
	CommitTransactionCommand();
	/* Try to execute command indefinitely until it succeeded or canceled */
	while (1948)
	{
		conn = PQconnectdb(connstr);
		if (PQstatus(conn) != CONNECTION_OK)
		{
			shmn_elog(NOTICE,
					  "add_node %s: connection to add_node node failed: %s",
					  connstr, PQerrorMessage(conn));
			goto attempt_failed;
		}

		/* Check if our extension is installed on the node */
		res = PQexec(conn,
					 "select installed_version from pg_available_extensions"
					 " where name = 'pg_shardman';");
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			shmn_elog(NOTICE,
					  "add_node %s: failed to check whether pg_shardman is installed, %s",
					  connstr, PQerrorMessage(conn));
			goto attempt_failed;
		}
		pg_shardman_installed = PQntuples(res) == 1 && !PQgetisnull(res, 0, 0);
		PQclear(res);

		if (pg_shardman_installed)
		{
			/* extension is installed, so we have to check whether this node
			 * is already in the cluster */
			res = PQexec(conn, "select shardman.my_id();");
			if (PQresultStatus(res) != PGRES_TUPLES_OK)
			{
				shmn_elog(NOTICE, "add_node %s: failed to get node id, %s",
						  connstr, PQerrorMessage(conn));
				goto attempt_failed;
			}

			if (!PQgetisnull(res, 0, 0))
			{
				/* Node is in cluster. Was it there before we started adding? */
				node_id = atoi(PQgetvalue(res, 0, 0));
				elog(DEBUG1, "node in cluster, %d", node_id);
				PQclear(res);
				if (node_in_cluster(node_id))
				{
					shmn_elog(WARNING, "node %d with connstring %s is already"
							  " in cluster, won't add it.", node_id, connstr);
					PQfinish(conn);
					update_cmd_status(cmd->id, "failed");
					return;
				}
			}
			else
				PQclear(res);
		}

		/*
		 * reinstall the extension to reset its state, whether is was
		 * installed before or not.
		 */
		res = PQexec(conn, "drop extension if exists pg_shardman;"
					 " create extension pg_shardman;");
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			shmn_elog(NOTICE, "add_node %s: failed to reinstall pg_shardman, %s",
					  connstr, PQerrorMessage(conn));
			goto attempt_failed;
		}
		PQclear(res);

		/* Create replication slot */
		sql = psprintf("select shardman.create_repslot('shardman_meta_sub_%d');",
					   node_id);
		void_spi(sql);
		pfree(sql);

		/* Create subscription and set node id on itself */
		sql = psprintf(
			"create subscription shardman_meta_sub connection '%s'"
			" publication shardman_meta_pub with (create_slot = false,"
			" slot_name = 'shardman_meta_sub_%d');"
			" select shardman.set_my_id(%d);",
			shardman_shardlord_connstring, node_id, node_id);
		res = PQexec(conn, sql);
		pfree(sql);
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			shmn_elog(NOTICE, "add_node %s: failed to create subscription and set node id, %s",
					  connstr, PQerrorMessage(conn));
			goto attempt_failed;
		}
		/* Alter system can't be executed in one multi-command string */
		PQclear(res);
		sql = psprintf("alter system set shardman.my_id to %d;", node_id);
		res = PQexec(conn, sql);
		pfree(sql);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			shmn_elog(NOTICE, "add_node %s: failed to set my_id guc, %s",
					  connstr, PQerrorMessage(conn));
			goto attempt_failed;
		}
		PQclear(res);
		res = PQexec(conn, "select pg_reload_conf();");
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			shmn_elog(NOTICE, "add_node %s: failed to reload conf, %s",
					  connstr, PQerrorMessage(conn));
			goto attempt_failed;
		}
		PQclear(res);

		/*
		 * Wait until initial tablesync is completed. This is necessary as
		 * e.g. we might miss UPDATE statements on partitions table, triggers
		 * on newly added node won't fire and metadata would be inconsistent.
		 */
		sql = GET_SUBSTATE_SQL("shardman_meta_sub");
		while (!tablesync_done)
		{
			int i;

			res = PQexec(conn, sql);
			if (PQresultStatus(res) != PGRES_TUPLES_OK)
			{
				shmn_elog(NOTICE, "add_node %s: failed to learn sub status, %s ",
						  connstr, PQerrorMessage(conn));
				goto attempt_failed;
			}

			tablesync_done = true;
			for (i = 0; i < PQntuples(res); i++)
			{
				char subrelstate = PQgetvalue(res, i, 0)[0];
				char *subrelid = PQgetvalue(res, i, 1);
				if (subrelstate != SUBREL_STATE_READY)
				{
					tablesync_done = false;
					shmn_elog(DEBUG1,
							  "add_node %s: init sync is not yet finished"
							  " for rel %s, its state is %c",
							  connstr, subrelid, subrelstate);
					pg_usleep(shardman_poll_interval * 1000L);
					SHMN_CHECK_FOR_INTERRUPTS();
					if (got_sigusr1)
					{
						reset_pqconn_and_res(&conn, res);
						cmd_canceled(cmd);
						return;
					}
					break;
				}
			}
			PQclear(res);
		}

		reset_pqconn(&conn);

		/*
		 * Mark add_node cmd as success and node as active, we must do that in
		 * one txn.
		 */
		sql = psprintf(
			"insert into shardman.nodes values (%d, '%s', 'active', false, %ld);"
			" update shardman.cmd_log set status = 'success' where id = %ld;",
			node_id, connstr, cmd->id, cmd->id);
		void_spi(sql);
		pfree(sql);

		/* done */
		shmn_elog(INFO, "Node %s successfully added, it is assigned id %d",
			 connstr, node_id);
		return;

attempt_failed: /* clean resources, sleep, check sigusr1 and try again */
		reset_pqconn_and_res(&conn, res);
		shmn_elog(LOG, "Attempt to execute add_node failed, sleeping and retrying");
		/* TODO: sleep using waitlatch? */
		pg_usleep(shardman_cmd_retry_naptime * 1000L);
		SHMN_CHECK_FOR_INTERRUPTS_CMD(cmd);
	}
}

/*
 * Returns true, if node 'id' is active node in cluster or rm in progress
 */
static bool
node_in_cluster(int id)
{
	char *sql;
	bool res;
	SPI_XACT_STATUS;

	SPI_PROLOG;
	sql = psprintf(
		"select id from shardman.nodes where id = %d and (shardlord or"
		" worker_status = 'active' or worker_status = 'rm_in_progress');",
		id); /* pfreed by spi */
	if (SPI_execute(sql, true, 0) < 0)
		shmn_elog(FATAL, "Stmt failed: %s", sql);
	res = SPI_processed == 1;

	SPI_EPILOG;
	return res;
}

/*
 * Remove node, losing all data on it. We
 * - ensure that there is active node with given id in the cluster
 * - mark node as rm_in_progress and commit so this reaches node via LR
 * - wait a bit to let it unsubscribe
 * - drop replication slot, remove node row and mark cmd as success
 * Everything is idempotent. Note that we are not allowed to remove repl slot
 * when the walsender connection is alive, that's why we sleep here.
 */
void
rm_node(Cmd *cmd)
{
	int32 node_id = atoi(cmd->opts[0]);
	bool force = pg_strcasecmp(cmd->opts[1], "true") == 0;
	char *sql;
	int e;
	int64 parts_on_node;

	if (force)
	{
		sql = psprintf("delete from shardman.partitions where owner=%d",
					   node_id);
		void_spi(sql);
		pfree(sql);
	}
	else
	{
		bool isnull;
		SPI_XACT_STATUS;

		SPI_PROLOG;
		sql = psprintf(
			"select count(*) from shardman.partitions where owner=%d",
			node_id);
		e = SPI_execute(sql, true, 0);
		if (e < 0)
			shmn_elog(FATAL, "Stmt failed: %s", sql);
		Assert(SPI_processed == 1);
		parts_on_node =
			DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0],
										SPI_tuptable->tupdesc, 1, &isnull));
		SPI_EPILOG;
	}
	if (!force && parts_on_node != 0)
	{
		shmn_elog(WARNING, "Can't remove node %d with existing shards. Add \"force\" option to ignore this",
				  node_id);
		update_cmd_status(cmd->id, "failed");
		return;
	}

	elog(INFO, "Removing node %d ", node_id);
	if (!node_in_cluster(node_id))
	{
		shmn_elog(WARNING, "node %d not in cluster, won't rm it.", node_id);
		update_cmd_status(cmd->id, "failed");
		return;
	}

	sql = psprintf(
		"update shardman.nodes set worker_status = 'rm_in_progress' where id = %d;",
		node_id);
	void_spi(sql);
	pfree(sql);

	/* Let node drop the subscription */
	pg_usleep(2 * 1000000L);

	/*
	 * It is extremely unlikely that node still keeps walsender process
	 * connected but ignored our node status update, so this should succeed.
	 * If not, bgw exits, but postmaster will restart us to try again.
	 * TODO: at this stage, user can't cancel command at all, this should be
	 * fixed.
	 */
	sql = psprintf(
		"select shardman.drop_repslot('shardman_meta_sub_%d', true);"
		"update shardman.nodes set worker_status = 'removed' where id = %d;"
		"update shardman.cmd_log set status = 'success' where id = %ld;",
		node_id, node_id, cmd->id);
	void_spi(sql);
	pfree(sql);
	elog(INFO, "Node %d successfully removed", node_id);
}

/*
 * Finish pq connection and set ptr to NULL. You must be sure that the
 * connection exists or ptr is NULL.
 */
void
reset_pqconn(PGconn **conn) { PQfinish(*conn); *conn = NULL; }
/*
 * Same, but also clear res. You must be sure that both connection and res
 * exist or they are NULL ptrs.
 */
void
reset_pqconn_and_res(PGconn **conn, PGresult *res)
{
	PQclear(res); reset_pqconn(conn);
}

/*
 * SQL to retrieve per-relation status of subscription with given name.
 */
char *
get_substate_sql(const char *subname)
{
	return psprintf(GET_SUBSTATE_SQL("%s"), subname);
}

/*
 * Get connstr of a lord/worker node with id = node_id.
 * Return NULL if there's no such node.
 * node_id might (should) be SHMN_INVALID_NODE_ID if we are searching for lord.
 */
char *
get_node_connstr(int32 node_id, ShmnNodeType node_type)
{
	MemoryContext oldcxt = CurrentMemoryContext;
	StringInfoData query;
	char *res;
	SPI_XACT_STATUS;

	/* We shouldn't use SHMN_INVALID_NODE_ID with SNT_WORKER */
	Assert(node_id != SHMN_INVALID_NODE_ID || node_type == SNT_LORD);

	SPI_PROLOG;

	/* We assume that node is either worker or lord, it can't be idle */
	initStringInfo(&query);
	appendStringInfo(&query,
					 "select connstring from shardman.nodes "
					 "where shardlord = %s ",
					 (node_type == SNT_LORD) ? "true" : "false");
	if (node_id != SHMN_INVALID_NODE_ID)
		appendStringInfo(&query, "and id = %d ", node_id);

	if (SPI_execute(query.data, true, 0) < 0)
	{
		shmn_elog(FATAL, "Stmt failed : %s", query.data);
	}

	if (SPI_processed == 0)
	{
		res = NULL;
	}
	else
	{
		HeapTuple tuple = SPI_tuptable->vals[0];
		TupleDesc rowdesc = SPI_tuptable->tupdesc;
		MemoryContext spicxt;

		/* We expect to see exactly one node */
		Assert(SPI_processed == 1);

		/* We need to allocate connstring in our ctxt, not spi's */
		spicxt = MemoryContextSwitchTo(oldcxt);
		res = SPI_getvalue(tuple, rowdesc, 1);
		MemoryContextSwitchTo(spicxt);
	}

	SPI_EPILOG;
	return res;
}

/*
 * Get array with all active worker nodes ids. Memory is palloced, size is
 * returned in 'num_workers'
 */
int32 *
get_workers(uint64 *num_workers)
{
	char *sql = "select id from shardman.nodes where worker_status = 'active'";
	bool isnull;
	int32 *workers;
	TupleDesc rowdesc;
	MemoryContext spicxt;
	MemoryContext oldcxt = CurrentMemoryContext;
	uint64 i;
	SPI_XACT_STATUS;

	SPI_PROLOG;

	if (SPI_execute(sql, true, 0) < 0)
		shmn_elog(FATAL, "Stmt failed : %s", sql);
	rowdesc = SPI_tuptable->tupdesc;

	*num_workers = SPI_processed;
	/* We need to allocate in our ctxt, not spi's */
	spicxt = MemoryContextSwitchTo(oldcxt);
	workers = palloc(sizeof(int32) * (*num_workers));
	MemoryContextSwitchTo(spicxt);
	for (i = 0; i < *num_workers; i++)
	{
		workers[i] = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[i],
												 rowdesc, 1, &isnull));
	}

	SPI_EPILOG;
	return workers;
}

/*
 * Get node id on which given primary is stored. SHMN_INVALID_NODE_ID is
 * returned if there is no such primary.
 */
int32
get_primary_owner(const char *part_name)
{
	char *sql;
	bool isnull;
	int owner;
	SPI_XACT_STATUS;

	SPI_PROLOG;
	sql = psprintf( /* allocated in SPI ctxt, freed with ctxt release */
		"select owner from shardman.partitions where part_name = '%s' and prv IS NULL;",
		part_name);

	if (SPI_execute(sql, true, 0) < 0)
		shmn_elog(FATAL, "Stmt failed : %s", sql);

	if (SPI_processed == 0)
		owner = SHMN_INVALID_NODE_ID;
	else
	{
		owner =	DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0],
											SPI_tuptable->tupdesc,
											1, &isnull));
	}

	SPI_EPILOG;
	return owner;
}

/*
 * Get node id on which the last replica in the 'part_name' replica chain
 * resides. SHMN_INVALID_NODE_ID is returned if such partition doesn't exist
 * at all.
 */
int32
get_reptail_owner(const char *part_name)
{
	char *sql;
	bool isnull;
	int owner;
	SPI_XACT_STATUS;

	SPI_PROLOG;
	sql = psprintf( /* allocated in SPI ctxt, freed with ctxt release */
		"select owner from shardman.partitions where part_name = '%s'"
		" and nxt is NULL;", part_name);

	if (SPI_execute(sql, true, 0) < 0)
		shmn_elog(FATAL, "Stmt failed : %s", sql);

	if (SPI_processed == 0)
		owner = SHMN_INVALID_NODE_ID;
	else
	{
		owner = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0],
											SPI_tuptable->tupdesc,
											1, &isnull));
	}

	SPI_EPILOG;
	return owner;
}

/*
 * Get node on which replica next to 'node_id' node in the 'part_name' replica
 * chain resides. SHMN_INVALID_NODE_ID is returned if such partition doesn't
 * exist at all or there is no next replica.
 */
int32
get_next_node(const char *part_name, int32 node_id)
{
	char *sql;
	bool isnull;
	int32 next;
	SPI_XACT_STATUS;

	SPI_PROLOG;
	sql = psprintf( /* allocated in SPI ctxt, freed with ctxt release */
		"select nxt from shardman.partitions where part_name = '%s'"
		" and owner = %d;", part_name, node_id);

	if (SPI_execute(sql, true, 0) < 0)
		shmn_elog(FATAL, "Stmt failed : %s", sql);

	if (SPI_processed == 0)
		next = SHMN_INVALID_NODE_ID;
	else
	{
		next = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0],
										   SPI_tuptable->tupdesc,
										   1, &isnull));
		if (isnull)
			next = SHMN_INVALID_NODE_ID;
	}

	SPI_EPILOG;
	return next;
}

/*
 * Get node on which replica prev to 'node_id' node in the 'part_name' replica
 * chain resides. SHMN_INVALID_NODE_ID is returned if such partition doesn't
 * exist at all on that node or there is no next replica. part_exists is set
 * to false in the former case.
 */
int32
get_prev_node(const char *part_name, int32 node_id, bool *part_exists)
{
	char *sql;
	bool isnull;
	int32 prev;
	SPI_XACT_STATUS;

	SPI_PROLOG;

	*part_exists = true;

	sql = psprintf( /* allocated in SPI ctxt, freed with ctxt release */
		"select prv from shardman.partitions where part_name = '%s'"
		" and owner = %d;", part_name, node_id);

	if (SPI_execute(sql, true, 0) < 0)
		shmn_elog(FATAL, "Stmt failed : %s", sql);

	if (SPI_processed == 0)
	{
		prev = SHMN_INVALID_NODE_ID;
		*part_exists = false;
	}
	else
	{
		prev = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0],
										   SPI_tuptable->tupdesc,
										   1, &isnull));
		if (isnull)
			prev = SHMN_INVALID_NODE_ID;
	}

	SPI_EPILOG;
	return prev;
}

/*
 * Node 'node' holds primary or replica of part_name?
 */
bool
node_has_partition(int32 node, const char *part_name)
{
	char *sql;
	int64 count = 0;
	SPI_XACT_STATUS;

	SPI_PROLOG;

	sql = psprintf("select count(*) from shardman.partitions"
				   " where part_name = '%s' and owner = %d", part_name, node);

	if (SPI_execute(sql, true, 0) < 0)
	{
		shmn_elog(FATAL, "Stmt failed : %s", sql);
	}
	if (SPI_processed != 0)
	{
		bool isnull;
		count = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0],
											SPI_tuptable->tupdesc,
											1,
											&isnull));
	}
	SPI_EPILOG;
	return count != 0;
}

/*
 * Get relation name of partition part_name. Memory is palloc'ed.
 * NULL is returned, if there is no such partition.
 */
char *
get_partition_relation(const char *part_name)
{
	MemoryContext oldcxt = CurrentMemoryContext;
	char *sql;
	char *res;
	SPI_XACT_STATUS;

	SPI_PROLOG;

	sql = psprintf("select relation from shardman.partitions"
				   " where part_name = '%s';", part_name);

	if (SPI_execute(sql, true, 0) < 0)
	{
		shmn_elog(FATAL, "Stmt failed : %s", sql);
	}

	if (SPI_processed == 0)
	{
		res = NULL;
	}
	else
	{
		HeapTuple tuple = SPI_tuptable->vals[0];
		TupleDesc rowdesc = SPI_tuptable->tupdesc;
		/* We need to allocate connstring in our ctxt, not spi's */
		MemoryContext spicxt = MemoryContextSwitchTo(oldcxt);
		res = SPI_getvalue(tuple, rowdesc, 1);
		MemoryContextSwitchTo(spicxt);
	}

	SPI_EPILOG;
	return res;
}

/*
 * Get array with all partitions of given relation. Memory is palloced, size
 * is returned in 'numparts'.
 */
Partition *
get_parts(const char *relation, uint64 *num_parts)
{
	char *sql;
	bool isnull;
	Partition *parts;
	TupleDesc rowdesc;
	MemoryContext spicxt;
	MemoryContext oldcxt = CurrentMemoryContext;
	uint64 i;
	SPI_XACT_STATUS;

	SPI_PROLOG;
	sql = psprintf( /* allocated in SPI ctxt, freed with ctxt release */
		"select part_name, owner from shardman.partitions where relation = '%s';",
		relation);

	if (SPI_execute(sql, true, 0) < 0)
		shmn_elog(FATAL, "Stmt failed : %s", sql);
	rowdesc = SPI_tuptable->tupdesc;

	*num_parts = SPI_processed;
	/* We need to allocate in our ctxt, not spi's */
	spicxt = MemoryContextSwitchTo(oldcxt);
	parts = palloc(sizeof(Partition) * (*num_parts));
	for (i = 0; i < *num_parts; i++)
	{
		HeapTuple tuple = SPI_tuptable->vals[i];
		parts[i].part_name = SPI_getvalue(tuple, rowdesc, 1);
		parts[i].owner = DatumGetInt32(SPI_getbinval(tuple, rowdesc, 2,
													 &isnull));
	}
	MemoryContextSwitchTo(spicxt);

	SPI_EPILOG;
	return parts;
}

/*
 * Calculate how many replicas has each partitions of given relation
 */
RepCount *
get_repcount(const char *relation, uint64 *num_parts)
{
	char *sql;
	bool isnull;
	RepCount *repcounts;
	TupleDesc rowdesc;
	MemoryContext spicxt;
	MemoryContext oldcxt = CurrentMemoryContext;
	uint64 i;
	SPI_XACT_STATUS;

	SPI_PROLOG;
	sql = psprintf( /* allocated in SPI ctxt, freed with ctxt release */
		"select part_name, count(case when prv is not null then 1 end) from"
		" shardman.partitions where relation = '%s' group by part_name;",
		relation);

	if (SPI_execute(sql, true, 0) < 0)
		shmn_elog(FATAL, "Stmt failed : %s", sql);
	rowdesc = SPI_tuptable->tupdesc;

	*num_parts = SPI_processed;
	/* We need to allocate in our ctxt, not spi's */
	spicxt = MemoryContextSwitchTo(oldcxt);
	repcounts = palloc(sizeof(RepCount) * (*num_parts));
	for (i = 0; i < *num_parts; i++)
	{
		HeapTuple tuple = SPI_tuptable->vals[i];
		repcounts[i].part_name = SPI_getvalue(tuple, rowdesc, 1);
		repcounts[i].count = DatumGetInt64(SPI_getbinval(tuple, rowdesc, 2,
														 &isnull));
	}
	MemoryContextSwitchTo(spicxt);

	SPI_EPILOG;
	return repcounts;
}
