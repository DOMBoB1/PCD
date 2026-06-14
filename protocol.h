/*
 * protocol.h — Definitia protocolului binar de comunicatie client-server
 *
 * Motivul alegerii unui protocol binar in locul unuia textual (ex: HTTP, FTP):
 * protocoalele binare au antete de dimensiune fixa si cunoscuta in avans, ceea
 * ce simplifica citirea din socket — se stie exact cati octeti sa fie cititi
 * inainte de a interpreta continutul, fara a cauta delimitatori sau a parsa
 * linii de text. Aceasta abordare este mai eficienta si mai putin susceptibila
 * la erori de parsare.
 *
 * Toate intregele pe mai multi octeti sunt transmise in ordinea retelei
 * (big-endian, conform RFC 1700). Aceasta conventie este obligatorie in
 * protocoalele de retea pentru a garanta interoperabilitatea intre arhitecturi
 * diferite: procesoarele x86/x64 folosesc little-endian intern, insa datele
 * transmise prin socket trebuie sa fie in big-endian pentru a fi interpretate
 * corect de orice sistem de pe cealalta parte a conexiunii. Functiile htonl(),
 * htons(), ntohl(), ntohs() realizeaza conversia necesara.
 *
 * Fluxul de comunicare:
 *   Client -> Server: ReqHeader (81 octeti) urmat imediat de code_size octeti
 *                     reprezentand codul sursa al programului de executat.
 *   Server -> Client: RespHeader (72 octeti) urmat imediat de file_size octeti
 *                     reprezentand continutul fisierului cu rezultatele executiei.
 *
 * Echivalente Python pentru serializare/deserializare (modulul struct,
 * '!' = big-endian conform standardului de retea):
 *   REQ_FMT  = '!BQ64sII'  — calcsize = 81 octeti (1 + 8 + 64 + 4 + 4)
 *   RESP_FMT = '!64sQ'     — calcsize = 72 octeti (64 + 8)
 * Aceste dimensiuni trebuie sa corespunda exact cu sizeof() din C; de aceea
 * structurile sunt declarate packed (fara padding de aliniere).
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>  /* tipuri intregi cu latime fixa: uint8_t, uint32_t, uint64_t;
                      * folosite in locul tipurilor native (int, long) deoarece
                      * dimensiunea acestora din urma variaza intre platforme pe
                      * 32 si 64 de biti, ceea ce ar rupe compatibilitatea binarului
                      * transmis prin retea */

/*
 * ReqHeader — antetul cererii trimise de client catre server.
 *
 * __attribute__((packed)) este esential: fara el, compilatorul ar insera octeti
 * de padding intre campuri pentru a alinia fiecare camp la frontiera sa naturala
 * de memorie (ex: uint64_t ar fi aliniat la 8 octeti, uint32_t la 4 octeti).
 * Acest padding este invizibil in cod, dar face ca sizeof(ReqHeader) sa depaseasca
 * suma dimensiunilor campurilor — serverul ar citi un numar gresit de octeti din
 * socket si ar interpreta eronat datele primite. Packed garanteaza ca structura
 * are exact 81 de octeti pe orice compilator si arhitectura.
 */
typedef struct {
    uint8_t  language;       /* Identifica limbajul de programare al codului sursa:
                              *   1 = C        (compilat cu gcc)
                              *   2 = Python   (interpretat cu python3)
                              *   3 = C++      (compilat cu g++)
                              *   4 = Java     (compilat cu javac, rulat cu java)
                              *   0xFF = comanda speciala de control (ex: PING,
                              *          LANGS, STATUS) — in acest caz codul sursa
                              *          este inlocuit cu textul comenzii.
                              * Un singur octet este suficient deoarece numarul de
                              * limbaje suportate este mic; valoarea 0xFF este aleasa
                              * intentionat ca marker de comanda pentru ca nu va fi
                              * niciodata un cod valid de limbaj de programare, evitand
                              * astfel necesitatea unui camp separat pentru tipul cererii. */

    uint64_t code_size;      /* Numarul exact de octeti ai codului sursa care urmeaza
                              * imediat dupa acest antet in fluxul TCP (transmis in
                              * big-endian, convertit cu htobe64/be64toh sau htonl echivalent).
                              * Serverul trebuie sa citeasca exact acest numar de octeti
                              * din socket pentru a obtine codul sursa complet — nici mai
                              * mult, nici mai putin. Se foloseste uint64_t (8 octeti) in
                              * locul unui uint32_t pentru a nu limita artificial marimea
                              * codului sursa la 4 GB; desi fisierele sursa sunt in practica
                              * mult mai mici, un tip mai larg evita probleme viitoare. */

    char     filename[64];   /* Numele original al fisierului sursa, stocat ca sir de
                              * caractere completat cu zerouri pana la 64 de octeti
                              * (null-padded). Nu este obligatoriu null-terminated daca
                              * numele are exact 64 de caractere, deci la citire trebuie
                              * folosit strnlen sau o copie cu null-terminator explicit.
                              * Dimensiunea de 64 de octeti este un compromis practic:
                              * acopera orice nume rezonabil de fisier (inclusiv extensia),
                              * fara a mari inutil antetul. Serverul foloseste acest camp
                              * pentru a reconstrui numele fisierului de iesire si pentru
                              * a include numele in mesajele de eroare catre utilizator. */

    uint32_t time_limit_s;   /* Limita maxima de timp de executie in secunde (timp real,
                              * wall-clock), transmisa in big-endian.
                              * Valoarea 0 semnifica absenta limitei (executie nelimitata).
                              * Serverul monitorizeaza procesul copil si ii trimite SIGKILL
                              * la depasirea acestei limite, prevenind consumul indefinit
                              * de resurse de catre programe cu bucle infinite sau algoritmi
                              * cu complexitate exponentiala. uint32_t permite limite de
                              * pana la ~136 de ani, mult peste orice nevoie practica. */

    uint32_t mem_limit_mb;   /* Limita maxima de memorie virtuala in megaocteti, transmisa
                              * in big-endian.
                              * Valoarea 0 semnifica absenta limitei (sau aplicarea limitei
                              * implicite din configuratia serverului, daca exista).
                              * Serverul aplica aceasta limita procesului copil folosind
                              * setrlimit(RLIMIT_AS, ...) pentru a preveni situatii in care
                              * un program gresit aloca memorie fara limita, epuizand
                              * resursele sistemului si afectand alti utilizatori.
                              * Unitatea MB este suficient de granulara pentru uz practic
                              * si permite limitari de pana la ~4 TB cu uint32_t. */
} __attribute__((packed)) ReqHeader;

/*
 * RespHeader — antetul raspunsului trimis de server catre client.
 *
 * La fel ca ReqHeader, este marcat cu __attribute__((packed)) pentru a garanta
 * ca dimensiunea sa este exact 72 de octeti (64 + 8), corespunzand calcsize-ului
 * din Python. Fara packed, compilatorul ar putea alinia uint64_t la o frontiera
 * de 8 octeti, inserand 4 octeti de padding dupa campul filename si marind
 * structura la 76 de octeti — o neconcordanta fatala cu codul Python al clientului.
 *
 * Serverul trimite acest antet urmat imediat de file_size octeti cu continutul
 * fisierului de iesire, care poate fi: stdout-ul programului executat, mesajele
 * de eroare ale compilatorului, sau raspunsul textual la o comanda de control.
 */
typedef struct {
    char     filename[64];   /* Numele fisierului de rezultat returnat de server,
                              * ex: "output.txt", "compile_error.txt", "response.txt",
                              * completat cu zerouri pana la 64 de octeti.
                              * Transmiterea numelui fisierului in raspuns (nu doar a
                              * continutului) serveste doua scopuri: clientul stie cum
                              * sa salveze fisierul local, si utilizatorul poate vedea
                              * ce tip de rezultat a primit (output normal vs eroare).
                              * Serverul poate astfel returna fisiere cu denumiri diferite
                              * in functie de rezultatul executiei, fara a adauga un camp
                              * suplimentar pentru tipul raspunsului. */

    uint64_t file_size;      /* Numarul exact de octeti ai continutului fisierului care
                              * urmeaza imediat dupa acest antet in fluxul TCP (transmis
                              * in big-endian).
                              * Clientul citeste exact acest numar de octeti dupa antet
                              * pentru a reconstitui fisierul rezultat complet. Se foloseste
                              * uint64_t pentru consistenta cu campul code_size din ReqHeader
                              * si pentru a permite fisiere rezultat de dimensiuni mari
                              * (programe care genereaza output voluminos nu sunt excluse). */
} __attribute__((packed)) RespHeader;

#endif /* PROTOCOL_H — sfarsitul gardei de includere multipla; previne redefinirea
        * tipurilor si macro-urilor in cazul includerii accidentale de mai multe ori
        * in acelasi fisier de compilare, o practica standard in C/C++ */
