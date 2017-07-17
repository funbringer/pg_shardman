/* -------------------------------------------------------------------------
 *
 * shardmaster.c
 *		Background worker executing sharding tasks.
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

#include "utils/guc.h"
#include "utils/snapmgr.h"
#include "executor/spi.h"
#include "access/xact.h"
#include "commands/extension.h"
#include "libpq-fe.h"

#include "pg_shardman.h"


/* ensure that extension won't load against incompatible version of Postgres */
PG_MODULE_MAGIC;

typedef struct Cmd
{
	int64 id;
	char *cmd_type;
	char *status;
	char **opts; /* array of n options, opts[n] is NULL */
} Cmd;

static Cmd *next_cmd(void);
static void update_cmd_status(int64 id, const char *new_status);
static PGconn *listen_cmd_log_inserts(void);
static uint64 void_spi(char *sql);
static void wait_notify(PGconn *conn);
static void shardmaster_sigterm(SIGNAL_ARGS);
static void shardmaster_sigusr1(SIGNAL_ARGS);
static void check_for_sigterm(void);
static void pg_shardman_installed_local(void);
static void cmd_canceled(Cmd *cmd);
static char *get_worker_node_connstring(int node_id);

static void add_node(Cmd *cmd);
static int insert_node(const char *connstring, int64 cmd_id);
static bool node_in_cluster(int id);

static void rm_node(Cmd *cmd);

static void create_hash_partitions(Cmd *cmd);

/* flags set by signal handlers */
static volatile sig_atomic_t got_sigterm = false;
static volatile sig_atomic_t got_sigusr1 = false;

/* GUC variables */
static bool shardman_master = false;
static char *shardman_master_dbname = "postgres";
static char *shardman_master_connstring = "";
static int shardman_cmd_retry_naptime = 10000;

/* Just global vars. */
/* Connection to local server for LISTEN notifications. Is is global for easy
 * cleanup after receiving SIGTERM.
 */
static PGconn *conn = NULL;

/*
 * Entrypoint of the module. Define variables and register background worker.
 */
void
_PG_init()
{
	BackgroundWorker shardmaster_worker;

	if (!process_shared_preload_libraries_in_progress)
	{
		ereport(ERROR, (errmsg("pg_shardman can only be loaded via shared_preload_libraries"),
						errhint("Add pg_shardman to shared_preload_libraries.")));
	}

	DefineCustomBoolVariable("shardman.master",
							 "This node is the master?",
							 NULL,
							 &shardman_master,
							 false,
							 PGC_POSTMASTER,
							 0,
							 NULL, NULL, NULL);

	DefineCustomStringVariable(
		"shardman.master_dbname",
		"Active only if shardman.master is on. Name of the database with"
		" on master node, shardmaster bgw will connect to it",
		NULL,
		&shardman_master_dbname,
		"postgres",
		PGC_POSTMASTER,
		0,
		NULL, NULL, NULL
		);

	DefineCustomStringVariable(
		"shardman.master_connstring",
		"Active only if shardman.master is on. Connstring to reach master from"
		"worker nodes to set up logical replication",
		NULL,
		&shardman_master_connstring,
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

	if (shardman_master)
	{
		/* register shardmaster */
		sprintf(shardmaster_worker.bgw_name, "shardmaster");
		shardmaster_worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
			BGWORKER_BACKEND_DATABASE_CONNECTION;
		shardmaster_worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
		shardmaster_worker.bgw_restart_time = 10;
		/* shardmaster_worker.bgw_restart_time = BGW_NEVER_RESTART; */
		sprintf(shardmaster_worker.bgw_library_name, "pg_shardman");
		sprintf(shardmaster_worker.bgw_function_name, "shardmaster_main");
		shardmaster_worker.bgw_notify_pid = 0;
		RegisterBackgroundWorker(&shardmaster_worker);
	}
	/* TODO: clean up publications if we were master before */
}

/*
 * shardmaster bgw starts here
 */
void
shardmaster_main(Datum main_arg)
{
	Cmd *cmd;
	shmn_elog(LOG, "Shardmaster started");

	/* Connect to the database to use SPI*/
	BackgroundWorkerInitializeConnection(shardman_master_dbname, NULL);
	/* sanity check */
	pg_shardman_installed_local();

	/* Establish signal handlers before unblocking signals. */
	pqsignal(SIGTERM, shardmaster_sigterm);
	pqsignal(SIGUSR1, shardmaster_sigusr1);
    /* We're now ready to receive signals */
    BackgroundWorkerUnblockSignals();

	void_spi("select shardman.master_boot();");
	conn = listen_cmd_log_inserts();

	/* main loop */
	while (1948)
	{
		/* TODO: new mem ctxt for every command */
		while ((cmd = next_cmd()) != NULL)
		{
			update_cmd_status(cmd->id, "in progress");
			shmn_elog(LOG, "Working on command %ld, %s, opts are",
				 cmd->id, cmd->cmd_type);
			for (char **opts = cmd->opts; *opts; opts++)
				shmn_elog(LOG, "%s", *opts);
			if (strcmp(cmd->cmd_type, "add_node") == 0)
				add_node(cmd);
			else if (strcmp(cmd->cmd_type, "rm_node") == 0)
				rm_node(cmd);
			else if (strcmp(cmd->cmd_type, "create_hash_partitions") == 0)
				create_hash_partitions(cmd);
			else
				shmn_elog(FATAL, "Unknown cmd type %s", cmd->cmd_type);
		}
		wait_notify(conn);
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
	char *connstring;
	PGresult   *res;

	connstring = psprintf("dbname = %s", shardman_master_dbname);
	conn = PQconnectdb(connstring);
	pfree(connstring);
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
wait_notify(PGconn *conn)
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

	PQconsumeInput(conn);
	/* eat all notifications at once */
	while ((notify = PQnotifies(conn)) != NULL)
	{
		shmn_elog(LOG, "NOTIFY %s received from backend PID %d",
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
	const char *sql;
	Cmd *cmd = NULL;
	MemoryContext oldcxt = CurrentMemoryContext;
	int e;

	SPI_PROLOG;

	sql = "select * from shardman.cmd_log t1 join"
		" (select MIN(id) id from shardman.cmd_log where status = 'waiting' OR"
		" status = 'in progress') t2 using (id);";
	e = SPI_execute(sql, true, 0);
	if (e < 0)
		shmn_elog(FATAL, "Stmt failed: %s", sql);

	if (SPI_processed > 0)
	{
		HeapTuple tuple = SPI_tuptable->vals[0];
		TupleDesc rowdesc = SPI_tuptable->tupdesc;
		bool isnull;
		uint64 i;

		/* copy the command itself to callee context */
		MemoryContext spicxt = MemoryContextSwitchTo(oldcxt);
		cmd = palloc(sizeof(Cmd));
		cmd->id = DatumGetInt64(SPI_getbinval(tuple, rowdesc,
											  SPI_fnumber(rowdesc, "id"),
											  &isnull));
		cmd->cmd_type = SPI_getvalue(tuple, rowdesc,
									 SPI_fnumber(rowdesc, "cmd_type"));
		MemoryContextSwitchTo(spicxt);

		/* Now get options. sql will be freed by SPI_finish */
		sql = psprintf("select opt from shardman.cmd_opts where"
						   " cmd_id = %ld order by id;", cmd->id);
		e = SPI_execute(sql, true, 0);
		if (e < 0)
			shmn_elog(FATAL, "Stmt failed: %s", sql);

		MemoryContextSwitchTo(oldcxt);
		/* +1 for NULL in the end */
		cmd->opts = palloc((SPI_processed + 1) * sizeof(char*));
		for (i = 0; i < SPI_processed; i++)
		{
			tuple = SPI_tuptable->vals[i];
			rowdesc = SPI_tuptable->tupdesc;
			cmd->opts[i] = SPI_getvalue(tuple, rowdesc,
										SPI_fnumber(rowdesc, "opt"));
		}
		cmd->opts[i] = NULL;

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

	SPI_PROLOG;
	sql = psprintf("update shardman.cmd_log set status = '%s' where id = %ld;",
				   new_status, id);
	e = SPI_exec(sql, 0);
	pfree(sql);
	if (e < 0)
	{
		shmn_elog(FATAL, "Stmt failed: %s", sql);
	}
	SPI_EPILOG;
}

/*
 * Verify that extension is installed locally. We must be connected to db at
 * this point
 */
void
pg_shardman_installed_local(void)
{
	bool installed = true;

	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	if (get_extension_oid("pg_shardman", true) == InvalidOid)
	{
		installed = false;
		shmn_elog(WARNING, "pg_shardman library is preloaded, but extenstion"
				  " is not created");
	}
	PopActiveSnapshot();
	CommitTransactionCommand();

	/* shardmaster won't run without extension */
	/* TODO: unregister bgw? */
	if (!installed)
		proc_exit(1);
}

/*
 * Signal handler for SIGTERM
 *		Set a flag to let the main loop to terminate.
 */
void
shardmaster_sigterm(SIGNAL_ARGS)
{
	got_sigterm = true;
}

/*
 * Signal handler for SIGUSR1
 *		Set a flag to let the main loop to terminate.
 */
void
shardmaster_sigusr1(SIGNAL_ARGS)
{
	got_sigusr1 = true;
}

/*
 * Cleanup and exit in case of SIGTERM
 */
void
check_for_sigterm(void)
{
	if (got_sigterm)
	{
		shmn_elog(LOG, "Shardmaster received SIGTERM, exiting");
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
	shmn_elog(LOG, "Command %ld canceled", cmd->id);
	update_cmd_status(cmd->id, "canceled");
}

/*
 * Adding node consists of
 * - verifying the node is not 'active' in the cluster, i.e. 'nodes' table
 * - adding node to the 'nodes' as not active, get its new id
 * - reinstalling extenstion
 * - recreating repslot
 * - recreating subscription
 * - setting node id on the node itself
 * - marking node as active and cmd as success
 * We do all this stuff to make all actions are idempodent to be able to retry
 * them in case of any failure.
 * TODO: node record might hang in 'add_in_progress' state, we should remove it.
 */
void
add_node(Cmd *cmd)
{
	PGconn *conn = NULL;
	const char *connstring = cmd->opts[0];
    PGresult *res = NULL;
	bool pg_shardman_installed;
	int node_id;
	char *sql;

	shmn_elog(INFO, "Adding node %s", connstring);
	/* Try to execute command indefinitely until it succeeded or canceled */
	while (!got_sigusr1 && !got_sigterm)
	{
		conn = PQconnectdb(connstring);
		if (PQstatus(conn) != CONNECTION_OK)
		{
			shmn_elog(NOTICE, "Connection to add_node node failed: %s",
				 PQerrorMessage(conn));
			goto attempt_failed;
		}

		/* Check if our extension is installed on the node */
		res = PQexec(conn,
					 "select installed_version from pg_available_extensions"
					 " where name = 'pg_shardman';");
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			shmn_elog(NOTICE, "Failed to check whether pg_shardman is installed on"
				 " node to add: %s", PQerrorMessage(conn));
			goto attempt_failed;
		}
		pg_shardman_installed = PQntuples(res) == 1 && !PQgetisnull(res, 0, 0);
		PQclear(res);

		if (pg_shardman_installed)
		{
			/* extension is installed, so we have to check whether this node
			 * is already in the cluster */
			res = PQexec(conn, "select shardman.get_node_id();");
			if (PQresultStatus(res) != PGRES_TUPLES_OK)
			{
				shmn_elog(NOTICE, "Failed to get node id, %s", PQerrorMessage(conn));
				goto attempt_failed;
			}

			if (!PQgetisnull(res, 0, 0))
			{
				/* Node is in cluster. Was it there before we started adding? */
				node_id = atoi(PQgetvalue(res, 0, 0));
				elog(INFO, "NODE IN CLUSTER, %d", node_id);
				PQclear(res);
				if (node_in_cluster(node_id))
				{
					shmn_elog(WARNING, "node %d with connstring %s is already"
							  " in cluster, won't add it.", node_id, connstring);
					PQfinish(conn);
					update_cmd_status(cmd->id, "failed");
					return;
				}
			}
			else
				PQclear(res);
		}

		/*
		 * Now add node to 'nodes' table, if we haven't done that yet, and
		 * record that we did so for this cmd
		 */
		node_id = insert_node(connstring, cmd->id);

		/*
		 * reinstall the extension to reset its state, whether is was
		 * installed before or not.
		 */
		res = PQexec(conn, "drop extension if exists pg_shardman;"
					 " create extension pg_shardman;");
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			shmn_elog(NOTICE, "Failed to reinstall pg_shardman, %s",
					  PQerrorMessage(conn));
			goto attempt_failed;
		}
		PQclear(res);

		/* Create replication slot, if not yet */
		sql = psprintf("select shardman.create_repslot('shardman_meta_sub_%d');",
					   node_id);
		void_spi(sql);
		pfree(sql);

		/* Create subscription and set node it on itself */
		sql = psprintf(
			"create subscription shardman_meta_sub connection '%s'"
			"publication shardman_meta_pub with (create_slot = false,"
			"slot_name = 'shardman_meta_sub_%d');"
			"select shardman.set_node_id(%d);",
			shardman_master_connstring, node_id, node_id);
		res = PQexec(conn, sql);
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			shmn_elog(NOTICE, "Failed to create subscription and set node id, %s",
					  PQerrorMessage(conn));
			goto attempt_failed;
		}

		PQclear(res);
		PQfinish(conn);

		/*
		 * Mark add_node cmd as success and node as active, we must do that in
		 * one txn.
		 */
		sql = psprintf(
			"update shardman.nodes set worker_status = 'active' where id = %d;"
			"update shardman.cmd_log set status = 'success' where id = %ld;",
			node_id, cmd->id);
		void_spi(sql);
		pfree(sql);

		/* done */
		elog(INFO, "Node %s successfully added, it is assigned id %d",
			 connstring, node_id);
		return;

attempt_failed: /* clean resources, sleep, check sigusr1 and try again */
		if (res != NULL)
			PQclear(res);
		if (conn != NULL)
			PQfinish(conn);

		shmn_elog(LOG, "Attempt to execute add_node failed, sleeping and retrying");
		/* TODO: sleep using waitlatch? */
		pg_usleep(shardman_cmd_retry_naptime * 1000L);
	}
	check_for_sigterm();

	cmd_canceled(cmd);
}

/* See sql func */
static int
insert_node(const char *connstring, int64 cmd_id)
{
	char *sql = psprintf("select shardman.insert_node('%s', %ld)",
							 connstring, cmd_id);
	int e;
	int node_id;
	bool isnull;

	SPI_PROLOG;
	e = SPI_exec(sql, 0);
	pfree(sql);
	if (e < 0)
		/* TODO: closing connections on such failures? */
		shmn_elog(FATAL, "Stmt failed: %s", sql);
	node_id = DatumGetInt32(
		SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1,
					  &isnull));
	SPI_EPILOG;

	return node_id;
}

/*
 * Returns true, if node 'id' is in cluster and not in add_in_progress state
 */
static bool
node_in_cluster(int id)
{
	char *sql = psprintf(
		"select id from shardman.nodes where id = %d and (master OR"
		" worker_status != 'add_in_progress');",
		id);
	bool res;

	SPI_PROLOG;
	if (SPI_execute(sql, true, 0) < 0)
		shmn_elog(FATAL, "Stmt failed: %s", sql);
	pfree(sql);
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
	int node_id = atoi(cmd->opts[0]);
	char *sql;

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
		"select shardman.drop_repslot('shardman_meta_sub_%d');"
		/* keep silent cmd_log fk constraint */
		"update shardman.cmd_log set node_id = null where node_id = %d;"
		"delete from shardman.nodes where id = %d;"
		"update shardman.cmd_log set status = 'success' where id = %ld;",
		node_id, node_id, node_id, cmd->id);
	void_spi(sql);
	pfree(sql);
	elog(INFO, "Node %d successfully removed", node_id);
}


/*
 * Steps are:
 * - Ensure table is not partitioned already;
 * - Partition table and get sql to create it;
 * - Add records about new table and partitions;
 */
static void create_hash_partitions(Cmd *cmd)
{
	int node_id = atoi(cmd->opts[0]);
	const char *relation = cmd->opts[1];
	const char *expr = cmd->opts[2];
	int partitions_count = atoi(cmd->opts[3]);
	char *connstring;
	PGconn *conn = NULL;
	PGresult *res = NULL;
	char *sql;
	uint64 table_exists;
	char *create_table_sql;

	shmn_elog(INFO, "Sharding table %s on node %d", relation, node_id);

	/* Check that table with such name is not already sharded */
	sql = psprintf(
		"select relation from shardman.tables where relation = '%s'",
		relation);
	table_exists = void_spi(sql);
	if (table_exists)
	{
		shmn_elog(WARNING, "table %s already sharded, won't partition it.",
				  relation);
		update_cmd_status(cmd->id, "failed");
		return;
	}
	/* connstring mem freed with ctxt */
	if ((connstring = get_worker_node_connstring(node_id)) == NULL)
	{
		shmn_elog(WARNING, "create_hash_partitions failed, no such worker node: %d",
				  node_id);
		update_cmd_status(cmd->id, "failed");
		return;
	}

	/* Note that we have to run statements in separate transactions, otherwise
	 * we have a deadlock between pathman and pg_dump */
	sql = psprintf(
		"begin; select create_hash_partitions('%s', '%s', %d); end;"
		"select shardman.gen_create_table_sql('%s', '%s');",
		relation, expr, partitions_count,
		relation, connstring);

	/* Try to execute command indefinitely until it succeeded or canceled */
	while (!got_sigusr1 && !got_sigterm)
	{
		conn = PQconnectdb(connstring);
		if (PQstatus(conn) != CONNECTION_OK)
		{
			shmn_elog(NOTICE, "Connection to node failed: %s",
					  PQerrorMessage(conn));
			goto attempt_failed;
		}

		/* Partition table and get sql to create it */
		res = PQexec(conn, sql);
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			shmn_elog(NOTICE, "Failed to partition table and get sql to create it: %s",
					  PQerrorMessage(conn));
			goto attempt_failed;
		}
		create_table_sql = PQgetvalue(res, 0, 0);

		/* TODO: if master fails at this moment (which is extremely unlikely
		 * though), after restart it will try to partition table again and
		 * fail. We should check if the table is already partitioned and don't
		 * do that again, except for, probably, the case when it was
		 * partitioned by someone else.
		 */
		/*
		 * Insert table to 'tables' table (no pun intended), insert partitions
		 * and mark partitioning cmd as successfull
		 */
		sql = psprintf("insert into shardman.tables values"
					   " ('%s', '%s', %d, $create_table$%s$create_table$, %d);"
					   " update shardman.cmd_log set status = 'success'"
					   " where id = %ld;",
					   relation, expr, partitions_count, create_table_sql,
					   node_id, cmd->id);
		void_spi(sql);
		pfree(sql);

		PQclear(res); /* can't free any earlier, it stores sql */
		PQfinish(conn);

		/* done */
		elog(INFO, "Table %s successfully partitioned", relation);
		return;

attempt_failed: /* clean resources, sleep, check sigusr1 and try again */
		if (res != NULL)
			PQclear(res);
		if (conn != NULL)
			PQfinish(conn);

		shmn_elog(LOG, "Attempt to execute create_hash_partitions failed,"
				  " sleeping and retrying");
		/* TODO: sleep using waitlatch? */
		pg_usleep(shardman_cmd_retry_naptime * 1000L);
	}
	check_for_sigterm();

	cmd_canceled(cmd);
}

/*
 * Get connstring of worker node with id node_id. Memory is palloc'ed.
 * NULL is returned, if there is no such node.
 */
char*
get_worker_node_connstring(int node_id)
{
	MemoryContext oldcxt = CurrentMemoryContext;
	char *sql = psprintf("select connstring from shardman.nodes where id = %d"
						 " and worker", node_id);
	char *res;

	SPI_PROLOG;

	if (SPI_execute(sql, true, 0) < 0)
	{
		shmn_elog(FATAL, "Stmt failed : %s", sql);
	}
	pfree(sql);

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