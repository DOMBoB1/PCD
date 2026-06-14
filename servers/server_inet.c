#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#include "protocol.h"
#include <libconfig.h>
#include <stdarg.h>
#include <uv.h>

/* ── Jurnalizare (logging) ────────────────────────────────────────────────────
 * Niveluri: FATAL(0) intotdeauna jurnalizat; INFO(1) < ERR(2) < WRN(3) < DBG(4).
 * Mesajele sunt scrise in doua locuri simultan:
 *   - stderr, cu culori ANSI pentru lizibilitate in terminal
 *   - session.log, text simplu fara coduri de culoare, pentru arhivare
 * Nivelul activ se configureaza cu log_level in server.cfg (implicit 4 = DBG,
 * afiseaza tot). Accesul la fiserul de jurnal este protejat de g_log_mutex
 * pentru a evita intercalarea randurilor din fire de executie diferite.
 * ─────────────────────────────────────────────────────────────────────────── */
#define LVL_FATAL 0
#define LVL_INFO  1
#define LVL_ERR   2
#define LVL_WRN   3
#define LVL_DBG   4

static int             g_log_level = LVL_DBG;
static int             g_log_fd    = -1;           /* session.log write fd  */
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

#define COL_RST "\033[0m"
#define COL_INF "\033[37m"
#define COL_ERR "\033[31m"
#define COL_FAT "\033[1;31m"
#define COL_WRN "\033[33m"
#define COL_DBG "\033[36m"

/*
 * log_emit — functia centrala de jurnalizare; apelata de toate macrocomenzile
 * LOG_*. Formateaza mesajul cu marca de timp, il scrie colorat la stderr si
 * in text simplu in session.log. Blocarea pe mutex garanteaza ca liniile nu
 * se intercaleaza cand mai multe fire scriu simultan.
 */
static void log_emit(int lvl, const char *tag, const char *col,
                     const char *fmt, va_list ap) {
  /* ignora mesajele sub nivelul configurat, cu exceptia FATAL */
  if (lvl != LVL_FATAL && lvl > g_log_level) return;
  time_t now = time(NULL);
  struct tm tm;
  localtime_r(&now, &tm);
  char ts[16];
  strftime(ts, sizeof ts, "%H:%M:%S", &tm); /* marca de timp HH:MM:SS */
  char body[2048];
  vsnprintf(body, sizeof body, fmt, ap); /* formateaza corpul mesajului */
  /* versiunea cu culori pentru stderr */
  char colored[2400], plain[2400];
  int nc = snprintf(colored, sizeof colored,
                    "%s[%s] [%s] %s%s\n", col, ts, tag, body, COL_RST);
  /* versiunea fara culori pentru fisier (codurile ANSI ar fi text literal) */
  int np = snprintf(plain,   sizeof plain,   "[%s] [%s] %s\n", ts, tag, body);
  pthread_mutex_lock(&g_log_mutex);
  write(STDERR_FILENO, colored, (nc > 0 ? (size_t)nc : 0));
  if (g_log_fd >= 0)
    write(g_log_fd, plain, (np > 0 ? (size_t)np : 0));
  pthread_mutex_unlock(&g_log_mutex);
}

#define _DEFLOG(fn, tag, col, lvl)                                   \
  static void fn(const char *fmt, ...)                               \
      __attribute__((format(printf, 1, 2)));                         \
  static void fn(const char *fmt, ...) {                             \
    va_list ap; va_start(ap, fmt);                                   \
    log_emit(lvl, tag, col, fmt, ap); va_end(ap); }
_DEFLOG(log_info,  "INF", COL_INF, LVL_INFO)
_DEFLOG(log_err,   "ERR", COL_ERR, LVL_ERR)
_DEFLOG(log_wrn,   "WRN", COL_WRN, LVL_WRN)
_DEFLOG(log_dbg,   "DBG", COL_DBG, LVL_DBG)
_DEFLOG(log_fatal, "FAT", COL_FAT, LVL_FATAL)

#define LOG_INFO(...)  log_info(__VA_ARGS__)
#define LOG_ERR(...)   log_err(__VA_ARGS__)
#define LOG_WRN(...)   log_wrn(__VA_ARGS__)
#define LOG_DBG(...)   log_dbg(__VA_ARGS__)
#define LOG_FATAL(...) log_fatal(__VA_ARGS__)

/*
 * ERR_AND_EXIT — jurnalizeaza o eroare fatala cu contextul errno si opreste
 * serverul. Folosit la orice apel de sistem critic care nu poate esua
 * (socket, bind, listen, pipe, pthread_create la pornire).
 */
#define ERR_AND_EXIT(msg) \
  do { LOG_FATAL(msg ": %s (errno=%d)", strerror(errno), errno); \
       exit(EXIT_FAILURE); } while (0)

/*
 * POLL_MAX_FDS — limita superioara a valorii unui descriptor de fisier pe
 * care poll() il poate monitoriza in aceasta implementare. select() era
 * limitat la FD_SETSIZE=1024; poll() nu are aceasta restrictie, dar vom
 * folosi o limita practica pentru dimensionarea array-ului pfds[].
 * Valoarea 4096 permite pana la ~4000 de clienti simultani.
 */
#define POLL_MAX_FDS 4096
/* numarul maxim de limbaje de programare configurabile in server.cfg */
#define MAX_LANGS 32
/* dimensiunea maxima a unui sablon de comanda (compile/run din config) */
#define CMD_MAX 512

/* ── Descriptor de limbaj de programare ─────────────────────────────────── */
/*
 * LangConfig — configuratia unui limbaj de programare incarcat din server.cfg.
 * serialize=1 inseamna ca executiile pentru acest limbaj sunt serializate
 * (un singur worker la un moment dat) prin serialize_mutex — util pentru
 * limbaje care nu pot rula in paralel (ex: Java cu un singur JDK instalat).
 * fixed_src: daca este setat, toate cererile pentru acest limbaj folosesc
 * acelasi fisier sursa (util pentru limbi cu un singur program de test).
 */
typedef struct {
  int id;                          /* identificatorul numeric din protocol (1=C, 2=Py, 3=C++) */
  char name[32];                   /* numele afisat (ex: "C", "Python", "C++")              */
  char extension[16];              /* extensia fisierului sursa (ex: "c", "py", "cpp")      */
  char fixed_src[128];             /* cale fixa pentru sursa; gol = cale generata dinamic   */
  char compile[CMD_MAX];           /* sablon de comanda de compilare; gol = interpretat      */
  char run[CMD_MAX];               /* sablon de comanda de executie                          */
  int serialize;                   /* 1 = executa secvential (un singur job simultan)        */
  pthread_mutex_t serialize_mutex; /* mutex de serializare (folosit doar daca serialize=1)   */
} LangConfig;

static LangConfig lang_configs[MAX_LANGS];
static int lang_count = 0;

/* ── Configuratie de rulare (populata de load_config la pornire) ─────────── */
static int cfg_port = 8080;                         /* portul TCP al serverului */
static int cfg_workers = 4;                         /* numarul de fire de lucru */
static int cfg_max_clients = 1024;                  /* clienti simultani maximi */
static uint64_t cfg_max_source_mb = 64;             /* limita sursa in MB; 0=nelimitat */
static uint32_t cfg_exec_mem_default_mb = 2048;     /* limita memoria implicita cand clientul trimite 0 */
static char cfg_login_file[256] = "servers/login.txt"; /* fisierul cu credentiale admin */
static char cfg_admin_socket[256] = "/tmp/server_admin.sock"; /* calea socket-ului Unix */
static int  cfg_admin_tcp_port    = 8082;           /* portul admin TCP; 0=dezactivat */
static char cfg_path_global[256]  = "server.cfg";  /* calea fisierului de config (pentru RELOAD) */

/* ── Incarcator de configuratie ──────────────────────────────────────────── */
/*
 * load_config — citeste fisierul de configuratie libconfig si populeaza
 * variabilele globale cfg_*. Apelata o data la pornire si optional la
 * comanda RELOAD din sesiunea admin. cfg_max_clients este limitat superior
 * la POLL_MAX_FDS pentru a preveni depasirea array-ului clients[].
 * La eroare de parsare, serverul se opreste imediat (FATAL).
 */
static void load_config(const char *path) {
  config_t cfg;
  config_init(&cfg);
  if (!config_read_file(&cfg, path)) {
    LOG_FATAL("Config error %s:%d — %s", config_error_file(&cfg),
              config_error_line(&cfg), config_error_text(&cfg));
    config_destroy(&cfg);
    exit(EXIT_FAILURE);
  }
  config_lookup_int(&cfg, "server.port", &cfg_port);
  config_lookup_int(&cfg, "server.workers", &cfg_workers);
  config_lookup_int(&cfg, "server.max_clients", &cfg_max_clients);
  if (cfg_max_clients > POLL_MAX_FDS)
    cfg_max_clients = POLL_MAX_FDS;
  int msm = 0;
  if (config_lookup_int(&cfg, "server.max_source_mb", &msm) && msm >= 0)
    cfg_max_source_mb = (uint64_t)msm;
  const char *s;
  if (config_lookup_string(&cfg, "server.login_file", &s))
    strncpy(cfg_login_file, s, sizeof cfg_login_file - 1);
  if (config_lookup_string(&cfg, "server.admin_socket", &s))
    strncpy(cfg_admin_socket, s, sizeof cfg_admin_socket - 1);
  config_lookup_int(&cfg, "server.admin_tcp_port", &cfg_admin_tcp_port);
  int ll = LVL_DBG;
  if (config_lookup_int(&cfg, "server.log_level", &ll))
    g_log_level = ll;
  int emd = (int)cfg_exec_mem_default_mb;
  if (config_lookup_int(&cfg, "server.exec_mem_default_mb", &emd) && emd >= 0)
    cfg_exec_mem_default_mb = (uint32_t)emd;
  strncpy(cfg_path_global, path, sizeof cfg_path_global - 1);

  config_setting_t *langs = config_lookup(&cfg, "languages");
  if (langs && config_setting_is_list(langs)) {
    lang_count = config_setting_length(langs);
    if (lang_count > MAX_LANGS)
      lang_count = MAX_LANGS;
    for (int i = 0; i < lang_count; i++) {
      config_setting_t *e = config_setting_get_elem(langs, i);
      LangConfig *lc = &lang_configs[i];
      config_setting_lookup_int(e, "id", &lc->id);
      if (config_setting_lookup_string(e, "name", &s))
        strncpy(lc->name, s, sizeof lc->name - 1);
      if (config_setting_lookup_string(e, "extension", &s))
        strncpy(lc->extension, s, sizeof lc->extension - 1);
      if (config_setting_lookup_string(e, "fixed_src", &s))
        strncpy(lc->fixed_src, s, sizeof lc->fixed_src - 1);
      if (config_setting_lookup_string(e, "compile", &s))
        strncpy(lc->compile, s, CMD_MAX - 1);
      if (config_setting_lookup_string(e, "run", &s))
        strncpy(lc->run, s, CMD_MAX - 1);
      int ser = 0;
      config_setting_lookup_bool(e, "serialize", &ser);
      lc->serialize = ser;
      pthread_mutex_init(&lc->serialize_mutex, NULL);
    }
  }
  config_destroy(&cfg);

  LOG_INFO("Config: port=%d workers=%d max_clients=%d login=%s admin_socket=%s",
           cfg_port, cfg_workers, cfg_max_clients, cfg_login_file,
           cfg_admin_socket);
  for (int i = 0; i < lang_count; i++) {
    LangConfig *lc = &lang_configs[i];
    LOG_DBG("lang[%d] %s (.%s)%s%s", lc->id, lc->name, lc->extension,
            lc->compile[0] ? "" : " [interpreted]",
            lc->serialize ? " [serialized]" : "");
  }
}

static LangConfig *find_lang(int id) {
  for (int i = 0; i < lang_count; i++)
    if (lang_configs[i].id == id)
      return &lang_configs[i];
  return NULL;
}

/* ── Registrul clientilor (indexat dupa fd, dimensiune POLL_MAX_FDS) ────── */

/*
 * Fiecare client obisnuit ocupa clients[fd] — fd-ul TCP este simultan cheia
 * din array. Conexiunile admin NU sunt stocate aici; ele sunt gestionate
 * complet de handle_admin_thread / handle_admin_inet_thread.
 *
 * Masina de stare a clientului:
 *   CS_READING   — citeste octeti din ReqHeader, apoi payload-ul de cod sursa
 *   CS_EXECUTING — task trimis in coada; fd-ul este exclus din multimea poll()
 *                  pana cand workerul scrie in notify_pipe si il readuce la CS_READING
 */
typedef enum { CS_READING = 0, CS_EXECUTING } ClientState;

typedef struct {
  int socket;               /* descriptorul TCP = indicele in array         */
  char ip[INET_ADDRSTRLEN]; /* adresa IP a clientului in format text        */
  int port;                 /* portul efemer al clientului                  */
  int id;                   /* ID secvential atribuit la conectare          */
  char uuid[37];            /* UUID v4 generat la conectare (36 car + '\0') */
  int active;               /* 1 daca slotul este ocupat de un client activ */
  int kicked;               /* 1 daca a fost expulzat de admin; nu mai inchidem fd-ul din nou */
  ClientState state;        /* starea curenta in masina de stare            */
  /* stare de citire partiala pentru faza CS_READING */
  char hdr_buf[sizeof(ReqHeader)]; /* buffer acumulare antet cerere         */
  size_t hdr_read;          /* octeti din antet cititi pana acum            */
  /* octetii codului sursa sunt transmisi direct pe disc, fara buffer RAM */
  int  code_fd;             /* fd deschis pentru scriere in timp ce primim sursa; -1=niciun */
  char code_src[256];       /* calea fisierului sursa, ex: "code_7.py"      */
  uint64_t code_written;    /* octeti ai codului scrisi pana acum           */
  uint64_t code_size;       /* total octeti cod asteptati (din antet)       */
  int code_rejected;        /* 1 = sursa depaseste max_source_mb; dreneaza si refuza */
} ClientInfo;

/* clients[] este indexat direct dupa descriptorul de fisier */
static ClientInfo clients[POLL_MAX_FDS];
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
static int next_id = 1;            /* contor pentru ID-uri secventiale de client */
static int admin_connected      = 0; /* 1 daca un admin Unix este conectat */
static int admin_inet_connected = 0; /* 1 daca un admin TCP este conectat  */
static int active_client_count = 0;  /* clienti activi (protejat de clients_mutex) */
static int active_maxfd = 0;         /* cel mai mare fd de client inregistrat */
static time_t server_start_time;     /* momentul pornirii, pentru calcul uptime */

/*
 * notify_pipe — pipe de notificare worker->bucla principala.
 * Cand un worker termina un task, scrie fd-ul clientului (sizeof int) in
 * notify_pipe[1]. Bucla poll() citeste din notify_pipe[0] si readuce
 * clientul din CS_EXECUTING in CS_READING, permitandu-i sa trimita o noua cerere.
 * Scrierea unui int este atomica pe POSIX (< PIPE_BUF), nu se pot intercala.
 */
static int notify_pipe[2];

static void gen_uuid(char *out, size_t sz);

/* ── Client registry helpers ─────────────────────────────────────────────── */
static int register_client(int fd, const char *ip, int port) {
  if (fd < 0 || fd >= POLL_MAX_FDS) {
    LOG_WRN("register_client REJECTED: fd=%d >= POLL_MAX_FDS=%d", fd, POLL_MAX_FDS);
    return -1;
  }
  pthread_mutex_lock(&clients_mutex);
  if (active_client_count >= cfg_max_clients) {
    LOG_WRN("register_client REJECTED: fd=%d count=%d >= max=%d",
            fd, active_client_count, cfg_max_clients);
    pthread_mutex_unlock(&clients_mutex);
    return -1;
  }
  if (clients[fd].active) {
    LOG_WRN("register_client REJECTED: fd=%d already active (count=%d)",
            fd, active_client_count);
    pthread_mutex_unlock(&clients_mutex);
    return -1;
  }
  clients[fd] = (ClientInfo){0};
  clients[fd].socket = fd;
  clients[fd].port = port;
  clients[fd].id = next_id++;
  clients[fd].active = 1;
  clients[fd].state = CS_READING;
  clients[fd].code_fd = -1;
  strncpy(clients[fd].ip, ip, INET_ADDRSTRLEN - 1);
  gen_uuid(clients[fd].uuid, sizeof clients[fd].uuid);
  active_client_count++;
  if (fd > active_maxfd)
    active_maxfd = fd;
  LOG_INFO("Client %d connected {fd:%d,addr:%s,port:%d,UUID:%s}",
           clients[fd].id, fd, ip, port, clients[fd].uuid);
  pthread_mutex_unlock(&clients_mutex);
  /* Non-blocking I/O: recv() returns EAGAIN instead of blocking */
  fcntl(fd, F_SETFL, O_NONBLOCK);
  return fd;
}

static void disconnect_client(int fd) {
  if (fd < 0 || fd >= POLL_MAX_FDS)
    return;
  pthread_mutex_lock(&clients_mutex);
  if (!clients[fd].active) {
    pthread_mutex_unlock(&clients_mutex);
    return;
  }
  int need_close = !clients[fd].kicked;
  clients[fd].active = 0;
  /* Close and remove any partial source file written during streaming */
  if (clients[fd].code_fd >= 0) {
    close(clients[fd].code_fd);
    clients[fd].code_fd = -1;
    if (clients[fd].code_src[0])
      remove(clients[fd].code_src);
    clients[fd].code_src[0] = '\0';
  }
  active_client_count--;
  pthread_mutex_unlock(&clients_mutex);
  if (need_close)
    close(fd);
}

/* ── Byte-order helper ───────────────────────────────────────────────────── */
static uint64_t hton64(uint64_t v) {
  return ((uint64_t)htonl((uint32_t)(v & 0xffffffffULL)) << 32) |
         (uint64_t)htonl((uint32_t)(v >> 32));
}

/* ── UUID v4 generator ───────────────────────────────────────────────────── */
static void gen_uuid(char *out, size_t sz) {
  uint8_t b[16] = {0};
  int rfd = open("/dev/urandom", O_RDONLY);
  if (rfd >= 0) {
    read(rfd, b, sizeof b);
    close(rfd);
  }
  b[6] = (b[6] & 0x0f) | 0x40; /* version 4 */
  b[8] = (b[8] & 0x3f) | 0x80; /* variant 1 */
  snprintf(
      out, sz,
      "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11],
      b[12], b[13], b[14], b[15]);
}

/* ── Cross-platform memory usage ─────────────────────────────────────────── */
/*
 * Fills *rss_mb and *virt_mb with the process RSS and virtual-memory size in
 * megabytes.  Both are left at 0.0 if the platform is not supported.
 */
static void get_mem_mb(double *rss_mb, double *virt_mb) {
  *rss_mb = 0.0;
  *virt_mb = 0.0;
#if defined(__APPLE__)
  struct mach_task_basic_info mi;
  mach_msg_type_number_t mc = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&mi,
                &mc) == KERN_SUCCESS) {
    *rss_mb = mi.resident_size / 1048576.0;
    *virt_mb = mi.virtual_size / 1048576.0;
  }
#elif defined(__linux__)
  int sfd = open("/proc/self/status", O_RDONLY);
  if (sfd >= 0) {
    char sbuf[4096];
    ssize_t snr = read(sfd, sbuf, sizeof sbuf - 1);
    close(sfd);
    if (snr > 0) {
      sbuf[snr] = '\0';
      long kb;
      char *sp = sbuf;
      while (*sp) {
        if (sscanf(sp, "VmRSS: %ld", &kb) == 1)
          *rss_mb = kb / 1024.0;
        if (sscanf(sp, "VmSize: %ld", &kb) == 1)
          *virt_mb = kb / 1024.0;
        while (*sp && *sp != '\n') sp++;
        if (*sp == '\n') sp++;
      }
    }
  }
#endif
}

/* ── Execution via libuv uv_spawn ────────────────────────────────────────── */

typedef struct {
  int out_fd;
  int timed_out;
  int term_signal;
  int64_t exit_status;
  int timer_started;
  uv_process_t proc;
  uv_timer_t timer;
} ExecCtx;

static void on_exec_timer(uv_timer_t *h) {
  ExecCtx *ctx = (ExecCtx *)h->data;
  ctx->timed_out = 1;
  uv_process_kill(&ctx->proc, SIGKILL);
}

static void on_exec_proc_exit(uv_process_t *proc, int64_t exit_status,
                              int term_signal) {
  ExecCtx *ctx = (ExecCtx *)proc->data;
  ctx->exit_status = exit_status;
  ctx->term_signal = term_signal;
  if (ctx->timer_started) {
    uv_timer_stop(&ctx->timer);
    uv_close((uv_handle_t *)&ctx->timer, NULL);
  }
  uv_close((uv_handle_t *)proc, NULL);
}

static const char *signal_name(int sig, char *buf, size_t sz) {
  switch (sig) {
  case SIGSEGV:
    return "Segmentation fault (SIGSEGV)";
  case SIGABRT:
    return "Aborted (SIGABRT)";
  case SIGFPE:
    return "Floating-point exception (SIGFPE)";
  case SIGBUS:
    return "Bus error (SIGBUS)";
  case SIGILL:
    return "Illegal instruction (SIGILL)";
  case SIGKILL:
    return "Killed (SIGKILL)";
  default:
    snprintf(buf, sz, "Signal %d", sig);
    return buf;
  }
}

/*
 * Run `cmd` via /bin/sh; stdout and stderr go to `output_file`.
 * time_limit_s: kill the child with SIGKILL after this many seconds (0=off).
 * mem_limit_mb: prepend `ulimit -v N` to the shell command (0=off).
 * Each worker thread owns a private uv_loop_t so loops never share state.
 */
static void execute_cmd(const char *cmd, const char *output_file,
                        uint32_t time_limit_s, uint32_t mem_limit_mb,
                        int append) {
  int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
  int out_fd = open(output_file, flags, 0644);
  if (out_fd < 0) {
    LOG_ERR("open output '%s': %s", output_file, strerror(errno));
    return;
  }

  /*
   * Build the shell command.
   * `ulimit -v` (virtual address space) works on Linux: when a process
   * tries to exceed the limit, malloc returns NULL and Python raises
   * MemoryError — a clean, readable traceback.
   * macOS does not support ulimit -v or ulimit -m in any meaningful way;
   * both return "Invalid argument".  On macOS we run the command directly
   * and rely on exit-code 137 detection to report OOM kills.
   */
  char full_cmd[CMD_MAX * 2 + 64];
#if defined(__linux__)
  if (mem_limit_mb > 0)
    snprintf(full_cmd, sizeof full_cmd, "ulimit -v %llu; %s",
             (unsigned long long)mem_limit_mb * 1024ULL, cmd);
  else
    snprintf(full_cmd, sizeof full_cmd, "%s", cmd);
#else
  snprintf(full_cmd, sizeof full_cmd, "%s", cmd);
  (void)mem_limit_mb; /* enforced via OS-level OOM on non-Linux */
#endif

  uv_loop_t loop;
  uv_loop_init(&loop);

  ExecCtx ctx;
  memset(&ctx, 0, sizeof ctx);
  ctx.out_fd = out_fd;
  ctx.proc.data = &ctx;
  ctx.timer.data = &ctx;

  uv_stdio_container_t io[3];
  io[0].flags = UV_IGNORE;
  io[1].flags = UV_INHERIT_FD;
  io[1].data.fd = out_fd;
  io[2].flags = UV_INHERIT_FD;
  io[2].data.fd = out_fd;

  char *sh_args[] = {"sh", "-c", full_cmd, NULL};
  uv_process_options_t opts;
  memset(&opts, 0, sizeof opts);
  opts.file = "sh";
  opts.args = sh_args;
  opts.env = NULL;
  opts.stdio = io;
  opts.stdio_count = 3;
  opts.exit_cb = on_exec_proc_exit;

  int r = uv_spawn(&loop, &ctx.proc, &opts);
  if (r < 0) {
    const char *err = uv_strerror(r);
    write(out_fd, err, strlen(err));
    write(out_fd, "\n", 1);
  } else {
    if (time_limit_s > 0) {
      uv_timer_init(&loop, &ctx.timer);
      uv_timer_start(&ctx.timer, on_exec_timer, (uint64_t)time_limit_s * 1000u,
                     0);
      ctx.timer_started = 1;
    }
    uv_run(&loop, UV_RUN_DEFAULT);

    if (ctx.timed_out) {
      char msg[128];
      snprintf(msg, sizeof msg, "\n[Killed: time limit of %us exceeded]\n",
               time_limit_s);
      write(out_fd, msg, strlen(msg));
    } else if (ctx.term_signal != 0) {
      /* uv_spawn wrapper itself was killed by a signal */
      char sigbuf[32];
      const char *sname = signal_name(ctx.term_signal, sigbuf, sizeof sigbuf);
      char msg[256];
      snprintf(msg, sizeof msg, "\n[Process terminated by signal: %s]\n",
               sname);
      write(out_fd, msg, strlen(msg));
    } else if (ctx.exit_status > 128 && ctx.exit_status <= 192) {
      /* sh exits 128+N when its child was killed by signal N.
       * This is the usual path for OOM: kernel sends SIGKILL to python3,
       * sh catches SIGCHLD and exits 137. */
      int sig = (int)(ctx.exit_status - 128);
      char sigbuf[32];
      const char *sname = signal_name(sig, sigbuf, sizeof sigbuf);
      char msg[512];
      int mlen = snprintf(msg, sizeof msg,
                          "\n[Process killed by signal: %s (exit code %lld)]\n",
                          sname, (long long)ctx.exit_status);
      write(out_fd, msg, (size_t)(mlen > 0 ? mlen : 0));
      if (sig == SIGKILL) {
        const char *hint =
            "[Hint: process was killed by the OS — likely exceeded available "
            "memory.\n"
            " Set a mem_limit_mb in your request or increase exec_mem_default_mb"
            " in server.cfg]\n";
        write(out_fd, hint, strlen(hint));
        LOG_WRN("execute_cmd: child killed by SIGKILL (OOM?) exit=%lld",
                (long long)ctx.exit_status);
      }
    }
    /* On non-Linux platforms ulimit is unavailable; document the limitation */
#if !defined(__linux__)
    if (mem_limit_mb > 0) {
      char msg[128];
      snprintf(msg, sizeof msg,
               "[Note: memory limit (%u MB) not enforced on this platform — "
               "OOM kill is reported via exit code]\n",
               mem_limit_mb);
      write(out_fd, msg, strlen(msg));
    }
#endif
  }

  uv_loop_close(&loop);
  close(out_fd);
}

static int file_has_error(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return 0;
  char buf[4096];
  ssize_t n = read(fd, buf, sizeof buf - 1);
  close(fd);
  if (n <= 0)
    return 0;
  buf[n] = '\0';
  return strstr(buf, "error") != NULL;
}

static void expand_cmd(char *out, size_t sz, const char *tmpl, const char *src,
                       const char *bin) {
  size_t wi = 0;
  const char *p = tmpl;
  while (*p && wi < sz - 1) {
    if (strncmp(p, "{src}", 5) == 0) {
      size_t n = strlen(src);
      if (wi + n < sz - 1) {
        memcpy(out + wi, src, n);
        wi += n;
      }
      p += 5;
    } else if (strncmp(p, "{bin}", 5) == 0) {
      size_t n = strlen(bin);
      if (wi + n < sz - 1) {
        memcpy(out + wi, bin, n);
        wi += n;
      }
      p += 5;
    } else
      out[wi++] = *p++;
  }
  out[wi] = '\0';
}

/*
 * Reliable send for O_NONBLOCK sockets.
 * When the kernel send buffer is full, send() returns EAGAIN instead of
 * blocking.  send_all() waits for the socket to become writable again
 * (via poll) and retries until all bytes are delivered or the connection
 * is broken.  Returns 1 on success, 0 on error/disconnect.
 */
static int send_all(int fd, const void *buf, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t s = send(fd, (const char *)buf + sent, len - sent, 0);
    if (s > 0) {
      sent += (size_t)s;
    } else if (s < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      struct pollfd pfd = {.fd = fd, .events = POLLOUT};
      if (poll(&pfd, 1, -1) < 0 && errno != EINTR)
        return 0;
    } else {
      return 0; /* connection closed or hard error */
    }
  }
  return 1;
}

/* Forward declaration: defined after the task queue and worker (avoids
 * reordering) */
static void handle_client_command(int fd, const char *cmd, const char *peer_ip,
                                  int peer_port);

/* ── Producer-consumer task queue ────────────────────────────────────────── */
typedef struct Task {
  int client_socket;
  uint8_t language;      /* 0xFF = async text command */
  char client_uuid[37];  /* UUID of the submitting client (for logging)    */
  char src_path[256];    /* source file already written to disk            */
  uint32_t time_limit_s; /* 0 = no limit */
  uint32_t mem_limit_mb; /* 0 = no limit */
  /* fields used only when language == 0xFF */
  char cmd[65];
  char peer_ip[INET_ADDRSTRLEN];
  int peer_port;
  struct Task *next;
} Task;

typedef struct {
  Task *head, *tail;
  pthread_mutex_t mutex;
  pthread_cond_t not_empty;
  int len;
} TaskQueue;

static TaskQueue task_queue;

static void queue_init(void) {
  task_queue.head = task_queue.tail = NULL;
  task_queue.len = 0;
  pthread_mutex_init(&task_queue.mutex, NULL);
  pthread_cond_init(&task_queue.not_empty, NULL);
}
static void queue_push(Task *t) {
  t->next = NULL;
  pthread_mutex_lock(&task_queue.mutex);
  if (task_queue.tail)
    task_queue.tail->next = t;
  else
    task_queue.head = t;
  task_queue.tail = t;
  task_queue.len++;
  pthread_cond_signal(&task_queue.not_empty);
  pthread_mutex_unlock(&task_queue.mutex);
}
static Task *queue_pop(void) {
  pthread_mutex_lock(&task_queue.mutex);
  while (!task_queue.head)
    pthread_cond_wait(&task_queue.not_empty, &task_queue.mutex);
  Task *t = task_queue.head;
  task_queue.head = t->next;
  if (!task_queue.head)
    task_queue.tail = NULL;
  task_queue.len--;
  pthread_mutex_unlock(&task_queue.mutex);
  return t;
}

/* ── Worker thread (consumer) ────────────────────────────────────────────── */
static void *worker_thread(void *arg) {
  (void)arg;
  while (1) {
    Task *t = queue_pop();
    int cfd = t->client_socket;

    if (t->language == 0xFF) {
      /*
       * Async text command: processed here in the worker pool rather than
       * on the main select() thread, so the event loop is never stalled.
       */
      handle_client_command(cfd, t->cmd, t->peer_ip, t->peer_port);
      free(t);
    } else {
      /* Code execution: compile + run, stream output back to client */
      char out[128];
      snprintf(out, sizeof out, "out_%d.txt", cfd);

      LangConfig *lc = find_lang((int)t->language);
      if (!lc) {
        /* Unknown language: write error to output file */
        int efd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (efd >= 0) {
          char emsg[64];
          int elen = snprintf(emsg, sizeof emsg,
                              "Unsupported language ID: %d\n", t->language);
          write(efd, emsg, (size_t)(elen > 0 ? elen : 0));
          close(efd);
        }
        LOG_WRN("Client {fd:%d,UUID:%s} unknown language %d",
                cfd, t->client_uuid, (int)t->language);
        remove(t->src_path);
      } else {
        /* Source file already on disk at t->src_path — no fwrite needed */
        const char *src = t->src_path;
        char bin[128];
        snprintf(bin, sizeof bin, "exe_%d", cfd);

        LOG_INFO("Client {fd:%d,UUID:%s} exec lang=%s src=%s tl=%us ml=%uMB",
                 cfd, t->client_uuid, lc->name, src,
                 t->time_limit_s, t->mem_limit_mb);

        if (lc->serialize)
          pthread_mutex_lock(&lc->serialize_mutex);
        int ok = 1;
        char cmd[CMD_MAX * 2];
        if (lc->compile[0]) {
          expand_cmd(cmd, sizeof cmd, lc->compile, src, bin);
          execute_cmd(cmd, out, 0, 0, 0);   /* O_TRUNC: fresh output */
          if (file_has_error(out)) {
            ok = 0;
            LOG_WRN("Client {fd:%d,UUID:%s} compile failed", cfd, t->client_uuid);
            if (!lc->fixed_src[0])
              remove(src);
          }
        }
        if (ok) {
          expand_cmd(cmd, sizeof cmd, lc->run, src, bin);
          /* Apply server default memory cap when client sends 0 */
          uint32_t eff_mem = t->mem_limit_mb > 0
                             ? t->mem_limit_mb
                             : cfg_exec_mem_default_mb;
          /* If compile ran first, append run output after compile output */
          execute_cmd(cmd, out, t->time_limit_s, eff_mem,
                      lc->compile[0] ? 1 : 0);
          if (!lc->fixed_src[0])
            remove(src);
          if (lc->compile[0])
            remove(bin);
        }
        if (lc->serialize)
          pthread_mutex_unlock(&lc->serialize_mutex);

        LOG_DBG("Client {fd:%d,UUID:%s} execution done ok=%d",
                cfd, t->client_uuid, ok);
      }

      /* Send RespHeader + output file to client */
      struct stat st;
      uint64_t fsize = 0;
      if (stat(out, &st) == 0)
        fsize = (uint64_t)st.st_size;
      RespHeader resp;
      memset(&resp, 0, sizeof resp);
      strncpy(resp.filename, "output.txt", sizeof resp.filename - 1);
      resp.file_size = hton64(fsize);
      send_all(cfd, &resp, sizeof resp);
      int out_rfd = open(out, O_RDONLY);
      if (out_rfd >= 0) {
        if (fsize > 0) {
          char chunk[65536];
          ssize_t nr;
          while ((nr = read(out_rfd, chunk, sizeof chunk)) > 0) {
            if (!send_all(cfd, chunk, (size_t)nr))
              break;
          }
        }
        close(out_rfd);
      }
      remove(out);
      free(t);
    }

    /* Notify main select() loop: this client fd is ready for the next request
     */
    ssize_t wn = write(notify_pipe[1], &cfd, sizeof cfd);
    (void)wn;
  }
  return NULL;
}

/* ── Admin session ───────────────────────────────────────────────────────── */
static int authenticate(const char *user, const char *pass) {
  int fd = open(cfg_login_file, O_RDONLY);
  if (fd < 0)
    return 0;
  char buf[4096];
  ssize_t nr = read(fd, buf, sizeof buf - 1);
  close(fd);
  if (nr <= 0)
    return 0;
  buf[nr] = '\0';
  char *p = buf;
  while (*p) {
    char fu[100] = {0}, fp2[100] = {0};
    if (sscanf(p, "%99s %99s", fu, fp2) == 2 &&
        strcmp(user, fu) == 0 && strcmp(pass, fp2) == 0)
      return 1;
    while (*p && *p != '\n') p++;
    if (*p == '\n') p++;
  }
  return 0;
}

static void handle_admin_session(int fd) {
  char buf[1024];
  ssize_t n;
  while (1) {
    n = recv(fd, buf, sizeof buf - 1, 0);
    if (n <= 0)
      break;
    buf[n] = '\0';
    buf[strcspn(buf, "\n")] = '\0';

    if (strcmp(buf, "LIST") == 0) {
      char resp[8192] = {0};
      int found = 0;
      pthread_mutex_lock(&clients_mutex);
      for (int i = 0; i <= active_maxfd; i++) {
        if (!clients[i].active)
          continue;
        char line[128];
        snprintf(line, sizeof line, "[%d] %s:%d\n", clients[i].id,
                 clients[i].ip, clients[i].port);
        strncat(resp, line, sizeof resp - strlen(resp) - 1);
        found = 1;
      }
      pthread_mutex_unlock(&clients_mutex);
      if (!found)
        strncat(resp, "No clients connected.\n",
                sizeof resp - strlen(resp) - 1);
      strncat(resp, ".\n", sizeof resp - strlen(resp) - 1);
      send(fd, resp, strlen(resp), 0);

    } else if (strncmp(buf, "KICK ", 5) == 0) {
      int target = atoi(buf + 5), found = 0, sock_to_close = -1;
      pthread_mutex_lock(&clients_mutex);
      for (int i = 0; i <= active_maxfd; i++) {
        if (clients[i].active && clients[i].id == target) {
          clients[i].active = 0;
          clients[i].kicked = 1;
          sock_to_close = clients[i].socket;
          found = 1;
          break;
        }
      }
      if (found)
        active_client_count--;
      pthread_mutex_unlock(&clients_mutex);
      if (sock_to_close != -1)
        close(sock_to_close);
      send(fd, found ? "OK\n" : "NOT_FOUND\n", found ? 3 : 10, 0);

    } else if (strcmp(buf, "STATUS") == 0) {
      pthread_mutex_lock(&clients_mutex);
      int count = active_client_count;
      pthread_mutex_unlock(&clients_mutex);
      char line[64];
      snprintf(line, sizeof line, "Active clients: %d\n", count);
      send(fd, line, strlen(line), 0);

    } else if (strcmp(buf, "STATS") == 0) {
      time_t now = time(NULL), elapsed = now - server_start_time;
      int h = (int)(elapsed / 3600), m = (int)((elapsed % 3600) / 60),
          s2 = (int)(elapsed % 60);
      struct rusage ru;
      getrusage(RUSAGE_SELF, &ru);
      double ucpu = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6;
      double scpu = ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
      double rss = 0, virt = 0;
      get_mem_mb(&rss, &virt);
      pthread_mutex_lock(&clients_mutex);
      int conns = active_client_count;
      pthread_mutex_unlock(&clients_mutex);
      char resp[2048];
      snprintf(
          resp, sizeof resp,
          "Uptime:      %dh %dm %ds\nCPU user:    %.3fs\nCPU sys:     %.3fs\n"
          "Mem RSS:     %.1f MB\nMem virtual: %.1f MB\nConnections: "
          "%d\nWorkers:     %d\n.\n",
          h, m, s2, ucpu, scpu, rss, virt, conns, cfg_workers);
      send(fd, resp, strlen(resp), 0);

    } else if (strcmp(buf, "UPTIME") == 0) {
      time_t elapsed = time(NULL) - server_start_time;
      int h = (int)(elapsed / 3600), m = (int)((elapsed % 3600) / 60),
          s2 = (int)(elapsed % 60);
      char line[64];
      snprintf(line, sizeof line, "%dh %dm %ds\n", h, m, s2);
      send(fd, line, strlen(line), 0);

    } else if (strcmp(buf, "LANGS") == 0) {
      char resp[4096] = {0};
      for (int i = 0; i < lang_count; i++) {
        LangConfig *lc = &lang_configs[i];
        char line[256];
        snprintf(line, sizeof line, "[%d] %-8s .%-6s  %s%s\n", lc->id, lc->name,
                 lc->extension, lc->compile[0] ? "compiled" : "interpreted",
                 lc->serialize ? " (serialized)" : "");
        strncat(resp, line, sizeof resp - strlen(resp) - 1);
      }
      strncat(resp, ".\n", sizeof resp - strlen(resp) - 1);
      send(fd, resp, strlen(resp), 0);

    } else if (strcmp(buf, "EXIT") == 0) {
      send(fd, "BYE\n", 4, 0);
      break;
    } else {
      send(fd, "UNKNOWN_CMD\n", 12, 0);
    }
  }
}

/* ── Admin INET session (TCP, one connection at a time) ──────────────────── */

/* Values saved before session-only SET commands; restored on disconnect */
static struct {
  int      max_source_mb_changed;
  uint64_t orig_max_source_mb;
  int      max_clients_changed;
  int      orig_max_clients;
  int      lang_compile_changed[MAX_LANGS];
  char     lang_compile_orig[MAX_LANGS][CMD_MAX];
  int      lang_run_changed[MAX_LANGS];
  char     lang_run_orig[MAX_LANGS][CMD_MAX];
} g_admsess;

static void admin_session_restore(void) {
  if (g_admsess.max_source_mb_changed) {
    cfg_max_source_mb = g_admsess.orig_max_source_mb;
    g_admsess.max_source_mb_changed = 0;
  }
  if (g_admsess.max_clients_changed) {
    cfg_max_clients = g_admsess.orig_max_clients;
    g_admsess.max_clients_changed = 0;
  }
  for (int i = 0; i < lang_count; i++) {
    if (g_admsess.lang_compile_changed[i]) {
      strncpy(lang_configs[i].compile, g_admsess.lang_compile_orig[i],
              CMD_MAX - 1);
      g_admsess.lang_compile_changed[i] = 0;
    }
    if (g_admsess.lang_run_changed[i]) {
      strncpy(lang_configs[i].run, g_admsess.lang_run_orig[i], CMD_MAX - 1);
      g_admsess.lang_run_changed[i] = 0;
    }
  }
}

/* persist=0: session-only (saved for restore); persist=1: write to file too */
static int admin_cfg_set(const char *key, const char *value, int persist) {
  if (strcmp(key, "exec_mem_default_mb") == 0) {
    int v = atoi(value);
    if (v < 0) return 0;
    cfg_exec_mem_default_mb = (uint32_t)v;
    return 1;
  } else if (strcmp(key, "max_source_mb") == 0) {
    long long v = atoll(value);
    if (v < 0) return 0;
    if (!persist && !g_admsess.max_source_mb_changed) {
      g_admsess.orig_max_source_mb    = cfg_max_source_mb;
      g_admsess.max_source_mb_changed = 1;
    }
    cfg_max_source_mb = (uint64_t)v;
  } else if (strcmp(key, "max_clients") == 0) {
    int v = atoi(value);
    if (v < 1) return 0;
    if (!persist && !g_admsess.max_clients_changed) {
      g_admsess.orig_max_clients    = cfg_max_clients;
      g_admsess.max_clients_changed = 1;
    }
    cfg_max_clients = v;
  } else if (strncmp(key, "lang.", 5) == 0) {
    int lid = atoi(key + 5);
    const char *dot2 = strchr(key + 5, '.');
    if (!dot2) return 0;
    dot2++;
    for (int i = 0; i < lang_count; i++) {
      if (lang_configs[i].id != lid) continue;
      if (strcmp(dot2, "compile") == 0) {
        if (!persist && !g_admsess.lang_compile_changed[i]) {
          strncpy(g_admsess.lang_compile_orig[i], lang_configs[i].compile,
                  CMD_MAX - 1);
          g_admsess.lang_compile_changed[i] = 1;
        }
        strncpy(lang_configs[i].compile, value, CMD_MAX - 1);
        return 1;
      }
      if (strcmp(dot2, "run") == 0) {
        if (!persist && !g_admsess.lang_run_changed[i]) {
          strncpy(g_admsess.lang_run_orig[i], lang_configs[i].run, CMD_MAX - 1);
          g_admsess.lang_run_changed[i] = 1;
        }
        strncpy(lang_configs[i].run, value, CMD_MAX - 1);
        return 1;
      }
      return 0;
    }
    return 0;
  } else {
    return 0;
  }

  if (persist) {
    config_t pcfg;
    config_init(&pcfg);
    if (config_read_file(&pcfg, cfg_path_global)) {
      config_setting_t *s = NULL;
      if (strcmp(key, "max_source_mb") == 0)
        s = config_lookup(&pcfg, "server.max_source_mb");
      else if (strcmp(key, "max_clients") == 0)
        s = config_lookup(&pcfg, "server.max_clients");
      if (s) {
        config_setting_set_int(s, (int)(strcmp(key, "max_source_mb") == 0
                                            ? (int)cfg_max_source_mb
                                            : cfg_max_clients));
        config_write_file(&pcfg, cfg_path_global);
      }
    }
    config_destroy(&pcfg);
  }
  return 1;
}

static void handle_admin_inet_session(int fd) {
  memset(&g_admsess, 0, sizeof g_admsess);
  char buf[1024];
  ssize_t n;
  while (1) {
    n = recv(fd, buf, sizeof buf - 1, 0);
    if (n <= 0) break;
    buf[n] = '\0';
    buf[strcspn(buf, "\r\n")] = '\0';

    if (strcmp(buf, "LIST") == 0) {
      char resp[32768] = {0};
      snprintf(resp, sizeof resp,
               "max_source_mb=%llu\nmax_clients=%d\nworkers=%d\n"
               "port=%d\nadmin_tcp_port=%d\nexec_mem_default_mb=%u\n",
               (unsigned long long)cfg_max_source_mb,
               cfg_max_clients, cfg_workers, cfg_port, cfg_admin_tcp_port,
               cfg_exec_mem_default_mb);
      for (int i = 0; i < lang_count; i++) {
        LangConfig *lc = &lang_configs[i];
        char line[CMD_MAX + 64];
        snprintf(line, sizeof line, "lang.%d.name=%s\n", lc->id, lc->name);
        strncat(resp, line, sizeof resp - strlen(resp) - 1);
        snprintf(line, sizeof line, "lang.%d.extension=%s\n", lc->id,
                 lc->extension);
        strncat(resp, line, sizeof resp - strlen(resp) - 1);
        snprintf(line, sizeof line, "lang.%d.compile=%s\n", lc->id,
                 lc->compile);
        strncat(resp, line, sizeof resp - strlen(resp) - 1);
        snprintf(line, sizeof line, "lang.%d.run=%s\n", lc->id, lc->run);
        strncat(resp, line, sizeof resp - strlen(resp) - 1);
      }
      strncat(resp, ".\n", sizeof resp - strlen(resp) - 1);
      send(fd, resp, strlen(resp), 0);

    } else if (strcmp(buf, "CLIENTS") == 0) {
      char resp[8192] = {0};
      int any = 0;
      pthread_mutex_lock(&clients_mutex);
      for (int i = 0; i <= active_maxfd; i++) {
        if (!clients[i].active) continue;
        char line[160];
        snprintf(line, sizeof line, "%d %s %d %s\n", clients[i].id,
                 clients[i].ip, clients[i].port, clients[i].uuid);
        strncat(resp, line, sizeof resp - strlen(resp) - 1);
        any = 1;
      }
      pthread_mutex_unlock(&clients_mutex);
      if (!any) strncat(resp, "NONE\n", sizeof resp - strlen(resp) - 1);
      strncat(resp, ".\n", sizeof resp - strlen(resp) - 1);
      send(fd, resp, strlen(resp), 0);

    } else if (strncmp(buf, "KICK ", 5) == 0) {
      int target = atoi(buf + 5), found = 0, sck = -1;
      pthread_mutex_lock(&clients_mutex);
      for (int i = 0; i <= active_maxfd; i++) {
        if (clients[i].active && clients[i].id == target) {
          clients[i].active = 0;
          clients[i].kicked = 1;
          sck = clients[i].socket;
          found = 1;
          break;
        }
      }
      if (found) active_client_count--;
      pthread_mutex_unlock(&clients_mutex);
      if (sck != -1) close(sck);
      send(fd, found ? "OK\n" : "NOT_FOUND\n", found ? 3 : 10, 0);

    } else if (strncmp(buf, "SET ", 4) == 0) {
      char key[128] = {0}, value[CMD_MAX] = {0};
      if (sscanf(buf + 4, "%127s %511[^\n]", key, value) >= 1)
        send(fd, admin_cfg_set(key, value, 0) ? "OK\n" : "ERR\n", 4, 0);
      else
        send(fd, "ERR\n", 4, 0);

    } else if (strncmp(buf, "PERSIST ", 8) == 0) {
      char key[128] = {0}, value[CMD_MAX] = {0};
      if (sscanf(buf + 8, "%127s %511[^\n]", key, value) >= 1)
        send(fd, admin_cfg_set(key, value, 1) ? "OK\n" : "ERR\n", 4, 0);
      else
        send(fd, "ERR\n", 4, 0);

    } else if (strcmp(buf, "RELOAD") == 0) {
      admin_session_restore();
      load_config(cfg_path_global);
      send(fd, "OK\n", 3, 0);

    } else if (strcmp(buf, "EXIT") == 0 || strcmp(buf, "QUIT") == 0) {
      send(fd, "BYE\n", 4, 0);
      break;
    } else {
      send(fd, "ERR_CMD\n", 8, 0);
    }
  }
}

static void *handle_admin_inet_thread(void *arg) {
  int fd = (int)(intptr_t)arg;
  char buf[256];
  ssize_t n = recv(fd, buf, sizeof buf - 1, 0);
  if (n > 0) {
    buf[n] = '\0';
    buf[strcspn(buf, "\r\n")] = '\0';
    char u[100] = {0}, p[100] = {0};
    if (strncmp(buf, "AUTH ", 5) == 0 &&
        sscanf(buf + 5, "%99s %99s", u, p) == 2 && authenticate(u, p)) {
      send(fd, "AUTH_OK\n", 8, 0);
      handle_admin_inet_session(fd);
    } else {
      send(fd, "AUTH_FAIL\n", 10, 0);
    }
  }
  admin_session_restore();
  pthread_mutex_lock(&clients_mutex);
  admin_inet_connected = 0;
  pthread_mutex_unlock(&clients_mutex);
  close(fd);
  return NULL;
}

/* ── Inet-client text-command handler ────────────────────────────────────── */
static void client_send_text(int fd, const char *cmd, const char *text) {
  RespHeader resp;
  memset(&resp, 0, sizeof resp);
  strncpy(resp.filename, cmd, sizeof resp.filename - 1);
  size_t len = strlen(text);
  resp.file_size = hton64((uint64_t)len);
  send_all(fd, &resp, sizeof resp);
  send_all(fd, text, len);
}

static void handle_client_command(int fd, const char *cmd, const char *peer_ip,
                                  int peer_port) {
  char out[8192] = {0};
  if (strcmp(cmd, "PING") == 0) {
    strcpy(out, "PONG — server is alive and responding.\n");
  } else if (strcmp(cmd, "LANGS") == 0) {
    for (int i = 0; i < lang_count; i++) {
      LangConfig *lc = &lang_configs[i];
      char line[256];
      snprintf(line, sizeof line, "[%d] %-8s  .%-6s  %s%s\n", lc->id, lc->name,
               lc->extension, lc->compile[0] ? "compiled" : "interpreted",
               lc->serialize ? " (serialized)" : "");
      strncat(out, line, sizeof out - strlen(out) - 1);
    }
  } else if (strcmp(cmd, "STATUS") == 0) {
    time_t elapsed = time(NULL) - server_start_time;
    int h = (int)(elapsed / 3600), m = (int)((elapsed % 3600) / 60),
        s2 = (int)(elapsed % 60);
    pthread_mutex_lock(&clients_mutex);
    int conns = active_client_count;
    pthread_mutex_unlock(&clients_mutex);
    snprintf(out, sizeof out,
             "Uptime:      %dh %02dm %02ds\nConnections: %d / %d max\nWorkers: "
             "    %d\n",
             h, m, s2, conns, cfg_max_clients, cfg_workers);
  } else if (strcmp(cmd, "QUEUE") == 0) {
    pthread_mutex_lock(&task_queue.mutex);
    int qlen = task_queue.len;
    pthread_mutex_unlock(&task_queue.mutex);
    snprintf(out, sizeof out,
             "Pending tasks in queue: %d\nWorker threads:         %d\n", qlen,
             cfg_workers);
  } else if (strcmp(cmd, "WHOAMI") == 0) {
    pthread_mutex_lock(&clients_mutex);
    int cid = -1;
    char uuid[37] = {0};
    if (fd >= 0 && fd < POLL_MAX_FDS && clients[fd].active) {
      cid = clients[fd].id;
      strncpy(uuid, clients[fd].uuid, 36);
    }
    pthread_mutex_unlock(&clients_mutex);
    snprintf(out, sizeof out,
             "Client ID:   %d\nUUID:        %s\nRemote addr: %s:%d\nSocket fd: "
             "  %d\n",
             cid, uuid, peer_ip, peer_port, fd);
  } else if (strcmp(cmd, "HELP") == 0) {
    strcpy(out,
           "Available commands:\n"
           "  PING    — check connectivity and measure round-trip latency\n"
           "  LANGS   — list supported programming languages\n"
           "  STATUS  — server uptime and active connection count\n"
           "  QUEUE   — current task queue depth and worker count\n"
           "  WHOAMI  — your client ID, IP address, and port\n"
           "  HELP    — this help text\n"
           "\nSend a source file via 'Run Code' to compile/run it.\n");
  } else {
    snprintf(out, sizeof out, "Unknown command: '%s'\n", cmd);
  }
  client_send_text(fd, cmd, out);
}

/* ── Admin thread ────────────────────────────────────────────────────────── */
typedef struct {
  int socket;
} AdminArg;

static void *handle_admin_thread(void *arg) {
  AdminArg *a = (AdminArg *)arg;
  int fd = a->socket;
  free(a);

  /* Admin client sends "ADMIN <user> <pass>" as its first message */
  char buf[1024];
  ssize_t n = recv(fd, buf, sizeof buf - 1, 0);
  if (n > 0) {
    buf[n] = '\0';
    char u[100], p[100];
    if (sscanf(buf + 6, "%99s %99s", u, p) == 2 && authenticate(u, p)) {
      send(fd, "AUTH_OK\n", 8, 0);
      handle_admin_session(fd);
    } else {
      send(fd, "AUTH_FAIL\n", 10, 0);
    }
  }
  pthread_mutex_lock(&clients_mutex);
  admin_connected = 0;
  pthread_mutex_unlock(&clients_mutex);
  close(fd);
  return NULL;
}

/* ── Per-client I/O state machine (called from select() loop) ────────────── */
/*
 * All INET clients start in CS_READING (no admin detection needed — admin
 * connections arrive on the separate Unix-domain socket).
 * The socket is set O_NONBLOCK in register_client; recv() returns EAGAIN
 * when no data is available, allowing the select() loop to continue.
 */
static void handle_client_data(int fd) {
  pthread_mutex_lock(&clients_mutex);
  int ok = clients[fd].active && !clients[fd].kicked;
  pthread_mutex_unlock(&clients_mutex);
  if (!ok)
    return;

  ClientInfo *c = &clients[fd];

  /* ── Phase 1: accumulate ReqHeader ─────────────────────────────────── */
  if (c->hdr_read < sizeof(ReqHeader)) {
    ssize_t n =
        recv(fd, c->hdr_buf + c->hdr_read, sizeof(ReqHeader) - c->hdr_read, 0);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return;
      disconnect_client(fd);
      return;
    }
    if (n == 0) {
      disconnect_client(fd);
      return;
    }
    c->hdr_read += (size_t)n;
    if (c->hdr_read < sizeof(ReqHeader))
      return;

    ReqHeader *req = (ReqHeader *)c->hdr_buf;

    /* Command mode (language == 0xFF): push to worker queue, do not block
     * select loop */
    if (req->language == 0xFF) {
      Task *task = calloc(1, sizeof *task);
      task->client_socket = fd;
      task->language = 0xFF;
      strncpy(task->client_uuid, c->uuid, sizeof task->client_uuid - 1);
      strncpy(task->cmd, req->filename, 64);
      strncpy(task->peer_ip, c->ip, INET_ADDRSTRLEN - 1);
      task->peer_port = c->port;
      c->hdr_read = 0;
      c->state = CS_EXECUTING;
      queue_push(task);
      return;
    }

    /* Code submission: determine destination path, open file for streaming */
    c->code_size = hton64(req->code_size); /* hton64 is its own inverse */
    c->code_written = 0;
    c->code_rejected = 0;

    {
      LangConfig *lc_hint = find_lang((int)req->language);
      if (lc_hint && lc_hint->fixed_src[0])
        strncpy(c->code_src, lc_hint->fixed_src, sizeof c->code_src - 1);
      else if (lc_hint)
        snprintf(c->code_src, sizeof c->code_src, "code_%d.%s", fd,
                 lc_hint->extension);
      else
        snprintf(c->code_src, sizeof c->code_src, "code_%d.tmp", fd);

      c->code_fd = open(c->code_src, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (c->code_fd < 0) {
        LOG_ERR("open source file '%s': %s", c->code_src, strerror(errno));
        disconnect_client(fd);
        return;
      }
    }
  }

  /* ── Phase 2: stream code bytes to disk ────────────────────────────────── */
  if (c->code_fd >= 0 && c->code_written < c->code_size) {
    char buf[65536];
    uint64_t remaining = c->code_size - c->code_written;
    size_t want = remaining > sizeof buf ? sizeof buf : (size_t)remaining;
    ssize_t n = recv(fd, buf, want, 0);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return;
      close(c->code_fd);
      c->code_fd = -1;
      remove(c->code_src);
      disconnect_client(fd);
      return;
    }
    if (n == 0) {
      close(c->code_fd);
      c->code_fd = -1;
      remove(c->code_src);
      disconnect_client(fd);
      return;
    }
    write(c->code_fd, buf, (size_t)n);
    c->code_written += (size_t)n;
    if (c->code_written < c->code_size)
      return;
  }

  /* ── All bytes received ─────────────────────────────────────────────────── */
  if (c->code_fd >= 0 && c->code_written == c->code_size) {
    close(c->code_fd);
    c->code_fd = -1;

    ReqHeader *req = (ReqHeader *)c->hdr_buf;
    Task *task = calloc(1, sizeof *task);
    task->client_socket = fd;
    task->language = req->language;
    strncpy(task->client_uuid, c->uuid, sizeof task->client_uuid - 1);
    strncpy(task->src_path, c->code_src, sizeof task->src_path - 1);
    task->time_limit_s = ntohl(req->time_limit_s);
    task->mem_limit_mb = ntohl(req->mem_limit_mb);

    LOG_INFO("Client {fd:%d,UUID:%s} queued lang=%d src=%s tl=%us ml=%uMB",
             fd, c->uuid, (int)req->language, c->code_src,
             task->time_limit_s, task->mem_limit_mb);

    c->state = CS_EXECUTING;
    c->hdr_read = 0;
    queue_push(task);
  }
}

/* ── Unix socket cleanup on exit ─────────────────────────────────────────── */
static void cleanup_unix_socket(void) { unlink(cfg_admin_socket); }

/* ── main ────────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
  /* Open session.log before load_config so all startup messages are captured */
  g_log_fd = open("session.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
  /* non-fatal if it fails — messages still go to stderr */

  const char *cfg_path = argc > 1 ? argv[1] : "server.cfg";
  load_config(cfg_path);

  /*
   * Ignore SIGPIPE so that send_all() gets EPIPE (errno) instead of a
   * fatal signal when a client disconnects during a large file transfer.
   */
  signal(SIGPIPE, SIG_IGN);

  /* Notification pipe: worker → main select() loop */
  if (pipe(notify_pipe) != 0)
    ERR_AND_EXIT("pipe");
  fcntl(notify_pipe[0], F_SETFL,
        O_NONBLOCK); /* drain loop needs non-blocking */

  /* ── INET server socket (regular clients) ── */
  int inet_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (inet_fd < 0)
    ERR_AND_EXIT("socket(AF_INET)");
  int opt = 1;
  setsockopt(inet_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
  struct sockaddr_in inet_addr = {0};
  inet_addr.sin_family = AF_INET;
  inet_addr.sin_addr.s_addr = INADDR_ANY;
  inet_addr.sin_port = htons((uint16_t)cfg_port);
  if (bind(inet_fd, (struct sockaddr *)&inet_addr, sizeof inet_addr) < 0)
    ERR_AND_EXIT("bind(inet)");
  if (listen(inet_fd, 128) < 0)
    ERR_AND_EXIT("listen(inet)");

  /* ── Unix-domain socket (admin only) ── */
  atexit(cleanup_unix_socket);
  unlink(cfg_admin_socket); /* remove stale socket file from prior run */
  int unix_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (unix_fd < 0)
    ERR_AND_EXIT("socket(AF_UNIX)");
  struct sockaddr_un unix_addr = {0};
  unix_addr.sun_family = AF_UNIX;
  strncpy(unix_addr.sun_path, cfg_admin_socket, sizeof(unix_addr.sun_path) - 1);
  if (bind(unix_fd, (struct sockaddr *)&unix_addr, sizeof unix_addr) < 0)
    ERR_AND_EXIT("bind(unix)");
  if (listen(unix_fd, 5) < 0)
    ERR_AND_EXIT("listen(unix)");

  /* ── Admin TCP socket (INET admin client) ── */
  int admin_tcp_fd = -1;
  if (cfg_admin_tcp_port > 0) {
    admin_tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (admin_tcp_fd >= 0) {
      setsockopt(admin_tcp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
      struct sockaddr_in atcp_addr = {0};
      atcp_addr.sin_family      = AF_INET;
      atcp_addr.sin_addr.s_addr = INADDR_ANY;
      atcp_addr.sin_port        = htons((uint16_t)cfg_admin_tcp_port);
      if (bind(admin_tcp_fd, (struct sockaddr *)&atcp_addr,
               sizeof atcp_addr) < 0 ||
          listen(admin_tcp_fd, 5) < 0) {
        LOG_ERR("admin_tcp bind/listen: %s", strerror(errno));
        close(admin_tcp_fd);
        admin_tcp_fd = -1;
      }
    }
  }

  server_start_time = time(NULL);
  queue_init();
  for (int i = 0; i < cfg_workers; i++) {
    pthread_t wid;
    if (pthread_create(&wid, NULL, worker_thread, NULL) != 0)
      ERR_AND_EXIT("pthread_create");
    pthread_detach(wid);
  }

  LOG_INFO("INET server listening port=%d max_clients=%d poll_max_fds=%d workers=%d",
           cfg_port, cfg_max_clients, POLL_MAX_FDS, cfg_workers);
  LOG_INFO("Admin socket: %s", cfg_admin_socket);
  if (admin_tcp_fd >= 0)
    LOG_INFO("Admin TCP port=%d", cfg_admin_tcp_port);

  /* ── poll() event loop ── */
  /* Fixed slot indices in the pfds[] array: */
  enum { PFD_INET = 0, PFD_UNIX = 1, PFD_NOTIFY = 2, PFD_ADMTCP = 3,
         PFD_FIXED = 4 };  /* client fds start at PFD_FIXED */

  struct sockaddr_in peer_in;
  socklen_t peer_len;

  while (1) {
    /* +4 fixed fds + up to POLL_MAX_FDS client fds */
    struct pollfd pfds[PFD_FIXED + POLL_MAX_FDS];

    pfds[PFD_INET].fd     = inet_fd;        pfds[PFD_INET].events    = POLLIN;
    pfds[PFD_UNIX].fd     = unix_fd;        pfds[PFD_UNIX].events    = POLLIN;
    pfds[PFD_NOTIFY].fd   = notify_pipe[0]; pfds[PFD_NOTIFY].events  = POLLIN;
    /* poll() ignores entries with fd == -1 */
    pfds[PFD_ADMTCP].fd   = admin_tcp_fd;
    pfds[PFD_ADMTCP].events = (admin_tcp_fd >= 0) ? POLLIN : 0;

    int nfds = PFD_FIXED;
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i <= active_maxfd && nfds < PFD_FIXED + POLL_MAX_FDS; i++) {
      if (clients[i].active && clients[i].state == CS_READING) {
        pfds[nfds].fd     = i;
        pfds[nfds].events = POLLIN;
        nfds++;
      }
    }
    pthread_mutex_unlock(&clients_mutex);

    int ready = poll(pfds, (nfds_t)nfds, -1);
    if (ready < 0) {
      if (errno == EINTR)
        continue;
      LOG_ERR("poll: %s", strerror(errno));
      break;
    }

    /* ── New INET connection (regular client) ── */
    if (pfds[PFD_INET].revents & POLLIN) {
      peer_len = sizeof peer_in;
      int new_fd = accept(inet_fd, (struct sockaddr *)&peer_in, &peer_len);
      if (new_fd >= 0) {
        if (new_fd >= POLL_MAX_FDS) {
          LOG_WRN("accept: fd=%d >= POLL_MAX_FDS=%d, rejecting", new_fd, POLL_MAX_FDS);
          send(new_fd, "SERVER_FULL\n", 12, 0);
          close(new_fd);
        } else {
          char ip[INET_ADDRSTRLEN];
          inet_ntop(AF_INET, &peer_in.sin_addr, ip, sizeof ip);
          int port = ntohs(peer_in.sin_port);
          if (register_client(new_fd, ip, port) < 0) {
            send(new_fd, "SERVER_FULL\n", 12, 0);
            close(new_fd);
          }
        }
      }
    }

    /* ── New Admin TCP connection ── */
    if (pfds[PFD_ADMTCP].revents & POLLIN) {
      struct sockaddr_in aaddr = {0};
      socklen_t aalen = sizeof aaddr;
      int afd = accept(admin_tcp_fd, (struct sockaddr *)&aaddr, &aalen);
      if (afd >= 0) {
        pthread_mutex_lock(&clients_mutex);
        int already = admin_inet_connected;
        if (!already) admin_inet_connected = 1;
        pthread_mutex_unlock(&clients_mutex);
        if (already) {
          send(afd, "ADMIN_BUSY\n", 11, 0);
          close(afd);
        } else {
          send(afd, "ADMIN_HELLO\n", 12, 0);
          pthread_t atid;
          if (pthread_create(&atid, NULL, handle_admin_inet_thread,
                             (void *)(intptr_t)afd) != 0) {
            pthread_mutex_lock(&clients_mutex);
            admin_inet_connected = 0;
            pthread_mutex_unlock(&clients_mutex);
            close(afd);
          } else {
            pthread_detach(atid);
          }
        }
      }
    }

    /* ── New Unix connection (admin) ── */
    if (pfds[PFD_UNIX].revents & POLLIN) {
      int admin_fd = accept(unix_fd, NULL, NULL);
      if (admin_fd >= 0) {
        pthread_mutex_lock(&clients_mutex);
        int already = admin_connected;
        if (!already)
          admin_connected = 1;
        pthread_mutex_unlock(&clients_mutex);

        if (already) {
          send(admin_fd, "ADMIN_BUSY\n", 11, 0);
          close(admin_fd);
        } else {
          AdminArg *aarg = malloc(sizeof(AdminArg));
          aarg->socket = admin_fd;
          pthread_t tid;
          if (pthread_create(&tid, NULL, handle_admin_thread, aarg) != 0) {
            free(aarg);
            pthread_mutex_lock(&clients_mutex);
            admin_connected = 0;
            pthread_mutex_unlock(&clients_mutex);
            close(admin_fd);
          } else {
            pthread_detach(tid);
          }
        }
      }
    }

    /* ── Worker-done notifications ── */
    if (pfds[PFD_NOTIFY].revents & POLLIN) {
      int cfd;
      /* Drain all pending ints (pipe read end is O_NONBLOCK) */
      while (read(notify_pipe[0], &cfd, sizeof cfd) == sizeof cfd) {
        if (cfd < 0 || cfd >= POLL_MAX_FDS)
          continue;
        pthread_mutex_lock(&clients_mutex);
        if (clients[cfd].active && clients[cfd].state == CS_EXECUTING) {
          clients[cfd].state = CS_READING;
          clients[cfd].hdr_read = 0;
          /* code_fd already closed before task was pushed */
          clients[cfd].code_fd = -1;
          clients[cfd].code_written = 0;
          clients[cfd].code_size = 0;
          clients[cfd].code_src[0] = '\0';
          clients[cfd].code_rejected = 0;
        }
        pthread_mutex_unlock(&clients_mutex);
      }
    }

    /* ── Readable client sockets ── */
    int cands[POLL_MAX_FDS], nc = 0;
    pthread_mutex_lock(&clients_mutex);
    for (int j = PFD_FIXED; j < nfds; j++) {
      if (pfds[j].revents & POLLIN) {
        int fd = pfds[j].fd;
        if (clients[fd].active && !clients[fd].kicked &&
            clients[fd].state == CS_READING)
          cands[nc++] = fd;
      }
    }
    pthread_mutex_unlock(&clients_mutex);
    for (int j = 0; j < nc; j++)
      handle_client_data(cands[j]);
  }

  close(inet_fd);
  close(unix_fd);
  return 0;
}
