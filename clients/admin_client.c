/*
 * admin_client.c
 *
 * Client de administrare cu interfata grafica in terminal (ncurses) pentru
 * serverul de executie cod. Se conecteaza la serverul principal prin socket
 * Unix, se autentifica cu credentiale de administrator si permite executarea
 * comenzilor administrative: listare clienti, expulzare client, stare server,
 * statistici, timp de functionare si limbaje suportate.
 */

#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* Calea implicita catre socket-ul Unix al serverului de admin.
 * Poate fi suprascris prin argumentul de linie de comanda (argv[1]). */
#define ADMIN_SOCK                                                             \
  "/tmp/server_admin.sock"

/* Latimea panoului de meniu din stanga ecranului (in coloane de caractere) */
#define MENU_W 24

/* Dimensiunea bufferului principal pentru continut primit de la server (64 KB).
 * Trebuie sa fie suficient de mare pentru raspunsuri cu mai multe linii
 * (ex: lista de clienti conectati sau statistici detaliate). */
#define CBUF (64 * 1024)

/* Descriptor de fisier global pentru socket-ul de conexiune la server.
 * Este global pentru a putea fi accesat din toate functiile de retea
 * fara a fi transmis ca parametru. */
static int sock;

/* ── functii auxiliare de retea ─────────────────────────────────────── */

/*
 * net_send - trimite un sir de caractere catre server
 *
 * Trimite intregul continut al sirului 's' prin socket-ul global 'sock'.
 * Nu adauga terminatorul null - trimite exact strlen(s) octeti.
 *
 * Parametri:
 *   s - sirul de trimis (se asteapta terminat cu '\n' pentru protocoale
 *       linie-cu-linie)
 */
static void net_send(const char *s) { send(sock, s, strlen(s), 0); }

/*
 * net_recv_line - citeste o singura linie de la server
 *
 * Citeste caracter cu caracter din socket pana intalneste '\n' sau epuizeaza
 * bufferul. Acest mod de citire (byte cu byte) este mai lent decat citirea
 * in bloc, dar asigura ca nu se citeste prea mult si nu se pierd date din
 * raspunsuri ulterioare.
 *
 * Parametri:
 *   buf - bufferul in care se stocheaza linia citita
 *   sz  - dimensiunea maxima a bufferului (inclusiv terminatorul null)
 *
 * Returneaza:
 *   1 daca s-a citit cel putin un caracter (linie valida)
 *   0 daca conexiunea a fost inchisa sau a aparut o eroare
 */
static int net_recv_line(char *buf, int sz) {
  int i = 0;
  while (i < sz - 1) {
    char c;
    /* Citim un singur octet; recv returneaza <= 0 la inchiderea conexiunii */
    if (recv(sock, &c, 1, 0) <= 0)
      return 0;
    buf[i++] = c;
    /* Ne oprim la sfarsitul liniei, dar includem '\n' in buffer */
    if (c == '\n')
      break;
  }
  buf[i] = '\0'; /* terminam sirul in mod corespunzator */
  return i > 0;
}

/*
 * net_recv_multi - citeste un raspuns cu mai multe linii de la server
 *
 * Citeste linii succesive pana intalneste linia marcator ".\n" (punct simplu
 * urmat de newline), care semnaleaza sfarsitul raspunsului multi-linie.
 * Acest protocol este similar cu SMTP/POP3 si permite serverului sa trimita
 * continut de lungime variabila fara a comunica in prealabil lungimea.
 *
 * Parametri:
 *   out - bufferul de iesire unde se acumuleaza toate liniile primite
 *   sz  - dimensiunea maxima a bufferului de iesire
 */
static void net_recv_multi(char *out, int sz) {
  int pos = 0;    /* pozitia curenta de scriere in bufferul de iesire */
  char line[512]; /* buffer temporar pentru fiecare linie citita */

  while (net_recv_line(line, sizeof line)) {
    /* Linia ".\n" este marcatorul de sfarsit de raspuns multi-linie */
    if (strcmp(line, ".\n") == 0)
      break;
    int n = (int)strlen(line);
    /* Copiem linia in bufferul de iesire doar daca mai incape,
     * prevenind depasirea de buffer (buffer overflow) */
    if (pos + n < sz - 1) {
      memcpy(out + pos, line, n);
      pos += n;
    }
  }
  out[pos] = '\0'; /* terminam sirul acumulat */
}

/* ── definitii perechi de culori ncurses ───────────────────────────── */

/* Identificatori pentru perechile de culori folosite in interfata.
 * ncurses necesita identificatori numerici (1-based) pentru perechi culoare. */
#define CP_HDR 1 /* header-ul din partea de sus: text alb bold pe fond albastru */
#define CP_STS 2 /* bara de stare din partea de jos: text negru pe fond alb */
#define CP_SEL 3 /* elementul de meniu selectat: text negru pe fond cyan */

/* ── variabile globale pentru ferestrele ncurses ───────────────────── */

/*
 * Patru ferestre ncurses care compun interfata:
 *   hdr - header (1 linie sus) - afiseaza titlul aplicatiei
 *   mnu - meniu (coloana stanga) - lista de actiuni disponibile
 *   cnt - continut (zona principala) - afiseaza rezultatele comenzilor
 *   sts - status (1 linie jos) - indicatii de tastatura si mesaje
 */
static WINDOW *hdr, *mnu, *cnt, *sts;

/*
 * init_wins - initializeaza si pozitioneaza cele patru ferestre ncurses
 *
 * Calculeaza dimensiunile ferestrelor in functie de dimensiunea curenta a
 * terminalului (LINES x COLS) si le creeaza. Activeaza citirea tastelor
 * speciale (sageti, Enter) pentru fereastra de meniu si de status.
 *
 * Nu are parametri si nu returneaza nimic. Apelat o singura data la pornire.
 */
static void init_wins(void) {
  int R = LINES, C = COLS; /* dimensiunile curente ale terminalului */

  /* Header: 1 linie, latimea totala, pozitionat sus */
  hdr = newwin(1, C, 0, 0);

  /* Meniu: inaltimea disponibila (minus header si status), latimea MENU_W,
   * pozitionat in stanga */
  mnu = newwin(R - 2, MENU_W, 1, 0);

  /* Continut: inaltimea disponibila, latimea ramasa, pozitionat la dreapta
   * meniului */
  cnt = newwin(R - 2, C - MENU_W, 1, MENU_W);

  /* Status: 1 linie, latimea totala, pozitionat jos */
  sts = newwin(1, C, R - 1, 0);

  /* Activam interpretarea tastelor speciale (sageti, F-keys etc.)
   * pentru ferestrele interactive */
  keypad(mnu, TRUE);
  keypad(sts, TRUE);
}

/*
 * draw_header - deseneaza bara de titlu din partea de sus a ecranului
 *
 * Seteaza culoarea de fundal, sterge continutul anterior si afiseaza titlul
 * centrat al aplicatiei. Centrarea se calculeaza dinamic in functie de
 * latimea curenta a terminalului, astfel incat sa arate corect la orice
 * dimensiune de fereastra.
 *
 * Nu are parametri si nu returneaza nimic.
 */
static void draw_header(void) {
  /* Setam fundalul ferestrei cu perechea de culori CP_HDR si atribut bold */
  wbkgd(hdr, COLOR_PAIR(CP_HDR) | A_BOLD);
  werase(hdr); /* stergem continutul anterior al ferestrei */

  const char *t = "SERVER ADMIN PANEL";
  /* Calculam pozitia de start pentru centrare orizontala */
  mvwprintw(hdr, 0, (COLS - (int)strlen(t)) / 2, "%s", t);
  wrefresh(hdr); /* actualizam ecranul fizic */
}

/*
 * draw_status - afiseaza un mesaj in bara de status din partea de jos
 *
 * Actualizeza bara de status cu mesajul dat. Este folosita atat pentru
 * indicatii permanente de navigare cat si pentru mesaje temporare de eroare
 * sau confirmare dupa executarea unei comenzi.
 *
 * Parametri:
 *   msg - sirul de afisat in bara de status
 */
static void draw_status(const char *msg) {
  wbkgd(sts, COLOR_PAIR(CP_STS)); /* setam culoarea barei de status */
  werase(sts);
  mvwprintw(sts, 0, 1, "%s", msg); /* afisam cu un spatiu de margine in stanga */
  wrefresh(sts);
}

/* Numarul total de optiuni din meniu */
#define N_ITEMS 7

/* Etichetele vizibile ale optiunilor de meniu, in ordinea afisarii.
 * Ordinea trebuie sa corespunda cu blocul switch din bucla principala,
 * deoarece indexul din array este folosit direct ca selector de actiune. */
static const char *labels[N_ITEMS] = {
    "List clients", /* 0 - listeaza clientii conectati */
    "Kick client",  /* 1 - expulzeaza un client dupa ID */
    "Status",       /* 2 - starea generala a serverului */
    "Stats",        /* 3 - statistici detaliate */
    "Uptime",       /* 4 - timpul de functionare al serverului */
    "Languages",    /* 5 - limbajele de programare suportate */
    "Quit",         /* 6 - iesire din panoul de admin */
};

/*
 * draw_menu - deseneaza panoul de meniu cu optiunea selectata evidentiata
 *
 * Redeseneaza complet meniul la fiecare schimbare de selectie. Elementul
 * selectat este afisat cu culori inverse (CP_SEL) si bold pentru vizibilitate.
 * Folosim formatare cu latime fixa (%-*s) pentru a asigura ca toate randurile
 * sunt de aceeasi latime si selectia acopera toata zona elementului.
 *
 * Parametri:
 *   sel - indexul (0-based) al elementului curent selectat
 */
static void draw_menu(int sel) {
  werase(mnu);
  box(mnu, 0, 0); /* desenam un chenar in jurul meniului */

  /* Titlul sectiunii de meniu, afisat cu bold */
  wattron(mnu, A_BOLD);
  mvwprintw(mnu, 1, 2, "MENU");
  wattroff(mnu, A_BOLD);

  /* Afisam fiecare element, incepand de la randul 3 pentru a lasa spatiu
   * sub titlu */
  for (int i = 0; i < N_ITEMS; i++) {
    if (i == sel) {
      /* Elementul selectat: culori inversate si bold pentru highlight */
      wattron(mnu, COLOR_PAIR(CP_SEL) | A_BOLD);
      mvwprintw(mnu, 3 + i, 1, " %-*s", MENU_W - 3, labels[i]);
      wattroff(mnu, COLOR_PAIR(CP_SEL) | A_BOLD);
    } else {
      /* Element neselectat: afisare normala fara atribute speciale */
      mvwprintw(mnu, 3 + i, 1, " %-*s", MENU_W - 3, labels[i]);
    }
  }
  wrefresh(mnu);
}

/*
 * show_content - afiseaza continut text in zona principala a ecranului
 *
 * Sterge zona de continut, deseneaza un chenar cu titlul dat si afiseaza
 * textul caracter cu caracter, respectand marginile ferestrei si
 * interpretand corect newline-urile pentru trecerea la randul urmator.
 *
 * Textul care depaseste numarul de randuri disponibile este trunchiat
 * (nu se face scroll automat). Textul care depaseste latimea unui rand
 * este de asemenea trunchiat (fara wrap automat de cuvinte).
 *
 * Parametri:
 *   title - titlul afisat in chenarul ferestrei de continut
 *   text  - continutul de afisat (poate contine '\n' pentru linii noi)
 */
static void show_content(const char *title, const char *text) {
  werase(cnt);
  box(cnt, 0, 0); /* chenar exterior al zonei de continut */

  /* Titlul ferestrei, afisat bold in chenarul de sus */
  wattron(cnt, A_BOLD);
  mvwprintw(cnt, 0, 2, " %s ", title);
  wattroff(cnt, A_BOLD);

  /* Calculam spatiul util din interiorul chenarului */
  int mr = getmaxy(cnt) - 2; /* randuri maxime disponibile pentru text */
  int mc = getmaxx(cnt) - 3; /* coloane maxime disponibile pentru text */
  int r = 1, c = 1;          /* pozitia curenta de scriere (in interiorul chenarului) */

  /* Parcurgem textul caracter cu caracter pentru control precis al pozitiei */
  for (const char *p = text; *p && r <= mr; p++) {
    if (*p == '\n') {
      /* La newline: trecem la randul urmator si resetam coloana */
      r++;
      c = 1;
    } else if (c <= mc) {
      /* Caracter normal: il afisam si avansam coloana */
      mvwaddch(cnt, r, c++, *p);
    }
    /* Caracterele care depasesc latimea randului sunt ignorate (trunchiere) */
  }
  wrefresh(cnt);
}

/* ── dialog de autentificare ────────────────────────────────────────── */

/*
 * do_login - afiseaza dialogul de autentificare si citeste credentialele
 *
 * Creeaza o fereastra de dialog centrata care solicita utilizatorul si parola
 * administratorului. Parola este citita cu echo dezactivat pentru securitate
 * (caracterele tastate nu apar pe ecran), spre deosebire de username care
 * este vizibil.
 *
 * Dupa citire, dialogul este distrus si ecranul principal este restaurat.
 * Credentialele sunt stocate in bufferele furnizate de apelant.
 *
 * Parametri:
 *   user - buffer de iesire pentru numele de utilizator (minim 100 bytes)
 *   pass - buffer de iesire pentru parola (minim 100 bytes)
 */
static void do_login(char *user, char *pass) {
  int dh = 9, dw = 44; /* dimensiunile dialogului de login */

  /* Cream fereastra de dialog centrata pe ecran */
  WINDOW *dlg = newwin(dh, dw, (LINES - dh) / 2, (COLS - dw) / 2);
  keypad(dlg, TRUE); /* activam tastele speciale in dialog */
  box(dlg, 0, 0);    /* chenar in jurul dialogului */

  /* Titlul dialogului, centrat si bold */
  wattron(dlg, A_BOLD);
  mvwprintw(dlg, 0, (dw - 14) / 2, " ADMIN LOGIN ");
  wattroff(dlg, A_BOLD);

  /* Etichetele campurilor de input */
  mvwprintw(dlg, 2, 3, "Username:");
  mvwprintw(dlg, 4, 3, "Password:");

  /* Activam echo si cursorul pentru a oferi feedback vizual la introducere
   * username-ului - utilizatorul trebuie sa vada ce tasteaza */
  echo();
  curs_set(1);
  wmove(dlg, 2, 14); /* pozitionam cursorul la campul de username */
  wrefresh(dlg);
  wgetnstr(dlg, user, 99);

  /* Dezactivam echo pentru parola - caracterele nu vor fi afisate pe ecran,
   * prevenind astfel vizualizarea parolei de catre persoane din jur */
  noecho();
  wmove(dlg, 4, 14); /* pozitionam cursorul la campul de parola */
  wrefresh(dlg);
  wgetnstr(dlg, pass, 99);

  /* Restauram starea terminalului: cursor invizibil */
  curs_set(0);

  /* Distrugem fereastra de dialog si actualizam ecranul principal
   * touchwin forteaza redesenarea completa a stdscr la urmatorul refresh */
  delwin(dlg);
  touchwin(stdscr); /* marcam tot ecranul pentru redesenare */
  refresh();
}

/* ── expulzare client: prompt in bara de status ─────────────────────── */

/* Buffer global pentru stocarea raspunsurilor de la server.
 * Dimensiunea mare (64 KB) permite primirea listelor lungi de clienti
 * sau a statisticilor detaliate intr-un singur buffer. */
static char cbuf[CBUF];

/*
 * do_kick - solicita si executa expulzarea unui client conectat
 *
 * Afiseaza un prompt in bara de status, citeste ID-ul clientului de
 * expulzat, trimite comanda KICK la server si afiseaza rezultatul in zona
 * de continut. Daca utilizatorul nu introduce niciun ID (apasa Enter direct),
 * operatiunea este anulata si bara de status este restaurata la mesajul
 * de ajutor standard.
 *
 * Nu are parametri si nu returneaza nimic.
 */
static void do_kick(void) {
  char id[32] = {0}; /* bufferul pentru ID-ul clientului de expulzat */

  /* Pregatim bara de status pentru a primi input-ul ID-ului.
   * Refolosim bara de status ca zona de input pentru a nu deschide
   * un dialog separat si a pastra interfata curata. */
  wbkgd(sts, COLOR_PAIR(CP_STS));
  werase(sts);
  mvwprintw(sts, 0, 1, "Kick client ID: "); /* prompt vizibil pentru utilizator */
  echo();    /* activam echo pentru a vedea ce tastam */
  curs_set(1);
  wrefresh(sts);

  /* Citim ID-ul direct din bara de status, incepand dupa textul promptului
   * (coloana 17 = 1 spatiu margine + 16 caractere "Kick client ID: ") */
  mvwgetnstr(sts, 0, 17, id, (int)sizeof(id) - 1);
  noecho();
  curs_set(0);

  /* Eliminam eventualul newline de la sfarsitul stringului citit */
  id[strcspn(id, "\n")] = '\0';

  /* Daca utilizatorul nu a introdus nimic, anulam operatiunea si
   * restauram bara de status la mesajul de ajutor standard */
  if (id[0] == '\0') {
    draw_status("Arrow keys: navigate   Enter: select   q: quit");
    return;
  }

  /* Construim si trimitem comanda KICK cu ID-ul specificat */
  char cmd[64];
  snprintf(cmd, sizeof cmd, "KICK %s\n", id);
  net_send(cmd);

  /* Citim raspunsul de la server (o singura linie: OK sau eroare) */
  char resp[64] = {0};
  net_recv_line(resp, sizeof resp);

  /* Construim mesajul de afisat in functie de raspunsul serverului */
  char msg[128];
  if (strncmp(resp, "OK", 2) == 0)
    /* Serverul a confirmat expulzarea cu succes */
    snprintf(msg, sizeof msg, "Client %s kicked successfully.", id);
  else
    /* Serverul a raportat ca ID-ul specificat nu exista sau nu mai e conectat */
    snprintf(msg, sizeof msg, "Client ID %s not found.", id);

  /* Afisam rezultatul operatiunii in zona principala de continut */
  show_content("KICK", msg);
}

/* ── functia principala ─────────────────────────────────────────────── */

/*
 * main - punctul de intrare al clientului de administrare
 *
 * Initializeaza conexiunea la server, interfata ncurses, realizeaza
 * autentificarea si ruleaza bucla principala de interactiune cu meniul.
 * La final, curata resursele si inchide conexiunea.
 *
 * Parametri:
 *   argc - numarul de argumente din linia de comanda
 *   argv - vectorul de argumente (argv[1] poate specifica calea socket-ului)
 *
 * Returneaza:
 *   0 la iesire normala
 *   1 la eroare de conexiune sau autentificare
 */
int main(int argc, char *argv[]) {
  /* Folosim calea socket-ului din argument sau valoarea implicita */
  const char *sock_path = (argc > 1) ? argv[1] : ADMIN_SOCK;

  /* Construim structura de adresa pentru socket-ul Unix local */
  struct sockaddr_un srv = {0};
  srv.sun_family = AF_UNIX;
  strncpy(srv.sun_path, sock_path, sizeof(srv.sun_path) - 1);

  /* Cream socket-ul de tip stream (similar TCP, dar comunicare locala) */
  sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock < 0) {
    perror("socket");
    return 1;
  }

  /* Ne conectam la serverul de admin prin socket-ul Unix */
  if (connect(sock, (struct sockaddr *)&srv, sizeof srv) < 0) {
    perror("connect");
    close(sock);
    return 1;
  }

  /* ── initializare ncurses ────────────────────────────────────── */
  initscr();       /* initializeaza biblioteca ncurses si ecranul */
  start_color();   /* activeaza suportul pentru culori in terminal */
  cbreak();        /* dezactiveaza buffering-ul de linie - tastele sunt citite imediat,
                    * fara a astepta apasarea Enter */
  noecho();        /* dezactiveaza afisarea automata a tastelor apasate */
  curs_set(0);     /* ascundem cursorul text pentru aspect mai curat al interfetei */

  /* Definim perechile de culori folosite in interfata.
   * Fiecare pereche combina o culoare pentru text si una pentru fundal. */
  init_pair(CP_HDR, COLOR_WHITE, COLOR_BLUE);  /* header: text alb pe albastru */
  init_pair(CP_STS, COLOR_BLACK, COLOR_WHITE); /* status: text negru pe alb */
  init_pair(CP_SEL, COLOR_BLACK, COLOR_CYAN);  /* selectie meniu: negru pe cyan */

  /* Cream si pozitionam ferestrele interfetei in functie de dimensiunea
   * curenta a terminalului */
  init_wins();
  draw_header(); /* afisam titlul aplicatiei in header */

  /* ── autentificare ───────────────────────────────────────────── */

  /* Colectam credentialele administratorului prin dialogul grafic */
  char user[100] = {0}, pass[100] = {0};
  do_login(user, pass);

  /* Redesenam interfata de baza inainte de trimiterea credentialelor,
   * deoarece dialogul de login poate lasa artefacte vizuale */
  draw_header();
  draw_status("Authenticating...");

  /* Construim si trimitem comanda de autentificare la server.
   * Formatul este: "ADMIN <username> <password>" */
  char auth[256];
  snprintf(auth, sizeof auth, "ADMIN %s %s", user, pass);
  net_send(auth);

  /* Asteptam raspunsul serverului la cererea de autentificare */
  char resp[64] = {0};
  net_recv_line(resp, sizeof resp);

  /* Verificam daca o alta sesiune de admin este deja activa.
   * Serverul permite un singur administrator simultan pentru a preveni
   * conflictele si accesul concurent necontrolat. */
  if (strncmp(resp, "ADMIN_BUSY", 10) == 0) {
    endwin(); /* restauram terminalul inainte de iesire */
    fprintf(
        stderr,
        "Admin session already active. Only one admin allowed at a time.\n");
    close(sock);
    return 1;
  }

  /* Verificam daca autentificarea a reusit (credentiale corecte) */
  if (strncmp(resp, "AUTH_OK", 7) != 0) {
    endwin();
    fprintf(stderr, "Authentication failed.\n");
    close(sock);
    return 1;
  }

  /* ── bucla principala de interactiune ───────────────────────── */

  /* Starea initiala a interfetei: primul element selectat, mesaj de bun venit */
  int sel = 0;
  draw_menu(sel);
  show_content("OUTPUT", "Select an action from the menu.");
  draw_status("Arrow keys: navigate   Enter: select   q: quit");

  /* Bucla principala: asteptam taste si executam actiunile corespunzatoare.
   * Rulam la nesfarsit pana cand utilizatorul alege Quit sau apasa 'q'. */
  while (1) {
    int ch = wgetch(mnu); /* asteptam blocant o tasta in fereastra de meniu */

    switch (ch) {
    /* Sageata sus: selectam elementul anterior cu wrapping circular.
     * Modulul (% N_ITEMS) asigura ca de la primul element ne intoarcem
     * la ultimul in loc sa mergem la un index negativ. */
    case KEY_UP:
      sel = (sel - 1 + N_ITEMS) % N_ITEMS;
      draw_menu(sel);
      break;

    /* Sageata jos: selectam elementul urmator cu wrapping circular.
     * De la ultimul element revenim automat la primul. */
    case KEY_DOWN:
      sel = (sel + 1) % N_ITEMS;
      draw_menu(sel);
      break;

    /* Enter: executam actiunea corespunzatoare elementului selectat.
     * Acceptam atat '\n' cat si KEY_ENTER pentru compatibilitate cu
     * diverse terminale si emulatoare. */
    case '\n':
    case KEY_ENTER:
      switch (sel) {
      case 0:
        /* Listare clienti conectati: raspuns multi-linie terminat cu ".\n" */
        net_send("LIST\n");
        net_recv_multi(cbuf, CBUF);
        /* Afisam "(none)" daca nu exista clienti conectati */
        show_content("CONNECTED CLIENTS", cbuf[0] ? cbuf : "(none)");
        break;
      case 1:
        /* Expulzare client: gestionata separat din cauza inputului interactiv */
        do_kick();
        break;
      case 2:
        /* Starea serverului: raspuns pe o singura linie */
        net_send("STATUS\n");
        net_recv_line(cbuf, CBUF);
        show_content("STATUS", cbuf);
        break;
      case 3:
        /* Statistici detaliate ale serverului: raspuns multi-linie */
        net_send("STATS\n");
        net_recv_multi(cbuf, CBUF);
        show_content("SERVER STATS", cbuf);
        break;
      case 4:
        /* Timp de functionare al serverului: raspuns pe o singura linie */
        net_send("UPTIME\n");
        net_recv_line(cbuf, CBUF);
        show_content("UPTIME", cbuf);
        break;
      case 5:
        /* Limbaje de programare suportate: raspuns multi-linie */
        net_send("LANGS\n");
        net_recv_multi(cbuf, CBUF);
        show_content("SUPPORTED LANGUAGES", cbuf);
        break;
      case 6:
        /* Iesire: notificam serverul sa inchida sesiunea administrativa
         * si parasim bucla principala */
        net_send("EXIT\n");
        net_recv_line(cbuf, CBUF);
        goto done; /* iesim din bucla while prin goto pentru claritate */
      }
      /* Restauram mesajul de ajutor dupa executarea oricarei comenzi */
      draw_status("Arrow keys: navigate   Enter: select   q: quit");
      break;

    /* Tasta 'q' sau 'Q': iesire rapida fara a mai selecta din meniu */
    case 'q':
    case 'Q':
      net_send("EXIT\n");
      net_recv_line(cbuf, CBUF);
      goto done;
    }
  }

done:
  /* Curatenie la iesire: restauram terminalul si inchidem conexiunea.
   * endwin() este esential pentru a restaura terminalul la starea initiala,
   * altfel shell-ul parinte va ramane cu setarile ncurses active. */
  endwin();    /* restauram terminalul la starea de dinaintea ncurses */
  close(sock); /* inchidem socket-ul de conexiune la server */
  return 0;
}
