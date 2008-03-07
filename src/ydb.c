/* -*- mode: C; c-basic-offset: 4 -*- */
#ident "Copyright (c) 2007, 2008 Tokutek Inc.  All rights reserved."

#ident "The technology is licensed by the Massachusetts Institute of Technology, Rutgers State University of New Jersey, and the Research Foundation of State University of New York at Stony Brook under United States of America Serial No. 11/760379 and to the patents and/or patent applications resulting from it."

const char *toku_patent_string = "The technology is licensed by the Massachusetts Institute of Technology, Rutgers State University of New Jersey, and the Research Foundation of State University of New York at Stony Brook under United States of America Serial No. 11/760379 and to the patents and/or patent applications resulting from it.";
const char *toku_copyright_string = "Copyright (c) 2007, 2008 Tokutek Inc.  All rights reserved.";

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <ctype.h>
#include <unistd.h>
#include <libgen.h>
#include <pthread.h>

#include "ydb-internal.h"

#include "brt-internal.h"
#include "cachetable.h"
#include "log.h"
#include "memory.h"


/** The default maximum number of persistent locks in a lock tree  */
const u_int32_t __toku_env_default_max_locks = 1000;

/* the ydb reference is used to cleanup the library when there are no more references to it */
static int toku_ydb_refs = 0;

static inline void ydb_add_ref() {
    ++toku_ydb_refs;
}

static inline void ydb_unref() {
    assert(toku_ydb_refs > 0);
    if (--toku_ydb_refs == 0) {
        /* call global destructors */
        toku_malloc_cleanup();
    }
}

/* env methods */
static int toku_env_close(DB_ENV *env, u_int32_t flags);
static int toku_env_set_data_dir(DB_ENV * env, const char *dir);
static int toku_env_set_lg_dir(DB_ENV * env, const char *dir);
static int toku_env_set_tmp_dir(DB_ENV * env, const char *tmp_dir);

static inline void env_add_ref(DB_ENV *env) {
    ++env->i->ref_count;
}

static inline void env_unref(DB_ENV *env) {
    assert(env->i->ref_count > 0);
    if (--env->i->ref_count == 0)
        toku_env_close(env, 0);
}

static inline int env_opened(DB_ENV *env) {
    return env->i->cachetable != 0;
}


/* db methods */
static inline int db_opened(DB *db) {
    return db->i->full_fname != 0;
}

static int toku_db_put(DB * db, DB_TXN * txn, DBT * key, DBT * data, u_int32_t flags);
static int toku_db_get (DB * db, DB_TXN * txn, DBT * key, DBT * data, u_int32_t flags);
static int toku_db_pget (DB *db, DB_TXN *txn, DBT *key, DBT *pkey, DBT *data, u_int32_t flags);
static int toku_db_cursor(DB *db, DB_TXN * txn, DBC **c, u_int32_t flags);

/* txn methods */

/* cursor methods */
static int toku_c_get(DBC * c, DBT * key, DBT * data, u_int32_t flag);
static int toku_c_get_noassociate(DBC * c, DBT * key, DBT * data, u_int32_t flag);
static int toku_c_pget(DBC * c, DBT *key, DBT *pkey, DBT *data, u_int32_t flag);
static int toku_c_del(DBC *c, u_int32_t flags);
static int toku_c_count(DBC *cursor, db_recno_t *count, u_int32_t flags);
static int toku_c_close(DBC * c);
static int toku_save_original_data(DBT* dst, DBT* src);

/* misc */
static char *construct_full_name(const char *dir, const char *fname);
static int do_associated_inserts (DB_TXN *txn, DBT *key, DBT *data, DB *secondary);
    
#if NEED_TEST

static int env_parse_config_line(DB_ENV* dbenv, char *command, char *value) {
    int r;
    
    if (!strcmp(command, "set_data_dir")) {
        r = toku_env_set_data_dir(dbenv, value);
    }
    else if (!strcmp(command, "set_tmp_dir")) {
        r = toku_env_set_tmp_dir(dbenv, value);
    }
    else if (!strcmp(command, "set_lg_dir")) {
        r = toku_env_set_lg_dir(dbenv, value);
    }
    else r = -1;
        
    return r;
}

static int env_read_config(DB_ENV *env) {
    HANDLE_PANICKED_ENV(env);
    const char* config_name = "DB_CONFIG";
    char* full_name = NULL;
    char* linebuffer = NULL;
    int buffersize;
    FILE* fp = NULL;
    int r = 0;
    int r2 = 0;
    char* command;
    char* value;
    
    full_name = construct_full_name(env->i->dir, config_name);
    if (full_name == 0) {
        r = ENOMEM;
        goto cleanup;
    }
    if ((fp = fopen(full_name, "r")) == NULL) {
        //Config file is optional.
        if (errno == ENOENT) {
            r = EXIT_SUCCESS;
            goto cleanup;
        }
        r = errno;
        goto cleanup;
    }
    //Read each line, applying configuration parameters.
    //After ignoring leading white space, skip any blank lines
    //or comments (starts with #)
    //Command contains no white space.  Value may contain whitespace.
    int linenumber;
    int ch = '\0';
    BOOL eof = FALSE;
    char* temp;
    char* end;
    int index;
    
    buffersize = 1<<10; //1KB
    linebuffer = toku_malloc(buffersize);
    if (!linebuffer) {
        r = ENOMEM;
        goto cleanup;
    }
    for (linenumber = 1; !eof; linenumber++) {
        /* Read a single line. */
        for (index = 0; TRUE; index++) {
            if ((ch = getc(fp)) == EOF) {
                eof = TRUE;
                if (ferror(fp)) {
                    /* Throw away current line and print warning. */
                    r = errno;
                    goto readerror;
                }
                break;
            }
            if (ch == '\n') break;
            if (index + 1 >= buffersize) {
                //Double the buffer.
                buffersize *= 2;
                linebuffer = toku_realloc(linebuffer, buffersize);
                if (!linebuffer) {
                    r = ENOMEM;
                    goto cleanup;
                }
            }
            linebuffer[index] = ch;
        }
        linebuffer[index] = '\0';
        end = &linebuffer[index];

        /* Separate the line into command/value */
        command = linebuffer;
        //Strip leading spaces.
        while (isspace(*command) && command < end) command++;
        //Find end of command.
        temp = command;
        while (!isspace(*temp) && temp < end) temp++;
        *temp++ = '\0'; //Null terminate command.
        value = temp;
        //Strip leading spaces.
        while (isspace(*value) && value < end) value++;
        if (value < end) {
            //Strip trailing spaces.
            temp = end;
            while (isspace(*(temp-1))) temp--;
            //Null terminate value.
            *temp = '\0';
        }
        //Parse the line.
        if (strlen(command) == 0 || command[0] == '#') continue; //Ignore Comments.
        r = env_parse_config_line(env, command, value < end ? value : "");
        if (r != 0) goto parseerror;
    }
    if (0) {
readerror:
        toku_ydb_do_error(env, r, "Error reading from DB_CONFIG:%d.\n", linenumber);
    }
    if (0) {
parseerror:
        toku_ydb_do_error(env, r, "Error parsing DB_CONFIG:%d.\n", linenumber);
    }
cleanup:
    if (full_name) toku_free(full_name);
    if (linebuffer) toku_free(linebuffer);
    if (fp) r2 = fclose(fp);
    return r ? r : r2;
}

#endif

static int toku_env_open(DB_ENV * env, const char *home, u_int32_t flags, int mode) {
    HANDLE_PANICKED_ENV(env);
    int r;

    if (env_opened(env)) {
	return toku_ydb_do_error(env, EINVAL, "The environment is already open\n");
    }

    if ((flags & DB_USE_ENVIRON) && (flags & DB_USE_ENVIRON_ROOT)) {
	return toku_ydb_do_error(env, EINVAL, "DB_USE_ENVIRON and DB_USE_ENVIRON_ROOT are incompatible flags\n");
    }

    if (home) {
        if ((flags & DB_USE_ENVIRON) || (flags & DB_USE_ENVIRON_ROOT)) {
	    return toku_ydb_do_error(env, EINVAL, "DB_USE_ENVIRON and DB_USE_ENVIRON_ROOT are incompatible with specifying a home\n");
	}
    }
    else if ((flags & DB_USE_ENVIRON) ||
             ((flags & DB_USE_ENVIRON_ROOT) && geteuid() == 0)) home = getenv("DB_HOME");

    if (!home) home = ".";

	// Verify that the home exists.
	{
	struct stat buf;
	r = stat(home, &buf);
	if (r!=0) {
	    return toku_ydb_do_error(env, errno, "Error from stat(\"%s\",...)\n", home);
	}
    }

    if (!(flags & DB_PRIVATE)) {
	return toku_ydb_do_error(env, EINVAL, "TokuDB requires DB_PRIVATE when opening an env\n");
    }

    if (env->i->dir)
        toku_free(env->i->dir);
    env->i->dir = toku_strdup(home);
    if (env->i->dir == 0) {
	return toku_ydb_do_error(env, ENOMEM, "Out of memory\n");
    }
    if (0) {
        died1:
        toku_free(env->i->dir);
        env->i->dir = NULL;
        return r;
    }
#if NEED_TEST
    if ((r = env_read_config(env)) != 0) {
	goto died1;
    }
#endif
    env->i->open_flags = flags;
    env->i->open_mode = mode;

    if (flags & (DB_INIT_TXN | DB_INIT_LOG)) {
        char* full_dir = NULL;
        if (env->i->lg_dir) full_dir = construct_full_name(env->i->dir, env->i->lg_dir);
	assert(env->i->logger);
        r = toku_logger_open(full_dir ? full_dir : env->i->dir, env->i->logger);
        if (full_dir) toku_free(full_dir);
	if (r!=0) {
	    toku_ydb_do_error(env, r, "Could not open logger\n");
	died2:
	    toku_logger_close(&env->i->logger);
	    goto died1;
	}
    }

    r = toku_brt_create_cachetable(&env->i->cachetable, env->i->cachetable_size, ZERO_LSN, env->i->logger);
    if (r!=0) goto died2;

    toku_logger_set_cachetable(env->i->logger, env->i->cachetable);

    return 0;
}

static int toku_env_close(DB_ENV * env, u_int32_t flags) {
    // Even if the env is panicedk, try to close as much as we can.
    int is_panicked = toku_env_is_panicked(env);
    int r0=0,r1=0;
    if (env->i->cachetable)
        r0=toku_cachetable_close(&env->i->cachetable);
    if (env->i->logger)
        r1=toku_logger_close(&env->i->logger);
    if (env->i->data_dirs) {
        u_int32_t i;
        assert(env->i->n_data_dirs > 0);
        for (i = 0; i < env->i->n_data_dirs; i++) {
            toku_free(env->i->data_dirs[i]);
        }
        toku_free(env->i->data_dirs);
    }
    if (env->i->lg_dir)
        toku_free(env->i->lg_dir);
    if (env->i->tmp_dir)
        toku_free(env->i->tmp_dir);
    toku_free(env->i->dir);
    toku_free(env->i);
    toku_free(env);
    ydb_unref();
    if (flags!=0) return EINVAL;
    if (r0) return r0;
    if (r1) return r1;
    if (is_panicked) return EINVAL;
    return 0;
}

static int toku_env_log_archive(DB_ENV * env, char **list[], u_int32_t flags) {
    env=env; flags=flags; // Suppress compiler warnings.
    *list = NULL;
    return 0;
}

static int toku_env_log_flush(DB_ENV * env, const DB_LSN * lsn __attribute__((__unused__))) {
    HANDLE_PANICKED_ENV(env);
    // We just flush everything.  MySQL uses lsn==0 which means flush everything.  For anyone else using the log, it is correct to flush too much, so we are OK.
    return toku_logger_fsync(env->i->logger);
}

static int toku_env_set_cachesize(DB_ENV * env, u_int32_t gbytes, u_int32_t bytes, int ncache) {
    HANDLE_PANICKED_ENV(env);
    if (ncache != 1)
        return EINVAL;
    u_int64_t cs64 = ((u_int64_t) gbytes << 30) + bytes;
    unsigned long cs = cs64;
    if (cs64 > cs)
        return EINVAL;
    env->i->cachetable_size = cs;
    return 0;
}

#if DB_VERSION_MAJOR == 4 && DB_VERSION_MINOR >= 3

static int toku_env_get_cachesize(DB_ENV * env, u_int32_t *gbytes, u_int32_t *bytes, int *ncache) {
    HANDLE_PANICKED_ENV(env);
    *gbytes = env->i->cachetable_size >> 30;
    *bytes = env->i->cachetable_size & ((1<<30)-1);
    *ncache = 1;
    return 0;
}

static int locked_env_get_cachesize(DB_ENV *env, u_int32_t *gbytes, u_int32_t *bytes, int *ncache) {
    toku_ydb_lock(); int r = toku_env_get_cachesize(env, gbytes, bytes, ncache); toku_ydb_unlock(); return r;
}

#endif

static int toku_env_set_data_dir(DB_ENV * env, const char *dir) {
    HANDLE_PANICKED_ENV(env);
    u_int32_t i;
    int r;
    char** temp;
    char* new_dir;
    
    if (env_opened(env) || !dir) {
	return toku_ydb_do_error(env, EINVAL, "You cannot set the data dir after opening the env\n");
    }
    
    if (env->i->data_dirs) {
        assert(env->i->n_data_dirs > 0);
        for (i = 0; i < env->i->n_data_dirs; i++) {
            if (!strcmp(dir, env->i->data_dirs[i])) {
                //It is already in the list.  We're done.
                return 0;
            }
        }
    }
    else assert(env->i->n_data_dirs == 0);
    new_dir = toku_strdup(dir);
    if (0) {
        died1:
        toku_free(new_dir);
        return r;
    }
    if (new_dir==NULL) {
	assert(errno == ENOMEM);
	return toku_ydb_do_error(env, errno, "Out of memory\n");
    }
    temp = (char**) toku_realloc(env->i->data_dirs, (1 + env->i->n_data_dirs) * sizeof(char*));
    if (temp==NULL) {assert(errno == ENOMEM); r = ENOMEM; goto died1;}
    else env->i->data_dirs = temp;
    env->i->data_dirs[env->i->n_data_dirs] = new_dir;
    env->i->n_data_dirs++;
    return 0;
}

static void toku_env_set_errcall(DB_ENV * env, toku_env_errcall_t errcall) {
    env->i->errcall = errcall;
}

static void toku_env_set_errfile(DB_ENV*env, FILE*errfile) {
    env->i->errfile = errfile;
}

static void toku_env_set_errpfx(DB_ENV * env, const char *errpfx) {
    env->i->errpfx = errpfx;
}

static int toku_env_set_flags(DB_ENV * env, u_int32_t flags, int onoff) {
    HANDLE_PANICKED_ENV(env);

    u_int32_t change = 0;
    if (flags & DB_AUTO_COMMIT) {
        change |=  DB_AUTO_COMMIT;
        flags  &= ~DB_AUTO_COMMIT;
    }
    if (flags != 0 && onoff) {
	return toku_ydb_do_error(env, EINVAL, "TokuDB does not (yet) support any nonzero ENV flags other than DB_AUTO_COMMIT\n");
    }
    if   (onoff) env->i->open_flags |=  change;
    else         env->i->open_flags &= ~change;
    return 0;
}

static int toku_env_set_lg_bsize(DB_ENV * env, u_int32_t bsize) {
    HANDLE_PANICKED_ENV(env);
    bsize=bsize;
    return toku_ydb_do_error(env, EINVAL, "TokuDB does not (yet) support ENV->set_lg_bsize\n");
}

static int toku_env_set_lg_dir(DB_ENV * env, const char *dir) {
    HANDLE_PANICKED_ENV(env);
    if (env_opened(env)) {
	return toku_ydb_do_error(env, EINVAL, "Cannot set log dir after opening the env\n");
    }

    if (env->i->lg_dir) toku_free(env->i->lg_dir);
    if (dir) {
        env->i->lg_dir = toku_strdup(dir);
        if (!env->i->lg_dir) {
	    return toku_ydb_do_error(env, ENOMEM, "Out of memory\n");
	}
    }
    else env->i->lg_dir = NULL;
    return 0;
}

static int toku_env_set_lg_max(DB_ENV * env, u_int32_t lg_max) {
    HANDLE_PANICKED_ENV(env);
    lg_max=lg_max;
    return toku_ydb_do_error(env, EINVAL, "TokuDB does not (yet) support set_lg_max\n");
}

static int toku_env_set_lk_detect(DB_ENV * env, u_int32_t detect) {
    HANDLE_PANICKED_ENV(env);
    detect=detect;
    return toku_ydb_do_error(env, EINVAL, "TokuDB does not (yet) support set_lk_detect\n");
}

static int toku_env_set_lk_max_locks(DB_ENV *dbenv, u_int32_t max) {
    HANDLE_PANICKED_ENV(dbenv);
    if (env_opened(dbenv))  return EINVAL;
    if (!max)               return EINVAL;
    dbenv->i->max_locks = max;
    return 0;
}

#if DB_VERSION_MAJOR == 4 && DB_VERSION_MINOR <= 4
static int toku_env_set_lk_max(DB_ENV * env, u_int32_t lk_max) {
    return toku_env_set_lk_max_locks(env, lk_max);
}

static int locked_env_set_lk_max(DB_ENV * env, u_int32_t lk_max) {
    toku_ydb_lock(); int r = toku_env_set_lk_max(env, lk_max); toku_ydb_unlock(); return r;
}
#endif

static int toku_env_get_lk_max_locks(DB_ENV *dbenv, u_int32_t *lk_maxp) {
    HANDLE_PANICKED_ENV(dbenv);
    if (!lk_maxp)           return EINVAL;
    *lk_maxp = dbenv->i->max_locks;
    return 0;
}

static int locked_env_set_lk_max_locks(DB_ENV *dbenv, u_int32_t max) {
    toku_ydb_lock(); int r = toku_env_set_lk_max_locks(dbenv, max); toku_ydb_unlock(); return r;
}

static int __attribute__((unused)) locked_env_get_lk_max_locks(DB_ENV *dbenv, u_int32_t *lk_maxp) {
    toku_ydb_lock(); int r = toku_env_get_lk_max_locks(dbenv, lk_maxp); toku_ydb_unlock(); return r;
}

//void toku__env_set_noticecall (DB_ENV *env, void (*noticecall)(DB_ENV *, db_notices)) {
//    env->i->noticecall = noticecall;
//}

static int toku_env_set_tmp_dir(DB_ENV * env, const char *tmp_dir) {
    HANDLE_PANICKED_ENV(env);
    if (env_opened(env)) {
	return toku_ydb_do_error(env, EINVAL, "Cannot set the tmp dir after opening an env\n");
    }
    if (!tmp_dir) {
	return toku_ydb_do_error(env, EINVAL, "Tmp dir bust be non-null\n");
    }
    if (env->i->tmp_dir)
        toku_free(env->i->tmp_dir);
    env->i->tmp_dir = toku_strdup(tmp_dir);
    return env->i->tmp_dir ? 0 : ENOMEM;
}

static int toku_env_set_verbose(DB_ENV * env, u_int32_t which, int onoff) {
    HANDLE_PANICKED_ENV(env);
    which=which; onoff=onoff;
    return 1;
}

static int toku_env_txn_checkpoint(DB_ENV * env, u_int32_t kbyte, u_int32_t min, u_int32_t flags) {
    env=env; kbyte=kbyte; min=min; flags=flags;
    return 0;
}

static int toku_env_txn_stat(DB_ENV * env, DB_TXN_STAT ** statp, u_int32_t flags) {
    HANDLE_PANICKED_ENV(env);
    statp=statp;flags=flags;
    return 1;
}

#if DB_VERSION_MAJOR == 4 && DB_VERSION_MINOR == 1
void toku_default_errcall(const char *errpfx, char *msg) {
#else
void toku_default_errcall(const DB_ENV *env, const char *errpfx, const char *msg) {
    env = env;
#endif
    fprintf(stderr, "YDB: %s: %s", errpfx, msg);
}

static int locked_env_open(DB_ENV * env, const char *home, u_int32_t flags, int mode) {
    toku_ydb_lock(); int r = toku_env_open(env, home, flags, mode); toku_ydb_unlock(); return r;
}

static int locked_env_close(DB_ENV * env, u_int32_t flags) {
    toku_ydb_lock(); int r = toku_env_close(env, flags); toku_ydb_unlock(); return r;
}

static int locked_env_log_archive(DB_ENV * env, char **list[], u_int32_t flags) {
    toku_ydb_lock(); int r = toku_env_log_archive(env, list, flags); toku_ydb_unlock(); return r;
}

static int locked_env_log_flush(DB_ENV * env, const DB_LSN * lsn) {
    toku_ydb_lock(); int r = toku_env_log_flush(env, lsn); toku_ydb_unlock(); return r;
}

static int locked_env_set_cachesize(DB_ENV *env, u_int32_t gbytes, u_int32_t bytes, int ncache) {
    toku_ydb_lock(); int r = toku_env_set_cachesize(env, gbytes, bytes, ncache); toku_ydb_unlock(); return r;
}

static int locked_env_set_data_dir(DB_ENV * env, const char *dir) {
    toku_ydb_lock(); int r = toku_env_set_data_dir(env, dir); toku_ydb_unlock(); return r;
}

static int locked_env_set_flags(DB_ENV * env, u_int32_t flags, int onoff) {
    toku_ydb_lock(); int r = toku_env_set_flags(env, flags, onoff); toku_ydb_unlock(); return r;
}

static int locked_env_set_lg_bsize(DB_ENV * env, u_int32_t bsize) {
    toku_ydb_lock(); int r = toku_env_set_lg_bsize(env, bsize); toku_ydb_unlock(); return r;
}

static int locked_env_set_lg_dir(DB_ENV * env, const char *dir) {
    toku_ydb_lock(); int r = toku_env_set_lg_dir(env, dir); toku_ydb_unlock(); return r;
}

static int locked_env_set_lg_max(DB_ENV * env, u_int32_t lg_max) {
    toku_ydb_lock(); int r = toku_env_set_lg_max(env, lg_max); toku_ydb_unlock(); return r;
}

static int locked_env_set_lk_detect(DB_ENV * env, u_int32_t detect) {
    toku_ydb_lock(); int r = toku_env_set_lk_detect(env, detect); toku_ydb_unlock(); return r;
}

static int locked_env_set_tmp_dir(DB_ENV * env, const char *tmp_dir) {
    toku_ydb_lock(); int r = toku_env_set_tmp_dir(env, tmp_dir); toku_ydb_unlock(); return r;
}

static int locked_env_set_verbose(DB_ENV * env, u_int32_t which, int onoff) {
    toku_ydb_lock(); int r = toku_env_set_verbose(env, which, onoff); toku_ydb_unlock(); return r;
}

static int locked_env_txn_checkpoint(DB_ENV * env, u_int32_t kbyte, u_int32_t min, u_int32_t flags) {
    toku_ydb_lock(); int r = toku_env_txn_checkpoint(env, kbyte, min, flags); toku_ydb_unlock(); return r;
}

static int locked_env_txn_stat(DB_ENV * env, DB_TXN_STAT ** statp, u_int32_t flags) {
    toku_ydb_lock(); int r = toku_env_txn_stat(env, statp, flags); toku_ydb_unlock(); return r;
}

static int locked_txn_begin(DB_ENV * env, DB_TXN * stxn, DB_TXN ** txn, u_int32_t flags);

static int toku_env_create(DB_ENV ** envp, u_int32_t flags) {
    if (flags!=0) return EINVAL;
    DB_ENV *MALLOC(result);
    if (result == 0)
        return ENOMEM;
    memset(result, 0, sizeof *result);
    result->err = toku_locked_env_err;
    result->open = locked_env_open;
    result->close = locked_env_close;
    result->txn_checkpoint = locked_env_txn_checkpoint;
    result->log_flush = locked_env_log_flush;
    result->set_errcall = toku_env_set_errcall;
    result->set_errfile = toku_env_set_errfile;
    result->set_errpfx = toku_env_set_errpfx;
    //result->set_noticecall = locked_env_set_noticecall;
    result->set_flags = locked_env_set_flags;
    result->set_data_dir = locked_env_set_data_dir;
    result->set_tmp_dir = locked_env_set_tmp_dir;
    result->set_verbose = locked_env_set_verbose;
    result->set_lg_bsize = locked_env_set_lg_bsize;
    result->set_lg_dir = locked_env_set_lg_dir;
    result->set_lg_max = locked_env_set_lg_max;
    result->set_lk_max_locks = locked_env_set_lk_max_locks;
    result->get_lk_max_locks = locked_env_get_lk_max_locks;
    result->set_cachesize = locked_env_set_cachesize;
#if DB_VERSION_MAJOR == 4 && DB_VERSION_MINOR >= 3
    result->get_cachesize = locked_env_get_cachesize;
#endif
    result->set_lk_detect = locked_env_set_lk_detect;
#if DB_VERSION_MAJOR == 4 && DB_VERSION_MINOR <= 4
    result->set_lk_max = locked_env_set_lk_max;
#endif
    result->log_archive = locked_env_log_archive;
    result->txn_stat = locked_env_txn_stat;
    result->txn_begin = locked_txn_begin;

    MALLOC(result->i);
    if (result->i == 0) {
        toku_free(result);
        return ENOMEM;
    }
    memset(result->i, 0, sizeof *result->i);
    result->i->is_panicked=0;
    result->i->ref_count = 1;
    result->i->errcall = 0;
    result->i->errpfx = 0;
    result->i->errfile = 0;
    result->i->max_locks = __toku_env_default_max_locks;

    {
	int r = toku_logger_create(&result->i->logger);
	if (r!=0) {
	    toku_free(result->i);
	    toku_free(result);
	    return r;
	}
	assert(result->i->logger);
    }

    ydb_add_ref();
    *envp = result;
    return 0;
}

int db_env_create(DB_ENV ** envp, u_int32_t flags) {
    toku_ydb_lock(); int r = toku_env_create(envp, flags); toku_ydb_unlock(); return r;
}

static int toku_txn_release_locks(DB_TXN* txn) {
    assert(txn);
    toku_lth* lth = txn->i->lth;

    int r = 0;
    if (lth) {
        toku_lth_start_scan(lth);
        toku_lock_tree* next = toku_lth_next(lth);
        int r2;
        while (next) {
            r2 = toku_lt_unlock(next, txn);
            if (r2!=0 && !r) r = r2;
            next = toku_lth_next(lth);
        }
        toku_lth_close(lth);
        txn->i->lth = NULL;
    }
    return r;
}

static int toku_txn_commit(DB_TXN * txn, u_int32_t flags) {
    HANDLE_PANICKED_ENV(txn->mgrp);
    //toku_ydb_notef("flags=%d\n", flags);
    int r;
    int nosync = (flags & DB_TXN_NOSYNC)!=0;
    flags &= ~DB_TXN_NOSYNC;
    if (!txn) return EINVAL;
    if (flags!=0) goto return_invalid;
    r = toku_logger_commit(txn->i->tokutxn, nosync);
    if (0) {
    return_invalid:
	r = EINVAL;
	toku_free(txn->i->tokutxn);
    }
    // Cleanup */
    int r2 = toku_txn_release_locks(txn);
    if (txn->i)
        toku_free(txn->i);
    toku_free(txn);
    return r ? r : r2; // The txn is no good after the commit.
}

static u_int32_t toku_txn_id(DB_TXN * txn) {
    HANDLE_PANICKED_ENV(txn->mgrp);
    toku_ydb_barf();
    abort();
    return -1;
}

static TXNID next_txn = 0;

static int toku_txn_abort(DB_TXN * txn) {
    HANDLE_PANICKED_ENV(txn->mgrp);
    int r = toku_logger_abort(txn->i->tokutxn);

    toku_txn_release_locks(txn);
    toku_free(txn->i);
    toku_free(txn);
    return r;
}

static int toku_txn_begin(DB_ENV *env, DB_TXN * stxn, DB_TXN ** txn, u_int32_t flags);

static int locked_txn_begin(DB_ENV *env, DB_TXN * stxn, DB_TXN ** txn, u_int32_t flags) {
    toku_ydb_lock(); int r = toku_txn_begin(env, stxn, txn, flags); toku_ydb_unlock(); return r;
}

static u_int32_t locked_txn_id(DB_TXN *txn) {
    toku_ydb_lock(); u_int32_t r = toku_txn_id(txn); toku_ydb_unlock(); return r;
}

static int locked_txn_commit(DB_TXN *txn, u_int32_t flags) {
    toku_ydb_lock(); int r = toku_txn_commit(txn, flags); toku_ydb_unlock(); return r;
}

static int locked_txn_abort(DB_TXN *txn) {
    toku_ydb_lock(); int r = toku_txn_abort(txn); toku_ydb_unlock(); return r;
}

static int toku_txn_begin(DB_ENV *env, DB_TXN * stxn, DB_TXN ** txn, u_int32_t flags) {
    HANDLE_PANICKED_ENV(env);
    if (!toku_logger_is_open(env->i->logger)) return toku_ydb_do_error(env, EINVAL, "Environment does not have logging enabled\n");
    if (!(env->i->open_flags & DB_INIT_TXN))  return toku_ydb_do_error(env, EINVAL, "Environment does not have transactions enabled\n");
    flags=flags;
    DB_TXN *MALLOC(result);
    if (result == 0)
        return ENOMEM;
    memset(result, 0, sizeof *result);
    //toku_ydb_notef("parent=%p flags=0x%x\n", stxn, flags);
    result->mgrp = env;
    result->abort = locked_txn_abort;
    result->commit = locked_txn_commit;
    result->id = locked_txn_id;
    MALLOC(result->i);
    if (!result->i) {
        toku_free(result);
        return ENOMEM;
    }
    memset(result->i, 0, sizeof *result->i);
    result->i->parent = stxn;

    int r;
    if (env->i->open_flags & DB_INIT_LOCK) {
        r = toku_lth_create(&result->i->lth,
                            toku_malloc, toku_free, toku_realloc);
        if (r!=0) {
            toku_free(result->i);
            toku_free(result);
            return r;
        }
    }
    
    r = toku_logger_txn_begin(stxn ? stxn->i->tokutxn : 0, &result->i->tokutxn, next_txn++, env->i->logger);
    if (r != 0)
        return r;
    *txn = result;
    return 0;
}

#if 0
int txn_commit(DB_TXN * txn, u_int32_t flags) {
    fprintf(stderr, "%s:%d\n", __FILE__, __LINE__);
    return toku_logger_log_commit(txn->i->tokutxn);
}
#endif

int log_compare(const DB_LSN * a, const DB_LSN * b) {
    toku_ydb_lock();
    fprintf(stderr, "%s:%d log_compare(%p,%p)\n", __FILE__, __LINE__, a, b);
    abort();
    toku_ydb_unlock();
}

static int maybe_do_associate_create (DB_TXN*txn, DB*primary, DB*secondary) {
    DBC *dbc;
    int r = toku_db_cursor(secondary, txn, &dbc, 0);
    if (r!=0) return r;
    DBT key,data;
    r = toku_c_get(dbc, &key, &data, DB_FIRST);
    {
	int r2=toku_c_close(dbc);
	if (r!=DB_NOTFOUND) {
	    return r2;
	}
    }
    /* Now we know the secondary is empty. */
    r = toku_db_cursor(primary, txn, &dbc, 0);
    if (r!=0) return r;
    for (r = toku_c_get(dbc, &key, &data, DB_FIRST); r==0; r = toku_c_get(dbc, &key, &data, DB_NEXT)) {
	r = do_associated_inserts(txn, &key, &data, secondary);
	if (r!=0) {
	    toku_c_close(dbc);
	    return r;
	}
    }
    return 0;
}

static int toku_db_associate (DB *primary, DB_TXN *txn, DB *secondary,
			      int (*callback)(DB *secondary, const DBT *key, const DBT *data, DBT *result),
			      u_int32_t flags) {
    HANDLE_PANICKED_DB(primary);
    HANDLE_PANICKED_DB(secondary);
    unsigned int brtflags;
    
    if (secondary->i->primary) return EINVAL; // The secondary already has a primary
    if (primary->i->primary)   return EINVAL; // The primary already has a primary

    toku_brt_get_flags(primary->i->brt, &brtflags);
    if (brtflags & TOKU_DB_DUPSORT) return EINVAL;  //The primary may not have duplicate keys.
    if (brtflags & TOKU_DB_DUP)     return EINVAL;  //The primary may not have duplicate keys.

    if (!list_empty(&secondary->i->associated)) return EINVAL; // The secondary is in some list (or it is a primary)
    assert(secondary->i->associate_callback==0);      // Something's wrong if this isn't null we made it this far.
    secondary->i->associate_callback = callback;
#ifdef DB_IMMUTABLE_KEY
    secondary->i->associate_is_immutable = (DB_IMMUTABLE_KEY&flags)!=0;
    flags &= ~DB_IMMUTABLE_KEY;
#else
    secondary->i->associate_is_immutable = 0;
#endif
    if (flags!=0 && flags!=DB_CREATE) return EINVAL; // after removing DB_IMMUTABLE_KEY the flags better be 0 or DB_CREATE
    list_push(&primary->i->associated, &secondary->i->associated);
    secondary->i->primary = primary;
    if (flags==DB_CREATE) {
	// To do this:  If the secondary is empty, then open a cursor on the primary.  Step through it all, doing the callbacks.
	// Then insert each callback result into the secondary.
	return maybe_do_associate_create(txn, primary, secondary);
    }
    return 0;
}

static int toku_db_close(DB * db, u_int32_t flags) {
    if (db->i->primary==0) {
	// It is a primary.  Unlink all the secondaries. */
	while (!list_empty(&db->i->associated)) {
	    assert(list_struct(list_head(&db->i->associated),
			       struct __toku_db_internal,
			       associated)->primary==db);
	    list_remove(list_head(&db->i->associated));
	}
    } else {
	// It is a secondary.  Remove it from the list, (which it must be in .*/
	if (!list_empty(&db->i->associated)) {
	    list_remove(&db->i->associated);
	}
    }
    flags=flags;
    int r = toku_close_brt(db->i->brt);
    if (r != 0)
        return r;
    if (db->i->lt) {
        r = toku_lt_close(db->i->lt);
        if (r!=0) return r;
    }
    // printf("%s:%d %d=__toku_db_close(%p)\n", __FILE__, __LINE__, r, db);
    // Even if panicked, let's close as much as we can.
    int is_panicked = toku_env_is_panicked(db->dbenv); 
    env_unref(db->dbenv);
    toku_free(db->i->database_name);
    toku_free(db->i->full_fname);
    toku_free(db->i);
    toku_free(db);
    ydb_unref();
    if (r==0 && is_panicked) return EINVAL;
    return r;
}

/* Verify that an element from the secondary database is still consistent
   with the primary.
   \param secondary Secondary database
   \param pkey Primary key
   \param data Primary data
   \param skey Secondary key to test
   
   \return 
*/
static int verify_secondary_key(DB *secondary, DBT *pkey, DBT *data, DBT *skey) {
    int r = 0;
    DBT idx;

    assert(secondary->i->primary != 0);
    memset(&idx, 0, sizeof(idx));
    r = secondary->i->associate_callback(secondary, pkey, data, &idx);
    if (r==DB_DONOTINDEX) { r = DB_SECONDARY_BAD; goto clean_up; }
    if (r!=0) goto clean_up;
#ifdef DB_DBT_MULTIPLE
    if (idx.flags & DB_DBT_MULTIPLE) {
        r = EINVAL; // We aren't ready for this
        goto clean_up;
    }
#endif
    if (secondary->i->brt->compare_fun(secondary, skey, &idx) != 0) {
        r = DB_SECONDARY_BAD;
        goto clean_up;
    }
    clean_up:
    if (idx.flags & DB_DBT_APPMALLOC) {
        /* This should be free because idx.data is allocated by the user */
    	free(idx.data);
    }
    return r; 
}

//Get the main portion of a cursor flag (excluding the bitwise or'd components).
static int get_main_cursor_flag(u_int32_t flag) {
#ifdef DB_READ_UNCOMMITTED
    flag &= ~DB_READ_UNCOMMITTED;
#endif    
#ifdef DB_MULTIPLE
    flag &= ~DB_MULTIPLE;
#endif
#ifdef DB_MULTIPLE_KEY
    flag &= ~DB_MULTIPLE_KEY;
#endif    
    flag &= ~DB_RMW;
    return flag;
}

static inline BOOL toku_c_uninitialized(DBC* c) {
    return toku_brt_cursor_uninitialized(c->i->c);
}            

static int toku_c_get_current_unconditional(DBC* c, DBT* key, DBT* data) {
    assert(!toku_c_uninitialized(c));
    memset(key,  0, sizeof(DBT));
    memset(data, 0, sizeof(DBT));
    data->flags = key->flags = DB_DBT_MALLOC;
    TOKUTXN txn = c->i->txn ? c->i->txn->i->tokutxn : NULL;
    int r = toku_brt_cursor_get(c->i->c, key, data, DB_CURRENT_BINDING, txn);
    return r;
}

static inline void toku_swap_flag(u_int32_t* flag, u_int32_t* get_flag,
                                  u_int32_t new_flag) {
    *flag    -= *get_flag;
    *get_flag =  new_flag;
    *flag    += *get_flag;
}

static inline int toku_uninitialized_swap(DBC* c, DBT* key, DBT* data,
                                          u_int32_t* flag, u_int32_t* get_flag,
                                          u_int32_t new_flag) {
    /* DB_FIRST/DB_LAST do nothing in pre_lock so we can skip the goto.  */
    if (toku_c_uninitialized(c)) toku_swap_flag(flag, get_flag, new_flag);
    else return toku_c_get_current_unconditional(c, key, data);
    return 0;
}

/*
    Used for partial implementation of nested transactions.
    Work is done by children as normal, but all locking is done by the
    root of the nested txn tree.
    This may hold extra locks, and will not work as expected when
    a node has two non-completed txns at any time.
*/
static inline DB_TXN* toku_txn_ancestor(DB_TXN* txn) {
    while (txn && txn->i->parent) txn = txn->i->parent;
    return txn;
}

static int toku_c_get_pre_lock(DBC* c, DBT* key, DBT* data, u_int32_t* flag,
                               DBT* saved_key, DBT* saved_data) {
    assert(saved_key && saved_data && flag);
    DB* db  = c->dbp;
    if (!db->i->lt) return 0;
    saved_key->data = NULL;
    saved_data->data = NULL;
    DB_TXN* txn = c->i->txn;

    u_int32_t get_flag = get_main_cursor_flag(*flag);
    unsigned int brtflags;
    toku_brt_get_flags(db->i->brt, &brtflags);
    BOOL duplicates = (brtflags & TOKU_DB_DUPSORT) != 0;

    int r = 0;
    switch (get_flag) {
        case (DB_CURRENT):
        case (DB_SET):
        case (DB_FIRST):
        case (DB_LAST): {
            /* The above cases have all their code in toku_c_get_post_lock. */
            break;
        }
        case (DB_GET_BOTH): {
            get_both:
            r = toku_lt_acquire_read_lock(db->i->lt, toku_txn_ancestor(txn),
                                          key, data);
            break;
        }
        case (DB_SET_RANGE): {
            r = toku_save_original_data(saved_key, key);
            break;
        }
        case (DB_GET_BOTH_RANGE): {
            if (!duplicates) {
                toku_swap_flag(flag, &get_flag, DB_GET_BOTH); goto get_both; }
            r = toku_save_original_data(saved_data, data);
            break;
        }
        case (DB_NEXT):
        case (DB_NEXT_NODUP): {
            r = toku_uninitialized_swap(c, saved_key, saved_data, flag,
                                        &get_flag, DB_FIRST);
            break;
        }
        case (DB_PREV):
        case (DB_PREV_NODUP): {
            r = toku_uninitialized_swap(c, saved_key, saved_data, flag,
                                        &get_flag, DB_LAST);
            break;
        }
#ifdef DB_PREV_DUP
        case (DB_PREV_DUP):
#endif
        case (DB_NEXT_DUP): {
            if (!duplicates || toku_c_uninitialized(c)) r = EINVAL;
            else r = toku_c_get_current_unconditional(c, saved_key, saved_data);
            break;
        }
        default: {
            //TODO: Output an error.
            r = EINVAL;
            break;
        }
    }
    return r;
}

static int toku_c_get_post_lock(DBC* c, DBT* key, DBT* data, u_int32_t flag,
                                int r_last, DBT* saved_key, DBT* saved_data) {
    assert(saved_key && saved_data);
    DB*     db  = c->dbp;
    if (!db->i->lt) return r_last;
    int r = 0;
    if (r_last && r_last != DB_NOTFOUND && r_last != DB_KEYEMPTY) {
        r = r_last;
        goto cleanup;
    }

    DB_TXN* txn = c->i->txn;
    u_int32_t get_flag = get_main_cursor_flag(flag);
    if (r_last == DB_KEYEMPTY) {
        assert(get_flag == DB_CURRENT);
        return r_last;
    }
    assert(r_last == DB_NOTFOUND || r_last == 0);
    BOOL found = r_last == 0;

    BOOL lock = TRUE;
    const DBT* key_l;
    const DBT* key_r;
    const DBT* data_l;
    const DBT* data_r;
    switch (get_flag) {
        case (DB_CURRENT): {
            /* No locking necessary. You already own a lock by virtue
               of having a cursor pointing to this. */
            lock = FALSE;
            break;
        }
        case (DB_SET): {
            key_l  = key_r = key;
            data_l =                toku_lt_neg_infinity;
            data_r = found ? data : toku_lt_infinity;
            break;
        }
        case (DB_GET_BOTH): {
            /* All done in toku_c_get_pre_lock. */
            lock = FALSE;
            break;
        }
        case (DB_FIRST): {
            key_l  = data_l = toku_lt_neg_infinity;
            key_r  = found ? key  : toku_lt_infinity;
            data_r = found ? data : toku_lt_infinity;
            break;
        }
        case (DB_LAST): {
            key_l  = found ? key  : toku_lt_neg_infinity;
            data_l = found ? data : toku_lt_neg_infinity;
            key_r  = data_r = toku_lt_infinity;
            break;
        }
        case (DB_SET_RANGE): {
            key_l  = saved_key;
            data_l = toku_lt_neg_infinity;
            key_r  = found ? key  : toku_lt_infinity;
            data_r = found ? data : toku_lt_infinity;
            break;
        }
        case (DB_GET_BOTH_RANGE): {
            key_l  = key_r = key;
            data_l = saved_data;
            data_r = found ? data : toku_lt_infinity;
            break;
        }
        case (DB_NEXT):
        case (DB_NEXT_NODUP): {
            assert(!toku_c_uninitialized(c));
            key_l  = saved_key;
            data_l = saved_data;
            key_r  = found ? key  : toku_lt_infinity;
            data_r = found ? data : toku_lt_infinity;
            break;
        }
        case (DB_PREV):
        case (DB_PREV_NODUP): {
            assert(!toku_c_uninitialized(c));
            key_l  = found ? key  : toku_lt_neg_infinity;
            data_l = found ? data : toku_lt_neg_infinity;
            key_r  = saved_key;
            data_r = saved_data;
            break;
        }
        case (DB_NEXT_DUP): {
            assert(!toku_c_uninitialized(c));
            key_l  = key_r = saved_key;
            data_l = saved_data;
            data_r = found ? data : toku_lt_infinity;
            break;
        }
#ifdef DB_PREV_DUP
        case (DB_PREV_DUP): {
            assert(!toku_c_uninitialized(c));
            key_l  = key_r = saved_key;
            data_l = found ? data : toku_lt_neg_infinity;
            data_r = saved_data;
            break;
        }
#endif
        default: {
            r = EINVAL;
            lock = FALSE;
            break;
        }
    }
    if (lock) r = toku_lt_acquire_range_read_lock(db->i->lt,
                                                  toku_txn_ancestor(txn),
                                                  key_l, data_l,
                                                  key_r, data_r);
cleanup:
    if (saved_key->data)  toku_free(saved_key->data);
    if (saved_data->data) toku_free(saved_data->data);
    return r ? r : r_last;
}



static int toku_c_get_noassociate(DBC * c, DBT * key, DBT * data, u_int32_t flag) {
    HANDLE_PANICKED_DB(c->dbp);
    DBT saved_key;
    DBT saved_data;

    int r;
    r = toku_c_get_pre_lock(c, key, data, &flag, &saved_key, &saved_data);
    if (r!=0) return r;
    TOKUTXN txn = c->i->txn ? c->i->txn->i->tokutxn : NULL;
    r = toku_brt_cursor_get(c->i->c, key, data, flag, txn);
    r = toku_c_get_post_lock(c, key, data, flag, r, &saved_key, &saved_data);
    return r;
}

static int toku_c_del_noassociate(DBC * c, u_int32_t flags) {
    DB* db = c->dbp;
    HANDLE_PANICKED_DB(db);
    if (toku_c_uninitialized(c)) return EINVAL;

    int r;
    if (db->i->lt) {
        DBT saved_key;
        DBT saved_data;
        r = toku_c_get_current_unconditional(c, &saved_key, &saved_data);
        if (r!=0) return r;
        r = toku_lt_acquire_write_lock(db->i->lt, toku_txn_ancestor(c->i->txn),
                                       &saved_key, &saved_data);
        if (saved_key.data)  toku_free(saved_key.data);
        if (saved_data.data) toku_free(saved_data.data);
        if (r!=0) return r;
    }
    r = toku_brt_cursor_delete(c->i->c, flags, c->i->txn ? c->i->txn->i->tokutxn : 0);
    return r;
}

static int toku_save_original_data(DBT* dst, DBT* src) {
    int r;
    
    *dst = *src;
#ifdef DB_DBT_PARTIAL
#error toku_c_pget does not properly handle DB_DBT_PARTIAL
#endif
    //We may use this multiple times, we'll free only once at the end.
    dst->flags = DB_DBT_REALLOC;
    //Not using DB_DBT_USERMEM.
    dst->ulen = 0;
    if (src->size) {
        if (!src->data) return EINVAL;
        dst->data = toku_malloc(src->size);
        if (!dst->data) {
            r = ENOMEM;
            return r;
        }
        memcpy(dst->data, src->data, src->size);
    }
    else dst->data = NULL;
    return 0;
}

static int toku_c_pget(DBC * c, DBT *key, DBT *pkey, DBT *data, u_int32_t flag) {
    int r;
    int r2;
    int r3;
    DB *db = c->dbp;
    HANDLE_PANICKED_DB(db);
    DB *pdb = db->i->primary;
    
    if (!pdb) return EINVAL;  //c_pget does not work on a primary.
	// If data and primary_key are both zeroed, the temporary storage used to fill in data is different in the two cases because they come from different trees.
	assert(db->i->brt!=pdb->i->brt); // Make sure they realy are different trees.
    assert(db!=pdb);

    DBT copied_key;
    DBT copied_pkey;
    DBT copied_data;
    //Store original pointers.
    DBT* o_key = key;
    DBT* o_pkey = pkey;
    DBT* o_data = data;
    //Use copied versions for everything until/if success.
    key  = &copied_key;
    pkey = &copied_pkey;
    data = &copied_data;

    if (0) {
delete_silently_and_retry:
        //Free any old data.
        toku_free(key->data);
        toku_free(pkey->data);
        toku_free(data->data);
        //Silently delete and re-run.
        r = toku_c_del_noassociate(c, 0);
        if (r != 0) return r;
    }
    if (0) {
        died0:
        return r;
    }
    //Need to save all the original data.
    r = toku_save_original_data(&copied_key, o_key);   if (r!=0) goto died0;
    if (0) {
        died1:
        toku_free(key->data);
        goto died0;
    }
    r = toku_save_original_data(&copied_pkey, o_pkey); if (r!=0) goto died1;
    if (0) {
        died2:
        toku_free(pkey->data);
        goto died1;
    }
    r = toku_save_original_data(&copied_data, o_data); if (r!=0) goto died2;
    if (0) {
        died3:
        toku_free(data->data);
        goto died2;
    }

    r = toku_c_get_noassociate(c, key, pkey, flag);
    if (r != 0) goto died3;
    r = toku_db_get(pdb, c->i->txn, pkey, data, 0);
    if (r == DB_NOTFOUND)   goto delete_silently_and_retry;
    if (r != 0) goto died3;
    r = verify_secondary_key(db, pkey, data, key);
    if (r == DB_SECONDARY_BAD) goto delete_silently_and_retry;
    if (r != 0) goto died3;

    //Copy everything and return.
    assert(r==0);

    r  = toku_brt_dbt_set_key(db->i->brt,  o_key,  key->data,  key->size);
    r2 = toku_brt_dbt_set_key(pdb->i->brt, o_pkey, pkey->data, pkey->size);
    r3 = toku_brt_dbt_set_value(pdb->i->brt, o_data, data->data, data->size);

    //Cleanup.
    toku_free(key->data);
    toku_free(pkey->data);
    toku_free(data->data);
    if (r!=0) return r;
    if (r2!=0) return r2;
    return r3;
}

static int toku_c_get(DBC * c, DBT * key, DBT * data, u_int32_t flag) {
    DB *db = c->dbp;
    HANDLE_PANICKED_DB(db);
    int r;

    if (db->i->primary==0) {
        r = toku_c_get_noassociate(c, key, data, flag);
    }
    else {
        // It's a c_get on a secondary.
        DBT primary_key;
        
        /* It is an error to use the DB_GET_BOTH or DB_GET_BOTH_RANGE flag on a
         * cursor that has been opened on a secondary index handle.
         */
        u_int32_t get_flag = get_main_cursor_flag(flag);
        if ((get_flag == DB_GET_BOTH) ||
            (get_flag == DB_GET_BOTH_RANGE)) return EINVAL;
        memset(&primary_key, 0, sizeof(primary_key));
        r = toku_c_pget(c, key, &primary_key, data, flag);
    }
    return r;
}

static int toku_c_close(DBC * c) {
    int r = toku_brt_cursor_close(c->i->c);
    toku_free(c->i);
    toku_free(c);
    return r;
}

static inline int keyeq(DBC *c, DBT *a, DBT *b) {
    DB *db = c->dbp;
    return db->i->brt->compare_fun(db, a, b) == 0;
}

static int toku_c_count(DBC *cursor, db_recno_t *count, u_int32_t flags) {
    int r;
    DBC *count_cursor = 0;
    DBT currentkey; memset(&currentkey, 0, sizeof currentkey); currentkey.flags = DB_DBT_REALLOC;
    DBT currentval; memset(&currentval, 0, sizeof currentval); currentval.flags = DB_DBT_REALLOC;
    DBT key; memset(&key, 0, sizeof key); key.flags = DB_DBT_REALLOC;
    DBT val; memset(&val, 0, sizeof val); val.flags = DB_DBT_REALLOC;

    if (flags != 0) {
        r = EINVAL; goto finish;
    }

    r = toku_c_get(cursor, &currentkey, &currentval, DB_CURRENT_BINDING);
    if (r != 0) goto finish;
    
    r = toku_db_cursor(cursor->dbp, 0, &count_cursor, 0);
    if (r != 0) goto finish;

    *count = 0;
    r = toku_c_get(count_cursor, &currentkey, &currentval, DB_SET); 
    if (r != 0) {
        r = 0; goto finish; /* success, the current key must be deleted and there are no more */
    }

    for (;;) {
        *count += 1;
        r = toku_c_get(count_cursor, &key, &val, DB_NEXT);
        if (r != 0) break;
        if (!keyeq(count_cursor, &currentkey, &key)) break;
    }
    r = 0; /* success, we found at least one before the end */
finish:
    if (key.data) toku_free(key.data);
    if (val.data) toku_free(val.data);
    if (currentkey.data) toku_free(currentkey.data);
    if (currentval.data) toku_free(currentval.data);
    if (count_cursor) {
        int rr = toku_c_close(count_cursor); assert(rr == 0);
    }
    return r;
}

static int toku_db_get_noassociate(DB * db, DB_TXN * txn, DBT * key, DBT * data, u_int32_t flags) {
    int r;
    if (flags!=0 && flags!=DB_GET_BOTH) return EINVAL;

    DBC *dbc;
    r = toku_db_cursor(db, txn, &dbc, 0);
    if (r!=0) return r;
    r = toku_c_get_noassociate(dbc, key, data,
                               (flags == 0) ? DB_SET : DB_GET_BOTH);
    int r2 = toku_c_close(dbc);
    return r ? r : r2;
}

static int toku_db_del_noassociate(DB * db, DB_TXN * txn, DBT * key, u_int32_t flags) {
    int r;
    if (flags!=0 && flags!=DB_DELETE_ANY) return EINVAL;
    //DB_DELETE_ANY supresses the BDB DB->del return value indicating that the key was not found prior to the delete
    if (!(flags & DB_DELETE_ANY)) {
        DBT search_val; memset(&search_val, 0, sizeof search_val); 
        search_val.flags = DB_DBT_MALLOC;
        r = toku_db_get_noassociate(db, txn, key, &search_val, 0);
        if (r != 0)
            return r;
        toku_free(search_val.data);
    } 
    //Do the actual deleting.
    if (db->i->lt) {
        r = toku_lt_acquire_range_write_lock(db->i->lt, toku_txn_ancestor(txn),
                                             key, toku_lt_neg_infinity,
                                             key, toku_lt_infinity);
        if (r!=0) return r;
    }
    r = toku_brt_delete(db->i->brt, key, txn ? txn->i->tokutxn : 0);
    return r;
}

static int do_associated_deletes(DB_TXN *txn, DBT *key, DBT *data, DB *secondary) {
    u_int32_t brtflags;
    DBT idx;
    memset(&idx, 0, sizeof(idx));
    int r2 = 0;
    int r = secondary->i->associate_callback(secondary, key, data, &idx);
    if (r==DB_DONOTINDEX) { r = 0; goto clean_up; }
    if (r!=0) goto clean_up;
#ifdef DB_DBT_MULTIPLE
    if (idx.flags & DB_DBT_MULTIPLE) {
        r = EINVAL; // We aren't ready for this
        goto clean_up;
    }
#endif
    toku_brt_get_flags(secondary->i->brt, &brtflags);
    if (brtflags & TOKU_DB_DUPSORT) {
        //If the secondary has duplicates we need to use cursor deletes.
        DBC *dbc;
        r = toku_db_cursor(secondary, txn, &dbc, 0);
        if (r!=0) goto cursor_cleanup;
        r = toku_c_get_noassociate(dbc, &idx, key, DB_GET_BOTH);
        if (r!=0) goto cursor_cleanup;
        r = toku_c_del_noassociate(dbc, 0);
    cursor_cleanup:
        r2 = toku_c_close(dbc);
    } else 
        r = toku_db_del_noassociate(secondary, txn, &idx, DB_DELETE_ANY);
    clean_up:
    if (idx.flags & DB_DBT_APPMALLOC) {
        /* This should be free because idx.data is allocated by the user */
    	free(idx.data);
    }
    if (r!=0) return r;
    return r2;
}

static int toku_c_del(DBC * c, u_int32_t flags) {
    int r;
    DB* db = c->dbp;
    HANDLE_PANICKED_DB(db);
    
    //It is a primary with secondaries, or is a secondary.
    if (db->i->primary != 0 || !list_empty(&db->i->associated)) {
        DB* pdb;
        DBT pkey;
        DBT data;
        struct list *h;

        memset(&pkey, 0, sizeof(pkey));
        memset(&data, 0, sizeof(data));
        if (db->i->primary == 0) {
            pdb = db;
            r = toku_c_get(c, &pkey, &data, DB_CURRENT);
        } else {
            DBT skey;
            pdb = db->i->primary;
            memset(&skey, 0, sizeof(skey));
            r = toku_c_pget(c, &skey, &pkey, &data, DB_CURRENT);
        }
        if (r != 0) return r;
        
    	for (h = list_head(&pdb->i->associated); h != &pdb->i->associated; h = h->next) {
    	    struct __toku_db_internal *dbi = list_struct(h, struct __toku_db_internal, associated);
    	    if (dbi->db == db) continue;  //Skip current db (if its primary or secondary)
    	    r = do_associated_deletes(c->i->txn, &pkey, &data, dbi->db);
    	    if (r!=0) return r;
    	}
    	if (db->i->primary != 0) {
    	    //If this is a secondary, we did not delete from the primary.
    	    //Primaries cannot have duplicates, (noncursor) del is safe.
    	    r = toku_db_del_noassociate(pdb, c->i->txn, &pkey, DB_DELETE_ANY);
    	    if (r!=0) return r;
    	}
    }
    r = toku_c_del_noassociate(c, flags);
    return r;    
}

static int toku_c_put(DBC *dbc, DBT *key, DBT *data, u_int32_t flags) {
    DB* db = dbc->dbp;
    HANDLE_PANICKED_DB(db);
    unsigned int brtflags;
    int r;
    DBT* put_key  = key;
    DBT* put_data = data;
    DBT* get_key  = key;
    DBT* get_data = data;
    DB_TXN* txn = dbc->i->txn;
    
    //Cannot c_put in a secondary index.
    if (db->i->primary!=0) return EINVAL;
    toku_brt_get_flags(db->i->brt, &brtflags);
    //We do not support duplicates without sorting.
    if (!(brtflags & TOKU_DB_DUPSORT) && (brtflags & TOKU_DB_DUP)) return EINVAL;
    
    if (flags==DB_CURRENT) {
        DBT key_local;
        DBT data_local;
        memset(&key_local, 0, sizeof(DBT));
        memset(&data_local, 0, sizeof(DBT));
        //Can't afford to overwrite the local storage.
        key_local.flags = DB_DBT_MALLOC;
        data_local.flags = DB_DBT_MALLOC;
        r = toku_c_get(dbc, &key_local, &data_local, DB_CURRENT);
        if (0) {
            cleanup:
            if (flags==DB_CURRENT) {
                toku_free(key_local.data);
                toku_free(data_local.data);
            }
            return r;
        }
        if (r==DB_KEYEMPTY) return DB_NOTFOUND;
        if (r!=0) return r;
        if (brtflags & TOKU_DB_DUPSORT) {
            r = db->i->brt->dup_compare(db, &data_local, data);
            if (r!=0) {r = EINVAL; goto cleanup;}
        }
        //Remove old pair.
        if (db->i->lt) {
            /* Acquire all write locks before  */
            r = toku_lt_acquire_write_lock(db->i->lt, toku_txn_ancestor(txn),
                                           &key_local, &data_local);
            if (r!=0) goto cleanup;
            r = toku_lt_acquire_write_lock(db->i->lt, toku_txn_ancestor(txn),
                                           &key_local, data);
            if (r!=0) goto cleanup;
        }
        r = toku_c_del(dbc, 0);
        if (r!=0) goto cleanup;
        get_key = put_key  = &key_local;
        goto finish;
    }
    else if (flags==DB_KEYFIRST || flags==DB_KEYLAST) {
        goto finish;        
    }
    else if (flags==DB_NODUPDATA) {
        //Must support sorted duplicates.
        if (!(brtflags & TOKU_DB_DUPSORT)) return EINVAL;
        r = toku_c_get(dbc, key, data, DB_GET_BOTH);
        if (r==0) return DB_KEYEXIST;
        if (r!=DB_NOTFOUND) return r;
        goto finish;
    }
    //Flags must NOT be 0.
    else return EINVAL;
finish:
    //Insert new data with the key we got from c_get
    r = toku_db_put(db, dbc->i->txn, put_key, put_data, DB_YESOVERWRITE); // when doing the put, it should do an overwrite.
    if (r!=0) goto cleanup;
    r = toku_c_get(dbc, get_key, get_data, DB_GET_BOTH);
    goto cleanup;
}

static int locked_c_pget(DBC * c, DBT *key, DBT *pkey, DBT *data, u_int32_t flag) {
    toku_ydb_lock(); int r = toku_c_pget(c, key, pkey, data, flag); toku_ydb_unlock(); return r;
}

static int locked_c_get(DBC * c, DBT * key, DBT * data, u_int32_t flag) {
    toku_ydb_lock(); int r = toku_c_get(c, key, data, flag); toku_ydb_unlock(); return r;
}

static int locked_c_close(DBC * c) {
    toku_ydb_lock(); int r = toku_c_close(c); toku_ydb_unlock(); return r;
}

static int locked_c_count(DBC *cursor, db_recno_t *count, u_int32_t flags) {
    toku_ydb_lock(); int r = toku_c_count(cursor, count, flags); toku_ydb_unlock(); return r;
}

static int locked_c_del(DBC * c, u_int32_t flags) {
    toku_ydb_lock(); int r = toku_c_del(c, flags); toku_ydb_unlock(); return r;
}

static int locked_c_put(DBC *dbc, DBT *key, DBT *data, u_int32_t flags) {
    toku_ydb_lock(); int r = toku_c_put(dbc, key, data, flags); toku_ydb_unlock(); return r;
}

static int toku_db_cursor(DB * db, DB_TXN * txn, DBC ** c, u_int32_t flags) {
    HANDLE_PANICKED_DB(db);
    if (flags != 0)
        return EINVAL;
    DBC *MALLOC(result);
    if (result == 0)
        return ENOMEM;
    memset(result, 0, sizeof *result);
    result->c_get = locked_c_get;
    result->c_pget = locked_c_pget;
    result->c_put = locked_c_put;
    result->c_close = locked_c_close;
    result->c_del = locked_c_del;
    result->c_count = locked_c_count;
    MALLOC(result->i);
    assert(result->i);
    result->dbp = db;
    result->i->txn = txn;
    int r = toku_brt_cursor(db->i->brt, &result->i->c);
    assert(r == 0);
    *c = result;
    return 0;
}

static int toku_db_del(DB *db, DB_TXN *txn, DBT *key, u_int32_t flags) {
    HANDLE_PANICKED_DB(db);
    int r;

    //It is a primary with secondaries, or is a secondary.
    if (db->i->primary != 0 || !list_empty(&db->i->associated)) {
        DB* pdb;
        DBT data;
        DBT pkey;
        DBT *pdb_key;
        struct list *h;
        u_int32_t brtflags;

        memset(&data, 0, sizeof(data));

        toku_brt_get_flags(db->i->brt, &brtflags);
        if (brtflags & TOKU_DB_DUPSORT) {
            int r2;
    	    DBC *dbc;
    	    BOOL found = FALSE;

            /* If we are deleting all copies from a secondary with duplicates,
             * We have to make certain we cascade all the deletes. */

            assert(db->i->primary!=0);    //Primary cannot have duplicates.
            r = toku_db_cursor(db, txn, &dbc, 0);
            if (r!=0) return r;
            r = toku_c_get_noassociate(dbc, key, &data, DB_SET);
            while (r==0) {
                r = toku_c_del(dbc, 0);
                if (r==0) found = TRUE;
                if (r!=0 && r!=DB_KEYEMPTY) break;
                r = toku_c_get_noassociate(dbc, key, &data, DB_NEXT_DUP);
                if (r == DB_NOTFOUND) {
                    //If we deleted at least one we're happy.  Quit out.
                    if (found) r = 0;
                    break;
                }
            }

            r2 = toku_c_close(dbc);
            if (r != 0) return r;
            return r2;
        }

        inline void cleanup() {
            if (data.data) toku_free(data.data);
            if (pkey.data) toku_free(pkey.data);
        }

        memset(&data, 0, sizeof data); data.flags = DB_DBT_REALLOC;
        memset(&pkey, 0, sizeof pkey); pkey.flags = DB_DBT_REALLOC;

        if (db->i->primary == 0) {
            pdb = db;
            r = toku_db_get(db, txn, key, &data, 0);
            pdb_key = key;
        }
        else {
            pdb = db->i->primary;
            r = toku_db_pget(db, txn, key, &pkey, &data, 0);
            pdb_key = &pkey;
        }
        if (r != 0) { 
            cleanup(); return r; 
        }
        
    	for (h = list_head(&pdb->i->associated); h != &pdb->i->associated; h = h->next) {
    	    struct __toku_db_internal *dbi = list_struct(h, struct __toku_db_internal, associated);
    	    if (dbi->db == db) continue;                  //Skip current db (if its primary or secondary)
    	    r = do_associated_deletes(txn, pdb_key, &data, dbi->db);
    	    if (r!=0) { 
                cleanup(); return r;
            }
    	}
    	if (db->i->primary != 0) {
    	    //If this is a secondary, we did not delete from the primary.
    	    //Primaries cannot have duplicates, (noncursor) del is safe.
    	    r = toku_db_del_noassociate(pdb, txn, pdb_key, DB_DELETE_ANY);
    	    if (r!=0) { 
                cleanup(); return r;
            }
    	}

        cleanup();

    	//We know for certain it was already found, so no need to return DB_NOTFOUND.
    	flags |= DB_DELETE_ANY;
    }
    r = toku_db_del_noassociate(db, txn, key, flags);
    return r;
}

static inline int db_thread_need_flags(DBT *dbt) {
    return (dbt->flags & (DB_DBT_MALLOC+DB_DBT_REALLOC+DB_DBT_USERMEM)) == 0;
}

static int toku_db_get (DB * db, DB_TXN * txn, DBT * key, DBT * data, u_int32_t flags) {
    HANDLE_PANICKED_DB(db);
    int r;

    if ((db->i->open_flags & DB_THREAD) && db_thread_need_flags(data))
        return EINVAL;

    if (flags != 0 && flags != DB_GET_BOTH) return EINVAL;
    // We aren't ready to handle flags such as DB_READ_COMMITTED or DB_READ_UNCOMMITTED or DB_RMW

    DBC *dbc;
    r = toku_db_cursor(db, txn, &dbc, 0);
    if (r!=0) return r;
    r = toku_c_get(dbc, key, data, (flags == 0) ? DB_SET : DB_GET_BOTH);
    int r2 = toku_c_close(dbc);
    return r ? r : r2;
}

static int toku_db_pget (DB *db, DB_TXN *txn, DBT *key, DBT *pkey, DBT *data, u_int32_t flags) {
    HANDLE_PANICKED_DB(db);
    int r;
    int r2;
    DBC *dbc;
    if (!db->i->primary) return EINVAL; // pget doesn't work on a primary.
    assert(flags==0); // not ready to handle all those other options
    assert(db->i->brt != db->i->primary->i->brt); // Make sure they realy are different trees.
    assert(db!=db->i->primary);

    if ((db->i->open_flags & DB_THREAD) && (db_thread_need_flags(pkey) || db_thread_need_flags(data)))
        return EINVAL;

    r = toku_db_cursor(db, txn, &dbc, 0);
    if (r!=0) return r;
    r = toku_c_pget(dbc, key, pkey, data, DB_SET);
    if (r==DB_KEYEMPTY) r = DB_NOTFOUND;
    r2 = toku_c_close(dbc);
    if (r!=0) return r;
    return r2;    
}

#if 0
static int toku_db_key_range(DB * db, DB_TXN * txn, DBT * dbt, DB_KEY_RANGE * kr, u_int32_t flags) {
    HANDLE_PANICKED_DB(db);
    txn=txn; dbt=dbt; kr=kr; flags=flags;
    toku_ydb_barf();
    abort();
}
#endif

static int construct_full_name_in_buf(const char *dir, const char *fname, char* full, int length) {
    int l;

    if (!full) return EINVAL;
    l = snprintf(full, length, "%s", dir);
    if (l >= length) return ENAMETOOLONG;
    if (l == 0 || full[l - 1] != '/') {
        if (l + 1 == length) return ENAMETOOLONG;
            
        /* Didn't put a slash down. */
        if (fname[0] != '/') {
            full[l++] = '/';
            full[l] = 0;
        }
    }
    l += snprintf(full + l, length - l, "%s", fname);
    if (l >= length) return ENAMETOOLONG;
    return 0;
}

static char *construct_full_name(const char *dir, const char *fname) {
    if (fname[0] == '/')
        dir = "";
    {
        int dirlen = strlen(dir);
        int fnamelen = strlen(fname);
        int len = dirlen + fnamelen + 2;        // One for the / between (which may not be there).  One for the trailing null.
        char *result = toku_malloc(len);
        // printf("%s:%d len(%d)=%d+%d+2\n", __FILE__, __LINE__, len, dirlen, fnamelen);
        if (construct_full_name_in_buf(dir, fname, result, len) != 0) {
            toku_free(result);
            result = NULL;
        }
        return result;
    }
}

static int find_db_file(DB_ENV* dbenv, const char *fname, char** full_name_out) {
    u_int32_t i;
    int r;
    struct stat statbuf;
    char* full_name;
    
    assert(full_name_out);    
    if (dbenv->i->data_dirs!=NULL) {
        assert(dbenv->i->n_data_dirs > 0);
        for (i = 0; i < dbenv->i->n_data_dirs; i++) {
            full_name = construct_full_name(dbenv->i->data_dirs[0], fname);
            if (!full_name) return ENOMEM;
            r = stat(full_name, &statbuf);
            if (r == 0) goto finish;
            else {
                toku_free(full_name);
                r = errno;
                if (r != ENOENT) return r;
            }
        }
        //Did not find it at all.  Return the first data dir.
        full_name = construct_full_name(dbenv->i->data_dirs[0], fname);
        goto finish;
    }
    //Default without data_dirs is the environment directory.
    full_name = construct_full_name(dbenv->i->dir, fname);
    goto finish;

finish:
    if (!full_name) return ENOMEM;
    *full_name_out = full_name;
    return 0;    
}

static int toku_db_lt_panic(DB* db, int r) {
    assert(db && db->i && db->dbenv && db->dbenv->i);
    DB_ENV* env = db->dbenv;
    env->i->is_panicked = 1;
    if (r < 0) toku_ydb_do_error(env, 0, toku_lt_strerror(r));
    else       toku_ydb_do_error(env, r, "Error in locktree.\n");
    return EINVAL;
}

static int toku_txn_add_lt(DB_TXN* txn, toku_lock_tree* lt) {
    assert(txn && lt);
    toku_lth* lth = txn->i->lth;
    assert(lth);

    toku_lock_tree* find = toku_lth_find(lth, lt);
    if (find) {
        assert(find == lt);
        return 0;
    }
    int r = toku_lth_insert(lth, lt);
    return r;
}

static int toku_db_open(DB * db, DB_TXN * txn, const char *fname, const char *dbname, DBTYPE dbtype, u_int32_t flags, int mode) {
    HANDLE_PANICKED_DB(db);
    // Warning.  Should check arguments.  Should check return codes on malloc and open and so forth.
    BOOL need_locktree = (db->dbenv->i->open_flags & DB_INIT_LOCK) &&
                         (db->dbenv->i->open_flags & DB_INIT_TXN);

    int openflags = 0;
    int r;
    if (dbtype!=DB_BTREE && dbtype!=DB_UNKNOWN) return EINVAL;
    int is_db_excl    = flags & DB_EXCL;    flags&=~DB_EXCL;
    int is_db_create  = flags & DB_CREATE;  flags&=~DB_CREATE;
    int is_db_rdonly  = flags & DB_RDONLY;  flags&=~DB_RDONLY;
    int is_db_unknown = dbtype == DB_UNKNOWN;
    if (flags & ~DB_THREAD) return EINVAL; // unknown flags

    if (is_db_excl && !is_db_create) return EINVAL;
    if (dbtype==DB_UNKNOWN && is_db_excl) return EINVAL;

    /* tokudb supports no duplicates and sorted duplicates only */
    unsigned int tflags;
    r = toku_brt_get_flags(db->i->brt, &tflags);
    if (r != 0) 
        return r;
    if ((tflags & TOKU_DB_DUP) && !(tflags & TOKU_DB_DUPSORT))
        return EINVAL;

    if (db_opened(db))
        return EINVAL;              /* It was already open. */
    
    r = find_db_file(db->dbenv, fname, &db->i->full_fname);
    if (r != 0) goto error_cleanup;
    // printf("Full name = %s\n", db->i->full_fname);
    db->i->database_name = toku_strdup(dbname ? dbname : "");
    if (db->i->database_name == 0) {
        r = ENOMEM;
        goto error_cleanup;
    }
    if (is_db_rdonly)
        openflags |= O_RDONLY;
    else
        openflags |= O_RDWR;
    
    {
        struct stat statbuf;
        if (stat(db->i->full_fname, &statbuf) == 0) {
            /* If the database exists at the file level, and we specified no db_name, then complain here. */
            if (dbname == 0 && is_db_create) {
                if (is_db_excl) {
                    r = EEXIST;
                    goto error_cleanup;
                }
		is_db_create = 0; // It's not a create after all, since the file exists.
            }
        } else {
            if (!is_db_create) {
                r = ENOENT;
                goto error_cleanup;
            }
        }
    }
    if (is_db_create) openflags |= O_CREAT;

    db->i->open_flags = flags;
    db->i->open_mode = mode;

    if (need_locktree) {
        r = toku_lt_create(&db->i->lt, db, FALSE,
                           toku_db_lt_panic, db->dbenv->i->max_locks,
                           &db->dbenv->i->num_locks,
                           db->i->brt->compare_fun, db->i->brt->dup_compare,
                           toku_malloc, toku_free, toku_realloc);
        if (r!=0) goto error_cleanup;
        r = toku_lt_set_txn_add_lt_callback(db->i->lt, toku_txn_add_lt);
        assert(r==0);
    }
        
    

    r = toku_brt_open(db->i->brt, db->i->full_fname, fname, dbname,
		      is_db_create, is_db_excl, is_db_unknown,
		      db->dbenv->i->cachetable,
		      txn ? txn->i->tokutxn : NULL_TXN,
		      db);
    if (r != 0)
        goto error_cleanup;

    if (db->i->lt) {
        unsigned int brtflags;
        BOOL dups;
        /* Whether we have dups is only known starting now. */
        toku_brt_get_flags(db->i->brt, &brtflags);
        dups = (brtflags & TOKU_DB_DUPSORT || brtflags & TOKU_DB_DUP);
        r = toku_lt_set_dups(db->i->lt, dups);
        /* toku_lt_set_dups cannot return an error here. */
        assert(r==0);
    }

    return 0;
 
error_cleanup:
    if (db->i->database_name) {
        toku_free(db->i->database_name);
        db->i->database_name = NULL;
    }
    if (db->i->full_fname) {
        toku_free(db->i->full_fname);
        db->i->full_fname = NULL;
    }
    if (db->i->lt) {
        toku_lt_close(db->i->lt);
        db->i->lt = NULL;
    }
    return r;
}

static int toku_db_put_noassociate(DB * db, DB_TXN * txn, DBT * key, DBT * data, u_int32_t flags) {
    int r;

    unsigned int brtflags;
    r = toku_brt_get_flags(db->i->brt, &brtflags); assert(r == 0);

    /* limit the size of key and data */
    unsigned int nodesize;
    r = toku_brt_get_nodesize(db->i->brt, &nodesize); assert(r == 0);
    if (brtflags & TOKU_DB_DUPSORT) {
        unsigned int limit = nodesize / (2*BRT_FANOUT-1);
        if (key->size + data->size >= limit)
            return EINVAL;
    } else {
        unsigned int limit = nodesize / (3*BRT_FANOUT-1);
        if (key->size >= limit || data->size >= limit)
            return toku_ydb_do_error(db->dbenv, EINVAL, "The largest key or data item allowed is %d bytes", limit);
    }

    if (flags == DB_YESOVERWRITE) {
        /* tokudb does insert or replace */
        ;
    } else if (flags == DB_NOOVERWRITE) {
        /* check if the key already exists */
        DBT testfordata;
        r = toku_db_get_noassociate(db, txn, key, toku_init_dbt(&testfordata), 0);
        if (r == 0)
            return DB_KEYEXIST;
        if (r != DB_NOTFOUND) return r;
    } else if (flags != 0) {
        /* no other flags are currently supported */
        return EINVAL;
    } else {
        assert(flags == 0);
        if (brtflags & TOKU_DB_DUPSORT) {
#if TDB_EQ_BDB
            r = toku_db_get_noassociate(db, txn, key, data, DB_GET_BOTH);
            if (r == 0)
                return DB_KEYEXIST;
            if (r != DB_NOTFOUND) return r;
#else
	    return toku_ydb_do_error(db->dbenv, EINVAL, "Tokudb requires that db->put specify DB_YESOVERWRITE or DB_NOOVERWRITE on DB_DUPSORT databases");
#endif
        }
    }
    if (db->i->lt) {
        r = toku_lt_acquire_write_lock(db->i->lt, toku_txn_ancestor(txn),
                                       key, data);
        if (r!=0) return r;
    }
    r = toku_brt_insert(db->i->brt, key, data, txn ? txn->i->tokutxn : 0);
    //printf("%s:%d %d=__toku_db_put(...)\n", __FILE__, __LINE__, r);
    return r;
}

static int do_associated_inserts (DB_TXN *txn, DBT *key, DBT *data, DB *secondary) {
    DBT idx;
    memset(&idx, 0, sizeof(idx));
    int r = secondary->i->associate_callback(secondary, key, data, &idx);
    if (r==DB_DONOTINDEX) { r = 0; goto clean_up; }
    if (r != 0) goto clean_up;
#ifdef DB_DBT_MULTIPLE
    if (idx.flags & DB_DBT_MULTIPLE) {
	return EINVAL; // We aren't ready for this
    }
#endif
    r = toku_db_put_noassociate(secondary, txn, &idx, key, DB_YESOVERWRITE);
    clean_up:
    if (idx.flags & DB_DBT_APPMALLOC) {
        /* This should be free because idx.data is allocated by the user */
        free(idx.data);
    }
    return r;
}

static int toku_db_put(DB * db, DB_TXN * txn, DBT * key, DBT * data, u_int32_t flags) {
    HANDLE_PANICKED_DB(db);
    int r;

    //Cannot put directly into a secondary.
    if (db->i->primary != 0) return EINVAL;

    r = toku_db_put_noassociate(db, txn, key, data, flags);
    if (r!=0) return r;
    // For each secondary add the relevant records.
    assert(db->i->primary==0);
    // Only do it if it is a primary.   This loop would run an unknown number of times if we tried it on a secondary.
    struct list *h;
    for (h=list_head(&db->i->associated); h!=&db->i->associated; h=h->next) {
        struct __toku_db_internal *dbi=list_struct(h, struct __toku_db_internal, associated);
        r=do_associated_inserts(txn, key, data, dbi->db);
        if (r!=0) return r;
    }
    return 0;
}

static int toku_db_remove(DB * db, const char *fname, const char *dbname, u_int32_t flags) {
    HANDLE_PANICKED_DB(db);
    int r;
    int r2;
    char *full_name;

    //TODO: Verify DB* db not yet opened
    if (dbname) {
        //TODO: Verify the target db is not open
        //TODO: Use master database (instead of manual edit) when implemented.

        if ((r = toku_db_open(db, NULL, fname, dbname, DB_BTREE, 0, 0777)) != 0) goto cleanup;
        r = toku_brt_remove_subdb(db->i->brt, dbname, flags);
cleanup:
        r2 = toku_db_close(db, 0);
        return r ? r : r2;
    }
    //TODO: Verify db file not in use. (all dbs in the file must be unused)
    r = find_db_file(db->dbenv, fname, &full_name);
    if (r!=0) return r;
    assert(full_name);
    r2 = toku_db_close(db, 0);
    if (r == 0 && r2 == 0) {
        if (unlink(full_name) != 0) r = errno;
    }
    toku_free(full_name);
    return r ? r : r2;
}

static int toku_db_rename(DB * db, const char *namea, const char *nameb, const char *namec, u_int32_t flags) {
    HANDLE_PANICKED_DB(db);
    if (flags!=0) return EINVAL;
    char afull[PATH_MAX], cfull[PATH_MAX];
    int r;
    assert(nameb == 0);
    r = snprintf(afull, PATH_MAX, "%s%s", db->dbenv->i->dir, namea);
    assert(r < PATH_MAX);
    r = snprintf(cfull, PATH_MAX, "%s%s", db->dbenv->i->dir, namec);
    assert(r < PATH_MAX);
    return rename(afull, cfull);
}

static int toku_db_set_bt_compare(DB * db, int (*bt_compare) (DB *, const DBT *, const DBT *)) {
    HANDLE_PANICKED_DB(db);
    int r = toku_brt_set_bt_compare(db->i->brt, bt_compare);
    return r;
}

static int toku_db_set_dup_compare(DB *db, int (*dup_compare)(DB *, const DBT *, const DBT *)) {
    HANDLE_PANICKED_DB(db);
    int r = toku_brt_set_dup_compare(db->i->brt, dup_compare);
    return r;
}

static int toku_db_set_flags(DB *db, u_int32_t flags) {
    HANDLE_PANICKED_DB(db);

    /* the following matches BDB */
    if (db_opened(db) && flags != 0) return EINVAL;

    u_int32_t tflags;
    int r = toku_brt_get_flags(db->i->brt, &tflags);
    if (r!=0) return r;
    
    if (flags & DB_DUP)
        tflags += TOKU_DB_DUP;
    if (flags & DB_DUPSORT)
        tflags += TOKU_DB_DUPSORT;
    r = toku_brt_set_flags(db->i->brt, tflags);
    return r;
}

static int toku_db_get_flags(DB *db, u_int32_t *pflags) {
    HANDLE_PANICKED_DB(db);
    if (!pflags) return EINVAL;
    u_int32_t tflags;
    u_int32_t flags = 0;
    int r = toku_brt_get_flags(db->i->brt, &tflags);
    if (r!=0) return r;
    if (tflags & TOKU_DB_DUP) {
        tflags &= ~TOKU_DB_DUP;
        flags  |= DB_DUP;
    }
    if (tflags & TOKU_DB_DUPSORT) {
        tflags &= ~TOKU_DB_DUPSORT;
        flags  |= DB_DUPSORT;
    }
    assert(tflags == 0);
    *pflags = flags;
    return 0;
}

static int toku_db_set_pagesize(DB *db, u_int32_t pagesize) {
    HANDLE_PANICKED_DB(db);
    int r = toku_brt_set_nodesize(db->i->brt, pagesize);
    return r;
}

#if 0
static int toku_db_stat(DB * db, void *v, u_int32_t flags) {
    HANDLE_PANICKED_DB(db);
    v=v; flags=flags;
    toku_ydb_barf();
    abort();
}
#endif

static int toku_db_fd(DB *db, int *fdp) {
    HANDLE_PANICKED_DB(db);
    if (!db_opened(db)) return EINVAL;
    return toku_brt_get_fd(db->i->brt, fdp);
}

//TODO: DB_AUTO_COMMIT.
//TODO: Nowait only conditionally?
//TODO: NOSYNC change to SYNC if DB_ENV has something in set_flags
static inline int toku_db_construct_autotxn(DB* db, DB_TXN **txn, BOOL* changed,
                                            BOOL force_auto_commit) {
    assert(db && txn && changed);
    DB_ENV* env = db->dbenv;
    if (*txn || !(env->i->open_flags & DB_INIT_TXN)) {
        *changed = FALSE;
        return 0;
    }
    BOOL nosync = !force_auto_commit && !(env->i->open_flags & DB_AUTO_COMMIT);
    u_int32_t txn_flags = DB_TXN_NOWAIT | (nosync ? DB_TXN_NOSYNC : 0);
    int r = toku_txn_begin(env, NULL, txn, txn_flags);
    if (r!=0) return r;
    *changed = TRUE;
    return 0;
}

static inline int toku_db_destruct_autotxn(DB_TXN *txn, int r, BOOL changed) {
    if (!changed) return r;
    if (r==0) return toku_txn_commit(txn, 0);
    toku_txn_abort(txn);
    return r; 
}

static inline int autotxn_db_associate(DB *primary, DB_TXN *txn, DB *secondary,
                                       int (*callback)(DB *secondary, const DBT *key, const DBT *data, DBT *result), u_int32_t flags) {
    BOOL changed; int r;
    r = toku_db_construct_autotxn(primary, &txn, &changed, FALSE);
    if (r!=0) return r;
    r = toku_db_associate(primary, txn, secondary, callback, flags);
    return toku_db_destruct_autotxn(txn, r, changed);
}

static int locked_db_associate (DB *primary, DB_TXN *txn, DB *secondary,
                                int (*callback)(DB *secondary, const DBT *key, const DBT *data, DBT *result), u_int32_t flags) {
    toku_ydb_lock(); int r = autotxn_db_associate(primary, txn, secondary, callback, flags); toku_ydb_unlock(); return r;
}

static int locked_db_close(DB * db, u_int32_t flags) {
    toku_ydb_lock(); int r = toku_db_close(db, flags); toku_ydb_unlock(); return r;
}

static inline int autotxn_db_cursor(DB *db, DB_TXN *txn, DBC **c, u_int32_t flags) {
    if (!txn && (db->dbenv->i->open_flags & DB_INIT_TXN)) {
        return toku_ydb_do_error(db->dbenv, EINVAL,
              "Cursors in a transaction environment must have transactions.\n");
    }
    return toku_db_cursor(db, txn, c, flags);
}

static int locked_db_cursor(DB *db, DB_TXN *txn, DBC **c, u_int32_t flags) {
    toku_ydb_lock(); int r = autotxn_db_cursor(db, txn, c, flags); toku_ydb_unlock(); return r;
}

static inline int autotxn_db_del(DB* db, DB_TXN* txn, DBT* key,
                                 u_int32_t flags) {
    BOOL changed; int r;
    r = toku_db_construct_autotxn(db, &txn, &changed, FALSE);
    if (r!=0) return r;
    r = toku_db_del(db, txn, key, flags);
    return toku_db_destruct_autotxn(txn, r, changed);
}

static int locked_db_del(DB * db, DB_TXN * txn, DBT * key, u_int32_t flags) {
    toku_ydb_lock(); int r = autotxn_db_del(db, txn, key, flags); toku_ydb_unlock(); return r;
}

static inline int autotxn_db_get(DB* db, DB_TXN* txn, DBT* key, DBT* data,
                                 u_int32_t flags) {
    BOOL changed; int r;
    r = toku_db_construct_autotxn(db, &txn, &changed, FALSE);
    if (r!=0) return r;
    r = toku_db_get(db, txn, key, data, flags);
    return toku_db_destruct_autotxn(txn, r, changed);
}

static int locked_db_get (DB * db, DB_TXN * txn, DBT * key, DBT * data, u_int32_t flags) {
    toku_ydb_lock(); int r = autotxn_db_get(db, txn, key, data, flags); toku_ydb_unlock(); return r;
}

static inline int autotxn_db_pget(DB* db, DB_TXN* txn, DBT* key, DBT* pkey,
                                  DBT* data, u_int32_t flags) {
    BOOL changed; int r;
    r = toku_db_construct_autotxn(db, &txn, &changed, FALSE);
    if (r!=0) return r;
    r = toku_db_pget(db, txn, key, pkey, data, flags);
    return toku_db_destruct_autotxn(txn, r, changed);
}

static int locked_db_pget (DB *db, DB_TXN *txn, DBT *key, DBT *pkey, DBT *data, u_int32_t flags) {
    toku_ydb_lock(); int r = autotxn_db_pget(db, txn, key, pkey, data, flags); toku_ydb_unlock(); return r;
}

static inline int autotxn_db_open(DB* db, DB_TXN* txn, const char *fname, const char *dbname, DBTYPE dbtype, u_int32_t flags, int mode) {
    BOOL changed; int r;
    r = toku_db_construct_autotxn(db, &txn, &changed, flags & DB_AUTO_COMMIT);
    if (r!=0) return r;
    r = toku_db_open(db, txn, fname, dbname, dbtype, flags & ~DB_AUTO_COMMIT, mode);
    return toku_db_destruct_autotxn(txn, r, changed);
}

static int locked_db_open(DB *db, DB_TXN *txn, const char *fname, const char *dbname, DBTYPE dbtype, u_int32_t flags, int mode) {
    toku_ydb_lock(); int r = autotxn_db_open(db, txn, fname, dbname, dbtype, flags, mode); toku_ydb_unlock(); return r;
}

static inline int autotxn_db_put(DB* db, DB_TXN* txn, DBT* key, DBT* data,
                                 u_int32_t flags) {
    BOOL changed; int r;
    r = toku_db_construct_autotxn(db, &txn, &changed, FALSE);
    if (r!=0) return r;
    r = toku_db_put(db, txn, key, data, flags);
    return toku_db_destruct_autotxn(txn, r, changed);
}

static int locked_db_put(DB * db, DB_TXN * txn, DBT * key, DBT * data, u_int32_t flags) {
    toku_ydb_lock(); int r = autotxn_db_put(db, txn, key, data, flags); toku_ydb_unlock(); return r;
}

static int locked_db_remove(DB * db, const char *fname, const char *dbname, u_int32_t flags) {
    toku_ydb_lock(); int r = toku_db_remove(db, fname, dbname, flags); toku_ydb_unlock(); return r;
}

static int locked_db_rename(DB * db, const char *namea, const char *nameb, const char *namec, u_int32_t flags) {
    toku_ydb_lock(); int r = toku_db_rename(db, namea, nameb, namec, flags); toku_ydb_unlock(); return r;
}

static int locked_db_set_bt_compare(DB * db, int (*bt_compare) (DB *, const DBT *, const DBT *)) {
    toku_ydb_lock(); int r = toku_db_set_bt_compare(db, bt_compare); toku_ydb_unlock(); return r;
}

static int locked_db_set_dup_compare(DB * db, int (*dup_compare) (DB *, const DBT *, const DBT *)) {
    toku_ydb_lock(); int r = toku_db_set_dup_compare(db, dup_compare); toku_ydb_unlock(); return r;
}

static void locked_db_set_errfile (DB *db, FILE *errfile) {
    db->dbenv->set_errfile(db->dbenv, errfile);
}

static int locked_db_set_flags(DB *db, u_int32_t flags) {
    toku_ydb_lock(); int r = toku_db_set_flags(db, flags); toku_ydb_unlock(); return r;
}

static int locked_db_get_flags(DB *db, u_int32_t *flags) {
    toku_ydb_lock(); int r = toku_db_get_flags(db, flags); toku_ydb_unlock(); return r;
}

static int locked_db_set_pagesize(DB *db, u_int32_t pagesize) {
    toku_ydb_lock(); int r = toku_db_set_pagesize(db, pagesize); toku_ydb_unlock(); return r;
}

static int locked_db_fd(DB *db, int *fdp) {
    toku_ydb_lock(); int r = toku_db_fd(db, fdp); toku_ydb_unlock(); return r;
}

static int toku_db_create(DB ** db, DB_ENV * env, u_int32_t flags) {
    int r;

    if (flags) return EINVAL;

    /* if the env already exists then add a ref to it
       otherwise create one */
    if (env) {
        if (!env_opened(env))
            return EINVAL;
        env_add_ref(env);
    } else {
        r = toku_env_create(&env, 0);
        if (r != 0)
            return r;
        r = toku_env_open(env, ".", DB_PRIVATE + DB_INIT_MPOOL, 0);
        if (r != 0) {
            toku_env_close(env, 0);
            return r;
        }
        assert(env_opened(env));
    }
    
    DB *MALLOC(result);
    if (result == 0) {
        env_unref(env);
        return ENOMEM;
    }
    memset(result, 0, sizeof *result);
    result->dbenv = env;
    result->associate = locked_db_associate;
    result->close = locked_db_close;
    result->cursor = locked_db_cursor;
    result->del = locked_db_del;
    result->get = locked_db_get;
    //    result->key_range = locked_db_key_range;
    result->open = locked_db_open;
    result->pget = locked_db_pget;
    result->put = locked_db_put;
    result->remove = locked_db_remove;
    result->rename = locked_db_rename;
    result->set_bt_compare = locked_db_set_bt_compare;
    result->set_dup_compare = locked_db_set_dup_compare;
    result->set_errfile = locked_db_set_errfile;
    result->set_pagesize = locked_db_set_pagesize;
    result->set_flags = locked_db_set_flags;
    result->get_flags = locked_db_get_flags;
    //    result->stat = locked_db_stat;
    result->fd = locked_db_fd;
    MALLOC(result->i);
    if (result->i == 0) {
        toku_free(result);
        env_unref(env);
        return ENOMEM;
    }
    memset(result->i, 0, sizeof *result->i);
    result->i->db = result;
    result->i->freed = 0;
    result->i->header = 0;
    result->i->database_number = 0;
    result->i->full_fname = 0;
    result->i->database_name = 0;
    result->i->open_flags = 0;
    result->i->open_mode = 0;
    result->i->brt = 0;
    list_init(&result->i->associated);
    result->i->primary = 0;
    result->i->associate_callback = 0;
    r = toku_brt_create(&result->i->brt);
    if (r != 0) {
        toku_free(result->i);
        toku_free(result);
        env_unref(env);
        return ENOMEM;
    }
    ydb_add_ref();
    *db = result;
    return 0;
}

int db_create(DB ** db, DB_ENV * env, u_int32_t flags) {
    toku_ydb_lock(); int r = toku_db_create(db, env, flags); toku_ydb_unlock(); return r;
}

/* need db_strerror_r for multiple threads */

char *db_strerror(int error) {
    char *errorstr;
    if (error >= 0) {
        errorstr = strerror(error);
        if (errorstr)
            return errorstr;
    }
    
    if (error==DB_BADFORMAT) {
	return "Database Bad Format (probably a corrupted database)";
    }

    static char unknown_result[100];    // Race condition if two threads call this at the same time. However even in a bad case, it should be some sort of null-terminated string.
    errorstr = unknown_result;
    snprintf(errorstr, sizeof unknown_result, "Unknown error code: %d", error);
    return errorstr;
}

const char *db_version(int *major, int *minor, int *patch) {
    if (major)
        *major = DB_VERSION_MAJOR;
    if (minor)
        *minor = DB_VERSION_MINOR;
    if (patch)
        *patch = DB_VERSION_PATCH;
    return DB_VERSION_STRING;
}
