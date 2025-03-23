/*-------------------------------------------------------------------------
 *
 * connection.c
 *		  Connection management functions for monetdb_fdw
 *
 * Portions Copyright (c) 2025, Halo Tech Co.,Ltd. All rights reserved.
 * Portions Copyright (c) 2012-2023, PostgreSQL Global Development Group
 * 
 * Author: zengman <zengman@halodbtech.com>
 *
 * IDENTIFICATION
 *		  connection.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/pg_user_mapping.h"
#include "commands/defrem.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "monetdb_fdw.h"
#include "storage/fd.h"
#include "storage/latch.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

/*
 * Connection cache hash table entry
 *
 * The lookup key in this hash table is the user mapping OID. We use just one
 * connection per user mapping ID, which ensures that all the scans use the
 * same snapshot during a query.  Using the user mapping OID rather than
 * the foreign server OID + user OID avoids creating multiple connections when
 * the public user mapping applies to all user OIDs.
 *
 * The "conn" pointer can be NULL if we don't currently have a live connection.
 * When we do have a connection, xact_depth tracks the current depth of
 * transactions and subtransactions open on the remote side.  We need to issue
 * commands at the same nesting depth on the remote as we're executing at
 * ourselves, so that rolling back a subtransaction will kill the right
 * queries and not the wrong ones.
 */
typedef Oid ConnCacheKey;

typedef struct ConnCacheEntry
{
	ConnCacheKey key;			/* hash key (must be first) */
	Mapi	   conn;			/* connection to foreign server, or NULL */
	/* Remaining fields are invalid when conn is NULL: */
	int			xact_depth;		/* 0 = no xact open, 1 = main xact open, 2 =
								 * one level of subxact open, etc */
	bool		changing_xact_state;	/* xact state change in process */
	bool		invalidated;	/* true if reconnect is pending */
	Oid			serverid;		/* foreign server OID used to get server name */
	uint32		server_hashvalue;	/* hash value of foreign server OID */
	uint32		mapping_hashvalue;	/* hash value of user mapping OID */
} ConnCacheEntry;

/*
 * Connection cache (initialized on first use)
 */
static HTAB *ConnectionHash = NULL;

/* tracks whether any work is needed in callback functions */
static bool xact_got_connection = false;

/* prototypes of private functions */
static void disconnect_monetdb_server(ConnCacheEntry *entry);
static void begin_remote_xact(ConnCacheEntry *entry);
static void monetdb_fdw_xact_callback(XactEvent event, void *arg);
static void monetdb_fdw_subxact_callback(SubXactEvent event,
											SubTransactionId mySubid,
											SubTransactionId parentSubid,
											void *arg);
static void monetdb_fdw_inval_callback(Datum arg, int cacheid, uint32 hashvalue);
static void monetdb_fdw_reject_incomplete_xact_state_change(ConnCacheEntry *entry);
static void monetdb_fdw_reset_xact_state(ConnCacheEntry *entry, bool toplevel);

/*
 * Get a PGconn which can be used to execute queries on the remote PostgreSQL
 * server with the user's authorization.  A new connection is established
 * if we don't already have a suitable one, and a transaction is opened at
 * the right subtransaction nesting depth if we didn't do that already.
 *
 * will_prep_stmt must be true if caller intends to create any prepared
 * statements.  Since those don't go away automatically at transaction end
 * (not even on error), we need this flag to cue manual cleanup.
 *
 * If state is not NULL, *state receives the per-connection state associated
 * with the PGconn.
 */
Mapi
GetConnection(UserMapping *user, ForeignServer *server)
{
	bool		found;
	ConnCacheEntry *entry = NULL;
	ConnCacheKey key;

	char 		*host = NULL;
	char 		*port = NULL;
	char 		*user_str = NULL;
	char 		*password = NULL;
	char 		*dbname = NULL;
	List	   	*options = NIL;
	ListCell  	*cell = NULL;


	/* First time through, initialize connection cache hashtable */
	if (ConnectionHash == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(ConnCacheKey);
		ctl.entrysize = sizeof(ConnCacheEntry);
		ConnectionHash = hash_create("monetdb_fdw connections", 8,
									 &ctl,
									 HASH_ELEM | HASH_BLOBS);

		/*
		 * Register some callback functions that manage connection cleanup.
		 * This should be done just once in each backend.
		 */
		RegisterXactCallback(monetdb_fdw_xact_callback, NULL);
		RegisterSubXactCallback(monetdb_fdw_subxact_callback, NULL);
		CacheRegisterSyscacheCallback(FOREIGNSERVEROID,
									  monetdb_fdw_inval_callback, (Datum) 0);
		CacheRegisterSyscacheCallback(USERMAPPINGOID,
									  monetdb_fdw_inval_callback, (Datum) 0);
	}

	/* Set flag that we did GetConnection during the current transaction */
	xact_got_connection = true;

	/* Create hash key for the entry.  Assume no pad bytes in key struct */
	key = user->umid;

	/*
	 * Find or create cached entry for requested connection.
	 */
	entry = hash_search(ConnectionHash, &key, HASH_ENTER, &found);
	if (!found)
	{
		/*
		 * We need only clear "conn" here; remaining fields will be filled
		 * later when "conn" is set.
		 */
		entry->conn = NULL;
	}

	/* Reject further use of connections which failed abort cleanup. */
	monetdb_fdw_reject_incomplete_xact_state_change(entry);

	/*
	 * If the connection needs to be remade due to invalidation, disconnect as
	 * soon as we're out of all transactions.
	 */
	if (entry->conn != NULL && entry->invalidated && entry->xact_depth == 0)
	{
		elog(DEBUG2, "closing connection %p for option changes to take effect",
			 entry->conn);
		disconnect_monetdb_server(entry);
	}

	/*
	 * If cache entry doesn't have a connection, we have to establish a new connection.
	 */
	if (entry->conn == NULL)
	{
		MapiHdl hdl = NULL;
		entry->xact_depth = 0;
		entry->changing_xact_state = false;
		entry->invalidated = false;
		entry->serverid = server->serverid;
		entry->server_hashvalue =
			GetSysCacheHashValue1(FOREIGNSERVEROID,
								ObjectIdGetDatum(server->serverid));
		entry->mapping_hashvalue =
			GetSysCacheHashValue1(USERMAPPINGOID,
								ObjectIdGetDatum(user->umid));

		options = list_concat(options, server->options);
		options = list_concat(options, user->options);

		foreach(cell, options)
		{
			DefElem    *def = (DefElem *) lfirst(cell);

			if (strcmp(def->defname, "host") == 0)
				host = defGetString(def);
			else if (strcmp(def->defname, "port") == 0)
				port = defGetString(def);
			else if (strcmp(def->defname, "user") == 0)
				user_str = defGetString(def);
			else if (strcmp(def->defname, "password") == 0)
				password = defGetString(def);
			else if (strcmp(def->defname, "dbname") == 0)
				dbname = defGetString(def);
		}
		list_free(options);

		elog(DEBUG2, "monetdb: host: %s port: %s user: %s password: %s dbname: %s", host, port, user_str, password, dbname);
		entry->conn = mapi_connect(host, atoi(port), user_str, password, "sql", dbname);
		if (entry->conn == NULL || mapi_error(entry->conn))
			die(entry->conn, hdl);
	}

	/* Start a new transaction or subtransaction if needed. */
	begin_remote_xact(entry);

	return entry->conn;
}

/*
 * Disconnect any open connection for a connection cache entry.
 */
static void
disconnect_monetdb_server(ConnCacheEntry *entry)
{
	if (entry->conn != NULL)
	{
		mapi_destroy(entry->conn);
		entry->conn = NULL;
	}
}

/*
 * Convenience subroutine to issue a non-data-returning SQL command to remote
 */
void
do_sql_command(Mapi conn, const char *sql)
{
	MapiHdl	hdl;
	if ((hdl = mapi_query(conn, sql)) == NULL ||
		mapi_error(conn) != MOK)
	{
		if (mapi_error(conn))
			die(conn, hdl);
		if (mapi_close_handle(hdl) != MOK)
			die(conn, hdl);
		mapi_destroy(conn);
	}
}


/*
 * Start remote transaction or subtransaction, if needed.
 *
 * Note that we always use at least REPEATABLE READ in the remote session.
 * This is so that, if a query initiates multiple scans of the same or
 * different foreign tables, we will get snapshot-consistent results from
 * those scans.  A disadvantage is that we can't provide sane emulation of
 * READ COMMITTED behavior --- it would be nice if we had some other way to
 * control which remote queries share a snapshot.
 */
static void
begin_remote_xact(ConnCacheEntry *entry)
{
	int			curlevel = GetCurrentTransactionNestLevel();

	/* Start main transaction if we haven't yet */
	if (entry->xact_depth <= 0)
	{
		const char *sql;

		elog(DEBUG2, "monetdb_fdw: begin remote transaction");
		sql = "START TRANSACTION";
		entry->changing_xact_state = true;
		do_sql_command(entry->conn, sql);
		entry->xact_depth = 1;
		entry->changing_xact_state = false;
	}

	/*
	 * If we're in a subtransaction, stack up savepoints to match our level.
	 * This ensures we can rollback just the desired effects when a
	 * subtransaction aborts.
	 */
	while (entry->xact_depth < curlevel)
	{
		char		sql[64];

		snprintf(sql, sizeof(sql), "SAVEPOINT s%d", entry->xact_depth + 1);
		entry->changing_xact_state = true;
		do_sql_command(entry->conn, sql);
		entry->xact_depth++;
		entry->changing_xact_state = false;
	}
}

/*
 * Release connection reference count created by calling GetConnection.
 */
void
ReleaseConnection(Mapi conn)
{
	/*
	 * Currently, we don't actually track connection references because all
	 * cleanup is managed on a transaction or subtransaction basis instead. So
	 * there's nothing to do here.
	 */
}

/*
 * monetdb_fdw_xact_callback --- cleanup at main-transaction end.
 *
 * This runs just late enough that it must not enter user-defined code
 * locally.  (Entering such code on the remote side is fine.  Its remote
 * COMMIT may run deferred triggers.)
 */
static void
monetdb_fdw_xact_callback(XactEvent event, void *arg)
{
	HASH_SEQ_STATUS scan;
	ConnCacheEntry *entry;

	/* Quick exit if no connections were touched in this transaction. */
	if (!xact_got_connection)
		return;

	/*
	 * Scan all connection cache entries to find open remote transactions, and
	 * close them.
	 */
	hash_seq_init(&scan, ConnectionHash);
	while ((entry = (ConnCacheEntry *) hash_seq_search(&scan)))
	{
		/* Ignore cache entry if no open connection right now */
		if (entry->conn == NULL)
			continue;

		/* If it has an open remote transaction, try to close it */
		if (entry->xact_depth > 0)
		{
			elog(DEBUG3, "closing remote transaction on connection %p",
				 entry->conn);

			switch (event)
			{
				case XACT_EVENT_PARALLEL_PRE_COMMIT:
				case XACT_EVENT_PRE_COMMIT:

					/*
					 * If abort cleanup previously failed for this connection,
					 * we can't issue any more commands against it.
					 */
					monetdb_fdw_reject_incomplete_xact_state_change(entry);

					elog(DEBUG2 ,"monetdb_fdw: commit remote transaction");
					/* Commit all remote transactions during pre-commit */
					entry->changing_xact_state = true;
					do_sql_command(entry->conn, "COMMIT");
					entry->changing_xact_state = false;
					break;
				case XACT_EVENT_PRE_PREPARE:

					/*
					 * We disallow any remote transactions, since it's not
					 * very reasonable to hold them open until the prepared
					 * transaction is committed.  For the moment, throw error
					 * unconditionally; later we might allow read-only cases.
					 * Note that the error will cause us to come right back
					 * here with event == XACT_EVENT_ABORT, so we'll clean up
					 * the connection state at that point.
					 */
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot PREPARE a transaction that has operated on monetdb_fdw foreign tables")));
					break;
				case XACT_EVENT_PARALLEL_COMMIT:
				case XACT_EVENT_COMMIT:
				case XACT_EVENT_PREPARE:
					/* Pre-commit should have closed the open transaction */
					elog(ERROR, "missed cleaning up connection during pre-commit");
					break;
				case XACT_EVENT_PARALLEL_ABORT:
				case XACT_EVENT_ABORT:
				
					elog(DEBUG2 ,"monetdb_fdw: rollback remote transaction");
					do_sql_command(entry->conn, "ROLLBACK");
					break;
			}
		}

		/* Reset state to show we're out of a transaction */
		monetdb_fdw_reset_xact_state(entry, true);
	}

	/*
	 * Regardless of the event type, we can now mark ourselves as out of the
	 * transaction.  (Note: if we are here during PRE_COMMIT or PRE_PREPARE,
	 * this saves a useless scan of the hashtable during COMMIT or PREPARE.)
	 */
	xact_got_connection = false;
}

/*
 * monetdb_fdw_subxact_callback --- cleanup at subtransaction end.
 */
static void
monetdb_fdw_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
					   SubTransactionId parentSubid, void *arg)
{
	HASH_SEQ_STATUS scan;
	ConnCacheEntry *entry;
	int			curlevel;

	/* Nothing to do at subxact start, nor after commit. */
	if (!(event == SUBXACT_EVENT_PRE_COMMIT_SUB ||
		  event == SUBXACT_EVENT_ABORT_SUB))
		return;

	/* Quick exit if no connections were touched in this transaction. */
	if (!xact_got_connection)
		return;

	/*
	 * Scan all connection cache entries to find open remote subtransactions
	 * of the current level, and close them.
	 */
	curlevel = GetCurrentTransactionNestLevel();
	hash_seq_init(&scan, ConnectionHash);
	while ((entry = (ConnCacheEntry *) hash_seq_search(&scan)))
	{
		char		sql[100];

		/*
		 * We only care about connections with open remote subtransactions of
		 * the current level.
		 */
		if (entry->conn == NULL || entry->xact_depth < curlevel)
			continue;

		if (entry->xact_depth > curlevel)
			elog(ERROR, "missed cleaning up remote subtransaction at level %d",
				 entry->xact_depth);

		if (event == SUBXACT_EVENT_PRE_COMMIT_SUB)
		{
			/*
			 * If abort cleanup previously failed for this connection, we
			 * can't issue any more commands against it.
			 */
			monetdb_fdw_reject_incomplete_xact_state_change(entry);

			/* Commit all remote subtransactions during pre-commit */
			snprintf(sql, sizeof(sql), "RELEASE SAVEPOINT s%d", curlevel);
			entry->changing_xact_state = true;
			do_sql_command(entry->conn, sql);
			entry->changing_xact_state = false;
		}

		/* OK, we're outta that level of subtransaction */
		monetdb_fdw_reset_xact_state(entry, false);
	}
}

/*
 * Connection invalidation callback function
 *
 * After a change to a pg_foreign_server or pg_user_mapping catalog entry,
 * close connections depending on that entry immediately if current transaction
 * has not used those connections yet. Otherwise, mark those connections as
 * invalid and then make monetdb_fdw_xact_callback() close them at the end of current
 * transaction, since they cannot be closed in the midst of the transaction
 * using them. Closed connections will be remade at the next opportunity if
 * necessary.
 *
 * Although most cache invalidation callbacks blow away all the related stuff
 * regardless of the given hashvalue, connections are expensive enough that
 * it's worth trying to avoid that.
 *
 * NB: We could avoid unnecessary disconnection more strictly by examining
 * individual option values, but it seems too much effort for the gain.
 */
static void
monetdb_fdw_inval_callback(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS scan;
	ConnCacheEntry *entry;

	Assert(cacheid == FOREIGNSERVEROID || cacheid == USERMAPPINGOID);

	/* ConnectionHash must exist already, if we're registered */
	hash_seq_init(&scan, ConnectionHash);
	while ((entry = (ConnCacheEntry *) hash_seq_search(&scan)))
	{
		/* Ignore invalid entries */
		if (entry->conn == NULL)
			continue;

		/* hashvalue == 0 means a cache reset, must clear all state */
		if (hashvalue == 0 ||
			(cacheid == FOREIGNSERVEROID &&
			 entry->server_hashvalue == hashvalue) ||
			(cacheid == USERMAPPINGOID &&
			 entry->mapping_hashvalue == hashvalue))
		{
			/*
			 * Close the connection immediately if it's not used yet in this
			 * transaction. Otherwise mark it as invalid so that
			 * monetdb_fdw_xact_callback() can close it at the end of this
			 * transaction.
			 */
			if (entry->xact_depth == 0)
			{
				elog(DEBUG2, "monetdb_fdw: discarding connection %p", entry->conn);
				disconnect_monetdb_server(entry);
			}
			else
				entry->invalidated = true;
		}
	}
}

/*
 * Raise an error if the given connection cache entry is marked as being
 * in the middle of an xact state change.  This should be called at which no
 * such change is expected to be in progress; if one is found to be in
 * progress, it means that we aborted in the middle of a previous state change
 * and now don't know what the remote transaction state actually is.
 * Such connections can't safely be further used.  Re-establishing the
 * connection would change the snapshot and roll back any writes already
 * performed, so that's not an option, either. Thus, we must abort.
 */
static void
monetdb_fdw_reject_incomplete_xact_state_change(ConnCacheEntry *entry)
{
	ForeignServer *server;

	/* nothing to do for inactive entries and entries of sane state */
	if (entry->conn == NULL || !entry->changing_xact_state)
		return;

	/* make sure this entry is inactive */
	disconnect_monetdb_server(entry);

	/* find server name to be shown in the message below */
	server = GetForeignServer(entry->serverid);

	ereport(ERROR,
			(errcode(ERRCODE_CONNECTION_EXCEPTION),
			 errmsg("connection to server \"%s\" was lost",
					server->servername)));
}

/*
 * Reset state to show we're out of a (sub)transaction.
 */
static void
monetdb_fdw_reset_xact_state(ConnCacheEntry *entry, bool toplevel)
{
	if (toplevel)
	{
		/* Reset state to show we're out of a transaction */
		entry->xact_depth = 0;

		if (mapi_error(entry->conn))
		{
			elog(DEBUG2, "monetdb_fdw: discarding connection %p", entry->conn);
			disconnect_monetdb_server(entry);
		}
	}
	else
	{
		/* Reset state to show we're out of a subtransaction */
		entry->xact_depth--;
	}
}
