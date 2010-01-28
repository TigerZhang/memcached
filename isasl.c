#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <stdint.h>
#include <pthread.h>
#include <stdbool.h>
#include <sysexits.h>
#include <sys/stat.h>

#include "hash.h"
#include "isasl.h"
#include "memcached.h"

static struct stat prev_stat = { 0 };

static pthread_mutex_t uhash_lock = PTHREAD_MUTEX_INITIALIZER;

static user_db_entry_t **user_ht;
static const int n_uht_buckets = 12289;

static void kill_whitey(char *s) {
    for(int i = strlen(s) - 1; i > 0 && isspace(s[i]); i--) {
        s[i] = '\0';
    }
}

static int u_hash_key(const char *u)
{
    uint32_t h = hash(u, strlen(u), 0) % n_uht_buckets;
    assert(h < n_uht_buckets);
    return h;
}

static char *find_pw(const char *u, char **cfg)
{
    assert(u);
    assert(user_ht);

    int h = u_hash_key(u);

    user_db_entry_t *e = user_ht[h];
    while (e && strcmp(e->username, u) != 0) {
        e = e->next;
    }

    if (e != NULL) {
        *cfg = e->config;
        return e->password;
    } else {
        return NULL;
    }
}

static void store_pw(user_db_entry_t **ht, const char *u, const char *p, const char *cfg)
{
    assert(ht);
    assert(u);
    assert(p);
    user_db_entry_t *e = calloc(1, sizeof(user_db_entry_t));
    assert(e);
    e->username = strdup(u);
    assert(e->username);
    e->password = strdup(p);
    assert(e->password);
    e->config = cfg ? strdup(cfg) : NULL;
    assert(!cfg || e->config);

    int h = u_hash_key(u);

    e->next = ht[h];
    ht[h] = e;
}

static void free_user_ht(void)
{
    if (user_ht) {
        for (int i = 0; i < n_uht_buckets; i++) {
            while (user_ht[i]) {
                user_db_entry_t *e = user_ht[i];
                user_db_entry_t *n = e->next;
                free(e->username);
                free(e->password);
                free(e->config);
                free(e);
                user_ht[i] = n;
            }
        }
        free(user_ht);
        user_ht = NULL;
    }
}

static const char *get_isasl_filename(void)
{
    return getenv("ISASL_PWFILE");
}

static int load_user_db(void)
{
    const char *filename = get_isasl_filename();
    if (!filename) {
        fprintf(stderr, "No ISASL_PWFILE defined.\n");
        return SASL_FAIL;
    }

    FILE *sfile = fopen(filename, "r");
    if (!sfile) {
        perror(filename);
        return SASL_FAIL;
    }

    user_db_entry_t **new_ut = calloc(n_uht_buckets,
                                      sizeof(user_db_entry_t*));
    if (!new_ut) {
        fclose(sfile);
        return SASL_NOMEM;
    }

    // File may have comment lines that must being with '#'.
    // Otherwise, lines should be newline terminated, and look like...
    //   <NAME><whitespace><PASSWORD><optional_whitespace>
    // or...
    //   <NAME><optional_whitespace>
    // The last signifies an empty PASSWORD string.
    //
    char up[128];
    while (fgets(up, sizeof(up), sfile)) {
        if (up[0] != '#') {
            char *uname = up, *p = up, *cfg = NULL;
            kill_whitey(up);
            while (*p && !isspace(p[0])) {
                p++;
            }
            if (p[0] != '\0') {
                p[0] = '\0';
                p++;
                while (*p && isspace(*p)) {
                    p++;
                }
                cfg = p;
                // Skip the password
                while (*cfg && !isspace(cfg[0])) {
                    cfg++;
                }
                if (cfg[0] != '\0') {
                    cfg[0] = '\0';
                    cfg++;
                    // Skip whitespace
                    while (*cfg && isspace(p[0])) {
                        cfg++;
                    }
                }
            }
            if (strlen(uname) > 0) {
                store_pw(new_ut, uname, p, cfg);
            }
        }
    }

    fclose(sfile);

    if (settings.verbose) {
        fprintf(stderr, "Loaded isasl db from %s\n", filename);
    }

    pthread_mutex_lock(&uhash_lock);
    free_user_ht();
    user_ht = new_ut;
    pthread_mutex_unlock(&uhash_lock);

    return SASL_OK;
}

void sasl_dispose(sasl_conn_t **pconn)
{
    free((*pconn)->username);
    free((*pconn)->config);
    free(*pconn);
    *pconn = NULL;
}

static bool isasl_is_fresh(void)
{
    bool rv = false;
    struct stat st;

    if (stat(get_isasl_filename(), &st) < 0) {
        perror(get_isasl_filename());
    } else {
        rv = prev_stat.st_mtime == st.st_mtime;
        prev_stat = st;
    }
    return rv;
}

static void* check_isasl_db_thread(void* arg)
{
    uint32_t sleep_time = *(int*)arg;
    if (settings.verbose > 1) {
        fprintf(stderr, "isasl checking DB every %ds\n", sleep_time);
    }
    for(;;) {
        sleep(sleep_time);
        if (!isasl_is_fresh()) {
            load_user_db();
        }
    }
    /* NOTREACHED */
    return NULL;
}

int sasl_server_init(const sasl_callback_t *callbacks,
                     const char *appname)
{
    int rv = load_user_db();
    if (rv == SASL_OK) {
        static pthread_t t;
        static uint32_t sleep_time;
        const char *sleep_time_str = getenv("ISASL_DB_CHECK_TIME");
        if (! (sleep_time_str && safe_strtoul(sleep_time_str, &sleep_time))) {
            // If we can't find a more frequent sleep time, set it to 60s.
            sleep_time = 60;
        }
        if(pthread_create(&t, NULL, check_isasl_db_thread, &sleep_time) != 0) {
            perror("couldn't create isasl db update thread.");
            exit(EX_OSERR);
        }
    }
    return rv;
}

int sasl_server_new(const char *service,
                    const char *serverFQDN,
                    const char *user_realm,
                    const char *iplocalport,
                    const char *ipremoteport,
                    const sasl_callback_t *callbacks,
                    unsigned flags,
                    sasl_conn_t **pconn)
{
    *pconn = calloc(1, sizeof(sasl_conn_t));
    return *pconn ? SASL_OK : SASL_NOMEM;
}

int sasl_listmech(sasl_conn_t *conn,
                  const char *user,
                  const char *prefix,
                  const char *sep,
                  const char *suffix,
                  const char **result,
                  unsigned *plen,
                  int *pcount)
{
    // We use this in a very specific way in the codebase.  If that ever
    // changes, detect it quickly.
    assert(strcmp(prefix, "") == 0);
    assert(strcmp(sep, " ") == 0);
    assert(strcmp(suffix, "") == 0);

    *result = "PLAIN";
    *plen = strlen(*result);
    return SASL_OK;
}

static bool check_up(const char *username, const char *password, char **cfg)
{
    pthread_mutex_lock(&uhash_lock);
    char *pw = find_pw(username, cfg);
    bool rv = pw && (strcmp(password, pw) == 0);
    pthread_mutex_unlock(&uhash_lock);
    return rv;
}

int sasl_server_start(sasl_conn_t *conn,
                      const char *mech,
                      const char *clientin,
                      unsigned clientinlen,
                      const char **serverout,
                      unsigned *serveroutlen)
{
    int rv = SASL_FAIL;
    *serverout = "";
    *serveroutlen = 0;

    if(strcmp(mech, "PLAIN") == 0) {
        // 128 is an arbitrary ``large enough'' number.
        // The clientin string looks like "\0username\0password"
        if (clientinlen > 2 && clientinlen < 128 && clientin[0] == '\0') {
            const char *username = clientin + 1;
            char password[128];
            char *cfg;
            int pwlen = clientinlen - 2 - strlen(username);
            if (pwlen > 0) {
                password[pwlen] = '\0';
                memcpy(password, clientin + 2 + strlen(username), pwlen);

                if (check_up(username, password, &cfg)) {
                    if (conn->username) {
                        free(conn->username);
                        conn->username = NULL;
                    }
                    if (conn->config) {
                        free(conn->config);
                        conn->config = NULL;
                    }
                    conn->username = strdup(username);
                    assert(conn->username);
                    conn->config = strdup(cfg);
                    assert(conn->config);
                    rv = SASL_OK;
                }
            }
        }
    }

    return rv;
}

int sasl_server_step(sasl_conn_t *conn,
                     const char *clientin,
                     unsigned clientinlen,
                     const char **serverout,
                     unsigned *serveroutlen)
{
    // This is only useful when the above returns SASL_CONTINUE.  In this
    // implementation, only PLAIN is supported, so it never will.
    return SASL_FAIL;
}

int sasl_getprop(sasl_conn_t *conn, int propnum,
                 const void **pvalue)
{
    switch (propnum) {
        case SASL_USERNAME:
            *pvalue = conn->username;
            break;
        case ISASL_CONFIG:
            *pvalue = conn->config;
            break;
        default:
            return SASL_BADPARAM;
    }

    return SASL_OK;
}
