/* -------------------------------------------------------------------------
 *
 * udf.c
 *		SQL functions.
 *
 * Copyright (c) 2017, Postgres Professional
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "commands/event_trigger.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/lsyscache.h"
#include "commands/dbcommands.h"
#include "miscadmin.h"
#include "funcapi.h"
#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "storage/lmgr.h"
#include "replication/logicalworker.h"
#include "replication/syncrep.h"
#include "libpq-fe.h"
#include "tcop/tcopprot.h"

#include "pg_shardman.h"

/*
 * Must be called iff we are dropping extension. Checks that we are dropping
 * pg_shardman extension and calls pg_shardman_cleanup to perform the actual
 * cleanup.
 */
PG_FUNCTION_INFO_V1(pg_shardman_cleanup_c);
Datum
pg_shardman_cleanup_c(PG_FUNCTION_ARGS)
{
	EventTriggerData *trigdata;
	DropStmt *stmt;
	Value *value;
	char *ext_name;

	if (!CALLED_AS_EVENT_TRIGGER(fcinfo))  /* internal error */
		elog(ERROR, "not fired by event trigger manager");

	trigdata = (EventTriggerData *) fcinfo->context;
	Assert(trigdata->parsetree->type == T_DropStmt);
	stmt = (DropStmt *) trigdata->parsetree;
	Assert(stmt->removeType == OBJECT_EXTENSION);
	/* So it is list of pointers to Nodes */
	Assert(stmt->objects->type == T_List);
	/* To Value nodes, actually */
	Assert(((Node *) (linitial(stmt->objects)))->type == T_String);
	/* Seems like no way to use smth like linitial_node, because Value node
	 * can have different types	*/

	value = (Value *) linitial(stmt->objects);
	ext_name = strVal(value);
	if (strcmp(ext_name, "pg_shardman") == 0)
	{
		/* So we are dropping pg_shardman */
		const char *cmd_sql = "select shardman.pg_shardman_cleanup();";

		SPI_connect();
		if (SPI_execute(cmd_sql, true, 0) < 0)
			elog(FATAL, "Stmt failed: %s", cmd_sql);
		SPI_finish();
	}

	PG_RETURN_NULL();
}

/*
 * Generate CREATE TABLE sql for relation via pg_dump. We use it for root
 * (parent) tables because pg_dump dumps all the info -- indexes, constrains,
 * defaults, everything. Parameter is not REGCLASS because pg_dump can't
 * handle oids anyway. Connstring must be proper libpq connstring, it is feed
 * to pg_dump.
 * TODO: actually we should have much more control on what is dumped, so we
 * need to copy-paste parts of messy pg_dump or collect the needed data
 * manually walking over catalogs.
 */
PG_FUNCTION_INFO_V1(gen_create_table_sql);
Datum
gen_create_table_sql(PG_FUNCTION_ARGS)
{
	char pg_dump_path[MAXPGPATH];
	/* let the mmgr free that */
	char *relation = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char *connstring =  text_to_cstring(PG_GETARG_TEXT_PP(1));
	const size_t chunksize = 5; /* read max that bytes at time */
	/* how much already allocated *including header* */
	size_t pallocated = VARHDRSZ + chunksize;
	text *sql = (text *) palloc(pallocated);
	char *ptr = VARDATA(sql); /* ptr to first free byte */
	char *cmd;
	FILE *fp;
	size_t bytes_read;

	SET_VARSIZE(sql, VARHDRSZ);

	/* find pg_dump location querying pg_config */
	SPI_connect();
	if (SPI_execute("select setting from pg_config where name = 'BINDIR';",
					true, 0) < 0)
		elog(FATAL, "Failed to query pg_config");
	strcpy(pg_dump_path,
		   SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1));
	SPI_finish();
	join_path_components(pg_dump_path, pg_dump_path, "pg_dump");
	canonicalize_path(pg_dump_path);

	cmd = psprintf("%s -t '%s' --schema-only --dbname='%s' 2>&1",
				   pg_dump_path, relation, connstring);

	if ((fp = popen(cmd, "r")) == NULL)
	{
		elog(ERROR, "Failed to run pg_dump, cmd %s", cmd);
	}

	while ((bytes_read = fread(ptr, sizeof(char), chunksize, fp)) != 0)
	{
		SET_VARSIZE(sql, VARSIZE_ANY(sql) + bytes_read);
		if (pallocated - VARSIZE_ANY(sql) < chunksize)
		{
			pallocated *= 2;
			sql = (text *) repalloc(sql, pallocated);
		}
		/* since we realloc, can't just += bytes_read here */
		ptr = VARDATA(sql) + VARSIZE_ANY_EXHDR(sql);
	}

	if (pclose(fp))	{
		elog(ERROR, "pg_dump exited with error status, output was\n%scmd was \n%s",
			 text_to_cstring(sql), cmd);
	}

	PG_RETURN_TEXT_P(sql);
}

/*
 * Reconstruct attrs part of CREATE TABLE stmt, e.g. (i int NOT NULL, j int).
 * The only constraint reconstructed is NOT NULL.
 */
PG_FUNCTION_INFO_V1(reconstruct_table_attrs);
Datum
reconstruct_table_attrs(PG_FUNCTION_ARGS)
{
	StringInfoData query;
	Oid	relid = PG_GETARG_OID(0);
	Relation local_rel = heap_open(relid, AccessExclusiveLock);
	TupleDesc local_descr = RelationGetDescr(local_rel);
	int i;

	initStringInfo(&query);
	appendStringInfoChar(&query, '(');

	for (i = 0; i < local_descr->natts; i++)
	{
		Form_pg_attribute attr = local_descr->attrs[i];

		if (i != 0)
			appendStringInfoString(&query, ", ");

		/* NAME TYPE[(typmod)] [NOT NULL] [COLLATE "collation"] */
		appendStringInfo(&query, "%s %s%s%s",
						 quote_identifier(NameStr(attr->attname)),
						 format_type_with_typemod_qualified(attr->atttypid,
															attr->atttypmod),
						 (attr->attnotnull ? " NOT NULL" : ""),
						 (attr->attcollation ?
						  psprintf(" COLLATE \"%s\"",
								   get_collation_name(attr->attcollation)) :
						  ""));
	}

	appendStringInfoChar(&query, ')');

	/* Let xact unlock this */
	heap_close(local_rel, NoLock);
	PG_RETURN_TEXT_P(cstring_to_text(query.data));
}

/*
 * Basically, this is an sql wrapper around PQconninfoParse. Given libpq
 * connstring, it returns a pair of keywords and values arrays with valid
 * nonempty options.
 */
PG_FUNCTION_INFO_V1(pq_conninfo_parse);
Datum
pq_conninfo_parse(PG_FUNCTION_ARGS)
{
	TupleDesc            tupdesc;
	/* array of keywords and array of vals as in PQconninfoOption */
	Datum		values[2];
	bool		nulls[2] = { false, false };
	ArrayType *keywords; /* array of keywords */
	ArrayType *vals; /* array of vals */
	text **keywords_txt; /* we construct array of keywords from it */
	text **vals_txt; /* array of vals constructed from it */
	Datum *elems; /* just to convert text * to it */
	int32 text_size;
	int numopts = 0;
	int i;
	size_t len;
	int16 typlen;
	bool typbyval;
	char typalign;
	char *pqerrmsg;
	char *errmsg_palloc;
	char *conninfo = text_to_cstring(PG_GETARG_TEXT_PP(0));
	PQconninfoOption *opts = PQconninfoParse(conninfo, &pqerrmsg);
	PQconninfoOption *opt;

	if (pqerrmsg != NULL)
	{
		/* free malloced memory to avoid leakage */
		errmsg_palloc = pstrdup(pqerrmsg);
		PQfreemem((void *) pqerrmsg);
		elog(ERROR, "PQconninfoParse failed: %s", errmsg_palloc);
	}

	/* compute number of opts and allocate text ptrs */
	for (opt = opts; opt->keyword != NULL; opt++)
	{
		/* We are interested only in filled values */
		if (opt->val != NULL)
			numopts++;
	}
	keywords_txt = palloc(numopts * sizeof(text*));
	vals_txt = palloc(numopts * sizeof(text*));

	/* Fill keywords and vals */
	for (opt = opts, i = 0; opt->keyword != NULL; opt++)
	{
		if (opt->val != NULL)
		{
			len = strlen(opt->keyword);
			text_size = VARHDRSZ + len;
			keywords_txt[i] = (text *) palloc(text_size);
			SET_VARSIZE(keywords_txt[i], text_size);
			memcpy(VARDATA(keywords_txt[i]), opt->keyword, len);

			len = strlen(opt->val);
			text_size = VARHDRSZ + len;
			vals_txt[i] = (text *) palloc(text_size);
			SET_VARSIZE(vals_txt[i], text_size);
			memcpy(VARDATA(vals_txt[i]), opt->val, len);
			i++;
		}
	}

	/* Now construct arrays */
	elems = (Datum*) palloc(numopts * sizeof(Datum));
	/* get info about text type, we will pass it to array constructor */
	get_typlenbyvalalign(TEXTOID, &typlen, &typbyval, &typalign);

	/* cast text * to datums for purity and construct array */
	for (i = 0; i < numopts; i++) {
		elems[i] = PointerGetDatum(keywords_txt[i]);
	}
	keywords = construct_array(elems, numopts, TEXTOID, typlen, typbyval,
							   typalign);
	/* same for valus */
	for (i = 0; i < numopts; i++) {
		elems[i] = PointerGetDatum(vals_txt[i]);
	}
	vals = construct_array(elems, numopts, TEXTOID, typlen, typbyval,
							   typalign);

	/* prepare to form the tuple */
	values[0] = PointerGetDatum(keywords);
	values[1] = PointerGetDatum(vals);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function returning record called in context "
						"that cannot accept type record")));
	BlessTupleDesc(tupdesc); /* Inshallah */

	PQconninfoFree(opts);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/* Are we a logical apply worker? */
PG_FUNCTION_INFO_V1(inside_apply_worker);
Datum
inside_apply_worker(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(IsLogicalWorker());
}

/*
 * Check whether 'standby' is present in current value of
 * synchronous_standby_names. If yes, return NULL. Otherwise, form properly
 * quoted new value of the setting with 'standby' appended. Currently we
 * support only the case when *all* standbys must agree on commit, so FIRST or
 * ANY doesn't matter here. '*' wildcard is not supported too.
 */
PG_FUNCTION_INFO_V1(ensure_sync_standby_c);
Datum
ensure_sync_standby_c(PG_FUNCTION_ARGS)
{
	char *standby = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char *cur_standby_name;
	StringInfoData standby_list;
	char *newval;
	int processed = 0;

	initStringInfo(&standby_list);
	if (SyncRepConfig != NULL)
	{
		Assert(SyncRepConfig->num_sync == SyncRepConfig->nmembers);

		cur_standby_name = SyncRepConfig->member_names;
		for (; processed < SyncRepConfig->nmembers; processed++)
		{
			Assert(strcmp(cur_standby_name, "*") != 0);
			if (pg_strcasecmp(cur_standby_name, standby) == 0)
			{
				PG_RETURN_NULL(); /* already set */
			}

			if (processed != 0)
				appendStringInfoString(&standby_list, ", ");
			appendStringInfoString(&standby_list, quote_identifier(cur_standby_name));

			cur_standby_name += strlen(cur_standby_name) + 1;
		}
	}

	if (processed != 0)
		appendStringInfoString(&standby_list, ", ");
	appendStringInfoString(&standby_list, quote_identifier(standby));

	newval = psprintf("FIRST %d (%s)", processed + 1, standby_list.data);
	PG_RETURN_TEXT_P(cstring_to_text(newval));
}

/*
 * Check whether 'standby' is present in current value of
 * synchronous_standby_names. If no, return NULL. Otherwise, form properly
 * quoted new value of the setting with 'standby' removed. Currently we
 * support only the case when *all* standbys must agree on commit, so FIRST or
 * ANY doesn't matter here. '*' wildcard is not supported too.
 * All entries are removed.
 */
PG_FUNCTION_INFO_V1(remove_sync_standby_c);
Datum
remove_sync_standby_c(PG_FUNCTION_ARGS)
{
	char *standby = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char *cur_standby_name;
	StringInfoData standby_list;
	char *newval;
	int processed;
	int num_sync;

	if (SyncRepConfig == NULL)
		PG_RETURN_NULL();

	initStringInfo(&standby_list);
	Assert(SyncRepConfig->num_sync == SyncRepConfig->nmembers);

	cur_standby_name = SyncRepConfig->member_names;
	num_sync = 0;
	for (processed = 0; processed < SyncRepConfig->nmembers; processed++)
	{
		Assert(strcmp(cur_standby_name, "*") != 0);
		if (pg_strcasecmp(cur_standby_name, standby) != 0)
		{
			if (num_sync != 0)
				appendStringInfoString(&standby_list, ", ");
			appendStringInfoString(&standby_list, quote_identifier(cur_standby_name));
			num_sync++;
		}

		cur_standby_name += strlen(cur_standby_name) + 1;
	}

	if (SyncRepConfig->num_sync == num_sync)
		PG_RETURN_NULL(); /* nothing was removed */

	if (num_sync > 0)
		newval = psprintf("FIRST %d (%s)", num_sync, standby_list.data);
	else
		newval = "";
	PG_RETURN_TEXT_P(cstring_to_text(newval));
}

/*
 * Execute ALTER SYSTEM SET 'arg0' TO 'arg1'. We can't do that from usual
 * function, because ALTER SYSTEM cannon be executed within transaction, so we
 * resort to another exquisite hack: we connect to ourselves via libpq and
 * perform the job.
 */
PG_FUNCTION_INFO_V1(alter_system_c);
Datum
alter_system_c(PG_FUNCTION_ARGS)
{
	char *opt = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char *val = quote_literal_cstr(text_to_cstring(PG_GETARG_TEXT_PP(1)));
	char *cmd = psprintf("alter system set %s to %s", opt, val);
	char *my_connstr_sql = "select shardman.my_connstr_strict();";
	char *connstr;
	PGconn *conn = NULL;
	PGresult *res = NULL;

	SPI_connect();
	if (SPI_execute(my_connstr_sql, true, 0) < 0 || SPI_processed != 1)
		elog(FATAL, "Stmt failed: %s", my_connstr_sql);
	connstr = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);

	conn = PQconnectdb(connstr);
	if (PQstatus(conn) != CONNECTION_OK)
	{
		elog(WARNING, "Connection to myself with connstr %s failed: %s",
			 connstr, PQerrorMessage(conn));
		goto fail;
	}
	SPI_finish(); /* Can't do earlier since connstr is allocated there */
	res = PQexec(conn, cmd);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		elog(WARNING, "setting %s to %s failed", opt, val);
		goto fail;
	}

	PQclear(res);
	PQfinish(conn);
	PG_RETURN_VOID();

fail:
	reset_pqconn_and_res(&conn, res);
	elog(ERROR, "alter_system_c failed");
}

/*
 * Send utility command to shardlord if it's run by worker.
 */
PG_FUNCTION_INFO_V1(execute_on_lord_c);
Datum
execute_on_lord_c(PG_FUNCTION_ARGS)
{
	char *cmd_type = TextDatumGetCString(PG_GETARG_TEXT_P(0));
	ArrayType *cmd_opts = PG_GETARG_ARRAYTYPE_P(1);

	Oid opts_elemtype;
	int16 opts_elemlen;
	bool opts_elembyval;
	char opts_elemalign;
	Datum *opts_values;
	bool *opts_nulls;
	int opts_count;

	StringInfoData query;
	const char **query_params;

	char *connstr;
	PGconn *conn = NULL;
	PGresult *res = NULL;
	char *res_str;
	int i;

	/* Oops, lord should not call this function! */
	if (shardman_shardlord)
		elog(ERROR, "Shardlord should never call function %s", __FUNCTION__);

	/* Extract array of command options */
	opts_elemtype = ARR_ELEMTYPE(cmd_opts);
	get_typlenbyvalalign(opts_elemtype, &opts_elemlen,
						 &opts_elembyval, &opts_elemalign);
	deconstruct_array(cmd_opts,
					  opts_elemtype,
					  opts_elemlen,
					  opts_elembyval,
					  opts_elemalign,
					  &opts_values,
					  &opts_nulls,
					  &opts_count);

	/* Allocate some space for query params */
	query_params = palloc(opts_count * sizeof(char *));

	/* Prepare query */
	initStringInfo(&query);
	appendStringInfo(&query, "select shardman.%s(", quote_identifier(cmd_type));
	for (i = 0; i < opts_count; i++)
	{
		int param = i + 1;
		appendStringInfo(&query, "$%d", param);

		if (param != opts_count)
			appendStringInfoChar(&query, ',');

		/* Save plaintext params for this query */
		query_params[i] = opts_nulls[i] ?
							NULL :
							TextDatumGetCString(opts_values[i]);
	}
	appendStringInfo(&query, ");");


	/* Get a connection string of the current lord */
	if ((connstr = get_node_connstr(SHMN_INVALID_NODE_ID, SNT_LORD)) == NULL)
	{
		shmn_elog(NOTICE, "%s: failed to find the shardlord", query.data);

		goto attempt_failed;
	}

	conn = PQconnectdb(connstr);
	if (PQstatus(conn) != CONNECTION_OK)
	{
		shmn_elog(NOTICE,
				  "%s: failed to connect to the shardlord, %s",
				  connstr, PQerrorMessage(conn));

		goto attempt_failed;
	}

	res = PQexecParams(conn, query.data, opts_count,
					   NULL, query_params, NULL, NULL, 0);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		shmn_elog(NOTICE,
				  "%s: failed to execute command on the shardlord, %s",
				  connstr, PQerrorMessage(conn));

		goto attempt_failed;
	}

	/* Extract the result produced by the API method */
	Assert(PQntuples(res) == 1);
	Assert(PQnfields(res) == 1);
	res_str = PQgetvalue(res, 0, 0);

	PQclear(res);
	PQfinish(conn);
	pfree(connstr);
	pfree(query.data);

	/* Finally, return the result */
	PG_RETURN_TEXT_P(cstring_to_text(res_str));

attempt_failed: /* clean resources */
		reset_pqconn_and_res(&conn, res);

		shmn_elog(ERROR, "Attempt to execute %s failed", query.data);
}
