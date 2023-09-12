// Progetto di Reti Informatiche 575II
// Ceccanti Lorenzo 564490
// Codice del Device

#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <errno.h>
#include <signal.h>
#include <time.h>

// Elenco delle costanti
#define UNIXTERM_WIDTH 80 // Numero di caratteri che stanno dentro una riga di bash
#define MSG_MAX_LEN 1802
#define BUF_LEN 256
#define INDIRIZZO_IP "127.0.0.1"

// Elenco delle strutture dati

// Definizione della struttura dei messaggi
// n.b.: si suppone di non gestire il
// Year 2038 problem. time_t è su 64 bit ma non è detto
// che l'host remoto sia una macchina a 64 bit
// Lo faccio per utilizzare soltanto funzioni standard per la 
// gestione dell'endianess

struct message{
    uint8_t userSend[UNIXTERM_WIDTH];
    uint8_t userDest[UNIXTERM_WIDTH];
    uint32_t epochTimeInvio;
    uint32_t epochTimeRicezione;
    uint32_t dimensionePayload;
    uint32_t orarioSessione;
    uint16_t destPort;
    uint8_t* payload; // testo del messaggio allocato dinamicamente
    struct message* pun; // serve per creare una lista di messaggi
};

// Realizza il concetto di lista degli utenti a cui ho scritto almeno una volta
struct stringList{
    // str è il nome utente
    char str[UNIXTERM_WIDTH];
    // nel caso dei gruppi port invece di esprimere la porta esprime se l'utente è stato aggiunto
    // al pool dei contattati con /a
    int port;
    // nel caso dei gruppi è significativo anche l'utilizzo di p2pOpen, online, newPort
    int peerOpen;
    int online;
    int newPort;
    struct stringList* pun;
};

// Hanging
struct notificationEntry{
    char user[UNIXTERM_WIDTH]; // nome utente
    uint32_t count; // numero di messaggi pendenti in ingresso
    time_t lastTimestamp; // timestamp del messaggio più recente
    struct notificationEntry* pun;
};

struct stringList* strL = NULL; // Lista degli utenti contattati almeno una volta
char tempUniqueDest[UNIXTERM_WIDTH]; // destinatario temporaneo
char tempUsername[UNIXTERM_WIDTH]; // username loggato nella sessione corrente

// Lista dei messaggi ricevuti ma non salvati su file
struct message* listaMsgLocali = NULL;
// Lista dei messaggi ricevuti da file
struct message* listaMsgScaricati = NULL;
// Lista degli utenti contattabili che possono essere aggiunti ad una chat di gruppo
struct stringList* listaUsrGrp;
// Lista da inviare ogni volta che un device ci richiede un hanging
struct notificationEntry* deviceNotifiersList;

// Elenco delle variabili globali

// array di stringhe che contiene un argomento per locazione
// l'allocazione è gestita dinamicamente sulla base del numero di comandi effettivamente digitati
char** cmdBuffer;
// socket locale utilizzato dal client per connettersi al server
int clientSock;
// sockaddr del server - per l'invio
struct sockaddr_in sv_addr;
struct sockaddr_in sv_addr_reg;
// indirizzo dell'host remoto riempito dalla accept in ricezione
struct sockaddr_in sender_addr;
// indirizzo dell'host destinatario necessario al sender per l'invio
struct sockaddr_in dest_addr;
// socket utilizzato dal sender per connettersi al destinatario finale
int peerSock = -1;
// numero di porta del client
int clientPort;
// numero di porta del server
int serverPort;

// --- socket per le comunicazioni punto - punto
// e per gli handshake con il server
int listeningSocket = -1;
int connectedSocket = -1;
int newsd = -1;
// ---

// Variabile che esprime se il device ha una connessione in corso con il server
// 0: connessione non ancora stabilita, 1 connessione già stabilita
int connected = 0; 

// -- Variabili utili per la gestione della chat
int inChat = 0; // se non siamo in chat vale 0, 1 altrimenti
int inGroup = 0;
char tempMessage[MSG_MAX_LEN];
int peerOpen = 0;
int new_port = 0;
int esitoConnessione = -1;
// --

void welcomeMessage()
{
    printf("Digitare uno tra i seguenti comandi\n");
    printf("in srv_port username password\n");
    printf("signup username password\n");
    printf("\n");
}

void stampaNegatoAuth()
{
    printf("Errore di autenticazione\n");
    printf("Possibili cause:\n");
    printf("- Porta del device già in uso\n");
    printf("- Porta del device non rilasciata a causa di disconnessioni impreviste\n");
    printf("- Username già online\n");
    printf("- Username non registrato\n");
    printf("- Username o password errati\n");
    printf("\n");
}

void stampaListaComandi()
{
    printf("                   Menù iniziale:                  \n");
    printf("Comandi disponibili:\n");
    printf(".)   hanging\n");
    printf(".)   show \'username\'\n");
    printf(".)   chat \'username\'\n");
    printf(".)   out\n");
}

// Funzione di utilità
// Restituisce il numero di caratteri spazio presenti nella stringa v passata come argomento
int countBlankSpaces(char* v)
{
    int spaces = 0;
    int i;
    for(i = 0; v[i] != '\0'; i++)
    {
        if(v[i] == ' ')
        {
            if(i == 0) // il primo argomento è uno spazio
                return spaces;
            if(i > 0 && v[i-1] == ' ') // ci sono due spazi di fila
                return spaces;
            spaces++;
        }       
    }
    return spaces;
}

void convertFromEpoch(time_t rawtime, char** buf)
{
    *buf = malloc(BUF_LEN*sizeof(char));
    if(rawtime == -1)
    {
        strcpy(*buf, "N/A");
        return;
    }
    struct tm *ptm = localtime(&rawtime);
    memset(*buf, 0, BUF_LEN);
    strftime(*buf, BUF_LEN, "%F %T", ptm);
    return;
}

// Funzione di utilità
// Prende in ingresso una stringa e restituisce per riferimento
// un'array di stringhe i cui valori sono separati dal carattere spazio
void chunckingString(char* str)
{
    int i = 0;
    char* token;

    // strtok è una funzione della libreria string.h
    // ed è in questo caso utilizzata per generare una
    // serie di token (chunk di una stringa) a partire
    // da un unico array di char
    //strtok ritorna il next token
    // char* strtok(char str[], const char* delims)

    // Cerco il primo delimitatore
    // Alla prima chiamata di strtok è necessario indicare
    // la stringa da spezzare in token
    token = strtok(str, " ");

    // Continuo a separare la stringa in token
    // fintantoché trovo un delimitatore
    while(token != NULL)
    {
        cmdBuffer[i++] = token;
        // Per le successive chiamate a strtok è necessario
        // passare NULL come primo argomento. In questo
        // modo si dice di proseguire a spezzettare
        // a partire dall'ultimo delimiter incontrato 
        // con la precedente chiamata a strtok
        token = strtok(NULL, " ");
    }
    return;
}

// Funzione che elimina newline parassita introdotto a causa 
// dell'utilizzo della funzione fgets.
// Richiede in input la dimensione dell'array di stringhe
// Va ad agire direttamente modificando la zona di memoria puntata
void deleteExtraNewline(int size)
{
    char* pos;
    // strchr è una funzione della libreria string.h
    // che serve ad individurare l'indice della prima
    // occorrenza specificata al secondo parametro
    // nel buffer specificato nel primo parametro
    // In questo caso viene controllato che esista almeno
    // un'occorenza del carattere newline (evenetualmente inserito
    // sicuramente in precedenza dalla fgets e non voluto).
    // In caso affermativo, va sostituito newline con
    // il carattere di fine stringa

    if((pos=strchr(cmdBuffer[(size-1)], '\n')) != NULL)
        *pos = '\0';
    return;
}

// Funzione di utilità.
// Restituisce -1 se il numero di argomenti passati eccede la soglia indicata come secondo parametro
// 0 altrimenti
int controllaLunghezzaArgomenti(int nargs_, int a)
{
    if(nargs_ != a)
    {
        printf("(Device): ERRORE. Numero inatteso di argomenti per il comando digitato.\nRiprovare.\n");
        return -1;
    }
    return 0;
}

// Funzione di utilità.
// Prende in ingresso una stringa e la dimensione indicata in numero di caratteri
// Restituisce 1 se la stringa contiene un numero di porta valido, 0 se la stringa contiene un numero
// di porta riservato, -1 se non è un numero di porta

int checkPortNumber(char* port, int len)
{
    int i; int convPort;
    for(i = 0; i < len; i++)
    {
        if(port[i] >= '0' && port[i] <= '9')
            continue;
        else
            return -1;
    }
    // Dopo aver convertito la stringa in intero, controllo se la porta in uso è una di quelle riservate
    convPort = atoi(port);
    if(convPort >= 0 && convPort <= 1023)
        return 0;
    else if(convPort > 65535) // 2^16 - 1
        return -1;
    else
        return 1;
}


// Restituisce 0 se str presente in lista, -1 altrimenti
int cercaInStringList(char* str, int* porta)
{
    struct stringList* p = strL;
    while(p != NULL)
    {
        if( strcmp(p->str, str) == 0)
        {
            *porta = p->port;
            return 0;
        }
        p = p->pun;
    }
    return -1;
}

// Visita la listaUsrGrp alla ricerca dell'elemento che contiene 
// il destinatario s. Se lo trova, restituisce il puntatore
// all'elemento oppure NULL in caso contrario.
struct stringList* visitaListaUsrGrp(char* s)
{
    struct stringList* p;
    for(p = listaUsrGrp; p != NULL; p = p->pun)
        if(strcmp(p->str, s) == 0)
            return p;
    return NULL;
}

void deviceInserisciStringList(char* s, int porta)
{
    struct stringList* p; struct stringList* q;
    for(q = strL; q != NULL; q = q->pun)
        p = q;
    q = malloc(sizeof(struct stringList));
    strcpy(q->str, s);
    q->port = porta;
    q->pun = NULL;
    if(strL == NULL)
        strL = q;
    else
        p->pun = q;
}

void stampaStringList(struct stringList* p)
{
    while(p != NULL)
    {
        if(p->online == 1)
            printf("%s ", p->str);
        p = p->pun;
    }
    printf("\n");
}

void stampaSelezionatiGruppo(struct stringList* p)
{
    printf("Partecipanti:");
    while(p != NULL)
    {
        if(p->port == 1 && p->online == 1)
            printf("%s ", p->str);
        p = p->pun;
    }
    printf("\n");
}
void stampaStringListDeb(struct stringList* p)
{
    while(p != NULL)
    {

        printf("%s p2pOpen: %d online: %d ", p->str, p->peerOpen, p->online);
        p = p->pun;
    }
    printf("\n");
}

int contaUtentiSelezionatiGrp()
{
    int conta = 0;
    struct stringList* p;
    for(p = listaUsrGrp; p != NULL; p = p->pun)
        if(p->online == 1 && p->port == 1)
            conta++;
    return conta;
}

void deviceInserisciListaUsrGrp(char* s, int n, int po)
{
    // Deve cercare se esiste già un username nella lista
    struct stringList* ret = visitaListaUsrGrp(s);
    if(ret == NULL)
    {   // se non esiste lo aggiunge
        struct stringList* p; struct stringList* q;
        for(q = listaUsrGrp; q != NULL; q = q->pun)
            p = q;
        q = malloc(sizeof(struct stringList));
        strcpy(q->str, s);
        q->port = n;
        q->peerOpen = po;
        q->online = 1;
        q->newPort = 0;
        q->pun = NULL;
        if(listaUsrGrp == NULL)
            listaUsrGrp = q;
        else
            p->pun = q;
    } else {
        // Se esiste già aggiorna il campo 
        // rendendolo semplicemente online e lasciando inalterati
        // gli altri campi
        ret->online = 1;
        ret->port = n; // anche il dato della selezione deve aggiornato
    }
    return;
}

// se mod vale 1, l'epochTimeRicezione viene passato con il terzo parametro
void deviceInserisciListaMsgLocali(struct message msg, int mod, uint32_t timeR)
{
    struct message* p; struct message* q;
    for(q = listaMsgLocali; q != NULL; q = q->pun)
        p = q;
    
    q = malloc(sizeof(struct message));
    if(q == NULL)
        printf("errore malloc");

    strcpy((char*)q->userSend, (char*)msg.userSend);
    strcpy((char*)q->userDest, (char*)msg.userDest);
    q->epochTimeInvio = msg.epochTimeInvio;
    if(mod == 1) 
        q->epochTimeRicezione = timeR; 
    else 
        q->epochTimeRicezione = msg.epochTimeRicezione;
    q->dimensionePayload = msg.dimensionePayload;
    
    q->payload = (uint8_t*)malloc(sizeof(uint8_t)*msg.dimensionePayload);
    strcpy((char*)q->payload, (char*)msg.payload);
    q->pun = NULL;
    if(listaMsgLocali == NULL)
        listaMsgLocali = q;
    else
        p->pun = q;
}

// se mod vale 1, l'epochTimeRicezione viene passato con il terzo parametro
// old vale 1 se il messaggio è uno della cronologia, 0 altrimenti
void deviceInserisciListaScaricati(struct message msg, int mod, uint32_t timeR)
{
    struct message* p; struct message* q;
    for(q = listaMsgScaricati; q != NULL; q = q->pun)
        p = q;
    
    q = malloc(sizeof(struct message));
    if(q == NULL)
        printf("errore malloc");

    strcpy((char*)q->userSend, (char*)msg.userSend);
    strcpy((char*)q->userDest, (char*)msg.userDest);
    q->epochTimeInvio = msg.epochTimeInvio;
    if(mod == 1) 
        q->epochTimeRicezione = timeR; 
    else 
        q->epochTimeRicezione = msg.epochTimeRicezione;
    q->dimensionePayload = msg.dimensionePayload;
    
    q->payload = (uint8_t*)malloc(sizeof(uint8_t)*msg.dimensionePayload);
    strcpy((char*)q->payload, (char*)msg.payload);
    q->pun = NULL;
    if(listaMsgScaricati == NULL)
        listaMsgScaricati = q;
    else
        p->pun = q;
}

void distruggiListaTipoMessage(struct message** testa)
{
    struct message* p = *testa;
    struct message* q;
    while(p != NULL)
    {
        q = p->pun;
        free(p);
        p = q;
    }
    *testa = NULL;
}

void distruggiListaTipoStringList(struct stringList** testa)
{
    struct stringList* p = *testa;
    struct stringList* q;
    while(p != NULL)
    {
        q = p->pun;
        free(p);
        p = q;
    }
    *testa = NULL;
}
// Resetta i campi online e della selezione fatta con /a della listaUsrGrp
void ricalcolaUtentiListaUsrGrp()
{
    struct stringList* p;
    for(p = listaUsrGrp; p != NULL; p = p->pun)
    {
        p->online = 0;
        p->port = 0; // anche l'informazione della selezione viene resettata
    }
    return;    
}

void stampaDeviceListaMsg(char* usr, char* dst, int param)
{
    // Prima di ogni stampa pulisco
    system("clear");
    printf("------- Stai chattando con: ------\n");
    printf("%s\n", dst);
    struct message* p = listaMsgScaricati;
    char v[2]; // max 2 byte per la notifica di consegna
    // Prima iterazione: stampo i messaggi vecchi
    while(p != NULL)
    {
            // Messaggio che appartiene alla chat tra usr e dst
            int cond1 = strcmp((char*)p->userSend, dst);
            int cond2 = strcmp((char*)p->userDest, dst);
            if(cond1 == 0 || cond2 == 0)
            {
                // Converto elemento in formato TIMESTAMP
                char *oraInvio;
                convertFromEpoch(p->epochTimeInvio, &oraInvio);
                if(p->epochTimeRicezione == -1)
                    strcpy(v, "*");
                else
                    strcpy(v, "**");
                 if(cond1 == 0) // il messaggio è in arrivo
                    printf("%s: %s %s", p->userSend, p->payload, oraInvio);
                else
                    printf("%s: %s %s (%s)", p->userSend, p->payload, oraInvio, v);
                printf("\n");
            }
        p = p->pun;
    }
    p = listaMsgLocali;
    while(p != NULL)
    // Seconda iterazione: stampo i messaggi nuovi
    {
            int cond1 = strcmp((char*)p->userSend, dst);
            int cond2 = strcmp((char*)p->userDest, dst);
            if(cond1 == 0 || cond2 == 0)
            {
                char *oraInvio;
                convertFromEpoch(p->epochTimeInvio, &oraInvio);
                if(p->epochTimeRicezione == -1)
                    strcpy(v, "*");
                else
                    strcpy(v, "**");
                if(cond1 == 0) // il messaggio è in arrivo
                    printf("%s: %s %s", p->userSend, p->payload, oraInvio);
                else
                    printf("%s: %s %s (%s)", p->userSend, p->payload, oraInvio, v);
                printf("\n");
            }
        p = p->pun;
    }
    // Stampa dei messaggi ausiliari in fondo alla cronologia
    switch(param)
    {
        case 0:
            printf("Stato server: non connesso. Impossibile bufferizzare in caso di disconessione peer.\n");
            break;
        case 1:
            printf("Messaggio bufferizzato: il peer non è al momento raggiungibile\n");
            break;
    }
}

void stampaDeviceListaMsgGruppi(char* snd, int param)
{
    // Prima di ogni stampa pulisco
    system("clear");
    printf("------- Chat di gruppo: ------\n");
    stampaSelezionatiGruppo(listaUsrGrp);
    struct message* p = listaMsgScaricati;
    struct stringList* sl;
    char v[2]; // max 2 byte per la notifica di consegna
    // Prima iterazione: stampo i messaggi vecchi
    while(p != NULL) // per ogni elemento della lista
    {
        // e per ogni destinatario possibile della
        // listaUsrGrp
        for(sl = listaUsrGrp; sl != NULL && sl->port == 1; sl = sl->pun)
        {
            // Messaggio che appartiene alla chat tra usr e dst
            int cond1 = strcmp((char*)p->userSend, sl->str);
            int cond2 = strcmp((char*)p->userDest, sl->str);
            if(cond1 == 0 || cond2 == 0)
            {
                // Converto elemento in formato TIMESTAMP
                char *oraInvio;
                convertFromEpoch(p->epochTimeInvio, &oraInvio);
                if(p->epochTimeRicezione == -1)
                    strcpy(v, "*");
                else
                    strcpy(v, "**");
                if(cond1 == 0) // messaggio in arrivo
                    printf("%s -> %s: %s %s", p->userSend, p->userDest, p->payload, oraInvio);
                else
                    printf("%s -> %s: %s %s (%s)", p->userSend, p->userDest, p->payload, oraInvio, v);
                printf("\n");
            }
        }
        p = p->pun;
    }
    p = listaMsgLocali;
    // seconda iterazione: stampo i messaggi nuovi
    while(p != NULL) // per ogni elemento della lista
    {
        // e per ogni destinatario possibile della
        // listaUsrGrp
        for(sl = listaUsrGrp; sl != NULL && sl->port == 1; sl = sl->pun)
        {
            // Messaggio che appartiene alla chat tra usr e dst
            int cond1 = strcmp((char*)p->userSend, sl->str);
            int cond2 = strcmp((char*)p->userDest, sl->str);
            if(cond1 == 0 || cond2 == 0)
            {
                // Converto elemento in formato TIMESTAMP
                char *oraInvio;
                convertFromEpoch(p->epochTimeInvio, &oraInvio);
                if(p->epochTimeRicezione == -1)
                    strcpy(v, "*");
                else
                    strcpy(v, "**");
                if(cond1 == 0) // messaggio in arrivo
                    printf("%s -> %s: %s %s", p->userSend, p->userDest, p->payload, oraInvio);
                else
                    printf("%s -> %s: %s %s (%s)", p->userSend, p->userDest, p->payload, oraInvio, v);
                printf("\n");
            }
        }
        p = p->pun;
    }
    // Stampa dei messaggi ausiliari in fondo alla cronologia
    switch(param)
    {
        case 0:
            printf("Stato server: non connesso. Impossibile bufferizzare in caso di disconessione peer.\n");
            break;
        case 1:
            printf("Messaggio bufferizzato: il peer non è al momento raggiungibile\n");
            break;
        case 2:
            printf("L'utente è stato aggiunto al gruppo\n");
            break;
    }
}


void stampaDeviceListaMsgLocaliDebug()
{
    struct message* p = listaMsgScaricati;
    printf("Elenco dei messaggi non ancora letti\n");
    while(p != NULL)
    {
        printf("---------------------------------------\n");
         // Converto elemento in formato TIMESTAMP
        char *oraInvio; char* oraRicezione;
        convertFromEpoch(p->epochTimeInvio, &oraInvio);
        if(p->epochTimeRicezione == -1)
            oraRicezione = "N/A";
        else
            convertFromEpoch(p->epochTimeRicezione, &oraRicezione);
        printf("Da: %s a: %s\n", p->userSend, p->userDest);
        printf("Istante invio: %s\n", oraInvio);
        printf("Istante ricezione: %s\n", oraRicezione);
        printf("Testo del messaggio: %s\n", p->payload);
        printf("---------------------------------------\n");
        p = p->pun;
    }
    printf("Elenco completo\n");
}


// Funzione di utilità: serve per spedire un comando tramite il socket sock già precedentemente
// aperto. Funziona per tutti i comandi rappresentati su 8 byte e passati tramite la stringa str
// Ritorna 1 in caso di errore EPIPE, 0 altrimenti
int spedisciComando(int sock, char* str)
{
    int ret;
    char cmd[8]; // Tutti i comandi da inviare al server stanno su 8 byte
    strcpy(cmd, str);
    ret = send(sock, (void*)cmd, sizeof(cmd), 0);
    if(ret < 0)
    {
        if(errno == EPIPE)
            return 1;
        else
        {
            perror("Errore in fase di invio comando");
            exit(1);
        }    
    }
    return 0;
}

void riceviComando(int sock, char** cmd)
{
    int ret;
    *cmd = malloc(8*sizeof(char));
    ret = recv(sock, (void*)*cmd, sizeof(*cmd), 0);
    if(ret < 0)
    {
        perror("Errore in fase di ricezione comando");
        exit(1);
    }
}

void inviaOpaco16Bit(int sock, uint16_t num)
{
    int ret;
    num = htons(num);
    ret = send(sock, (void*)&num, sizeof(uint16_t), MSG_NOSIGNAL);
    if(ret < 0)
    {
        perror("Errore in fase di invio OPACO");
        exit(1);
    }
}

void inviaOpaco32Bit(int sock, uint32_t num)
{
    int ret;
    num = htonl(num);
    ret = send(sock, (void*)&num, sizeof(uint32_t), 0);
    if(ret < 0)
    {
        perror("Errore in fase di invio");
        exit(1);
    }
}

uint16_t riceviOpaco16Bit(int sock)
{
    int ret, len;
    uint16_t lmsg;
    ret = recv(sock, (void*)&lmsg, sizeof(uint16_t), 0);
    if(ret < 0)
    {
        perror("Errore in fase di ricezione");
        exit(1);
    }
    len = ntohs(lmsg);
    return len;
}

uint32_t riceviOpaco32Bit(int sock)
{
    int ret, len;
    uint32_t lmsg;
    ret = recv(sock, (void*)&lmsg, sizeof(uint32_t), 0);
    if(ret < 0)
    {
        perror("Errore in fase di ricezione");
        exit(1);
    }
    len = ntohl(lmsg);
    return len;
}

// Funzione di utilità: spedisce una stringa di dimensione variabile
// Ritorna 1 in caso di disconnessione dell'endpoint, 0 altrimenti
int spedisciStringa(int sd, uint8_t* str)
{
    int ret;
    uint16_t len;
    len = strlen((char*)str) + 1;
    inviaOpaco16Bit(sd, len);
    ret = send(sd, (void*)str, len, 0);
    if(ret < 0)
    {
        if(errno == EPIPE)
            return 1;
        else
        {
            perror("Errore in fase di send STRINGA:");
            exit(1);
        }
    }
    return 0;
}

void riceviStringa(int sd, char** str)
{
    int ret, len;
    len = riceviOpaco16Bit(sd);
    // Alloco una giusta quantità
    *str = malloc(len);
    // Ricevo i dati
    ret = recv(sd, (void*)*str, len, 0);
    if(ret < 0)
    {
        perror("(Server). Errore in fase di ricezione");
        exit(1);
    }
}

void salvaCronologiaChat(char* utente)
{
    FILE* fd;
    char* u = malloc(strlen(utente)+5);
    strcpy(u, utente);
    strncat(u,".txt", 5);
    int len = strlen(u) + 1;
    char* percorso;
    percorso = malloc(len + strlen("devicefiles/crono"));
    strcpy(percorso, "devicefiles/crono");
    strncat(percorso, u, len);
    fd = fopen(percorso, "a");
    if(fd < 0)
    {
        perror("Non trovo il file");
        exit(1);
    }
    struct message* p = listaMsgLocali;
    // stampaDeviceListaMsgLocali();
    // Scorro la lista dei messaggi locali
    while(p != NULL)
    {
            // Converto elemento in formato TIMESTAMP
            char *timestampLogin; char* timestampLogout;
            convertFromEpoch(p->epochTimeInvio, &timestampLogin);
            if(p->epochTimeRicezione == -1)
                timestampLogout = "N/A";
            else
                convertFromEpoch(p->epochTimeRicezione, &timestampLogout);
            // Serializzazione da oggetto a stringa
            fprintf(fd,"_STHEAD_\n");
            fprintf(fd,"f:%s\n", p->userSend);
            fprintf(fd,"t:%s\n", p->userDest);
            fprintf(fd,"ii:%d\n", p->epochTimeInvio);
            fprintf(fd,"ir:%d\n", p->epochTimeRicezione);
            fprintf(fd,"dp:%d\n", p->dimensionePayload);
            fprintf(fd,"_ENHEAD_\n");
            fprintf(fd,"_STPAY_\n");
            fprintf(fd,"txt:%s\n", p->payload);
            fprintf(fd,"$\n");
        
        p = p->pun;
    }
    fclose(fd);
    free(u);
    free(percorso);
    return;
}


// Va nel file della cronologia e recupera tutti i messaggi li presenti
// filtrando solo quelli che riguardano il sender che apre la chat e
// l'utente destinatario dest
void recuperaCronologiaChat(char* snd)
{
    distruggiListaTipoMessage(&listaMsgScaricati);
    struct message msg;
    char user[UNIXTERM_WIDTH];
    strcpy(user, snd);
    FILE* fd; char* testoLetto; int i;
    strncat(user,".txt", 5);
    int len = strlen(user) + 1;
    char* percorso;
    percorso = malloc(len + strlen("devicefiles/crono"));
    strcpy(percorso, "devicefiles/crono");
    strncat(percorso, user, len);
    fd = fopen(percorso, "a+");
    if(fd == NULL)
    {
        perror("Non trovo il file");
        exit(1);
    }
    // Devo allocare memoria per il buffer che riceve il contenuto del testo
    // Posiziono il cursore che punta al file alla fine
    fseek(fd, 0, SEEK_END);
    // ftell restituisce l'offset rispetto all'inizio del file del cursore,
    // espresso come numero di byte
    long nbytes = ftell(fd);
    fseek(fd, 0, SEEK_SET);
    testoLetto = malloc(nbytes);
    memset(testoLetto, 0, nbytes);
    // Il contenuto del file con la fread viene salvato nel buffer
    // parametri della fread: primo parametro è il buffer
    // secondo è l'unità di base del dato
    // terzo è il numero di unità
    // ritorna il numero di bytes effettivamente letti
    fread(testoLetto, sizeof(char), nbytes, fd);

    int niter = 0;
    // Conto il numero di caratteri "$", cioè il numero dei messaggi
    for(i = 0; i < nbytes; i++)
        if(testoLetto[i] == '$')
             niter++;

    // Inizializzazione dei puntatori ausiliari
    char* pun1 = testoLetto;
    char* pun2 = NULL;
    int j = 0;
    while(j < niter)
    {
        sscanf(pun1, "_STHEAD_\nf:%s\nt:%s\nii:%d\nir:%d\ndp:%d\n_ENHEAD_\n",
        (char*)msg.userSend, (char*)msg.userDest, &msg.epochTimeInvio, &msg.epochTimeRicezione, &msg.dimensionePayload);
        msg.payload = (uint8_t*)malloc(sizeof(uint8_t)*msg.dimensionePayload);
        pun2 = strstr(pun1, "_STPAY_\n");
        // correzione del bug relativo alla fine del parsing, uso [^\n] per catturare tutto ciò che non sia un \n
        sscanf(pun2, "_STPAY_\ntxt:%[^\n]\n$\n",(char*)msg.payload);
        msg.destPort = -1;
        msg.orarioSessione = -1;
        pun1 = strstr(pun2, "_STHEAD_\n");
        deviceInserisciListaScaricati(msg, 0, 0);
        j++;
    }
    fclose(fd);
    free(testoLetto);
    free(percorso);
    return;
}

void salvaIstanteDisconnessione(char* user)
{
    // +8 perché "out.txt\0"
    mkdir("devicefiles", 0700);
    char* u = malloc(strlen(user)+8);
    strcpy(u, user);
    strncat(u, "out.txt",8);
    char* p;
    int len = strlen(u) + 1;
    p = malloc(len + strlen("devicefiles/"));
    strcpy(p, "devicefiles/");
    strncat(p, u, len);
    // se il file non esiste lo crea, sennò sovrascrive contenuto
    FILE* fd = fopen(p, "w+"); 
    fprintf(fd, "%d", (int)time(NULL));
    fclose(fd);
    free(u);
    free(p);
}
// -1 se logout non è rilevato (user è online)
// 0 se logout è rilevato (user è offline)
int discoverIfLogoutFromFile(char* user, int* time)
{
    mkdir("devicefiles", 0700);
    char* u = malloc(strlen(user)+8);
    strcpy(u, user);
    strncat(u, "out.txt",8);
    // Non importa gestire il caso in cui il file non esiste
    // perché il server chiede di andare a vedere nel 
    // file soltanto se nel suo registro l'utente
    // risulta online, quindi se l'utente è la prima volta
    // che entra nel registro del server non ci è mai finito
    // e il problema non si pone
    char* p;
    int len = strlen(u) + 1;
    p = malloc(len + strlen("devicefiles/"));
    strcpy(p, "devicefiles/");
    strncat(p, u, len);
    FILE* fd = fopen(p, "r");
    // Se il file non esiste -> utente offline
    if(fd == NULL)
    {
         free(u);
         free(p);
         return 0;
    }
    // Se il file è vuoto -> utente online
    int sts = getc(fd);
    if(sts == EOF)
    {
        free(u); free(p);
        fclose(fd);
        return -1;
    }
    else
        ungetc(sts, fd);
    fscanf(fd, "%d", time);
    free(u);
    free(p);
    fclose(fd);
    return 0;
}

void pulisciFileOut(char* user)
{
    mkdir("devicefiles", 0700);
    char* u = malloc(strlen(user)+8);
    strcpy(u, user);
    strncat(u, "out.txt",8); //utenteout.bin
    char* p;
    int len = strlen(u) + 1;
    p = malloc(len + strlen("devicefiles/"));
    strcpy(p, "devicefiles/");
    strncat(p, u, len);

    fclose(fopen(p, "w"));
}
// Controlla se nella rubrica di user è presente dst
// Ritorna 0 se dst è presente, -1 altrimenti
int controllaRubrica(char* user, char* dst)
{
    int presente = -1;
    mkdir("devicefiles", 0700);
    char* u = malloc(strlen(user)+13);
    strcpy(u, user);
    strncat(u, "AddrBook.txt",13); //utenteAddrBook.txt
    char* p;
    int len = strlen(u) + 1;
    p = malloc(len + strlen("devicefiles/"));
    strcpy(p, "devicefiles/");
    strncat(p, u, len);
    // Motivo del parametro a+:
    // E' necessario controllare che il file della rubrica esista,
    // perché nel caso in cui un utente appena registrato voglia
    // avviare una chat con un altro utente, il file della sua rubrica
    // non esiste. Verrà creato un file dedicato alla rubrica di tale 
    // utente appena registrato.
    // Gestione semplificata della rubrica: non è previsto un comando
    // specifico per aggiungere un utente alla rubrica.
    // L'utente o chi per lui di sua "volontà" agirà sul file utenteAddrBook

    FILE* fd = fopen(p, "a+");
    while(feof(fd) == 0)
    {
        char str[UNIXTERM_WIDTH];
        fscanf(fd, "%s\n", str);
        if(strcmp(str, dst) == 0)
        {
            presente = 0;
            break;
        }
    }
    free(u); free(p);
    fclose(fd);
    return presente;
}

// Ritorna 1 se caduta la connessione
// Se il campo port vale -1, la porta utilizzata è quella già presente in ind_serv
// Altrimenti si modifica ind_serv indicando come porta la porta il terzo parametro port
int setupConnessioneTCPModoClient(int* sock, struct sockaddr_in* ind_serv, int port)
{
    struct sockaddr_in stateCopy;
    stateCopy= *ind_serv;

    int ret;
    *sock = socket(AF_INET, SOCK_STREAM, 0);
    if(*sock < 0)
    {
        perror("Errore nella creazione del socket");
        exit(1);
    }
    if(port != -1)
    {
        // Istanzio i campi dell'indirizzo del server
        memset(&stateCopy, 0, sizeof(stateCopy));
        stateCopy.sin_family = AF_INET;
        stateCopy.sin_port = htons(port);
        inet_pton(AF_INET, INDIRIZZO_IP, &stateCopy.sin_addr);
    }
    ret = connect(*sock, (struct sockaddr*)&stateCopy, sizeof(stateCopy));
    if(ret < 0)
    {
        if(errno == ECONNREFUSED)
            return 1;
        else
        {
            perror("Errore in fase di connessione");
            exit(1);
        }
    }
    *ind_serv = stateCopy;
    return 0;
}
int in(int nargs, int* valid, int* connected)
{
    int ret;
    int convPort;
    char* com;
    *valid = 1;

    // cmdBuffer[1] nel caso della in dovrebbe essere il numero di porta
    ret = checkPortNumber(cmdBuffer[1], strlen(cmdBuffer[1]));
    if(ret == 0)
    {
        system("clear");
        printf("(Device) ERRORE: La porta specificata è riservata\nRiprovare.\n");
        *valid = 0;
        return -1;
    }
    if(ret == -1)
    {
        system("clear");
        printf("(Device) ERRORE: Il secondo argomento non è un numero di porta valido\nRiprovare.\n");
        *valid = 0;
        return -1;
    }

    // Se la porta è valida il device prova ad effettuare una connessione 
    convPort = atoi(cmdBuffer[1]);
    ret = setupConnessioneTCPModoClient(&clientSock, &sv_addr, convPort);
    if(ret == 1)
    {
        printf("Messaggio di errore: Server non raggiungibile.\n");
        exit(1);
    }
    
    // 1. Invio un pacchetto TCP al server con su scritto SIG
    spedisciComando(clientSock, "SIG");
    // Per tutto il tempo in cui ricevo UASK devo rimanere ad inserire l'username
    // 2. Mi aspetto la ricezione di un acknowledgment del tipo UASK
    riceviComando(clientSock, &com);
    //3. Il device invia al server un pacchetto di TCP contente l'username che è scritto in cmdBuffer[2]
    // Prima però deve inviare la dimensione dato che non è nota al server
    spedisciStringa(clientSock, (uint8_t*)cmdBuffer[2]);
    riceviComando(clientSock, &com);
    // A questo punto il server avrà controllato l'esistenza dell'utente indicato in cmdBuffer[2]
    if(strcmp(com, "UASK") == 0) // se ri ottengo nuovamente UASK signigica avere un NACK quindi username non corretto/inesistente
    {
        system("clear");
        close(clientSock);
        *valid = 0;
        return 1;
    }
        
    // Altrimenti avrò ricevuto un ACK di tipo PSSW data l'affidabilità di TCP
    // Invio la password
    spedisciStringa(clientSock, (uint8_t*)cmdBuffer[3]);

    // Il server riceverà la password e la controllerà
    // Aspetto un GNT oppure un ERR se la password è giusta o sbagliata
    riceviComando(clientSock, &com);

    if(strcmp(com, "ERR") == 0)
    {
        system("clear");
        close(clientSock);
        *valid = 0;
        return 1;
    } else{
        // è un PSOK
        // Invio la clientPort
        uint16_t clPort; clPort = clientPort;
        inviaOpaco16Bit(clientSock, clPort);
        // Ricevo il comando che mi avvisa se controllare o meno il file
        riceviComando(clientSock, &com);
        if(strcmp(com, "SEEF") == 0)
        {
            int tlog;
            ret = discoverIfLogoutFromFile(cmdBuffer[2], &tlog);
            if(ret == 0) // trovato un orario nel file
            {
                spedisciComando(clientSock, "FOK");
                inviaOpaco32Bit(clientSock, tlog);
            } else { // non trovato orario -> connesso
                spedisciComando(clientSock, "FNOK");
            }
        }
    }
    riceviComando(clientSock, &com);
    if(strcmp(com, "GNT") == 0)
    {
       
        system("clear");
        strcpy(tempUsername, cmdBuffer[2]);
        printf("Salve %s\n", tempUsername);
        pulisciFileOut(tempUsername);
        stampaListaComandi();
        *connected = 1;
    } else {
        system("clear");
        close(clientSock);
        *valid = 0;
        return 1;
    }
    close(clientSock);
    free(com);
    return 0;
}

void signup(int nargs, int* valid)
{
    char* commandRcv;
    *valid = 0;
    // Nel caso del comando signup devono esserci 3 argomenti
    if(controllaLunghezzaArgomenti(nargs, 3) == -1)
        return;
    *valid = 1;
    
    int ret = setupConnessioneTCPModoClient(&clientSock, &sv_addr_reg, 3125);
    if(ret == 1)
    {
        printf("Messaggio di errore: Server non raggiungibile\n");
        exit(1);
    }
    // Primo controllo: l'account esiste?
    // 1. Invio un pacchetto TCP al server con su scritto REG
    spedisciComando(clientSock, "REG");
    // Invio dello username
    spedisciStringa(clientSock, (uint8_t*)cmdBuffer[1]);
    // 2. Aspetto un ACK o un NAK
    riceviComando(clientSock, &commandRcv);
    // Se ricevo un NACK significa che l'username specificato esiste già e non ha bisogno di essere registrato
    if(strcmp(commandRcv, "NAK") == 0)
    {
        printf("Errore: L'utente %s risulta già registrato\n", cmdBuffer[1]);
        *valid = 0;
        free(commandRcv);
        close(clientSock);
        return;
    }

    if(strcmp(commandRcv, "ACK") == 0)
        // Invio la password
        spedisciStringa(clientSock, (uint8_t*)cmdBuffer[2]);
    
    // Ricevo un ACK o NAK
    riceviComando(clientSock, &commandRcv);
    if(strcmp(commandRcv, "NAK") == 0)
    {
        system("clear");
        printf("Server response - ERROR: Impossibile registare %s al momento, riprovare\n", cmdBuffer[1]);
        *valid = 0;
        free(commandRcv);
        close(clientSock);
        return;
    }
    if(strcmp(commandRcv, "ACK") == 0)
    {
        system("clear");
        printf("Server response: Account %s registrato correttamente\n", cmdBuffer[1]);
        *valid = 0;
        free(commandRcv);
        close(clientSock);
        return;
    }
}

// Funzione che chiede al server una disconnessione
void chiediDisconnessione(int nargs, int* valid, char* usr)
{

    *valid = 0;

    // Controllo lunghezza comando
    // Nel caso di out deve esserci un solo argomento, altrimenti il comando non è valido
    if(controllaLunghezzaArgomenti(nargs, 1) == -1)
        return;
    *valid = 1;

    // salvo la cronologia delle chat fatte nel file
    salvaCronologiaChat(usr);
    // salvo istante di disconnessione sul file
    salvaIstanteDisconnessione(usr);

    int ret = setupConnessioneTCPModoClient(&clientSock, &sv_addr, serverPort);
    if(ret == 1)
    {
        printf("Disconnessione avvenuta con server disattivo\n");
        return;
    }   
    // Invio al server il comando OUT
    spedisciComando(clientSock, "OUT");
    // Invio l'username
    spedisciStringa(clientSock, (uint8_t*)usr);

    close(clientSock);
    // close(listeningSocket);
    close(connectedSocket);
    

    printf("Disconnessione avvenuta con successo\n");
    // kill(getpid(), SIGTERM);
    return;
}

int deviceInviaMessaggio(int clientSock, struct message msg)
{
    // struct message msg;
    int ret;

    // Scelto il protocollo BINARY
    // Per i campi di tipo array di char non c'è bisogno di utilizzare funzioni
    // di conversione, perché ciascun carattere sta su 8 bit
    // La conversione in network order è necessaria solo per i tipi numerici

    // Invio username mittente
    ret = spedisciStringa(clientSock, msg.userSend);
    if(ret == 1) // disconnessione
        return 1;
    // Invio username destinatario
    spedisciStringa(clientSock, msg.userDest);
    // Invio info sessione
    inviaOpaco32Bit(clientSock, msg.orarioSessione);
    // Invio porta destinatario
    inviaOpaco16Bit(clientSock, msg.destPort);
    // Invio epochTimeInvio
    inviaOpaco32Bit(clientSock, msg.epochTimeInvio);
    // Invio epochTimeRicezione
    inviaOpaco32Bit(clientSock, msg.epochTimeRicezione);
    // Invio del payload
    inviaOpaco32Bit(clientSock, msg.dimensionePayload);
    ret = send(clientSock, (void*)msg.payload, msg.dimensionePayload, 0);
    if(ret < 0)
    {
        perror("Errore in fase di invio");
        exit(1);
    }
    return 0;
}

// se ritorna 1 la successiva inviaACK non deve essere eseguita
// a causa di una disconnessione dell'altro peer
int deviceRiceviMessaggio(int clientSock, struct message* msg)
{
    int ret, len;
    int16_t lmsg;

    ret = recv(clientSock, (void*)&lmsg, sizeof(uint16_t), 0);
    // devo vedere se la connessione è stata chiusa
    if(ret == 0)
    {
        close(clientSock);
        return 1;
    }

    if(ret < 0)
    {
        perror("Errore in fase di ricezione");
        exit(1);
    }

    len = ntohs(lmsg);
    ret = recv(clientSock, (void*)msg->userSend, len, 0);
    if(ret < 0)
    {
        perror("Errore in fase di ricezione");
        exit(1);
    }

    len = riceviOpaco16Bit(clientSock);
    ret = recv(clientSock, (void*)msg->userDest, len, 0);
    if(ret < 0)
    {
        perror("Errore in fase di ricezione");
        exit(1);
    }
    msg->orarioSessione = riceviOpaco32Bit(clientSock);
    msg->destPort = riceviOpaco16Bit(clientSock);
    msg->epochTimeInvio = riceviOpaco32Bit(clientSock);
    msg->epochTimeRicezione = riceviOpaco32Bit(clientSock);
    msg->dimensionePayload = riceviOpaco32Bit(clientSock);
    
    msg->payload = (uint8_t*)malloc(sizeof(uint8_t)*msg->dimensionePayload);
    
    ret = recv(clientSock, (void*)msg->payload, msg->dimensionePayload, 0);
    if(ret < msg->dimensionePayload)
    {
        perror("Errore in fase di ricezione");
        exit(1);
    }
    // device che riceve, aggiorna il timestamp di ricezione
    msg->epochTimeRicezione = time(NULL);
    return 0;
}

int deviceRiceviMessaggioRidotto(int clientSock, struct message* msg)
{
    int ret, len;
    int16_t lmsg;

    ret = recv(clientSock, (void*)&lmsg, sizeof(uint16_t), 0);
    // devo vedere se la connessione è stata chiusa
    if(ret == 0)
    {
        close(clientSock);
        return 1;
    }

    if(ret < 0)
    {
        perror("Errore in fase di ricezione");
        exit(1);
    }

    len = ntohs(lmsg);
    ret = recv(clientSock, (void*)msg->userSend, len, 0);
    if(ret < 0)
    {
        perror("Errore in fase di ricezione");
        exit(1);
    }

    len = riceviOpaco16Bit(clientSock);
    ret = recv(clientSock, (void*)msg->userDest, len, 0);
    if(ret < 0)
    {
        perror("Errore in fase di ricezione");
        exit(1);
    }
    msg->epochTimeInvio = riceviOpaco32Bit(clientSock);
    return 0;
}

// Spedisce il file di nome filename presente nella current
// port al destinatario in ascolto sulla porta 
// Ritorna -1 se il file non è stato spedito, 0 altrimenti
int spedisciFile(char* filename, int port, char* dest)
{
    int ret; char* com;
    char* buf; // Buffer che contiene i byte del file di nome filename
    // Si controlla nella current folder se è presente il file
    // e si prepara un descrittore di file in modalità read binary
    FILE* fd = fopen(filename, "rb");
    if(fd == NULL)
    {
        if(inGroup == 0) // altrimenti ci sarebbero stampe doppie
            printf("File %s non trovato!\n", filename);
        return -1;
    }
    // Provo a connettermi all'host remoto
    int sts = setupConnessioneTCPModoClient(&peerSock, &dest_addr, port);
    if(sts == 1)
    {
        if(inGroup == 0) // se l'invio del file non è di gruppo evito doppie stampe
        {
            printf("Errore: Impossibile inviare il file.\n");
            printf("Causa: Il peer remoto non è raggiungibile\n");
        }
        return -1;
    }
    // Calcolo la dimensione del file
    fseek(fd, 0, SEEK_END);
    int nbytes = ftell(fd);
    fseek(fd, 0, SEEK_SET);
    // Alloco dinamicamente nbytes per il puntatore buf
    buf = (char*)malloc(nbytes);
    // Leggo dal file tutto in un colpo solo
    fread(buf, nbytes, 1, fd);

    // Invio il comando SHR
    // Gestisco eventuale disconnessione 
    ret = spedisciComando(peerSock, "SHR");
    if(ret == 1)
    {
        if(inGroup == 0)
        {
            printf("Errore: Impossibile inviare il file.\n");
            printf("Causa: Il peer remoto si è disconnesso improvvisamente\n");
        }   
        return -1;
    }
    // Invio anche il nome utente destinatario perché in questo modo
    // so da chi occupa la porta di destinazione su cui invio il file
    // se effettivamente l'username è proprio quello che il sorgente si aspetta
    // Questo perché potrebbe succedere che il device destinarario
    // si disconnetta e cambi porta rispetto alla prima porta utilizzata
    spedisciStringa(peerSock, (uint8_t*)dest);
    // Ricevo un Ack
    riceviComando(peerSock, &com);
    if(strcmp(com, "CHDST") == 0)
    {
        if(inGroup == 0)
        {
            printf("Errore: Impossibile inviare il file.\n");
            printf("Causa: Il peer remoto non è raggiungibile\n");
        }
        return -1;
    }
    // Invio il nome utente del mittente
    spedisciStringa(peerSock, (uint8_t*)tempUsername);
    // Invio il nome del file (anche a destinazione si chiamerà nello stesso modo)
    spedisciStringa(peerSock, (uint8_t*)filename);
    // Spedisco la dimensione del file
    inviaOpaco32Bit(peerSock, nbytes);
    // Spedisco il file
    int offset = 0; // cursore che contiene l'indice dell'ultima cella letta di buf
    int residui = nbytes;
    while(residui > 0)
    {
        // Gestisco anche il caso in cui il buffer del kernel non è sufficiente 
        // a contenere tutti i byte del file
        // Infatti, ciclo più volte perché se il file è troppo grande
        // con una sola send non riesco ad inviare tutti quanti i byte
        ret = send(peerSock, (void*)&buf[offset], residui, 0);
        if(ret < 0)
        {
            perror("Errore in fase di invio");
            exit(1);
        }
        // Send restituisce il numero di byte effettivamente inviati
        residui = residui - ret;
        // Scorro in avanti con il cursore
        offset = offset + ret;
    }
    close(peerSock);
    fclose(fd);
    free(buf); free(com);
    return 0;
}

void incomingFileTransfer(int peerSock)
{
    // Comando SHR già ricevuto ed è quello che mi ha permesso di entrare in 
    // questa funzione
    char* rcvUsr; char* rcvFilename; int size; char* rcvDest;
    char* buf; int ret; char* percorso; char* usr;
    char* foldName;
    char* filename;
    char* path;
    FILE* fd;
    // Ricevo l'username del destinatario
    riceviStringa(peerSock, &rcvDest);
    
    // Invio una risposta sulla base del confronto tra mio username e quello ricevuto
    if(strcmp(rcvDest, tempUsername) == 0)
        spedisciComando(peerSock, "SMDST"); // SAME DESTINATION
    else
    {
        spedisciComando(peerSock, "CHDST"); // CHANGED DESTINATION
        // Non proseguo con la ricezione
        free(rcvDest);
        return;
    }
        
    // Ricevo l'username del mittente del file
    riceviStringa(peerSock, &rcvUsr);
    // Ricevo il nome del file
    riceviStringa(peerSock, &rcvFilename);
    // Ricevo la grandezza del file
    size = riceviOpaco32Bit(peerSock);
    // Ricevo il file
    int offset = 0; int residui = size;
    // Alloco dinamicamente un numero di byte sufficenti per buf
    buf = (char*)malloc(size);
    while(residui > 0)
    {
        ret = recv(peerSock, (void*)&buf[offset], residui, 0);
        if(ret < 0)
        {
            perror("Errore in fase di ricezione");
            exit(1);
        }
        residui = residui - ret;
        offset = offset + ret;
    }
    // Scrivo su disco
    
    // Preparo nome della cartella
    int stlen = strlen(tempUsername) + 2; // perché serve anche lo spazio per /
    foldName = malloc(stlen + strlen("allegatiP2P"));
    strcpy(foldName, "allegatiP2P");
    strncat(foldName, tempUsername, stlen);
    mkdir(foldName, 0700);
    strncat(foldName, "/", 2);
    // Preparo percorso
    
    // Preparo la prima parte
    stlen = strlen(rcvUsr) + 1;
    usr = malloc(stlen + strlen("from_"));
    strcpy(usr, "from_");
    strncat(usr, rcvUsr, stlen);

    // Seconda parte
    stlen = strlen(rcvFilename) + 1;
    filename = malloc(stlen + strlen("_"));
    strcpy(filename, "_");
    strncat(filename, rcvFilename, stlen);

    // Merge della prima parte con la seconda parte
    stlen = strlen(filename) + 1;
    int stlen1 = strlen(usr);
    percorso = malloc(stlen + stlen1);
    strcpy(percorso, usr);
    strncat(percorso, filename, stlen);

    // Merge del nome della cartella con il merged nome file
    path = malloc(strlen(percorso) + strlen(foldName) + 1);
    strcpy(path, foldName);
    stlen = strlen(percorso) + 1;
    strncat(path, percorso, stlen);
    // strncat(foldName, percorso, stlen);

    // Se esiste già un file ricevuto dal medesimo utente
    // con il solito nome questo viene sovrascritto
    fd = fopen(path, "wb");
    fwrite(buf, size, 1, fd);

    printf("Ricevuto %s da %s\n", rcvFilename, rcvUsr);
    printf("Controllare la cartella degli allegati\n");
    fclose(fd); free(foldName); free(rcvDest);
    free(rcvUsr); free(usr);
    free(rcvFilename); free(filename);
    free(buf); free(percorso); free(path);
    return;
}

void outcomingFileTransfer(char* filename)
{
    int ret;
    // Distinguo il caso dei gruppi da quello delle chat singole
    if(inGroup == 0)
    {
        // chat singola
        if(new_port == 0) // non siamo in grado di stabilire il numero di porta di destinazione
        {
            printf("Errore: Impossibile inviare il file.\n");
            printf("CAUSA: INVIARE ALMENO UN MESSAGGIO.\n");
        } else {
            ret = spedisciFile(filename, new_port, tempUniqueDest);
            if(ret == 0)
                printf("File inviato!\n");
        }   
    } else {
        // chat di gruppo
        // va guardato il campo newPort di ciascun elemento della lista listaUsrGrp
        struct stringList* s;
        int contoFalliti = 0; int contoTotale = 0;
        for(s = listaUsrGrp; s != NULL; s = s->pun)
        {
            if(s->newPort != 0)
            {   
                ret = spedisciFile(filename, s->newPort, s->str);
                if(ret == -1)
                    contoFalliti++;
            }
            else
                contoFalliti++;
            contoTotale++;
        }
        // C'è stato almeno un fallimento e stampo un messaggio di errore
        if(contoFalliti != 0)
        {
            if(contoFalliti < contoTotale)
            {
                printf("Errore: Alcuni peer destinatari non hanno ricevuto il file\n");
                printf("POSSIBILI CAUSE:\n");
                printf("- ALCUNI PEER DESTINATARI NON SONO RAGGIUNGIBILI\n");
                printf("- INVIARE ALMENO UN MESSAGGIO DI GRUPPO\n");
            }
            if(contoFalliti == contoTotale)
            {
                printf("Errore: Nessun peer ha ricevuto il file\n");
                printf("POSSIBILI CAUSE:\n");
                printf("- FILE NON TROVATO\n");
                printf("- NESSUNO DEI PEER E' RAGGIUNGIBILE\n");
                printf("- INVIARE ALMENO UN MESSAGGIO DI GRUPPO\n");
            } 
        } else // nessun fallimento
            printf("File inviato in broadcast!\n");
    }
}

// Ritorna il numero di porta inviato dal server al device src
// oppure -1 se il server ha inviato OFF
int deviceRiceviPorta(int sock)
{
    int port;
    char* cmd; uint16_t stdPort;
    
    riceviComando(sock, &cmd);
    if(strcmp(cmd, "ON") == 0)
    {
        stdPort = riceviOpaco16Bit(clientSock);
        port = stdPort;
    }
    if(strcmp(cmd, "OFF") == 0)
        port = -1;
    free(cmd);
    return port;
}

int preparaChat(char* msg)
{
    char* temp; int included = 0;
    temp = malloc(strlen(msg)+1);
    strcpy(temp, msg);
    char* token = strtok(temp, " ");
    if(strcmp(token, "\\q") == 0)
    {
        // connessione clientSock già stata chiusa
        inChat = 0;
        // Eventuali chat di gruppo devono essere disattivate
        inGroup = 0;
        // Pulisco la lista
        ricalcolaUtentiListaUsrGrp();
        system("clear");
        printf("Chat terminata.\n");
        stampaListaComandi();
        free(temp);
        return 1;
    }
    else if(strcmp(token, "\\u") == 0)
    {
        // Chiedo al server la lista degli utenti
        // con il comando GPON
        int ret = setupConnessioneTCPModoClient(&clientSock, &sv_addr, -1);
        if(ret == 1)
        {
            printf("Impossibile avviare chat di gruppo. Server offline\n");
            free(temp);
            return 2;
        }
        inGroup = 1;
        printf("Modalità chat di gruppo avviata\n");
        ricalcolaUtentiListaUsrGrp();
        spedisciComando(clientSock, "GPON");
        // Ricevo il numero di utenti
        int nusers = riceviOpaco32Bit(clientSock);
        int count = 0;
        while(count < nusers)
        {
            char* u;
            // Ricevo ed inserisco nella lista i nomi utenti
            riceviStringa(clientSock, &u);
            // Escludo l'inserimento di me stesso nella lista
            if(strcmp(u, tempUsername) != 0)
            {
                // Nel campo intero della listaUsrGrp metto 1
                // solo per l'utente con cui ho iniziato a parlare
                if(strcmp(u, tempUniqueDest) == 0)
                {
                    included = 1;
                    // si passa peerOpen che è lo stato della connessione
                    // P2P per la chat privata già avviata
                    deviceInserisciListaUsrGrp(u, 1, peerOpen);
                }
                else
                    // per gli utenti nuovi aggiunti nel gruppo
                    // sicuramente P2P non è aperta perché sono stati appena
                    // aggiunti, e neppure sono selezionati
                    deviceInserisciListaUsrGrp(u, 0, 0);
            }
            free(u);
            count++;
        }
        printf(" ------ Lista degli utenti online ------ \n");
        stampaStringList(listaUsrGrp);
        printf(" --------------------------------------- \n");
        // L'utente tempUniqueDest, ovvero il destinatario originale
        // della conversazione fintantoché la chat era privata,
        // deve essere incluso nel gruppo indipendentemente dal fatto che sia
        // online o meno senza però che appaia in output nella lista degli utenti online
        // Lo aggiungo alla lista dopo aver stampato
        // se non lo avevo già fatto prima
        // Questo sono in grado di saperlo con la variabile included
        if(included == 0)
            deviceInserisciListaUsrGrp(tempUniqueDest, 1, peerOpen);
        free(temp);
        close(clientSock);
        return 2;
    }
    else if(strcmp(token, "\\a") == 0)
    {
        if(inGroup == 0)
        {
            printf("Comando non valido in una chat privata!\n");
            return 3;
        }
        char* partecipant = strtok(NULL, " ");
        // Guardo se partecipant è una stringa vuota
        if(partecipant == NULL)
        {
            printf("Sintassi comando non valida\n");
            return 3;
        }
        // Verifico appartenza di partecipant alla rubrica
        if(controllaRubrica(tempUsername, partecipant) == -1)
            printf("%s non in rubrica\n", partecipant);
        else
        {
            // Cerco nella lista user grp
            // Se non c'è notifico
            // Se c'è modifico il campo della lista e notifico
            struct stringList* p = visitaListaUsrGrp(partecipant);
            if(p == NULL)
                printf("L'utente %s non può essere aggiunto al gruppo\n", partecipant);
            else
            {
                p->port = 1;
                int print = 2; // Messaggio: l'utente è stato aggiunto al gruppo
                // stampo la cronologia dei messaggi
                stampaDeviceListaMsgGruppi(tempUsername, print);
            }
        }
        free(temp);
        return 3;   
    }
    else if(strcmp(token, "\\share") == 0)
    {
        char* fileName = strtok(NULL, " ");
        // Gestione caso stringa vuota
        if(fileName == NULL)
        {
            printf("Sintassi comando non valida\n");
            return 4;
        }
        // Parto con la condivisione del file
        outcomingFileTransfer(fileName);
        return 4;

    }
    else if(strcmp(token, "\\deb") == 0)
    {
        stampaStringListDeb(listaUsrGrp);
        return 5;
    }
    else // se non è nessuno dei precedenti è un messaggio normale
    {
        free(temp);
        return 0;
    }
}

int avviaChat(char* sndusr, char* dstusr, int lunghmsg, int* p2pOpen, int* newPort){

    char* cmd = NULL; int retSv; int print = -1; int chgP = 0;
    
    // Impacchetto il messaggio
    struct message msg;
    msg.dimensionePayload = lunghmsg;
    msg.payload = (uint8_t*)malloc(sizeof(uint8_t)*msg.dimensionePayload);
    strcpy((char*)msg.payload, tempMessage);
    strcpy((char*)msg.userSend, sndusr);
    strcpy((char*)msg.userDest, dstusr);
    msg.epochTimeInvio = time(NULL); // current_timestamp
    msg.epochTimeRicezione = -1;
    msg.pun = NULL;
    
    // Devo capire se è la prima volta che mando il messaggio
    int port = 0;
    int ret = cercaInStringList(dstusr, &port);
    if(ret == -1) // prima volta
    {  
        if(esitoConnessione == -1)
                retSv = setupConnessioneTCPModoClient(&clientSock, &sv_addr, -1);
            else
                retSv = esitoConnessione;
        if(retSv == 1) // Gestione del caso in cui la connessione con il server cada subito
        {
           printf("Errore: connessione con il server interrotta. Impossibile proseguire\n");
           salvaIstanteDisconnessione(tempUsername);
           exit(0);
        }    
        spedisciComando(clientSock, "TLK");
        printf("Connessione in corso.. Attendere prego\n");
        deviceInviaMessaggio(clientSock, msg);
        // Ci sarà l'handshake con il destinatario
        port = deviceRiceviPorta(clientSock); // risultato dell'handshake
        *newPort = port;
        // porta vale -1 se il server ha inviato un OFF
        // Registro il risulato dell'handshake con dstusr
        deviceInserisciStringList(dstusr, port);
        if(port == -1)
        {
            *p2pOpen = 0;
             // Destinatario non raggiungibile
            deviceInserisciListaMsgLocali(msg, 1, -1);
        } else {
            *p2pOpen = 1; // le prossime volte mi connetto p2p
            deviceInserisciListaMsgLocali(msg, 1, time(NULL));
        }
    } else { // le volte successive
        if(*p2pOpen == 1)
        {
            if(esitoConnessione == -1)
                retSv = setupConnessioneTCPModoClient(&clientSock, &sv_addr, -1);
            else
                retSv = esitoConnessione;
            if(retSv != 1) // se server disconnesso con p2p chat continuano
            {
                spedisciComando(clientSock, "TLK");
                deviceInviaMessaggio(clientSock, msg);
                // si guarda se eventualmente è cambiata porta, se è cambiata ci pensa da sè a cambiarla
                *newPort = riceviOpaco16Bit(clientSock);
                if(*newPort == 2)
                    chgP = 1;
            } else{
                print = 0; // Messaggio: Il server si è disconnesso
                // new_port inviariata
            }
            ret = setupConnessioneTCPModoClient(&peerSock, &dest_addr, *newPort);
            if(ret == 1 || chgP == 1) // caduta la connessione
            {
                // *p2pOpen = 0;
                print = 1;
                // se il server è spento esco perché messaggio non viene
                // nè inviato a dev dst, nè bufferizzato
                if(retSv == 1)
                {
                    printf("Errore: Impossibile bufferizzare i messaggi. Server offline\n");
                    salvaIstanteDisconnessione(tempUsername);
                    exit(0);
                }
                deviceInserisciListaMsgLocali(msg, 1, -1);
            } else {
                spedisciComando(peerSock, "CHT");
                deviceInviaMessaggio(peerSock, msg);
                riceviComando(peerSock, &cmd);
                close(peerSock);
                deviceInserisciListaMsgLocali(msg, 1, time(NULL));
            }
        } 
        else {
            if(esitoConnessione == -1)
                retSv = setupConnessioneTCPModoClient(&clientSock, &sv_addr, -1);
            else
                retSv = esitoConnessione;
            if(retSv == 1)
            {
                printf("Errore: connessione con il server interrotta. Impossibile proseguire\n");
                salvaIstanteDisconnessione(tempUsername);
                exit(0);
            }
            spedisciComando(clientSock, "TLK");
            deviceInviaMessaggio(clientSock, msg);
            riceviOpaco16Bit(clientSock);
            deviceInserisciListaMsgLocali(msg, 1, -1);
        }    
    }
    close(clientSock);
    esitoConnessione = -1;
    if(cmd != NULL)
        free(cmd);
    return print;
}

void avviaChatGruppo(char* sender, int lmsg)
{
    // Scorro la lista dei destinatari selezionati
    struct stringList* p;
    char* dest;
    for(p = listaUsrGrp; p != NULL; p = p->pun)
    {
        // Per ogni utente destinatario selezionato chiamo la avviaChat
        if(p->port == 1)
        {
            dest = malloc(strlen(p->str)+1);
            strcpy(dest, p->str);
            avviaChat(sender, dest, lmsg, &p->peerOpen, &p->newPort);
        }
    }
    return;
}

void handlerSigInt()
{
    if(connected == 1)
    {
        // salvo la cronologia delle chat fatte nel file
        salvaCronologiaChat(tempUsername);
        // salvo istante di disconnessione sul file
        salvaIstanteDisconnessione(tempUsername);
        printf("\nUscita in corso(priva di OUT)...\n");
        exit(0);
    }
    // Se non connesso non faccio niente: esco e basta
    exit(0);
}

void handlerSigTerm()
{
    if(connected == 1)
        // salvo istante di disconnessione
        salvaIstanteDisconnessione(tempUsername);
    // Se non connesso esco e basta
    exit(0);
}

void incomingRiscontroMessaggioBufferizzato(struct message msg, int orario)
{
    // Prima visito la lista dei messaggi in cronologia
    struct message* p;
    for(p = listaMsgScaricati; p != NULL; p = p->pun)
    {
        // Cerco il messaggio e lo riscontro
        // NB: si ipotizza che nell'intervallo di tempo
        // di 1 secondo non esista più di un mesaggio
        // inviato da un certo utente per un certo destinatario
        if(strcmp((char*)p->userSend, (char*)msg.userSend) == 0 && 
        strcmp((char*)p->userDest, (char*)msg.userDest) == 0 && p->epochTimeInvio == msg.epochTimeInvio
        && p->epochTimeRicezione == -1)
            p->epochTimeRicezione = orario;
    }

    // Poi visito la lista dei messaggi locali
    for(p = listaMsgLocali; p != NULL; p = p->pun)
    {
        // Cerco il messaggio e lo riscontro
        // NB: si ipotizza che nell'intervallo di tempo
        // di 1 secondo non esista più di un mesaggio
        // inviato da un certo utente per un certo destinatario
        if(strcmp((char*)p->userSend, (char*)msg.userSend) == 0 && 
        strcmp((char*)p->userDest, (char*)msg.userDest) == 0 && p->epochTimeInvio == msg.epochTimeInvio
        && p->epochTimeRicezione == -1)
            p->epochTimeRicezione = orario;
    }
    return;
}

void distruggiListaNotifiers(struct notificationEntry** testa)
{
    struct notificationEntry* p = *testa;
    struct notificationEntry* q;
    while(p != NULL)
    {
        q = p->pun;
        free(p);
        p = q;
    }
    *testa = NULL;
}

void stampaNotifiersList()
{
    struct notificationEntry* p = deviceNotifiersList;
    printf("Notifiche: \n");

    // Caso lista vuota
    if(p == NULL)
    {
        printf("Nessuna notifica\n");
        return;
    }

    for(p = deviceNotifiersList; p != NULL; p = p->pun)
    {
        char *orario;
        convertFromEpoch(p->lastTimestamp, &orario);
        printf(".) %s num. m. ricevuti: %d ultimo m. del: %s", p->user, p->count, orario);
        printf("\n");
        free(orario);
    }
    return;
}

void inserisciNuovaNotifica(char* user, int count, time_t lastTime)
{
    struct notificationEntry* p; struct notificationEntry* q;
    for(q = deviceNotifiersList; q != NULL; q = q->pun)
        p = q;
    q = malloc(sizeof(struct notificationEntry));
    strcpy(q->user, user);
    q->count = count;
    q->lastTimestamp = lastTime;
    q->pun = NULL;
    if(deviceNotifiersList == NULL)
        deviceNotifiersList = q;
    else
        p->pun = q;
}

void chiediNotifiche()
{
    int ret; int countIter = 0; // numero totale di iterazioni da 
    char* rcvUsr;
    // Mi connetto al server
    ret = setupConnessioneTCPModoClient(&clientSock, &sv_addr, -1);
    if(ret == 1)
    {
        printf("Errore: Impossibile scaricare le notifiche. Server offline\n.");
        return;
    }

    // Invio al server il comando HAN che triggera
    // la funzione apposita di gestione delle richieste di notifiche
    // da parte dei peer
    spedisciComando(clientSock, "HAN");

    // Spedisco l'username che richiede la lista delle notifiche
    spedisciStringa(clientSock, (uint8_t*)tempUsername);

    // Mi aspetto:
    // 1. numero di elementi della lista (per sapere quante volte devo ciclare a ricevere)
    // 2. ciclicamente
    // 2.a username
    // 2.b numero di messaggio
    // 2.c ultimo timestamp
    // Distruggo la lista
    distruggiListaNotifiers(&deviceNotifiersList);
    countIter = riceviOpaco32Bit(clientSock);
    int count = 0; // conto attuale delle iterazioni fin ora eseguite
    while(count < countIter)
    {
        int rcvNum, rcvTime;
        // Ricevo username
        riceviStringa(clientSock, &rcvUsr);
        // Ricevo numero dei messaggi
        rcvNum = riceviOpaco32Bit(clientSock);
        // Ricevo ultimo timestamp
        rcvTime = riceviOpaco32Bit(clientSock);
        inserisciNuovaNotifica(rcvUsr, rcvNum, rcvTime);
        count++;
    }
    stampaNotifiersList();
    close(clientSock);
    free(rcvUsr);
    return;
    
}

void chiediMessaggiBufferizzati(char* username)
{
    // username è l'utente sorgente
    int ret;
    // Mi connetto al server
    ret = setupConnessioneTCPModoClient(&clientSock, &sv_addr, -1);
    if(ret == 1)
    {
        printf("Errore: Impossibile scaricare le notifiche. Server offline\n.");
        return;
    }

    // Invio al server il comando SHW che triggera
    // la funzione apposita di consegna dei messaggi bufferizzati
    spedisciComando(clientSock, "SHW");
    // Invio username sorgente dei messaggi
    spedisciStringa(clientSock, (uint8_t*)username);
    // Invio u destinatario dei messaggi
    spedisciStringa(clientSock, (uint8_t*)tempUsername);
    // Inizio a ricevere i messaggi bufferizzati
    // Mi aspetto
    // - numero dei messaggi
    // - un loop di riceviMessaggio
    int numMessaggi = riceviOpaco32Bit(clientSock);
    int count = 0;
    while(count < numMessaggi)
    {
        struct message rcvMsg;
        ret = deviceRiceviMessaggio(clientSock, &rcvMsg);
        if(ret != 1)
        {
            spedisciComando(clientSock, "ACK");
            deviceInserisciListaMsgLocali(rcvMsg, 0, 0);
            count++;
        } else {
            // altrimenti la connessione è caduta e non mi aspetto più ulteriori messaggi
            printf("Non è stato possibile scaricare l'elenco completo dei messaggi\n");
            printf("Causa: Connessione con il server persa\n");
            if(count != 1)
                printf("Sono stati scaricati %d messaggi\n", count);
            else
                printf("E' stato scaricato %d messaggio\n", count);
            close(clientSock);
            return;
        }
    }
    if(count != 1)
        printf("Sono stati scaricati %d messaggi\n", count);
    else
        printf("E' stato scaricato %d messaggio\n", count);
    close(clientSock);
    return;

}


void recuperaRiscontriPendenti(char* sender, char* dest)
{
    // Invio un comando al server
    // Il server risponde con il numero dei messaggi da riscontrare
    spedisciComando(clientSock, "RNFY");

    // Invio sender
    spedisciStringa(clientSock, (uint8_t*)sender);
    
    // Invio dest
    spedisciStringa(clientSock, (uint8_t*)dest);
    
    // Ricevo il numero di messaggi
    int nmsg = riceviOpaco32Bit(clientSock);
    int count = 0;
    while(count < nmsg)
    {
        struct message rcv; int orario;
        // ricevo l'orario di riscontro
        orario = riceviOpaco32Bit(clientSock);
        // ricevo qualcosa relativamente alla notifica di consegna
        deviceRiceviMessaggioRidotto(clientSock, &rcv);
        incomingRiscontroMessaggioBufferizzato(rcv, orario);
        count++;
    }
    close(clientSock);
}


int main(int argc, char* argv[])
{
    // La seguente chiamata a signal viene fatta per evitare che il sistema
    // operativo nel caso in cui un peer chiuda un socket faccia terminare 
    // il processo associato al main device. Ignorare il segnale di broken pipe
    // permette di gestire anche le disconnessioni tramite azioni
    // da me stabilite piuttosto che far intervenire il sistema operativo
    // con la terminazione di default del processo
    signal(SIGPIPE, SIG_IGN);

    // Gestisco anche l'arrivo del segnale SIGINT (CTRL+C), trattata nel progetto
    // come una disconnessione da parte del client senza eseguire out
    signal(SIGINT, handlerSigInt);

    // Gestisco il segnale SIGTERM che comporta la chiusura forzata di un device
    // con il salvataggio del timestamp di logout
    signal(SIGTERM, handlerSigTerm);

    // inizialmente sicuramente il comando non è valido poiché non ne è stato digitato alcuno
    int valid = 0;
    // numero di argomenti del comando digitato
    int nargs;
    // si suppone che un comando stia al più su due righe di shell
    char rcvInputfromBash[UNIXTERM_WIDTH * 2];
    int ret;
    char appUsername[UNIXTERM_WIDTH]; // nome utente del sender

    // Indirizzo per il socket di ascolto lato device
    struct sockaddr_in devAddr;

    // --- Parte relativa alla select
    fd_set master;
    fd_set read_fds;
    int fdmax = -1;
    int select_ret, i, addrlen;

    // Azzero i set
    FD_ZERO(&master);
    FD_ZERO(&read_fds);
    // ---

    printf("Device avviato...\n");

    if(argc != 2)
    {
        printf("ERRORE FATALE. Numero inatteso di argomenti\n");
        exit(1);
    }

    ret = checkPortNumber(argv[1], strlen(argv[1]));
    if(ret == 0)
    {
        printf("(Device) ERRORE: La porta specificata è riservata\nRiprovare.\n");
        return -1;
    }
    if(ret == -1)
    {
        printf("(Device) ERRORE: Il secondo argomento non è un numero di porta valido\nRiprovare.\n");
        return -1;
    }

    clientPort = atoi(argv[1]);
    
    // Se il comando non è valido non c'è dubbi: rimango a chiedere il comando
    // Avere il comando valido non basta per uscire dal ciclo, serve anche essere connessi
    while(!valid)
    {        
        while(!connected)
        {
            welcomeMessage();
            // Chiedo (nuovamente) un comando
            // Lo faccio leggendo un'intera riga
            fgets(rcvInputfromBash, UNIXTERM_WIDTH*2, stdin);

            // Calcolo il numero di argomenti: è il numero di spazi + 1 (argomento iniziale)
            nargs = countBlankSpaces(rcvInputfromBash) + 1;
        

            // Alloco un numero adeguato di locazioni
            cmdBuffer = (char**)malloc(sizeof(char*)*nargs);
            if(cmdBuffer == NULL)
            {
                perror("(FATAL ERROR): Spazio di memoria insufficiente");
                exit(0);
            }

            chunckingString(rcvInputfromBash); // scrive nella variabile globale cmdBuffer
            deleteExtraNewline(nargs);

            // Controlli lato client
            // 1. controllo se il comando è valido
            // nella fase iniziale sul device prima della connessione sono validi solamente signup e in
            // il nome del comando è contenuto in cmdBuffer[0]
            // Si suppone che il riconoscimento del comando sia case SENSITIVE
            if(strcmp(cmdBuffer[0], "in") == 0)
            {

                if(controllaLunghezzaArgomenti(nargs, 4) == -1)
                    continue;

                // Mi salvo il numero di porta in una variabile globale
                serverPort = atoi(cmdBuffer[1]);
                // Mi salvo anche il nome utente
                strcpy(appUsername, cmdBuffer[2]);
            
                // valid è passato per riferimento
                ret = in(nargs, &valid, &connected);
                if(ret == 1) // Stampa il messaggio che username è errato
                    stampaNegatoAuth();
            }
            else if(strcmp(cmdBuffer[0], "signup") == 0)
            {
                // valid è passato per riferimento
                signup(nargs, &valid);
            }
            else
            {
                printf("(Device): ERRORE. Comando non valido o inesistente\nRiprovare. \n");
                continue;
            }
        }
    }

    close(listeningSocket);
    // Parte relativa all'ascolto di richieste di chat e di richieste di hs con il server

    // Creo un socket di ascolto
    listeningSocket = socket(AF_INET, SOCK_STREAM, 0);
    if(listeningSocket < 0)
    {
        perror("Errore nella creazione del socket:");
        exit(1);
    }
    const int enable = 1;
    if (setsockopt(listeningSocket, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0)
    {
        perror("setsockopt(SO_REUSEADDR) failed");
        exit(1);
    }

    memset(&devAddr, 0, sizeof(devAddr));
    devAddr.sin_family = AF_INET;
    devAddr.sin_port = htons(clientPort);
    inet_pton(AF_INET, "127.0.0.1", &devAddr.sin_addr);

    ret = bind(listeningSocket, (struct sockaddr*) &devAddr, sizeof(devAddr));
    if(ret < 0)
    {
        perror("Errore nella bind:");
        exit(1);
    }

    ret = listen(listeningSocket, 10);
    if(ret < 0)
    {
        perror("Errore in fase di listen:");
        exit(1);
    }

     // Parte relativa alla select
    // metto i socket di ascolto nel set principale
    FD_SET(0, &master); // 0: stdin
    FD_SET(listeningSocket, &master);

    // tengo traccia del maggiore tra i due socket
    if(listeningSocket > 0)
        fdmax = listeningSocket;
    else
        fdmax = 0;
    // ---

    while(1)
    {
        read_fds = master;
        select_ret = select(fdmax + 1, &read_fds, NULL, NULL, NULL);
        if(select_ret < 0)
        {
            perror("Errore nella select:");
            exit(1);
        }
        for(i = 0; i<=fdmax; i++) {// scorro iterativamente il set dei descrittori
         // controllo i descrittori pronti in lettura
        if(FD_ISSET(i, &read_fds))
        {
            if(i == listeningSocket)
            {
                addrlen = sizeof(sender_addr);
                newsd = accept(listeningSocket, (struct sockaddr*)&sender_addr, (socklen_t*)&addrlen);
                if(newsd < 0)
                {
                    perror("Errore nella accept in fase di ricezione di un messaggio:");
                    exit(1);
                }
                // Aggiungo il socket connesso al master di lettura
                FD_SET(newsd, &master);
                if(newsd > fdmax) fdmax = newsd;
            }
            if(i == newsd)
            {
                connectedSocket = i;
                FD_CLR(i, &master);
                char* cmd;
                // Riceve un comando per distinguere 
                // chat in arrivo
                // file in arrivo
                // ack di un messaggio bufferizzato in arrivo
                riceviComando(connectedSocket, &cmd);
                if(strcmp(cmd, "CHT") == 0)
                {
                    int ret;
                    // connectedSocket è il socket di comunicazione
                    struct message rcv;
                    // è il messaggio ricevuto carne ed ossa
                    ret = deviceRiceviMessaggio(connectedSocket, &rcv);
                    if(ret != 1)
                    {
                        spedisciComando(connectedSocket, "ACK");
                        // Prima va controllato che il messaggio non esista già
                        deviceInserisciListaMsgLocali(rcv, 0, 0);
                        if(inChat == 1)
                        {
                            if(inGroup == 1)
                            {
                                // caso di un gruppo con un solo partecipante collassa
                                // a chat privata
                                int conto = contaUtentiSelezionatiGrp();
                                if(conto == 1)
                                    stampaDeviceListaMsg(tempUsername, tempUniqueDest, -1);
                                else
                                    stampaDeviceListaMsgGruppi(tempUsername, -1);
                            }
                            else // caso chat privata
                                stampaDeviceListaMsg(tempUsername, tempUniqueDest, -1);
                        }
                        close(connectedSocket);
                    }
                    else
                        close(connectedSocket);
                }
                if(strcmp(cmd,"SHR") == 0)
                {
                    // ricevo qualcosa relativamente al file
                    incomingFileTransfer(connectedSocket);
                    close(connectedSocket);
                }
                if(strcmp(cmd,"NFY") == 0)
                {
                    struct message rcv; int orario;
                    // ricevo l'orario di riscontro
                    orario = riceviOpaco32Bit(connectedSocket);
                    // ricevo qualcosa relativamente alla notifica di consegna
                    deviceRiceviMessaggio(connectedSocket, &rcv);
                    incomingRiscontroMessaggioBufferizzato(rcv, orario);
                    if(inChat == 1)
                    {
                            if(inGroup == 1)
                            {
                                // caso di un gruppo con un solo partecipante collassa
                                // a chat privata
                                int conto = contaUtentiSelezionatiGrp();
                                if(conto == 1)
                                    stampaDeviceListaMsg(tempUsername, tempUniqueDest, -1);
                                else
                                    stampaDeviceListaMsgGruppi(tempUsername, -1);
                            }
                            else // caso chat privata
                                stampaDeviceListaMsg(tempUsername, tempUniqueDest, -1);
                    }
                        close(connectedSocket);
                }
            }
            // Caso stdin
            if(i == 0)
            {
                if(inChat == 1)
                {
                    // stampaDeviceListaMsgLocali(tempUsername);
                    // ogni input deve essere intrepretato come messaggio
                    fgets(tempMessage, MSG_MAX_LEN, stdin);
                    uint32_t lunghmsg = strlen(tempMessage);
                    tempMessage[lunghmsg - 1] = '\0';
                    // Controllo se il messaggio digitato è un messaggio vuoto
                    if(strcmp(tempMessage, "\0") == 0)
                    {
                        printf("Digitare un messaggio!\n");
                        continue;
                    }
                    int ret = preparaChat(tempMessage);
                    if(ret != 0)
                        continue;
                    if(inGroup == 0) // la chat è privata
                    {
                        int p = avviaChat(appUsername, tempUniqueDest, lunghmsg, &peerOpen, &new_port);
                        stampaDeviceListaMsg(tempUsername, tempUniqueDest, p);
                        continue;
                    } else {
                        // la chat è di gruppo
                        // andrà chiamata tante volte la avviaChat quanti sono i destinatari 
                        // selezionati nella listaUsrGrp
                        avviaChatGruppo(tempUsername, lunghmsg);
                        // Devo controllare la listaUsrGrp per scoprire se c'è qualche
                        // utente selezionato con /a
                        int conto = contaUtentiSelezionatiGrp();
                        if(conto == 1)
                            stampaDeviceListaMsg(tempUsername, tempUniqueDest, -1);
                        else
                            stampaDeviceListaMsgGruppi(tempUsername, -1);
                        continue;
                    }
                    
                }
                // Resetto la variabile valid
                // stampaListaComandi();
                // Prende un comando
                fgets(rcvInputfromBash, UNIXTERM_WIDTH*2, stdin);
                nargs = countBlankSpaces(rcvInputfromBash) + 1;
                
                // Alloco un numero adeguato di locazioni
                cmdBuffer = (char**)malloc(sizeof(char*)*nargs);
                if(cmdBuffer == NULL)
                {
                    perror("(FATAL ERROR): Spazio di memoria insufficiente");
                    exit(0);
                }

                chunckingString(rcvInputfromBash); // scrive nella variabile globale cmdBuffer
                deleteExtraNewline(nargs);

                // Adesso cmdBuffer contiene per ogni locazione un argomento

                // Qua si fa la distinzione dei comandi digitati
                // Comando chat
                if(strcmp(cmdBuffer[0], "chat") == 0)
                {
                    if(controllaLunghezzaArgomenti(nargs, 2) == -1)
                    {
                        stampaListaComandi();
                        continue;
                    }
                    // Nel caso del comando chat devono esserci 2 argomenti
                    // il secondo argomento, quello contenuto nella posizione 1 del vettore
                    // è il nome utente nel caso del comando chat
                    strcpy(tempUniqueDest, cmdBuffer[1]);
                    
                    // Va controllata la rubrica
                    
                    int check = controllaRubrica(tempUsername, tempUniqueDest);
                    if(check == -1)
                    {
                        printf("%s non presente nella rubrica\n", tempUniqueDest);
                        continue;
                    }

                    // Prima di mettere inChat ad 1, bisogna verificare che il server
                    // ci sia sempre, perché solo le chat in corso possono proseguire
                    // Una volta chiusa la chat, se il server non è più disponibile, la chat non può essere avviata
                    // Per fare questa verifica inviamo un pacchetto al server

                    int esitoConnessione = setupConnessioneTCPModoClient(&clientSock, &sv_addr, -1);
                    if(esitoConnessione == 1)
                    {
                        printf("Errore: impossibile avviare chat. Server offline\n");
                        continue;
                    }
                    // altrimenti esitoConnessione = 0
                    
                    // Va scaricata dal file la lista dei messaggi vecchi per la cronologia
                    recuperaCronologiaChat(tempUsername);
                    // Vanno recuperati eventuali riscontri di messaggi pendenti
                    recuperaRiscontriPendenti(tempUsername, tempUniqueDest);
                    stampaDeviceListaMsg(tempUsername, tempUniqueDest, -1);
                    inChat = 1;
                    continue;
                }

                // Comando hanging
                else if(strcmp(cmdBuffer[0], "hanging") == 0)
                {
                    system("clear");
                    if(controllaLunghezzaArgomenti(nargs, 1) == -1)
                    {
                        stampaListaComandi();
                        continue;
                    }
                    chiediNotifiche();
                    stampaListaComandi();
                    continue;

                }
                // Comando show username
                else if(strcmp(cmdBuffer[0], "show") == 0)
                {
                    system("clear");
                    if(controllaLunghezzaArgomenti(nargs, 2) == -1)
                    {
                        stampaListaComandi();
                        continue;
                    }
                    // Il secondo parametro di cmdBuffer è l'username target
                    // cioè il sender dei messaggi
                    chiediMessaggiBufferizzati(cmdBuffer[1]);
                    stampaListaComandi();
                    continue;
                }

                // Comando out
                else if(strcmp(cmdBuffer[0], "out") == 0)
                {
                    // rompe il ciclo infinito
                    chiediDisconnessione(nargs, &valid, appUsername);
                    return 0;
                            
                }
                else if(strcmp(cmdBuffer[0], "debuglistamsg") == 0)
                {
                    stampaDeviceListaMsgLocaliDebug();
                    continue;
                }
                else{
                    printf("Sintassi comando errata. Riprovare \n");
                    continue;
                }
            }
        }
      }
   }
    return 0;
}