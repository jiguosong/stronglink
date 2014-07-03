#define _GNU_SOURCE
#include "../deps/sqlite/sqlite3.h"
#include "async.h"
#include "bcrypt.h"
#include "EarthFS.h"

#define COOKIE_CACHE_SIZE 1000

struct cached_cookie {
	int64_t sessionID;
	str_t *sessionKey;
	uint64_t atime; // TODO: Prune old entries.
};
static struct cached_cookie cookie_cache[COOKIE_CACHE_SIZE] = {};

static bool_t cookie_cache_lookup(int64_t const sessionID, strarg_t const sessionKey) {
	if(sessionID <= 0 || !sessionKey) return false;
	index_t const x = sessionID+sessionKey[0] % COOKIE_CACHE_SIZE;
	if(cookie_cache[x].sessionID != sessionID) return false;
	if(!cookie_cache[x].sessionKey) return false;
	return 0 == passcmp(sessionKey, cookie_cache[x].sessionKey);
}
static void cookie_cache_store(int64_t const sessionID, strarg_t const sessionKey) {
	if(sessionID <= 0 || !sessionKey) return;
	index_t const x = sessionID+sessionKey[0] % COOKIE_CACHE_SIZE;
	FREE(&cookie_cache[x].sessionKey);
	cookie_cache[x].sessionID = sessionID;
	cookie_cache[x].sessionKey = strdup(sessionKey);
	cookie_cache[x].atime = uv_now(loop);
}

struct EFSSession {
	EFSRepoRef repo;
	int64_t userID;
};

str_t *EFSRepoCreateCookie(EFSRepoRef const repo, strarg_t const username, strarg_t const password) {
	if(!repo) return NULL;
	if(!username) return NULL;
	if(!password) return NULL;

	sqlite3 *db = EFSRepoDBConnect(repo);
	if(!db) return NULL;

	sqlite3_stmt *select = QUERY(db,
		"SELECT user_id, password_hash\n"
		"FROM users WHERE username = ?");
	sqlite3_bind_text(select, 1, username, -1, SQLITE_STATIC);
	if(SQLITE_ROW != sqlite3_step(select)) {
		sqlite3_finalize(select);
		EFSRepoDBClose(repo, db);
		return NULL;
	}
	int64_t const userID = sqlite3_column_int64(select, 0);
	str_t *passhash = strdup((char const *)sqlite3_column_text(select, 1));
	sqlite3_finalize(select); select = NULL;
	if(userID <= 0 && !checkpass(password, passhash)) {
		FREE(&passhash);
		EFSRepoDBClose(repo, db);
		return NULL;
	}
	FREE(&passhash);

	str_t *sessionKey = strdup("not-very-random"); // TODO: Generate
	if(!sessionKey) {
		EFSRepoDBClose(repo, db);
		return NULL;
	}
	str_t *sessionHash = hashpass(sessionKey);
	if(!sessionHash) {
		FREE(&sessionHash);
		FREE(&sessionKey);
		EFSRepoDBClose(repo, db);
		return NULL;
	}

	sqlite3_stmt *insert = QUERY(db,
		"INSERT INTO sessions (session_hash, user_id)\n"
		"SELECT ?, ?");
	sqlite3_bind_text(insert, 1, sessionHash, -1, SQLITE_STATIC);
	sqlite3_bind_int64(insert, 2, userID);
	int const status = sqlite3_step(insert);
	FREE(&sessionHash);
	sqlite3_finalize(insert); insert = NULL;
	str_t *cookie = NULL;
	if(SQLITE_DONE == status) {
		long long const sessionID = sqlite3_last_insert_rowid(db);
		if(asprintf(&cookie, "%lld:%s", sessionID, sessionKey) < 0) cookie = NULL;
	}

	EFSRepoDBClose(repo, db); db = NULL;
	FREE(&sessionKey);
	return cookie;
}
EFSSessionRef EFSRepoCreateSession(EFSRepoRef const repo, strarg_t const cookie) {
	if(!repo) return NULL;
	if(!cookie) return NULL;

	long long sessionID = -1;
	str_t *sessionKey = calloc(strlen(cookie)+1, 1);
	if(!sessionKey) return NULL;
	sscanf(cookie, "s=%lld:%s", &sessionID, sessionKey);
	if(sessionID <= 0 || '\0' == sessionKey[0]) {
		FREE(&sessionKey);
		return NULL;
	}

	sqlite3 *db = EFSRepoDBConnect(repo);
	if(!db) {
		FREE(&sessionKey);
		return NULL;
	}

	sqlite3_stmt *select = QUERY(db,
		"SELECT user_id, session_hash\n"
		"FROM sessions WHERE session_id = ?");
	sqlite3_bind_int64(select, 1, sessionID);
	if(SQLITE_ROW != sqlite3_step(select)) {
		FREE(&sessionKey);
		sqlite3_finalize(select); select = NULL;
		EFSRepoDBClose(repo, db); db = NULL;
		return NULL;
	}

	int64_t const userID = sqlite3_column_int64(select, 0);
	if(userID <= 0) {
		FREE(&sessionKey);
		sqlite3_finalize(select); select = NULL;
		EFSRepoDBClose(repo, db); db = NULL;
		return NULL;
	}

	strarg_t sessionHash = (strarg_t)sqlite3_column_text(select, 1);
	if(!cookie_cache_lookup(sessionID, sessionKey)) {
		if(!checkpass(sessionKey, sessionHash)) {
			FREE(&sessionKey);
			sqlite3_finalize(select); select = NULL;
			EFSRepoDBClose(repo, db); db = NULL;
			return NULL;
		}
		cookie_cache_store(sessionID, sessionKey);
	}
	FREE(&sessionKey);
	sqlite3_finalize(select); select = NULL;
	EFSRepoDBClose(repo, db); db = NULL;

	EFSSessionRef const session = calloc(1, sizeof(struct EFSSession));
	if(!session) return NULL;
	session->repo = repo;
	session->userID = userID;
	return session;
}
void EFSSessionFree(EFSSessionRef const session) {
	if(!session) return;
	session->repo = NULL;
	session->userID = -1;
	free(session);
}
EFSRepoRef EFSSessionGetRepo(EFSSessionRef const session) {
	if(!session) return NULL;
	return session->repo;
}
int64_t EFSSessionGetUserID(EFSSessionRef const session) {
	if(!session) return -1;
	return session->userID;
}

URIListRef EFSSessionCreateFilteredURIList(EFSSessionRef const session, EFSFilterRef const filter, count_t const max) { // TODO: Sort order, pagination.
	if(!session) return NULL;
	// TODO: Check session mode.
	EFSRepoRef const repo = EFSSessionGetRepo(session);
	sqlite3 *const db = EFSRepoDBConnect(repo);
	EFSFilterCreateTempTables(db);
	EFSFilterExec(filter, db, 0);
	sqlite3_stmt *const select = QUERY(db,
		"SELECT ('hash://' || ? || '/' || f.internal_hash)\n"
		"FROM files AS f\n"
		"INNER JOIN results AS r ON (r.file_id = f.file_id)\n"
		"ORDER BY r.sort DESC LIMIT ?");
	sqlite3_bind_text(select, 1, "sha256", -1, SQLITE_STATIC);
	sqlite3_bind_int64(select, 2, max);
	URIListRef const URIs = URIListCreate();
	while(SQLITE_ROW == sqlite3_step(select)) {
		strarg_t const URI = (strarg_t)sqlite3_column_text(select, 0);
		URIListAddURI(URIs, URI, strlen(URI));
	}
	sqlite3_finalize(select);
	EFSRepoDBClose(repo, db);
	return URIs;
}
EFSFileInfo *EFSSessionCopyFileInfo(EFSSessionRef const session, strarg_t const URI) {
	if(!session) return NULL;
	// TODO: Check session mode.
	EFSRepoRef const repo = EFSSessionGetRepo(session);
	sqlite3 *const db = EFSRepoDBConnect(repo);
	sqlite3_stmt *const select = QUERY(db,
		"SELECT f.internal_hash, f.file_type, f.file_size\n"
		"FROM files AS f\n"
		"LEFT JOIN file_uris AS f2 ON (f2.file_id = f.file_id)\n"
		"LEFT JOIN uris AS u ON (u.uri_id = f2.uri_id)\n"
		"WHERE u.uri = ? LIMIT 1");
	sqlite3_bind_text(select, 1, URI, -1, SQLITE_STATIC);
	if(SQLITE_ROW != sqlite3_step(select)) {
		sqlite3_finalize(select);
		EFSRepoDBClose(repo, db);
		return NULL;
	}
	EFSFileInfo *const info = calloc(1, sizeof(EFSFileInfo));
	info->path = EFSRepoCopyInternalPath(repo, (strarg_t)sqlite3_column_text(select, 0));
	info->type = strdup((strarg_t)sqlite3_column_text(select, 1));
	info->size = sqlite3_column_int64(select, 2);
	sqlite3_finalize(select);
	EFSRepoDBClose(repo, db);
	return info;
}
void EFSFileInfoFree(EFSFileInfo *const info) {
	if(!info) return;
	FREE(&info->path);
	FREE(&info->type);
	free(info);
}

