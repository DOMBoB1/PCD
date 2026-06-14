/*
 * client_inet.c — Client TCP cu interfata ncurses pentru executia de cod
 *
 * Arhitectura:
 *   - Firul principal (main): gestioneaza interfata ncurses si trimite cereri
 *   - Firul de receptie (recv_thread): asteapta raspunsuri de la server in mod
 *     blocant; comunica cu firul principal prin variabilele resp_data/resp_ready
 *     protejate de mutex
 *
 * Comunicarea intre fire se face fara semnale sau pipe-uri: firul principal
 * verifica resp_ready la fiecare 50ms (timeout ncurses) si preia raspunsul
 * daca este disponibil.
 */
#include "protocol.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <ncurses.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ── Jurnalizare client ──────────────────────────────────────────────────────
 * ncurses controleaza terminalul, deci nu putem scrie la stderr/stdout fara
 * a corupe afisarea. Toate mesajele de jurnal merg exclusiv in fisierul
 * session_client.log, deschis la pornire in main().
 * ─────────────────────────────────────────────────────────────────────────── */
static int g_cli_log_fd = -1; /* descriptor de fisier pentru session_client.log */

/* cli_log — scrie o linie de jurnal formatata cu marca de timp si nivel */
static void cli_log(const char *lvl, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void cli_log(const char *lvl, const char *fmt, ...) {
  /* daca fisierul de jurnal nu a fost deschis, abandoneaza silentios */
  if (g_cli_log_fd < 0) return;
  /* obtine ora curenta pentru marca de timp */
  time_t t = time(NULL);
  struct tm tm;
  localtime_r(&t, &tm);
  char ts[16];
  strftime(ts, sizeof ts, "%H:%M:%S", &tm);
  /* formateaza corpul mesajului */
  char body[2048];
  va_list ap; va_start(ap, fmt);
  vsnprintf(body, sizeof body, fmt, ap);
  va_end(ap);
  /* asambleaza linia completa si o scrie atomic in fisier */
  char line[2200];
  int n = snprintf(line, sizeof line, "[%s] [%s] %s\n", ts, lvl, body);
  write(g_cli_log_fd, line, (n > 0 ? (size_t)n : 0));
}

/* macrocomenzi de convenienta pentru nivelurile de jurnal */
#define LOG_INFO(...)  cli_log("INF", __VA_ARGS__)
#define LOG_ERR(...)   cli_log("ERR", __VA_ARGS__)
#define LOG_DBG(...)   cli_log("DBG", __VA_ARGS__)

/* port implicit al serverului */
#define PORT 8080
/* latimea panoului de meniu din stanga */
#define MENU_W 22
/* numarul de optiuni din meniu */
#define N_ITEMS 8

/* ── Perechi de culori ncurses ───────────────────────────────────────────── */
#define CP_HDR 1 /* alb pe albastru — bara de titlu                 */
#define CP_STS 2 /* negru pe alb — bara de stare normala            */
#define CP_ERR 3 /* alb pe rosu — bara de stare pentru erori        */
#define CP_SEL 4 /* negru pe cyan — elementul selectat din meniu    */

/* ferestrele ncurses: titlu, meniu, continut, stare */
static WINDOW *hdr_win, *mnu_win, *cnt_win, *sts_win;
/* socketul TCP catre server; -1 inseamna neconectat */
static int sock = -1;

/* ── Functii de conversie ordine octeti (64 biti) ───────────────────────── */
/*
 * hton64 — converteste un intreg pe 64 de biti din ordinea gazdei in ordinea
 * retelei (big-endian). Folosim doua apeluri htonl() pe cele doua jumatati
 * de 32 biti deoarece POSIX nu garanteaza existenta htonll().
 */
static uint64_t hton64(uint64_t v) {
  return ((uint64_t)htonl((uint32_t)(v & 0xffffffffULL)) << 32) |
         (uint64_t)htonl((uint32_t)(v >> 32));
}
/* ntoh64 este inversa lui hton64 — operatia XOR de intoarcere este identica */
#define ntoh64 hton64

/* ── Primitive de retea (apelate exclusiv din firul de receptie) ───────────
 *
 * Toate functiile de receptie sunt apelate din recv_thread_func, nu din firul
 * principal, pentru a evita blocarea interfetei ncurses in timp ce se asteapta
 * date de la server.
 */

/*
 * recv_exact_sock — citeste exact n octeti din socket in buf.
 * Bucla este necesara deoarece recv() poate returna mai putini octeti decat
 * ceruti (partial reads), mai ales pentru mesaje mari.
 * Returneaza 1 la succes, 0 daca conexiunea s-a inchis sau a aparut o eroare.
 */
static int recv_exact_sock(void *buf, size_t n) {
  size_t got = 0;
  while (got < n) {
    ssize_t r = recv(sock, (char *)buf + got, n - got, 0);
    if (r <= 0)
      return 0; /* conexiune inchisa sau eroare */
    got += (size_t)r;
  }
  return 1;
}

/*
 * recv_to_file_sock — primeste fsize octeti din socket si ii scrie direct
 * in fisierul de la calea path. Fisierul este creat sau trunchiat la zero.
 * Se proceseaza in bucati de 64KB pentru a evita alocarea unui buffer mare.
 * Returneaza 1 la succes, 0 la eroare.
 */
static int recv_to_file_sock(const char *path, uint64_t fsize) {
  /* deschide fisierul de destinatie; O_TRUNC sterge continutul anterior */
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    LOG_ERR("recv_to_file_sock open '%s': %s", path, strerror(errno));
    return 0;
  }
  char chunk[65536]; /* buffer de 64KB pentru transfer in bucati */
  uint64_t rem = fsize; /* octeti ramasi de primit */
  while (rem > 0) {
    /* citeste cel mult dimensiunea buffer-ului sau cat a mai ramas */
    size_t want = rem < sizeof chunk ? (size_t)rem : sizeof chunk;
    ssize_t n = recv(sock, chunk, want, 0);
    if (n <= 0) {
      close(fd);
      return 0; /* conexiune intrerupta in mijlocul transferului */
    }
    write(fd, chunk, (size_t)n);
    rem -= (size_t)n;
  }
  close(fd);
  return 1;
}

/* ── Raspuns asincron (firul de receptie -> firul principal) ─────────────── */

/*
 * RespData — structura care transporta un raspuns complet de la firul de
 * receptie la firul principal. Transferul se face prin variabilele globale
 * resp_data/resp_ready, protejate de resp_mx.
 */
typedef struct {
  int is_file;          /* 1 daca raspunsul a fost salvat pe disc, 0 daca e text */
  char save_path[512];  /* calea fisierului salvat (valabil doar daca is_file=1) */
  uint64_t bytes;       /* dimensiunea totala a datelor primite                  */
  char *text;           /* pointer la text alocat heap; firul principal il elibereaza
                         * dupa afisare                                          */
  int error;            /* 1 daca a aparut o eroare (conexiune pierduta etc.)    */
} RespData;

/* mutex si variabile de sincronizare intre firul de receptie si cel principal */
static pthread_mutex_t resp_mx = PTHREAD_MUTEX_INITIALIZER;
static RespData resp_data = {0};
static int resp_ready = 0; /* steag: firul principal il citeste la 0 si-l reseteaza */

/*
 * hint_is_file / hint_save_path — informatii setate de firul principal
 * INAINTE de a trimite cererea, pentru a indica firului de receptie cum
 * sa trateze raspunsul. Ordinea garanteaza vizibilitatea: hint setat ->
 * octeti trimisi -> server proceseaza -> server trimite raspuns -> firul de
 * receptie se trezeste din recv() si citeste hint-ul.
 */
static pthread_mutex_t hint_mx = PTHREAD_MUTEX_INITIALIZER;
static int hint_is_file = 0;           /* 1 = salveaza raspunsul pe disc     */
static char hint_save_path[512] = {0}; /* calea unde se salveaza fisierul    */

/* ── Firul de receptie ───────────────────────────────────────────────────── */

/*
 * recv_thread_func — ruleaza in fundal si asteapta raspunsuri de la server.
 * La fiecare raspuns:
 *   1. Citeste antetul RespHeader (dimensiune fixa)
 *   2. Citeste corpul (fie in fisier, fie in memorie, conform hint-ului)
 *   3. Depune raspunsul in resp_data si seteaza resp_ready = 1
 * Firul principal preia resp_data la urmatoarea iteratie a buclei ncurses.
 */
static void *recv_thread_func(void *arg) {
  (void)arg;
  while (1) {
    /* asteapta un antet de raspuns de la server (blocat pana sosesc date) */
    RespHeader rh;
    if (!recv_exact_sock(&rh, sizeof rh)) {
      /* conexiunea s-a inchis sau a aparut o eroare — semnalizeaza firul principal */
      pthread_mutex_lock(&resp_mx);
      RespData err = {0};
      err.error = 1;
      free(resp_data.text); /* elibereaza orice text neconsumat anterior */
      resp_data = err;
      resp_ready = 1;
      pthread_mutex_unlock(&resp_mx);
      return NULL;
    }
    /* converteste dimensiunea fisierului din ordinea retelei in ordinea gazdei */
    uint64_t fsize = ntoh64(rh.file_size);

    /* citeste hint-ul setat de firul principal inainte de trimiterea cererii */
    pthread_mutex_lock(&hint_mx);
    int is_file = hint_is_file;
    char path[512];
    strncpy(path, hint_save_path, sizeof path - 1);
    pthread_mutex_unlock(&hint_mx);

    RespData rd = {0};
    if (is_file && path[0]) {
      /* cazul executiei de cod: salveaza iesirea direct pe disc */
      if (recv_to_file_sock(path, fsize)) {
        rd.is_file = 1;
        strncpy(rd.save_path, path, sizeof rd.save_path - 1);
        rd.bytes = fsize;
      } else {
        rd.error = 1;
      }
    } else {
      /* cazul comenzilor text: incarca raspunsul in memorie pentru afisare */
      char *text = malloc((size_t)fsize + 1); /* +1 pentru terminatorul nul */
      if (!text || !recv_exact_sock(text, (size_t)fsize)) {
        free(text);
        rd.error = 1;
      } else {
        text[fsize] = '\0'; /* asigura terminarea sirului */
        rd.is_file = 0;
        rd.bytes = fsize;
        rd.text = text;
      }
    }

    /* depune raspunsul si semnalizeaza firul principal */
    pthread_mutex_lock(&resp_mx);
    free(resp_data.text); /* arunca orice text neconsumat din raspunsul anterior */
    resp_data = rd;
    resp_ready = 1;
    pthread_mutex_unlock(&resp_mx);
  }
}

/* ── Functii auxiliare de interfata ncurses ──────────────────────────────── */

/*
 * init_wins — creaza cele patru ferestre ncurses ale aplicatiei:
 *   hdr_win: bara de titlu (1 linie, sus)
 *   mnu_win: panoul de meniu (stanga, MENU_W coloane)
 *   cnt_win: zona de continut (dreapta, restul coloanelor)
 *   sts_win: bara de stare (1 linie, jos)
 */
static void init_wins(void) {
  int R = LINES, C = COLS; /* dimensiunile terminalului */
  hdr_win = newwin(1, C, 0, 0);                    /* sus, latime intreaga    */
  mnu_win = newwin(R - 2, MENU_W, 1, 0);           /* stanga, sub titlu       */
  cnt_win = newwin(R - 2, C - MENU_W, 1, MENU_W);  /* dreapta, sub titlu      */
  sts_win = newwin(1, C, R - 1, 0);                /* jos, latime intreaga    */
  keypad(mnu_win, TRUE); /* permite taste speciale (sageti, Enter) in meniu   */
}

/* draw_header — redeseneaza bara de titlu centrata */
static void draw_header(void) {
  wbkgd(hdr_win, COLOR_PAIR(CP_HDR) | A_BOLD); /* fundal albastru, text ingrosat */
  werase(hdr_win);
  const char *t = "CODE EXECUTION CLIENT";
  /* centreaza textul orizontal */
  mvwprintw(hdr_win, 0, (COLS - (int)strlen(t)) / 2, "%s", t);
  wrefresh(hdr_win);
}

/*
 * set_status — actualizeaza bara de stare de jos.
 * is_error=1 foloseste culoarea de eroare (rosu), is_error=0 culoarea normala.
 */
static void set_status(const char *msg, int is_error) {
  wbkgd(sts_win, COLOR_PAIR(is_error ? CP_ERR : CP_STS));
  werase(sts_win);
  mvwprintw(sts_win, 0, 1, "%s", msg);
  wrefresh(sts_win);
}

/*
 * show_info_box — afiseaza o caseta de dialog centrata cu un mesaj scurt.
 * Folosita pentru stari tranzitorii: "Se trimite...", "Se executa...".
 * Caseta se deseneaza manual cu caracterele ACS (box-drawing characters).
 */
static void show_info_box(const char *msg) {
  werase(cnt_win);
  box(cnt_win, 0, 0);
  int R = getmaxy(cnt_win), C = getmaxx(cnt_win);
  int mlen = (int)strlen(msg);
  /* calculeaza dimensiunile casetei interioare */
  int bw = mlen + 6;
  if (bw < 30) bw = 30;
  if (bw > C - 4) bw = C - 4;
  int bh = 5, by = (R - bh) / 2, bx = (C - bw) / 2;
  /* desenaza marginea de sus a casetei */
  mvwaddch(cnt_win, by, bx, ACS_ULCORNER);
  for (int x = bx + 1; x < bx + bw - 1; x++) mvwaddch(cnt_win, by, x, ACS_HLINE);
  mvwaddch(cnt_win, by, bx + bw - 1, ACS_URCORNER);
  /* desenaza lateralele si fundul cu spatii */
  for (int y = by + 1; y < by + bh - 1; y++) {
    mvwaddch(cnt_win, y, bx, ACS_VLINE);
    for (int x = bx + 1; x < bx + bw - 1; x++) mvwaddch(cnt_win, y, x, ' ');
    mvwaddch(cnt_win, y, bx + bw - 1, ACS_VLINE);
  }
  /* desenaza marginea de jos a casetei */
  mvwaddch(cnt_win, by + bh - 1, bx, ACS_LLCORNER);
  for (int x = bx + 1; x < bx + bw - 1; x++) mvwaddch(cnt_win, by + bh - 1, x, ACS_HLINE);
  mvwaddch(cnt_win, by + bh - 1, bx + bw - 1, ACS_LRCORNER);
  /* titlul casetei pe marginea de sus */
  const char *ttl = " WORKING ";
  mvwprintw(cnt_win, by, bx + (bw - (int)strlen(ttl)) / 2, "%s", ttl);
  /* mesajul centrat in interiorul casetei, cu text ingrosat */
  wattron(cnt_win, A_BOLD);
  mvwprintw(cnt_win, by + 2, bx + (bw - mlen) / 2, "%s", msg);
  wattroff(cnt_win, A_BOLD);
  wrefresh(cnt_win);
}

/* etichetele elementelor din meniu, in ordinea indicilor */
static const char *labels[N_ITEMS] = {
    "Run Code",    "Ping Server", "Languages", "Server Status",
    "Queue Depth", "Who Am I",    "Help",      "Quit",
};

/*
 * draw_menu — redeseneaza panoul de meniu cu elementul sel evidentiat.
 * sel=-1 inseamna niciun element selectat (folosit in timpul dialogului
 * "Run Code").
 */
static void draw_menu(int sel) {
  werase(mnu_win);
  box(mnu_win, 0, 0); /* chenar */
  wattron(mnu_win, A_BOLD);
  mvwprintw(mnu_win, 1, 2, "MENU");
  wattroff(mnu_win, A_BOLD);
  for (int i = 0; i < N_ITEMS; i++) {
    if (i == sel) {
      /* evidentiaza elementul curent cu culoarea de selectie */
      wattron(mnu_win, COLOR_PAIR(CP_SEL) | A_BOLD);
      mvwprintw(mnu_win, 3 + i, 1, " %-*s", MENU_W - 3, labels[i]);
      wattroff(mnu_win, COLOR_PAIR(CP_SEL) | A_BOLD);
    } else {
      mvwprintw(mnu_win, 3 + i, 1, " %-*s", MENU_W - 3, labels[i]);
    }
  }
  wrefresh(mnu_win);
}

/*
 * show_content — afiseaza text in zona de continut cu chenar si titlu.
 * Textul este randat caracter cu caracter; newline-urile avanseaza la linia
 * urmatoare, iar caracterele care nu incap pe linie sunt trunchiate.
 */
static void show_content(const char *title, const char *text) {
  werase(cnt_win);
  box(cnt_win, 0, 0);
  wattron(cnt_win, A_BOLD);
  mvwprintw(cnt_win, 0, 2, " %s ", title);
  wattroff(cnt_win, A_BOLD);
  /* limiteaza randarea la dimensiunile ferestrei */
  int mr = getmaxy(cnt_win) - 2, mc = getmaxx(cnt_win) - 3, r = 1, c = 1;
  for (const char *p = text; *p && r <= mr; p++) {
    if (*p == '\n') {
      r++; /* trece la linia urmatoare */
      c = 1;
    } else if (c <= mc) {
      mvwaddch(cnt_win, r, c++, (unsigned char)*p);
    }
  }
  wrefresh(cnt_win);
}

/*
 * show_output_content — afiseaza continutul fisierului de iesire in zona de
 * continut. Daca fisierul este mai mare decat fereastra, afiseaza un mesaj
 * de trunchiere si indica utilizatorului sa deschida fisierul direct.
 * Citeste fisierul cu open/read/close (nu fopen) conform conventiei proiectului.
 */
static void show_output_content(const char *saved_as, uint64_t total_bytes) {
  werase(cnt_win);
  box(cnt_win, 0, 0);
  wattron(cnt_win, A_BOLD);
  mvwprintw(cnt_win, 0, 2, " OUTPUT — %s ", saved_as);
  wattroff(cnt_win, A_BOLD);
  int max_rows = getmaxy(cnt_win) - 2, max_cols = getmaxx(cnt_win) - 4;
  /* deschide fisierul salvat pe disc */
  int fd = open(saved_as, O_RDONLY);
  if (fd < 0) {
    mvwprintw(cnt_win, 1, 2, "(could not open %s)", saved_as);
    wrefresh(cnt_win);
    return;
  }
  /* calculeaza cati octeti sunt necesari pentru a umple fereastra */
  size_t cap = (size_t)(max_rows + 1) * ((size_t)max_cols + 2);
  char *buf = malloc(cap + 1);
  int truncated = 0;
  if (buf) {
    ssize_t nr = read(fd, buf, cap); /* citeste cel mult cat incape pe ecran */
    close(fd);
    if (nr > 0) {
      buf[nr] = '\0';
      /* marcheaza trunchierea daca fisierul e mai mare decat ce am citit */
      truncated = (uint64_t)nr < total_bytes;
      /* randeaza linie cu linie in fereastra */
      char *p = buf;
      int row = 1;
      while (row <= max_rows && *p) {
        char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if ((int)len > max_cols) len = (size_t)max_cols; /* trunchiaza linia lunga */
        mvwaddnstr(cnt_win, row++, 2, p, (int)len);
        p = nl ? nl + 1 : p + len;
        if (!nl) break;
      }
    }
    free(buf);
  } else {
    close(fd); /* malloc a esuat; inchide fisierul si continua fara continut */
  }
  /* afiseaza un aviz de trunchiere la final daca fisierul nu a incaput */
  if (truncated) {
    char tail[256];
    snprintf(tail, sizeof tail, "... %llu bytes total — see %s for full output",
             (unsigned long long)total_bytes, saved_as);
    if ((int)strlen(tail) > max_cols)
      tail[max_cols] = '\0';
    wattron(cnt_win, A_DIM);
    mvwprintw(cnt_win, max_rows + 1, 2, "%s", tail);
    wattroff(cnt_win, A_DIM);
  }
  wrefresh(cnt_win);
}

/* ── Functii ajutatoare pentru fisiere si limbaje ────────────────────────── */

/*
 * make_output_name — genereaza numele fisierului de iesire din numele fisierului
 * sursa. Ex: "examples/code.py" -> "output_code.txt".
 * Fisierul de iesire este salvat in directorul curent, nu langa sursa.
 */
static void make_output_name(char *out, size_t sz, const char *src_path) {
  /* extrage doar numele fisierului (fara cale) */
  const char *base = strrchr(src_path, '/');
  base = base ? base + 1 : src_path;
  /* copiaza si elimina extensia */
  char stem[256] = {0};
  strncpy(stem, base, sizeof stem - 1);
  char *dot = strrchr(stem, '.');
  if (dot)
    *dot = '\0'; /* termina sirul la punctul extensiei */
  snprintf(out, sz, "output_%s.txt", stem);
}

/*
 * detect_language — determina codul numeric al limbajului din extensia fisierului.
 * Returneaza 0 pentru extensii necunoscute (clientul va raporta eroare).
 * Codurile numerice sunt definite in protocol.h (implicit prin conventie).
 */
static int detect_language(const char *path) {
  const char *d = strrchr(path, '.'); /* cauta ultimul punct din cale */
  if (!d) return 0;
  d++; /* trece peste punct la extensia propriu-zisa */
  /* compara extensia cu toate limbajele suportate */
  if (strcmp(d, "c")     == 0) return 1;
  if (strcmp(d, "py")    == 0) return 2;
  if (strcmp(d, "cpp")   == 0) return 3;
  if (strcmp(d, "java")  == 0) return 4;
  if (strcmp(d, "sh")    == 0) return 5;
  if (strcmp(d, "js")    == 0) return 6;
  if (strcmp(d, "rb")    == 0) return 7;
  if (strcmp(d, "go")    == 0) return 8;
  if (strcmp(d, "rs")    == 0) return 9;
  if (strcmp(d, "php")   == 0) return 10;
  if (strcmp(d, "pl")    == 0) return 11;
  if (strcmp(d, "ts")    == 0) return 12;
  if (strcmp(d, "hs")    == 0) return 13;
  if (strcmp(d, "lua")   == 0) return 14;
  if (strcmp(d, "r")     == 0) return 15;
  if (strcmp(d, "swift") == 0) return 16;
  if (strcmp(d, "kt")    == 0) return 17;
  if (strcmp(d, "f90")   == 0) return 18;
  return 0; /* extensie necunoscuta */
}

/* lang_name — returneaza un sir lizibil pentru codul de limbaj (pentru afisare) */
static const char *lang_name(int id) {
  switch (id) {
  case 1:  return "C";
  case 2:  return "Python";
  case 3:  return "C++";
  case 4:  return "Java";
  default: return "Unknown";
  }
}

/* ── Starea cererii in curs (numai firul principal) ──────────────────────── */

static int waiting = 0;      /* 1 cat timp se asteapta un raspuns de la server */
static int pending_sel = -1; /* indicele elementului de meniu al cererii curente */
static struct timespec ping_t0; /* momentul trimiterii PING pentru masurarea RTT */

/* numele comenzilor corespunzatoare indicilor de meniu 1-6 */
static const char *cmd_names[] = {NULL, "PING", "LANGS", "STATUS",
                                  "QUEUE", "WHOAMI", "HELP"};
/* titlurile afisate in zona de continut pentru fiecare comanda */
static const char *cmd_titles[] = {
    NULL, "PING", "SUPPORTED LANGUAGES", "SERVER STATUS", "QUEUE DEPTH",
    "WHO AM I", "HELP"};

/* ── Afisarea raspunsului (firul principal, la detectarea resp_ready) ────── */

/*
 * handle_response — proceseaza un raspuns primit de la server si actualizeaza
 * interfata. Distinge intre fisiere (iesire cod) si text (comenzi).
 * Elibereaza rd->text dupa afisare (firul principal detine memoria).
 */
static void handle_response(RespData *rd) {
  waiting = 0; /* deblocheaza meniul pentru urmatoarea actiune */
  if (rd->error) {
    set_status("Connection lost.", 1); /* eroare critica — conexiunea a cazut */
    return;
  }

  if (rd->is_file) {
    /* raspuns la executie cod: afiseaza continutul fisierului salvat */
    show_output_content(rd->save_path, rd->bytes);
    char st[256];
    snprintf(st, sizeof st,
             "Saved '%s'  (%llu bytes)  — Arrow keys: navigate   q: quit",
             rd->save_path, (unsigned long long)rd->bytes);
    set_status(st, 0);
  } else {
    /* raspuns text la o comanda */
    const char *title = (pending_sel >= 1 && pending_sel <= 6)
                            ? cmd_titles[pending_sel]
                            : "RESPONSE";

    if (pending_sel == 1) {
      /* PING: adauga latenta RTT masurata la raspunsul serverului */
      struct timespec t1;
      clock_gettime(CLOCK_MONOTONIC, &t1);
      long ms = (t1.tv_sec - ping_t0.tv_sec) * 1000 +
                (t1.tv_nsec - ping_t0.tv_nsec) / 1000000L;
      char msg[8320];
      snprintf(msg, sizeof msg, "%s\nRound-trip latency: %ld ms\n",
               rd->text ? rd->text : "", ms);
      show_content(title, msg);
    } else {
      show_content(title, rd->text ? rd->text : "");
    }
    free(rd->text); /* elibereaza memoria alocata de firul de receptie */
    rd->text = NULL;
    set_status("Arrow keys: navigate   Enter: select   q: quit", 0);
  }
}

/* ── Trimiterea asincroma a unei comenzi text ─────────────────────────────── */

/*
 * send_cmd_async — trimite o comanda text (PING, LANGS, STATUS etc.) la server.
 * Seteaza hint_is_file=0 inainte de trimitere, pentru ca firul de receptie
 * sa stie ca raspunsul este text, nu un fisier de salvat pe disc.
 */
static void send_cmd_async(int sel) {
  /* informeaza firul de receptie ca raspunsul este text, nu fisier */
  pthread_mutex_lock(&hint_mx);
  hint_is_file = 0;
  hint_save_path[0] = '\0';
  pthread_mutex_unlock(&hint_mx);

  /* marcheaza momentul de inceput pentru masurarea RTT la PING */
  if (sel == 1)
    clock_gettime(CLOCK_MONOTONIC, &ping_t0);
  pending_sel = sel;
  waiting = 1; /* blocheaza inputul pana la primirea raspunsului */

  /* construieste antetul cererii cu language=0xFF pentru comenzi text */
  ReqHeader req;
  memset(&req, 0, sizeof req);
  req.language = 0xFF;  /* valoarea speciala care indica o comanda, nu cod sursa */
  req.code_size = htonl(0); /* nu urmeaza bytes de cod */
  strncpy(req.filename, cmd_names[sel], sizeof req.filename - 1);
  send(sock, &req, sizeof req, 0);
  show_info_box("Loading\342\200\246"); /* afiseaza indicator de asteptare */
  set_status("Loading\342\200\246", 0);
}

/* ── Dialogul "Run Code" ─────────────────────────────────────────────────── */

/*
 * do_run_code — afiseaza un dialog de intrare, citeste calea fisierului sursa,
 * limita de timp si limita de memorie, apoi trimite codul la server.
 * Fisierul este citit si trimis in bucati de 64KB fara a fi incarcat complet
 * in memorie — important pentru fisiere mari.
 */
static void do_run_code(void) {
  /* creaza fereastra de dialog centrata */
  int dh = 11, dw = 62;
  WINDOW *dlg = newwin(dh, dw, (LINES - dh) / 2, (COLS - dw) / 2);
  keypad(dlg, TRUE);
  box(dlg, 0, 0);
  wattron(dlg, A_BOLD);
  mvwprintw(dlg, 0, (dw - 12) / 2, " RUN CODE ");
  wattroff(dlg, A_BOLD);
  /* etichete pentru campurile de intrare */
  mvwprintw(dlg, 2, 3, "Source file path (blank to cancel):");
  mvwprintw(dlg, 3, 3, "> ");
  mvwprintw(dlg, 5, 3, "Time limit in seconds (0 = no limit):");
  mvwprintw(dlg, 6, 3, "> ");
  mvwprintw(dlg, 8, 3, "Memory limit in MB (0 = no limit):");
  mvwprintw(dlg, 9, 3, "> ");
  wrefresh(dlg);

  /* citeste datele introduse de utilizator */
  char filename[512] = {0};
  char tlimit_str[16] = {0};
  char mlimit_str[16] = {0};
  echo();    /* activeaza ecoul caracterelor tastate */
  curs_set(1); /* afiseaza cursorul */
  mvwgetnstr(dlg, 3, 5, filename, (int)sizeof filename - 1);
  mvwgetnstr(dlg, 6, 5, tlimit_str, (int)sizeof tlimit_str - 1);
  mvwgetnstr(dlg, 9, 5, mlimit_str, (int)sizeof mlimit_str - 1);
  noecho();
  curs_set(0);
  delwin(dlg);
  touchwin(stdscr); /* marcheaza toata fereastra pentru reafisare */
  refresh();
  draw_header();
  draw_menu(-1); /* redeseneaza fara selectie in timpul procesarii */

  /* elimina newline-ul de la sfarsitul numelui de fisier */
  filename[strcspn(filename, "\n")] = '\0';
  if (filename[0] == '\0') {
    set_status("Cancelled.", 0); /* utilizatorul a anulat */
    return;
  }

  /* detecteaza limbajul din extensia fisierului */
  int lang = detect_language(filename);
  if (lang == 0) {
    set_status("Unsupported extension. Supported: .c .py .cpp .java", 1);
    return;
  }

  /* converteste limitele din siruri de caractere in numere intregi */
  uint32_t time_limit = (uint32_t)strtoul(tlimit_str, NULL, 10);
  uint32_t mem_limit  = (uint32_t)strtoul(mlimit_str, NULL, 10);

  /* obtine dimensiunea fisierului cu stat() fara a-l deschide */
  struct stat st_buf;
  if (stat(filename, &st_buf) != 0) {
    char msg[256];
    snprintf(msg, sizeof msg, "Cannot stat '%s'", filename);
    set_status(msg, 1);
    return;
  }
  uint64_t code_size = (uint64_t)st_buf.st_size;

  /* deschide fisierul sursa pentru citire */
  int src_fd = open(filename, O_RDONLY);
  if (src_fd < 0) {
    char msg[256];
    snprintf(msg, sizeof msg, "Cannot open '%s'", filename);
    set_status(msg, 1);
    LOG_ERR("open source '%s': %s", filename, strerror(errno));
    return;
  }

  /* construieste antetul cererii cu toti parametrii */
  ReqHeader req;
  memset(&req, 0, sizeof req);
  req.language     = (uint8_t)lang;
  req.code_size    = hton64(code_size);      /* dimensiunea in octeti, big-endian */
  req.time_limit_s = htonl(time_limit);      /* limita de timp, big-endian        */
  req.mem_limit_mb = htonl(mem_limit);       /* limita de memorie, big-endian     */
  strncpy(req.filename, filename, sizeof req.filename - 1);

  /* IMPORTANT: seteaza hint-ul INAINTE de a trimite primul octet
   * Garanteaza ca firul de receptie stie sa salveze raspunsul pe disc
   * inainte ca serverul sa trimita vreo data */
  char out_name[512];
  make_output_name(out_name, sizeof out_name, filename);
  pthread_mutex_lock(&hint_mx);
  hint_is_file = 1;
  strncpy(hint_save_path, out_name, sizeof hint_save_path - 1);
  pthread_mutex_unlock(&hint_mx);

  pending_sel = 0; /* indicele 0 = "Run Code" */
  waiting = 1;     /* blocheaza meniul pana la primirea raspunsului */

  /* afiseaza informatii despre trimitere */
  char st[160];
  snprintf(st, sizeof st, "Sending %llu bytes (%s)\342\200\246",
           (unsigned long long)code_size, lang_name(lang));
  show_info_box(st);
  set_status(st, 0);
  LOG_INFO("sending %s lang=%d size=%llu", filename, lang,
           (unsigned long long)code_size);

  /* trimite mai intai antetul, apoi codul sursa in bucati
   * Nu incarcam intregul fisier in memorie — important pentru fisiere mari */
  send(sock, &req, sizeof req, 0);
  char chunk[65536]; /* buffer de transfer de 64KB */
  ssize_t nr;
  while ((nr = read(src_fd, chunk, sizeof chunk)) > 0) {
    /* trimite fiecare bucata complet (send() poate trimite partial) */
    size_t sent = 0;
    while (sent < (size_t)nr) {
      ssize_t s = send(sock, chunk + sent, (size_t)nr - sent, 0);
      if (s <= 0) {
        close(src_fd);
        set_status("Send error.", 1);
        LOG_ERR("send error after %llu bytes", (unsigned long long)code_size);
        return;
      }
      sent += (size_t)s;
    }
  }
  close(src_fd);
  /* raspunsul va sosi asincron prin recv_thread -> resp_ready */
  show_info_box("Executing on server \342\200\224 please wait\342\200\246");
  set_status("Executing on server \342\200\224 please wait\342\200\246", 0);
}

/* ── Functia principala ──────────────────────────────────────────────────── */
int main(void) {
  /* deschide fisierul de jurnal in modul append inainte de orice altceva */
  g_cli_log_fd = open("session_client.log", O_WRONLY | O_CREAT | O_APPEND, 0644);

  /* configureaza adresa serverului (localhost:8080) */
  struct sockaddr_in srv = {0};
  srv.sin_family = AF_INET;
  srv.sin_port   = htons(PORT);
  if (inet_pton(AF_INET, "127.0.0.1", &srv.sin_addr) <= 0) {
    LOG_ERR("inet_pton: %s", strerror(errno));
    fprintf(stderr, "inet_pton: %s\n", strerror(errno));
    return 1;
  }

  /* creeaza socketul TCP si conecteaza-te la server */
  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    LOG_ERR("socket: %s", strerror(errno));
    fprintf(stderr, "socket: %s\n", strerror(errno));
    return 1;
  }
  if (connect(sock, (struct sockaddr *)&srv, sizeof srv) < 0) {
    LOG_ERR("connect: %s", strerror(errno));
    fprintf(stderr, "connect: %s\n", strerror(errno));
    close(sock);
    return 1;
  }
  LOG_INFO("connected to server port=%d", PORT);

  /* porneste firul de receptie care asteapta raspunsuri de la server */
  pthread_t rtid;
  if (pthread_create(&rtid, NULL, recv_thread_func, NULL) != 0) {
    LOG_ERR("pthread_create: %s", strerror(errno));
    fprintf(stderr, "pthread_create: %s\n", strerror(errno));
    close(sock);
    return 1;
  }
  pthread_detach(rtid); /* nu mai avem nevoie sa asteptam firul la iesire */

  /* initializeaza ncurses */
  setlocale(LC_ALL, ""); /* necesara pentru caractere UTF-8 */
  initscr();
  start_color();
  cbreak();   /* citeste tastele imediat, fara a astepta Enter */
  noecho();   /* nu afisa tastele apasate automat */
  curs_set(0); /* ascunde cursorul in mod normal */
  /* defineste perechile de culori folosite in interfata */
  init_pair(CP_HDR, COLOR_WHITE, COLOR_BLUE);
  init_pair(CP_STS, COLOR_BLACK, COLOR_WHITE);
  init_pair(CP_ERR, COLOR_WHITE, COLOR_RED);
  init_pair(CP_SEL, COLOR_BLACK, COLOR_CYAN);

  /* creaza ferestrele si afiseaza starea initiala */
  init_wins();
  draw_header();
  int sel = 0;
  draw_menu(sel);
  show_content("OUTPUT", "Select an action from the menu.\n\n"
                         "  Run Code      — compile and run a source file\n"
                         "  Ping Server   — check connectivity and latency\n"
                         "  Languages     — list supported languages\n"
                         "  Server Status — uptime and connections\n"
                         "  Queue Depth   — pending task count\n"
                         "  Who Am I      — your client ID and address\n"
                         "  Help          — full command reference\n");
  set_status("Arrow keys: navigate   Enter: select   q: quit", 0);

  /* timeout de 50ms: wgetch returneaza ERR daca nu s-a apasat nicio tasta,
   * permitand buclei sa verifice resp_ready la fiecare ~50ms fara blocare */
  wtimeout(mnu_win, 50);

  /* ── Bucla principala de evenimente ncurses ── */
  while (1) {
    int ch = wgetch(mnu_win); /* asteapta o tasta sau expira timeout-ul de 50ms */

    /* verifica daca firul de receptie a depus un raspuns nou */
    pthread_mutex_lock(&resp_mx);
    int have = resp_ready;
    resp_ready = 0; /* reseteaza steagul */
    RespData rd = resp_data;
    resp_data.text = NULL; /* firul principal preia posesia buffer-ului de text */
    pthread_mutex_unlock(&resp_mx);
    if (have) {
      /* proceseaza si afiseaza raspunsul primit */
      handle_response(&rd);
      draw_menu(sel); /* redeseneaza meniul (poate fi sters de show_content) */
    }

    if (ch == ERR)
      continue; /* timeout de 50ms fara tasta — continua bucla */

    /* proceseaza tasta apasata */
    switch (ch) {
    case KEY_UP:
      /* navigare in sus in meniu cu revenire circulara */
      sel = (sel - 1 + N_ITEMS) % N_ITEMS;
      draw_menu(sel);
      break;
    case KEY_DOWN:
      /* navigare in jos in meniu cu revenire circulara */
      sel = (sel + 1) % N_ITEMS;
      draw_menu(sel);
      break;
    case '\n':
    case KEY_ENTER:
      if (waiting) {
        /* nu permite o noua actiune cat timp serverul nu a raspuns */
        show_info_box("Waiting for server response\342\200\246");
        set_status("Waiting for server response\342\200\246", 0);
        break;
      }
      if (sel == 0) {
        /* optiunea "Run Code" — deschide dialogul de trimitere fisier */
        do_run_code();
        draw_menu(sel);
      } else if (sel == 7) {
        /* optiunea "Quit" — iese din aplicatie */
        goto done;
      } else {
        /* toate celelalte optiuni sunt comenzi text trimise asincron */
        send_cmd_async(sel);
      }
      break;
    case 'q':
    case 'Q':
      goto done; /* scurtatura de iesire rapida */
    }
  }

done:
  /* curata resursele ncurses si inchide socketul */
  endwin();
  close(sock);
  return 0;
}
