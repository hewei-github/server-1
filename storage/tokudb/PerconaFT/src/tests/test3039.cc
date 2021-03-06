/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
/*======
This file is part of PerconaFT.


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.

----------------------------------------

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License, version 3,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.
======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

/* This is a performance test.  Releasing lock during I/O should mean that given two threads doing queries,
 * and one of them is in-memory and one of them is out of memory, then the in-memory one should not be slowed down by the out-of-memory one.
 * 
 * Step 1: Create a dictionary that doesn't fit in main memory.  Do it fast (sequential insertions).
 * Step 2: Measure performance of in-memory requests.
 * Step 3: Add a thread that does requests in parallel.
 */

#include "test.h"
#include <string.h>
#include <toku_time.h>
#include <toku_pthread.h>
#include <portability/toku_atomic.h>

static const int envflags = DB_INIT_MPOOL|DB_CREATE|DB_THREAD |DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_TXN|DB_PRIVATE;

#define ROWSIZE 100
static const char dbname[] = "data.db";
static unsigned long long n_rows;

static DB_ENV *env = NULL;
static DB *db;

// BDB cannot handle big transactions  by default (runs out of locks).
#define N_PER_XACTION 10000

static void create_db (uint64_t N) {
    n_rows = N;
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);
    { int r = db_env_create(&env, 0);                                          CKERR(r); }
    env->set_errfile(env, stderr);
    env->set_redzone(env, 0);
    { int r = env->set_cachesize(env, 0, 400*4096, 1);                        CKERR(r); }
    { int r = env->open(env, TOKU_TEST_FILENAME, envflags, S_IRWXU+S_IRWXG+S_IRWXO);       CKERR(r); }
    DB_TXN *txn;
    { int r = env->txn_begin(env, NULL, &txn, 0);                              CKERR(r); }
    { int r = db_create(&db, env, 0);                                          CKERR(r); }
    { int r = db->set_pagesize(db, 4096);                                      CKERR(r); }
    { int r = db->open(db, txn, dbname, NULL, DB_BTREE, DB_CREATE, 0666);      CKERR(r); }
    { int r = txn->commit(txn, DB_TXN_NOSYNC);                                 CKERR(r); }

    { int r = env->txn_begin(env, NULL, &txn, 0);                              CKERR(r); }
    uint64_t n_since_commit = 0;
    for (unsigned long long i=0; i<N; i++) {
	if (n_since_commit++ > N_PER_XACTION) {
	    { int r = txn->commit(txn, DB_TXN_NOSYNC);                         CKERR(r); }
	    { int r = env->txn_begin(env, NULL, &txn, 0);                      CKERR(r); }
	}
	char key[20];
	char data[200];
	snprintf(key,  sizeof(key),  "%016llx", i);
	snprintf(data, sizeof(data), "%08lx%08lx%66s", random(), random()%16, "");
	DBT keyd, datad;
	{
	    int r = db->put(db, txn, dbt_init(&keyd, key, strlen(key)+1), dbt_init(&datad, data, strlen(data)+1), 0);
	    CKERR(r);
	}
    }
    //printf("n_rows=%lld\n", n_rows);
    { int r = txn->commit(txn, DB_TXN_NOSYNC);                                 CKERR(r); }
}

struct reader_thread_state {
    /* output */
    double             elapsed_time;
    unsigned long long n_did_read;

    /* input */
    signed long long n_to_read;  // Negative if we just run forever
    int              do_local;

    /* communicate to the thread while running */
    volatile int finish;

};

static
void* reader_thread (void *arg)
// Return the time to read
{
    struct timeval start_time, end_time;
    gettimeofday(&start_time, 0);

    DB_TXN *txn;
    struct reader_thread_state *rs = (struct reader_thread_state *)arg;
    
    { int r = env->txn_begin(env, NULL, &txn, 0);                              CKERR(r); }
    char key[20];
    char data [200];
    DBT keyd, datad;
    keyd.data = key;
    keyd.size = 0;
    keyd.ulen = sizeof(key);
    keyd.flags = DB_DBT_USERMEM;
    datad.data = data;
    datad.size = 0;
    datad.ulen = sizeof(data);
    datad.flags = DB_DBT_USERMEM;

#define N_DISTINCT 16
    unsigned long long vals[N_DISTINCT];
    if (rs->do_local) {
	for (int i=0; i<N_DISTINCT; i++) {
	    vals[i] = random()%n_rows;
	}
    }
    
    uint64_t n_since_commit = 0;
    long long n_read_so_far = 0;
    while ((!rs->finish) && ((rs->n_to_read < 0) || (n_read_so_far < rs->n_to_read))) {

	if (n_since_commit++ > N_PER_XACTION) {
	    { int r = txn->commit(txn, DB_TXN_NOSYNC);                         CKERR(r); }
	    { int r = env->txn_begin(env, NULL, &txn, 0);                      CKERR(r); }
	    n_since_commit = 0;
	}
	long long value;
	if (rs->do_local) {
	    long which = random()%N_DISTINCT;
	    value = vals[which];
	    //printf("value=%lld\n", value);
	} else {
	    value = random()%n_rows;
	}
	snprintf(key,  sizeof(key),  "%016llx", value);
	keyd.size = strlen(key)+1;
	int r = db->get(db, txn, &keyd, &datad, 0);
#ifdef BLOCKING_ROW_LOCKS_READS_NOT_SHARED
        invariant(r == 0 || r == DB_LOCK_NOTGRANTED || r == DB_LOCK_DEADLOCK);
#else
	CKERR(r);
#endif
	rs->n_did_read++;
	n_read_so_far ++;
    }
    { int r = txn->commit(txn, DB_TXN_NOSYNC);                                 CKERR(r); }
    
    gettimeofday(&end_time, 0);
    rs->elapsed_time = toku_tdiff(&end_time, &start_time);
    return NULL;
}

static
void do_threads (unsigned long long N, int do_nonlocal) {
    toku_pthread_t ths[2];
    struct reader_thread_state rstates[2] = {{.elapsed_time = 0.0,
                                              .n_did_read = 0,
                                              .n_to_read = (long long signed)N,
                                              .do_local = 1,
                                              .finish = 0},
                                             {.elapsed_time = 0.0,
                                              .n_did_read = 0,
                                              .n_to_read = -1,
                                              .do_local = 0,
                                              .finish = 0}};
    int n_to_create = do_nonlocal ? 2 : 1;
    for (int i = 0; i < n_to_create; i++) {
        int r = toku_pthread_create(toku_uninstrumented,
                                    &ths[i],
                                    nullptr,
                                    reader_thread,
                                    static_cast<void *>(&rstates[i]));
        CKERR(r);
    }
    for (int i = 0; i < n_to_create; i++) {
        void *retval;
        int r = toku_pthread_join(ths[i], &retval);
        CKERR(r);
        assert(retval == 0);
        if (verbose) {
            printf("%9s thread time = %8.2fs on %9lld reads (%.3f us/read)\n",
                   (i == 0 ? "local" : "nonlocal"),
                   rstates[i].elapsed_time,
                   rstates[i].n_did_read,
                   rstates[i].elapsed_time / rstates[i].n_did_read * 1e6);
        }
        rstates[1].finish = 1;
    }
    if (verbose && do_nonlocal) {
	printf("total                                %9lld reads (%.3f us/read)\n",
	       rstates[0].n_did_read + rstates[1].n_did_read,
	       (rstates[0].elapsed_time)/(rstates[0].n_did_read + rstates[1].n_did_read) * 1e6);
    }
}

static volatile unsigned long long n_preads;

static ssize_t my_pread (int fd, void *buf, size_t count, off_t offset) {
    (void) toku_sync_fetch_and_add(&n_preads, 1);
    usleep(1000); // sleep for a millisecond
    return pread(fd, buf, count, offset);
}

unsigned long N_default = 100000;
unsigned long N;

static void my_parse_args (int argc, char * const argv[]) {
    const char *progname = argv[0];
    argc--; argv++;
    verbose = 0;
    N = N_default;
    while (argc>0) {
	if (strcmp(argv[0],"-v")==0) {
	    verbose++;
	} else if (strcmp(argv[0],"-q")==0) {
	    if (verbose>0) verbose--;
	} else if (strcmp(argv[0],"-n")==0) {
	    argc--; argv++;
	    if (argc==0) goto usage;
	    errno = 0; 
	    char *end;
	    N = strtol(argv[0], &end, 10);
	    if (errno!=0 || *end!=0) goto usage;
	} else {
	usage:
	    fprintf(stderr, "Usage:\n %s [-v] [-q] [-n <rowcount> (default %ld)]\n", progname, N_default);
	    fprintf(stderr, "  -n 10000     is probably good for valgrind.\n");
	    exit(1);
	}
	argc--; argv++;
    }

}

int test_main (int argc, char * const argv[])  {
    my_parse_args(argc, argv);

    unsigned long long M = N*10;

    db_env_set_func_pread(my_pread);

    create_db (N);
    if (verbose) printf("%lld preads\n", n_preads);
    do_threads (M, 0);
    if (verbose) printf("%lld preads\n", n_preads);
    do_threads (M, 0);
    if (verbose) printf("%lld preads\n", n_preads);
    do_threads (M, 1);
    if (verbose) printf("%lld preads\n", n_preads);
    { int r = db->close(db, 0);                                                CKERR(r); }
    { int r = env->close(env, 0);                                              CKERR(r); }
    if (verbose) printf("%lld preads\n", n_preads);
    return 0;
}

