/********************************* Includes ***********************************/
#include    "goahead.h"
#include    "userdb.h"

#include    <dlfcn.h>
#include    <errno.h>
#include    <sys/stat.h>
#include    <sys/types.h>

/********************************* Defines ************************************/

#define USERDB_OK       0
#define USERDB_ERROR   -1
#define USERDB_DENY     0
#define USERDB_ALLOW    1

#define SQLITE_OK       0
#define SQLITE_ROW      100
#define SQLITE_DONE     101
#define SQLITE_TRANSIENT ((void (*)(void *)) -1)

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;

typedef int (*sqlite3_open_fn)(const char*, sqlite3**);
typedef int (*sqlite3_close_fn)(sqlite3*);
typedef int (*sqlite3_exec_fn)(sqlite3*, const char*, int (*)(void*,int,char**,char**), void*, char**);
typedef int (*sqlite3_prepare_v2_fn)(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
typedef int (*sqlite3_bind_text_fn)(sqlite3_stmt*, int, const char*, int, void (*)(void*));
typedef int (*sqlite3_step_fn)(sqlite3_stmt*);
typedef const unsigned char *(*sqlite3_column_text_fn)(sqlite3_stmt*, int);
typedef int (*sqlite3_column_int_fn)(sqlite3_stmt*, int);
typedef int (*sqlite3_finalize_fn)(sqlite3_stmt*);
typedef const char *(*sqlite3_errmsg_fn)(sqlite3*);
typedef void (*sqlite3_free_fn)(void*);

typedef struct SqliteApi {
    void                    *handle;
    sqlite3_open_fn          open;
    sqlite3_close_fn         close;
    sqlite3_exec_fn          exec;
    sqlite3_prepare_v2_fn    prepare_v2;
    sqlite3_bind_text_fn     bind_text;
    sqlite3_step_fn          step;
    sqlite3_column_text_fn   column_text;
    sqlite3_column_int_fn    column_int;
    sqlite3_finalize_fn      finalize;
    sqlite3_errmsg_fn        errmsg;
    sqlite3_free_fn          free;
} SqliteApi;

/*********************************** Locals ***********************************/

static SqliteApi sqlite;
static sqlite3 *userdb;

/*************************** Forward Declarations *****************************/

static int loadSqlite(void);
static int ensureParentDir(const char *path);
static int execSql(const char *sql);
static int seedDefaultUser(void);
static int getUserCount(void);

/*********************************** Code *************************************/

int userdb_init(const char *db_path)
{
    if (userdb) {
        return USERDB_OK;
    }
    if (!db_path || !*db_path) {
        error("User database path is empty");
        return USERDB_ERROR;
    }
    if (loadSqlite() != USERDB_OK) {
        return USERDB_ERROR;
    }
    if (ensureParentDir(db_path) != USERDB_OK) {
        return USERDB_ERROR;
    }
    if (sqlite.open(db_path, &userdb) != SQLITE_OK) {
        error("Cannot open user database %s: %s", db_path, userdb ? sqlite.errmsg(userdb) : "unknown");
        if (userdb) {
            sqlite.close(userdb);
            userdb = NULL;
        }
        return USERDB_ERROR;
    }
    if (execSql(
            "CREATE TABLE IF NOT EXISTS users ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "username TEXT NOT NULL UNIQUE,"
            "password_hash TEXT NOT NULL,"
            "salt TEXT DEFAULT '',"
            "role TEXT NOT NULL DEFAULT 'admin',"
            "enabled INTEGER NOT NULL DEFAULT 1,"
            "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            "last_login_at TEXT"
            ");") != USERDB_OK) {
        return USERDB_ERROR;
    }
    if (seedDefaultUser() != USERDB_OK) {
        return USERDB_ERROR;
    }
    logmsg(2, "用户数据库初始化完成: %s", db_path);
    return USERDB_OK;
}

int userdb_verify_login(const char *username, const char *password)
{
    sqlite3_stmt    *stmt;
    const char      *hash;
    int             enabled;
    int             rc;
    int             result;

    if (!userdb || !username || !password || !*username || !*password) {
        return USERDB_DENY;
    }
    stmt = NULL;
    rc = sqlite.prepare_v2(userdb,
        "SELECT password_hash, enabled FROM users WHERE username = ? LIMIT 1;",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        error("Cannot prepare login query: %s", sqlite.errmsg(userdb));
        return USERDB_ERROR;
    }
    sqlite.bind_text(stmt, 1, username, -1, SQLITE_TRANSIENT);
    rc = sqlite.step(stmt);
    result = USERDB_DENY;
    if (rc == SQLITE_ROW) {
        hash = (const char*) sqlite.column_text(stmt, 0);
        enabled = sqlite.column_int(stmt, 1);
        if (enabled && hash && websCheckPassword(password, hash)) {
            result = USERDB_ALLOW;
        }
    } else if (rc != SQLITE_DONE) {
        error("Cannot read login user: %s", sqlite.errmsg(userdb));
        result = USERDB_ERROR;
    }
    sqlite.finalize(stmt);

    if (result == USERDB_ALLOW) {
        stmt = NULL;
        rc = sqlite.prepare_v2(userdb,
            "UPDATE users SET last_login_at = CURRENT_TIMESTAMP, updated_at = CURRENT_TIMESTAMP WHERE username = ?;",
            -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite.bind_text(stmt, 1, username, -1, SQLITE_TRANSIENT);
            sqlite.step(stmt);
            sqlite.finalize(stmt);
        }
    }
    return result;
}

int userdb_register_user(const char *username, const char *password, char *message, int message_len)
{
    sqlite3_stmt    *stmt;
    char            *hash;
    int             rc;
    int             result;

    if (message && message_len > 0) {
        *message = '\0';
    }
    if (!userdb) {
        snprintf(message, message_len, "user database not ready");
        return USERDB_ERROR;
    }
    if (!username || !password || strlen(username) < 3 || strlen(username) > 32) {
        snprintf(message, message_len, "username length must be 3-32");
        return USERDB_DENY;
    }
    if (strspn(username, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") != strlen(username)) {
        snprintf(message, message_len, "username contains invalid characters");
        return USERDB_DENY;
    }
    if (strlen(password) < 6 || strlen(password) > 64) {
        snprintf(message, message_len, "password length must be 6-64");
        return USERDB_DENY;
    }

    hash = websMakePassword(password, 16, 128);
    if (!hash) {
        snprintf(message, message_len, "cannot create password hash");
        return USERDB_ERROR;
    }
    stmt = NULL;
    rc = sqlite.prepare_v2(userdb,
        "INSERT INTO users (username, password_hash, role, enabled) VALUES (?, ?, 'user', 1);",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        snprintf(message, message_len, "cannot prepare register");
        wfree(hash);
        return USERDB_ERROR;
    }
    sqlite.bind_text(stmt, 1, username, -1, SQLITE_TRANSIENT);
    sqlite.bind_text(stmt, 2, hash, -1, SQLITE_TRANSIENT);
    rc = sqlite.step(stmt);
    sqlite.finalize(stmt);
    wfree(hash);

    if (rc == SQLITE_DONE) {
        snprintf(message, message_len, "register success");
        result = USERDB_ALLOW;
    } else {
        snprintf(message, message_len, "username already exists");
        result = USERDB_DENY;
    }
    return result;
}

static int loadSqlite(void)
{
    if (sqlite.handle) {
        return USERDB_OK;
    }
    sqlite.handle = dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!sqlite.handle) {
        error("Cannot load libsqlite3.so.0: %s", dlerror());
        return USERDB_ERROR;
    }
#define LOAD_SQLITE_SYMBOL(name) do { \
        sqlite.name = (sqlite3_##name##_fn) dlsym(sqlite.handle, "sqlite3_" #name); \
        if (!sqlite.name) { \
            error("Cannot load sqlite3_%s", #name); \
            return USERDB_ERROR; \
        } \
    } while (0)
    LOAD_SQLITE_SYMBOL(open);
    LOAD_SQLITE_SYMBOL(close);
    LOAD_SQLITE_SYMBOL(exec);
    LOAD_SQLITE_SYMBOL(prepare_v2);
    LOAD_SQLITE_SYMBOL(bind_text);
    LOAD_SQLITE_SYMBOL(step);
    LOAD_SQLITE_SYMBOL(column_text);
    LOAD_SQLITE_SYMBOL(column_int);
    LOAD_SQLITE_SYMBOL(finalize);
    LOAD_SQLITE_SYMBOL(errmsg);
    LOAD_SQLITE_SYMBOL(free);
#undef LOAD_SQLITE_SYMBOL
    return USERDB_OK;
}

static int ensureParentDir(const char *path)
{
    char    dir[ME_GOAHEAD_LIMIT_FILENAME];
    char    *slash;

    snprintf(dir, sizeof(dir), "%s", path);
    slash = strrchr(dir, '/');
    if (!slash) {
        return USERDB_OK;
    }
    *slash = '\0';
    if (*dir == '\0') {
        return USERDB_OK;
    }
    if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
        error("Cannot create user database directory %s: %s", dir, strerror(errno));
        return USERDB_ERROR;
    }
    return USERDB_OK;
}

static int execSql(const char *sql)
{
    char    *err;

    err = NULL;
    if (sqlite.exec(userdb, sql, NULL, NULL, &err) != SQLITE_OK) {
        error("SQLite exec failed: %s", err ? err : sqlite.errmsg(userdb));
        if (err) {
            sqlite.free(err);
        }
        return USERDB_ERROR;
    }
    return USERDB_OK;
}

static int seedDefaultUser(void)
{
    sqlite3_stmt    *stmt;
    char            *hash;
    int             count;
    int             rc;

    count = getUserCount();
    if (count < 0) {
        return USERDB_ERROR;
    }
    if (count > 0) {
        return USERDB_OK;
    }
    hash = websMakePassword("root", 16, 128);
    if (!hash) {
        error("Cannot create default user password hash");
        return USERDB_ERROR;
    }
    stmt = NULL;
    rc = sqlite.prepare_v2(userdb,
        "INSERT INTO users (username, password_hash, role, enabled) VALUES (?, ?, 'admin', 1);",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        error("Cannot prepare default user insert: %s", sqlite.errmsg(userdb));
        wfree(hash);
        return USERDB_ERROR;
    }
    sqlite.bind_text(stmt, 1, "root", -1, SQLITE_TRANSIENT);
    sqlite.bind_text(stmt, 2, hash, -1, SQLITE_TRANSIENT);
    rc = sqlite.step(stmt);
    sqlite.finalize(stmt);
    wfree(hash);
    if (rc != SQLITE_DONE) {
        error("Cannot insert default user: %s", sqlite.errmsg(userdb));
        return USERDB_ERROR;
    }
    logmsg(2, "用户数据库已创建默认账号 root/root");
    return USERDB_OK;
}

static int getUserCount(void)
{
    sqlite3_stmt    *stmt;
    int             count;
    int             rc;

    stmt = NULL;
    rc = sqlite.prepare_v2(userdb, "SELECT COUNT(*) FROM users;", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        error("Cannot prepare user count query: %s", sqlite.errmsg(userdb));
        return -1;
    }
    count = -1;
    rc = sqlite.step(stmt);
    if (rc == SQLITE_ROW) {
        count = sqlite.column_int(stmt, 0);
    } else {
        error("Cannot read user count: %s", sqlite.errmsg(userdb));
    }
    sqlite.finalize(stmt);
    return count;
}
