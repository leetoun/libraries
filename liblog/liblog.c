/*****************************************************************************
 * Copyright (C) 2014-2015
 * file:    liblog.c
 * author:  gozfree <gozfree@163.com>
 * created: 2015-04-20 01:08
 * updated: 2015-07-11 16:09
 *****************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <fcntl.h>
#include <syslog.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <linux/unistd.h>

#include "liblog.h"
#include "color.h"

#define LOG_IOVEC_MAX       (10)
#define FILENAME_LEN        (256)
#define FILESIZE_LEN        (10*1024*1024UL)
#define LOG_BUF_SIZE        (1024)
#define LOG_TIME_SIZE       (32)
#define LOG_LEVEL_SIZE      (32)
#define LOG_TAG_SIZE        (32)
#define LOG_PNAME_SIZE      (32)
#define LOG_TEXT_SIZE       (256)
#define PROC_NAME_LEN	    (512)
#define LOG_LEVEL_DEFAULT   LOG_INFO
#define LOG_IO_OPS

/*#define LOG_VERBOSE_ENABLE
*/

#define LOG_PREFIX_MASK     (0xFFFF)
#define LOG_FULL_BIT        (1<<31)
#define LOG_TAG_BIT         (1<<3)
#define LOG_TIMESTAMP_BIT   (1<<2)
#define LOG_PIDTID_BIT      (1<<1)
#define LOG_FUNCLINE_BIT    (1<<0)
#define LOG_VERBOSE_BIT \
	(LOG_TAG_BIT|LOG_TIMESTAMP_BIT|LOG_PIDTID_BIT|LOG_FUNCLINE_BIT)

#define LOG_TAG_MASK        (0x0F)
#define LOG_TIMESTAMP_MASK  (0x07)
#define LOG_PIDTID_MASK     (0x03)
#define LOG_FUNCLINE_MASK   (0x01)

#define UPDATE_LOG_PREFIX(log, bit) \
    log |= (((bit) & LOG_TIMESTAMP_MASK) | \
            ((bit) & LOG_PIDTID_MASK) | \
            ((bit) & LOG_TAG_MASK) | \
            ((bit) & LOG_FUNCLINE_MASK))

#define CHECK_LOG_PREFIX(log, bit)  \
    ((log & LOG_PREFIX_MASK) & (bit))

#define level_str(x)    (x ? x : "err")
#define output_str(x)   (x ? x : "stderr")
#define time_str(x)     (x ? x : "0")

#define is_str_equal(a,b) \
    ((strlen(a) == strlen(b)) && (0 == strcasecmp(a,b)))

#ifdef __GNUC__
#define LIKELY(x)       (__builtin_expect(!!(x), 1))
#define UNLIKELY(x)     (__builtin_expect(!!(x), 0))
#else
#define LIKELY(x)       (x)
#define UNLIKELY(x)     (x)
#endif


typedef struct log_ops {
    int (*open)(const char *path);
    ssize_t (*write)(struct iovec *vec, int n);
    int (*close)(void);
} log_ops_t;

typedef struct log_driver {
    int (*init)(const char *ident);
    void (*deinit)();
} log_driver_t;

/* from /usr/include/sys/syslog.h */
static const char *_log_level_str[] = {
    "EMERG",
    "ALERT",
    "CRIT",
    "ERR",
    "WARN",
    "NOTICE",
    "INFO",
    "DEBUG",
    "VERBOSE",
    NULL
};

static struct {
    const char     *name;
    const int       value;
} syslog_facilities[] = {
    {"user",    LOG_USER},
    {"local0",  LOG_LOCAL0},
    {"local1",  LOG_LOCAL1},
    {"local2",  LOG_LOCAL2},
    {"local3",  LOG_LOCAL3},
    {"local4",  LOG_LOCAL4},
    {"local5",  LOG_LOCAL5},
    {"local6",  LOG_LOCAL6},
    {"local7",  LOG_LOCAL7},
    {NULL, 0}
};

static int _is_log_init = 0;
static int _log_fd = 0;
static FILE *_log_fp = NULL;
static int _log_level = LOG_LEVEL_DEFAULT;
static int _log_syslog = 0;
static char _log_path[FILENAME_LEN];
static char _log_name[FILENAME_LEN];
static char _log_name_prefix[FILENAME_LEN];
static char _log_name_time[FILENAME_LEN];
static pthread_mutex_t _log_mutex;
static int _log_prefix = 0;
static int _log_output = 0;
static int _log_use_io = 0;
static char _proc_name[LOG_PNAME_SIZE];
static unsigned long long _log_file_size = FILESIZE_LEN;
static int _log_type;
static const char *_log_ident;


static unsigned long get_file_size(const char *path)
{
    struct stat buf;
    if (stat(path, &buf) < 0) {
        return 0;
    }
    return (unsigned long)buf.st_size;
}

#if defined (__ANDROID__)
#define get_file_size_by_fp(...) (unsigned long long)(1024*1024)
#else
static unsigned long long get_file_size_by_fp(FILE *fp)
{
    unsigned long long size;
    if (!fp || fp == stderr) {
        return 0;
    }
    fseek(fp, 0L, SEEK_END);
    size = ftell(fp);
    return size;
}
#endif

#if defined (__ANDROID__)
#define get_proc_name() (char *)(0)
#else
static char *get_proc_name(void)
{
    int i, ret;
    char proc_name[PROC_NAME_LEN];
    char *proc = NULL;
    char *ptr = NULL;
    memset(proc_name, 0, PROC_NAME_LEN);
    ret = readlink("/proc/self/exe", proc_name, PROC_NAME_LEN);
    if (ret < 0 || ret >= PROC_NAME_LEN) {
        fprintf(stderr, "get proc path failed!\n");
        return NULL;
    }
    for (i = ret, ptr = proc_name; i > 0; i--) {
        if (ptr[i] == '/') {
            ptr+= i+1;
            break;
        }
    }
    if (i == 0) {
        return NULL;
    }
    proc = (char *)calloc(1, i);
    if (proc) {
        strncpy(proc, ptr, i);
    }

    return proc;
}
#endif

#if defined (__ANDROID__)
#define _gettid()	(0)
#else
static pid_t _gettid(void)
{
    return syscall(__NR_gettid);
}
#endif

static void log_get_time(char *str, int len, int flag_name)
{
    char date_fmt[20];
    char date_ms[4];
    struct timeval tv;
    struct tm now_tm;
    int now_ms;
    time_t now_sec;
    gettimeofday(&tv, NULL);
    now_sec = tv.tv_sec;
    now_ms = tv.tv_usec/1000;
    localtime_r(&now_sec, &now_tm);

    if (flag_name == 0) {
        strftime(date_fmt, 20, "%Y-%m-%d %H:%M:%S", &now_tm);
        snprintf(date_ms, sizeof(date_ms), "%03d", now_ms);
        snprintf(str, len, "[%s.%s]", date_fmt, date_ms);
    } else {
        strftime(date_fmt, 20, "%Y_%m_%d_%H_%M_%S", &now_tm);
        snprintf(date_ms, sizeof(date_ms), "%03d", now_ms);
        snprintf(str, len, "%s_%s.log", date_fmt, date_ms);
    }
}

static const char *get_dir(const char *path)
{
    char *p = (char *)path + strlen(path);
    for (; p != path; p--) {
       if (*p == '/') {
           *(p + 1) = '\0';
           break;
       }
    }
    return path;
}

static int mkdir_r(const char *path, mode_t mode)
{
    if (!path) {
        return -1;
    }
    char *temp = strdup(path);
    char *pos = temp;
    int ret = 0;

    if (strncmp(temp, "/", 1) == 0) {
        pos += 1;
    } else if (strncmp(temp, "./", 2) == 0) {
        pos += 2;
    }
    for ( ; *pos != '\0'; ++ pos) {
        if (*pos == '/') {
            *pos = '\0';
            if (-1 == (ret = mkdir(temp, mode))) {
                if (errno == EEXIST) {
                    ret = 0;
                } else {
                    fprintf(stderr, "failed to mkdir %s: %d:%s\n",
                                    temp, errno, strerror(errno));
                    break;
                }
            }
            *pos = '/';
        }
    }
    if (*(pos - 1) != '/') {
        printf("if %s\n", temp);
        if (-1 == (ret = mkdir(temp, mode))) {
            if (errno == EEXIST) {
                ret = 0;
            } else {
                fprintf(stderr, "failed to mkdir %s: %d:%s\n",
                                temp, errno, strerror(errno));
            }
        }
    }
    free(temp);
    return ret;
}
static void check_dir(const char *path)
{
    char *path_org = NULL;
    const char *dir = NULL;
    if (strstr(path, "/")) {//file with dir
        path_org = strdup(path);
        dir = get_dir(path_org);
        if (-1 == access(dir, F_OK|W_OK|R_OK)) {
            if (-1 == mkdir_r(dir, 0775)) {
                fprintf(stderr, "mkdir %s failed\n", path_org);
            }
        }
        free(path_org);
    }
}

static int _log_fopen(const char *path)
{
    check_dir(path);
    _log_fp = fopen(path, "a+");
    if (!_log_fp) {
        fprintf(stderr, "fopen %s failed: %s\n", path, strerror(errno));
        fprintf(stderr, "use stderr as output\n");
        _log_fp = stderr;
    }
    return 0;
}

static int _log_fclose(void)
{
    return fclose(_log_fp);
}

static ssize_t _log_fwrite(struct iovec *vec, int n)
{
    int i, ret;
    unsigned long long tmp_size = get_file_size_by_fp(_log_fp);
    if (UNLIKELY(tmp_size > _log_file_size)) {
        if (CHECK_LOG_PREFIX(_log_prefix, LOG_VERBOSE_BIT)) {
            fprintf(stderr, "%s size= %llu reach max %llu, splited\n",
                    _log_name, tmp_size, _log_file_size);
        }
        if (EOF == _log_fclose()) {
            fprintf(stderr, "_log_fclose errno:%d", errno);
        }
        log_get_time(_log_name_time, sizeof(_log_name_time), 1);
        snprintf(_log_name, sizeof(_log_name), "%s%s_%s",
                _log_path, _log_name_prefix, _log_name_time);
        _log_fopen(_log_name);
        if (CHECK_LOG_PREFIX(_log_prefix, LOG_VERBOSE_BIT)) {
            fprintf(stderr, "splited file %s\n", _log_name);
        }
    }
    for (i = 0; i < n; i++) {
        ret = fprintf(_log_fp, "%s", (char *)vec[i].iov_base);
        if (ret != (int)vec[i].iov_len) {
            fprintf(stderr, "fprintf failed: %s\n", strerror(errno));
            return -1;
        }
        if (EOF == fflush(_log_fp)) {
            fprintf(stderr, "fflush failed: %s\n", strerror(errno));
            return -1;
        }
    }
    return 0;
}


static int _log_open(const char *path)
{
    check_dir(path);
    _log_fd = open(path, O_RDWR|O_CREAT|O_APPEND, 0644);
    if (_log_fd == -1) {
        fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
        fprintf(stderr, "use STDERR_FILEIO as output\n");
        _log_fd = STDERR_FILENO;
    }
    return 0;
}

static int _log_close(void)
{
    return close(_log_fd);
}

static ssize_t _log_write(struct iovec *vec, int n)
{
    unsigned long long tmp_size = get_file_size(_log_name);
    if (UNLIKELY(tmp_size > _log_file_size)) {
        fprintf(stderr, "%s size= %llu reach max %llu, splited\n",
                _log_name, tmp_size, _log_file_size);
        if (-1 == _log_close()) {
            fprintf(stderr, "_log_close errno:%d", errno);
        }
        log_get_time(_log_name_time, sizeof(_log_name_time), 1);
        snprintf(_log_name, sizeof(_log_name), "%s%s_%s",
                _log_path, _log_name_prefix, _log_name_time);

        _log_open(_log_name);
        fprintf(stderr, "splited file %s\n", _log_name);
    }

    return writev(_log_fd, vec, n);
}


static struct log_ops log_fio_ops = {
    .open = _log_fopen,
    .write = _log_fwrite,
    .close = _log_fclose,
};

static struct log_ops log_io_ops = {
    .open = _log_open,
    .write = _log_write,
    .close = _log_close
};

static struct log_ops *_log_handle = NULL;

/*
 *time: level: process[pid]: [tid] tag: message
 *             [verbose          ]
 */
static int _log_print(int lvl, const char *tag,
                      const char *file, int line,
                      const char *func, const char *msg)
{
    int ret = 0, i = 0;
    struct iovec vec[LOG_IOVEC_MAX];
    char s_time[LOG_TIME_SIZE];
    char s_lvl[LOG_LEVEL_SIZE];
    char s_tag[LOG_TAG_SIZE];
    char s_pname[LOG_PNAME_SIZE];
    char s_pid[LOG_PNAME_SIZE];
    char s_tid[LOG_PNAME_SIZE];
    char s_file[LOG_TEXT_SIZE];
    char s_msg[LOG_BUF_SIZE];

    pthread_mutex_lock(&_log_mutex);
    log_get_time(s_time, sizeof(s_time), 0);

    if (_log_fp == stderr || _log_fd == STDERR_FILENO) {
        switch(lvl) {
        case LOG_EMERG:
        case LOG_ALERT:
        case LOG_CRIT:
        case LOG_ERR:
            snprintf(s_lvl, sizeof(s_lvl),
                    B_RED("[%7s]"), _log_level_str[lvl]);
            snprintf(s_msg, sizeof(s_msg), RED("%s"), msg);
            break;
        case LOG_WARNING:
            snprintf(s_lvl, sizeof(s_lvl),
                    B_YELLOW("[%7s]"), _log_level_str[lvl]);
            snprintf(s_msg, sizeof(s_msg), YELLOW("%s"), msg);
            break;
        case LOG_INFO:
            snprintf(s_lvl, sizeof(s_lvl),
                    B_GREEN("[%7s]"), _log_level_str[lvl]);
            snprintf(s_msg, sizeof(s_msg), GREEN("%s"), msg);
            break;
        case LOG_DEBUG:
            snprintf(s_lvl, sizeof(s_lvl),
                    B_WHITE("[%7s]"), _log_level_str[lvl]);
            snprintf(s_msg, sizeof(s_msg), WHITE("%s"), msg);
            break;
        default:
            snprintf(s_lvl, sizeof(s_lvl),
                    "[%7s]", _log_level_str[lvl]);
            snprintf(s_msg, sizeof(s_msg), "%s", msg);
            break;
        }
    } else {
        snprintf(s_lvl, sizeof(s_lvl),
                "[%7s]", _log_level_str[lvl]);
        snprintf(s_msg, sizeof(s_msg), "%s", msg);
    }
    if (CHECK_LOG_PREFIX(_log_prefix, LOG_PIDTID_BIT)) {
        snprintf(s_pname, sizeof(s_pname), "[%s ", _proc_name);
        snprintf(s_pid, sizeof(s_pid), "pid:%d ", getpid());
        snprintf(s_tid, sizeof(s_tid), "tid:%d]", _gettid());
        snprintf(s_tag, sizeof(s_tag), "[%s]", tag);
        snprintf(s_file, sizeof(s_file), "[%s:%d: %s] ", file, line, func);
    }
    if (CHECK_LOG_PREFIX(_log_prefix, LOG_FUNCLINE_BIT)) {
        snprintf(s_file, sizeof(s_file), "[%s:%d: %s] ", file, line, func);
    }

    i = -1;
    if (CHECK_LOG_PREFIX(_log_prefix, LOG_TIMESTAMP_BIT)) {
        vec[++i].iov_base = (void *)s_time;
        vec[i].iov_len = strlen(s_time);
    }
    if (CHECK_LOG_PREFIX(_log_prefix, LOG_PIDTID_BIT)) {
        vec[++i].iov_base = (void *)s_pname;
        vec[i].iov_len = strlen(s_pname);
        vec[++i].iov_base = (void *)s_pid;
        vec[i].iov_len = strlen(s_pid);
        vec[++i].iov_base = (void *)s_tid;
        vec[i].iov_len = strlen(s_tid);
    }
    vec[++i].iov_base = (void *)s_lvl;
    vec[i].iov_len = strlen(s_lvl);
    if (CHECK_LOG_PREFIX(_log_prefix, LOG_TAG_BIT)) {
        vec[++i].iov_base = (void *)s_tag;
        vec[i].iov_len = strlen(s_tag);
    }
    if (CHECK_LOG_PREFIX(_log_prefix, LOG_FUNCLINE_BIT)) {
        vec[++i].iov_base = (void *)s_file;
        vec[i].iov_len = strlen(s_file);
    }
    vec[++i].iov_base = (void *)s_msg;
    vec[i].iov_len = strlen(s_msg);

    if (UNLIKELY(!_log_syslog)) {
        ret = _log_handle->write(vec, i+1);
    }
    pthread_mutex_unlock(&_log_mutex);
    return ret;
}

int log_print(int lvl, const char *tag, const char *file,
              int line, const char *func, const char *fmt, ...)
{
    va_list ap;
    char buf[LOG_BUF_SIZE] = {0};
    int n, ret;

    if (UNLIKELY(!_is_log_init)) {
        log_init(0, NULL);
    }

    if (lvl > _log_level) {
        return 0;
    }

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (UNLIKELY(n < 0)) {
        fprintf(stderr, "vsnprintf errno:%d\n", errno);
        return -1;
    }
    if (UNLIKELY(_log_syslog)) {
        syslog(lvl, "%s", buf);
    }
    ret = _log_print(lvl, tag, file, line, func, buf);

    return ret;
}

void log_set_level(int level)
{
    if (level > LOG_VERB || level < LOG_EMERG) {
        _log_level = LOG_LEVEL_DEFAULT;
    } else {
        _log_level = level;
    }
}

static void log_check_env(void)
{
    _log_level = LOG_LEVEL_DEFAULT;
    const char *levelstr = level_str(getenv(LOG_LEVEL_ENV));
    const char *outputstr = output_str(getenv(LOG_OUTPUT_ENV));
    const char *timestr = time_str(getenv(LOG_TIMESTAMP_ENV));
    int level = atoi(levelstr);
    int output = atoi(outputstr);
    int timestamp = atoi(timestr);

    switch (level) {
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
    case 8:
        _log_level = level;
        break;
    case 0:
        if (is_str_equal(levelstr, "error")) {
            _log_level = LOG_ERR;
        } else if (is_str_equal(levelstr, "warn")) {
            _log_level = LOG_WARNING;
        } else if (is_str_equal(levelstr, "notice")) {
            _log_level = LOG_NOTICE;
        } else if (is_str_equal(levelstr, "info")) {
            _log_level = LOG_INFO;
        } else if (is_str_equal(levelstr, "debug")) {
            _log_level = LOG_DEBUG;
        } else if (is_str_equal(levelstr, "verbose")) {
            _log_level = LOG_VERB;
        }
        break;
    default:
        break;
    }
    switch (output) {
    case 1:
    case 2:
    case 3:
    case 4:
        _log_output = output;
        break;
    case 0:
        if (is_str_equal(outputstr, "stderr")) {
            _log_output = LOG_STDERR;
        } else if (is_str_equal(outputstr, "file")) {
            _log_output = LOG_FILE;
        } else if (is_str_equal(outputstr, "rsyslog")) {
            _log_output = LOG_RSYSLOG;
        }
        break;
    default:
        break;
    }
    switch (timestamp) {
    case 1:
        UPDATE_LOG_PREFIX(_log_prefix, LOG_TIMESTAMP_BIT);
        break;
    case 0:
        if (is_str_equal(timestr, "y") ||
            is_str_equal(timestr, "yes") ||
            is_str_equal(timestr, "true")) {
             UPDATE_LOG_PREFIX(_log_prefix, LOG_TIMESTAMP_BIT);
        }
        break;
    default:
        break;
    }
    if (_log_level == LOG_DEBUG) {
        UPDATE_LOG_PREFIX(_log_prefix, LOG_FUNCLINE_BIT);
    }
    if (_log_level == LOG_VERB) {
        UPDATE_LOG_PREFIX(_log_prefix, LOG_VERBOSE_BIT);
    }
}

int log_set_split_size(int size)
{
    if ((uint32_t)size > FILESIZE_LEN || size < 0) {
        _log_file_size = FILESIZE_LEN;
    } else {
        _log_file_size = size;
    }
    return 0;
}

static int log_init_stderr(const char *ident)
{
    _log_fp = stderr;
    _log_fd = STDERR_FILENO;
    return 0;
}

static void log_deinit_stderr(void)
{
}

int log_set_path(const char *path)
{
    if (!path) {
        fprintf(stderr, "invalid path!\n");
        return -1;
    }
    if (strlen(path) == 0) {
        fprintf(stderr, "invalid path!\n");
        return -1;
    }
    strncpy(_log_path, path, sizeof(_log_path));
    return 0;
}

static void log_filename_parse(const char *ident)
{
/*
 *  full_path_name = _log_path/_log_name.log
 * _log_path = /path/of/file
 * _log_name = _log_name_prefix _log_name_time ._log_name_suffix
 */
    int i;
    char *p, *q;
    char *dot;
    if (!ident) {
        return;
    }

    memset(_log_path, 0, sizeof(_log_path));
    memset(_log_name_prefix, 0, sizeof(_log_name_prefix));
    p = (char *)strchr(ident, '/');
    q = (char *)strrchr(ident, '/');
    if (p == q || p == NULL) {
        p = (char *)ident;
    } else {
        for (i = 0, p = (char *)ident; p < q+1; p++, i++) {
            _log_path[i] = *p;
        }
    }
    dot = (char *)strrchr(ident, '.');
    if (dot) {
        for(i = 0; p < dot; p++, i++) {
            _log_name_prefix[i] = *p;
        }
    } else {
        for(i = 0; *p != '\0'; p++, i++) {
            _log_name_prefix[i] = *p;
        }
    }
}


static int log_init_file(const char *ident)
{
    log_filename_parse(ident);
    memset(_log_name, 0, sizeof(_log_name));
    if (ident == NULL) {
        log_get_time(_log_name, sizeof(_log_name), 1);
    } else {
        strncpy(_log_name, ident, sizeof(_log_name));
    }
    _log_fp = NULL;
    _log_fd = 0;
    _log_handle->open(_log_name);
    return 0;
}

static void log_deinit_file(void)
{
    _log_handle->close();
}

static int log_init_syslog(const char *facilitiy_str)
{
    const char *ident = NULL;
    int i = 0;
    int facilitiy = LOG_LOCAL0;
    _log_syslog = 1;
    for (i = 0; syslog_facilities[i].name; i++) {
        if (!strcasecmp(syslog_facilities[i].name, facilitiy_str)) {
            facilitiy = syslog_facilities[i].value;
            break;
        }
    }
    openlog(ident, LOG_NOWAIT | LOG_NDELAY | LOG_PID, facilitiy);
    return 0;
}

static void log_deinit_syslog(void)
{
    closelog();
}

static struct log_driver log_stderr_driver = {
    .init = log_init_stderr,
    .deinit = log_deinit_stderr,
};

static struct log_driver log_file_driver = {
    .init = log_init_file,
    .deinit = log_deinit_file,
};

static struct log_driver log_rsys_driver = {
    .init = log_init_syslog,
    .deinit = log_deinit_syslog,
};

static struct log_driver *_log_driver = NULL;

static void __log_init(void)
{
    int type = _log_type;
    const char *ident = _log_ident;
    if (_is_log_init) {
        return;
    }
    log_check_env();
#ifdef LOG_VERBOSE_ENABLE
    UPDATE_LOG_PREFIX(_log_prefix, LOG_VERBOSE_BIT);
#endif
    UPDATE_LOG_PREFIX(_log_prefix, LOG_FUNCLINE_BIT);

#ifdef LOG_IO_OPS
    _log_use_io = 1;
#endif
    if (_log_use_io) {
        _log_handle = &log_io_ops;
    } else {
        _log_handle = &log_fio_ops;
    }

    if (CHECK_LOG_PREFIX(_log_prefix, LOG_VERBOSE_BIT)) {
        memset(_proc_name, 0, sizeof(_proc_name));
        char *proc = get_proc_name();
        if (proc) {
            memset(_log_name, 0, sizeof(_log_name));
            strncpy(_proc_name, proc, strlen(proc));
            free(proc);
        }
    }
    if (type == 0) {
    } else {
        _log_output = type;
    }
    switch (_log_output) {
    case LOG_STDERR:
        _log_driver = &log_stderr_driver;
        break;
    case LOG_FILE:
        _log_driver = &log_file_driver;
        break;
    case LOG_RSYSLOG:
        _log_driver = &log_rsys_driver;
        break;
    default:
        fprintf(stderr, "unsupport log type!\n");
        break;
    }
    _log_driver->init(ident);
    _is_log_init = 1;
    pthread_mutex_init(&_log_mutex, NULL);
    return;
}

int log_init(int type, const char *ident)
{
    _log_type = type;
    _log_ident = ident;
    __log_init();
    return 0;
}

void log_deinit(void)
{
    if (!_log_driver) {
        return;
    }
    _log_driver->deinit();
    _log_driver = NULL;
    _is_log_init = 0;
    pthread_mutex_destroy(&_log_mutex);
}
