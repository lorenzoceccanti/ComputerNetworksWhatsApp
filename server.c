// Progetto di Reti Informatiche 575II
// Ceccanti Lorenzo 564490
// Codice del Server

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
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>

// Elenco delle costanti
#define INDIRIZZO_IP "127.0.0.1"
#define UNIXTERM_WIDTH 80
#define BUF_LEN 256

// Elenco delle strutture dati

// Definizione della struttura dei messaggi
struct message{
    uint8_t userSend[UNIXTERM_WIDTH];
    uint8_t userDest[UNIXTERM_WIDTH];
    uint32_t orarioSessione;
    uint16_t destPort;
    uint32_t epochTimeInvio;
    uint32_t epochTimeRicezione;
    uint32_t dimensionePayload; // questa è la dimensione effettiva, comunque
    // è al massimo 1802 byte
    uint8_t* payload; // testo del messaggio allocato dinamicamente
    struct message* pun; // serve per creare una lista di messaggi
};

struct account{
    // sicuramente non più di 80 caratteri
    char username[UNIXTERM_WIDTH];
    // sicuramente non più di 80 caratteri
    char password[UNIXTERM_WIDTH];
};

struct regEntry{
    char user_dest[UNIXTERM_WIDTH];
    uint16_t port;
    time_t timestamp_login;
    time_t timestamp_logout;
    struct regEntry* pun;
};
// Hanging
struct notificationEntry{
    char user[UNIXTERM_WIDTH]; // nome utente
    uint32_t count; // numero di messaggi pendenti in ingresso
    uint32_t lastTimestamp; // timestamp del messaggio più recente
    struct notificationEntry* pun;
};

// Realizza il concetto di lista delle sessioni che hanno fallito l'handshake con il destinatario
struct session{
    int num;
    struct session* pun;
};

// Lista di elementi di tipo struct regEntry con cui realizzo il concetto di regsitro
struct regEntry* serverRegisterList;

// Lista di elementi di tipo struct message con cui realizzo sia la coda dei messaggi in
// attesa di recapito (ad esclusione dei messaggi da notificare aglu username offline) sia la cronologia dei messaggi inviati
struct message* serverMessageList;

// Lista dei messaggi che sono in attesa di essere notificati agli originali mittenti (perché mittenti offline)
struct message* serverListaMsgDaNotificare;

// Lista da inviare ogni volta che un device ci richiede un hanging
struct notificationEntry* serverNotifiersList;

struct session* sessionFailedList;

// Elenco variabili globali
struct sockaddr_in sv_addr; // indirizzo principale del server
struct sockaddr_in cl_addr; // indirizzo del device mittente
struct sockaddr_in sv_addr_reg; // porta 3125 del server dedicata alle registrazioni
struct sockaddr_in endPoint_addr; // indirizzo del device destinatario
// Socket di ascolto
int sockServer = -1; int regSocket = -1;
// Socket di comunicazione
int newsd = -1; int clientSock = -1; int clDestSocket = -1;


// Elenco delle funzioni

void printServerCmd()
{
    printf("Digita un comando: \n");
    printf("1) help --> mostra i dettagli dei comandi\n");
    printf("2) list --> mostra un elenco degli utenti connessi\n");
    printf("3) esc  --> chiude il server\n");
}

// ---- Funzioni che manipolando la lista serverRegisterList

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

void inizializzaRegisterList()
{
    serverRegisterList = NULL;
}

// Inserisce un elemento di tipo struct regEntry in fondo alla lista serverRegisterList
void inserisciElementoRList(char* _user_dest, uint16_t _port, time_t _timestamp_login, time_t _timestamp_logout)
{
    struct regEntry* p; struct regEntry* q;
    for(q = serverRegisterList; q != NULL; q = q->pun)
        p = q;

    q = malloc(sizeof(struct regEntry));
    strcpy(q->user_dest, _user_dest);
    q->port = _port;

    q->timestamp_login = _timestamp_login;
    q->timestamp_logout = _timestamp_logout;
    q->pun = NULL;

    if(serverRegisterList == NULL)
        serverRegisterList = q;
    else
        p->pun = q;
}

// Inserisce una sessione nella lista delle sessioni fallite
void inserisciListaSession(int num)
{
    struct session* p; struct session* q;
    for(q = sessionFailedList; q != NULL; q = q->pun)
        p = q;
    q = malloc(sizeof(struct session));
    q->num = num;
    q->pun = NULL;
    if(sessionFailedList == NULL)
        sessionFailedList = q;
    else
        p->pun = q;
}

// Stabilisce se esiste la sessione passata è fallita
// Ritorna 1 se la sessione esiste nella lista dei fallimenti, 0 altrimenti
int isAFailedSession(int num)
{
    struct session* s;
    for(s = sessionFailedList; s != NULL; s = s->pun)
        if(s->num == num)
            return 1;
    return 0;
}

// Funzione per cercare un username nella lista
// Ritorna anche il puntatore all'elemento
struct regEntry* searchDeviceByUsername(char* usr)
{
    struct regEntry* q;
    for(q = serverRegisterList; q != NULL; q = q->pun)
    {
        // 1. Il server cerca nel suo registro l'username
        // se non c'è -> usr offline
        // se c'è -> si valuta l'altra condizione
        // 2. Il server guarda il campo timestamp_logout di usr
        // se è != -1 -> username offline
        // altrimenti -> username online 
        if(strcmp(q->user_dest, usr) == 0 && q->timestamp_logout == -1)
            break;
    }
    // Se non lo trovo q è arrivato a NULL e lo restituisco
    return q;
}
// Restituisce 0 se la porta è disponibile, -1 altrimenti
int portAvailable(uint16_t port)
{
    // timestamp_logout -1 significa esiste un login in corso su quella porta
    // (Non registrato ancora il logout)
    struct regEntry* r;
    for(r = serverRegisterList; r!=NULL; r = r->pun)
        if(r->port == port && r->timestamp_logout == -1) // porta occupata
            return -1;
    return 0;
}

// Ritorna 0 se usr è online, -1 altrimenti
int isUsernameOnline(char* usr)
{   
    struct regEntry* p = searchDeviceByUsername(usr);
    if(p == NULL)
        return -1;
    else
        return 0;
}

// Funzione per cercare una porta nella lista
// Ritorna anche il puntatore all'elemento
struct regEntry* disconnectDeviceByPort(uint16_t port)
{
    struct regEntry* q;
    for(q = serverRegisterList; q != NULL; q = q->pun)
    {   
        // Se lo trovo mi fermo
        if(q->port == port)
            break;
    }
    // Se non lo trovo q è arrivato a NULL e lo restituisco
    return q;
}

void stampaRListCompleta()
{
    printf("Modalità di debug: Stato attuale della serverRegisterList\n");
    struct regEntry* p = serverRegisterList;
    printf("|dest_username|port|timestamp_login|timestamp_logout|\n");
    while(p != NULL)
    {
        // Converto elemento in formato TIMESTAMP
        char *bufferTimestampLogin; char* bufferTimeStampLogout;
        convertFromEpoch(p->timestamp_login, &bufferTimestampLogin);
        convertFromEpoch(p->timestamp_logout, &bufferTimeStampLogout);
        printf("|%s|%d|%s|%s|\n",p->user_dest, p->port, bufferTimestampLogin, bufferTimeStampLogout);
        p = p->pun;
    }
}

void stampaDebugNotifyList()
{
    printf("Modo debug. Stampa della serverNotifiersList\n");
    struct notificationEntry* n = serverNotifiersList;
    while(n != NULL)
    {
        char* time;
        convertFromEpoch(n->lastTimestamp, &time);
        printf("%s %d %s\n", n->user, n->count, time);
        free(time);
        n = n->pun;
    }
    return;
}

void list()
{
    int entrato = 0;
    struct regEntry* p = serverRegisterList;
    printf("Utenti connessi alla rete: \n");
    while(p != NULL)
    {
        // Il processo di stampa lo eseguo solo per gli utenti attualmente online
        if(p->timestamp_logout == -1)
        {
            entrato = 1;
            // Converto in maniera appropriata il timestamp
            char *bufferTimestampLogin;
            convertFromEpoch(p->timestamp_login, &bufferTimestampLogin);
            printf(".) %s*%s*%d\n", p->user_dest, bufferTimestampLogin, p->port);
        }
        p = p->pun; 
    }
    
    // Se entrato continua ad essere 0 anche dopo il ciclo, nessun utente è online
    if(entrato == 0)
        printf("-- nessun utente risulta connesso --\n");
}

// Ritorna 0 se trova lo username usr / se la password è corretta
// Se p = 0 la funzione esegue il controllo sull'esistenza username
// In questo caso si passa NULL come psw
// Se p = 1 la funzione esegue il controllo della correttezza della password
// per l'username indicato usr
int usernameExists(char* usr, char* psw, int p)
{
    struct account *acc; // array che varia dinamicamente
    char* buf; int len; int trovato = 1;
    FILE* fd;
    int i=0, j=0;

    buf = malloc(1024);
    // Se la cartella non esiste la creo
    mkdir("serverfiles", 0700);
    // Apro il file in modo lettura e append 
    // in modo tale da creare il file se non esiste
    fd = fopen("serverfiles/accounts.txt", "a+");
    

    // Giro una prima volta per vedere quante righe sono
    while(feof(fd) == 0)
    {
        fgets(buf, sizeof(struct account), fd);
        i++;
    }
    i = i/2;

    acc = (struct account*)malloc(sizeof(struct account)*i);

    // Devo riposizionare il file descriptor
    fseek(fd, 0, SEEK_SET);

    while(j < i)
    {   
        // Ricavo username dal file con rimozione del newline
        fgets(acc[j].username, UNIXTERM_WIDTH, fd);
        len = strlen(acc[j].username);
        acc[j].username[len-1] = '\0';

        // Ricavo password dal file con rimozione del newline
        fgets(acc[j].password, UNIXTERM_WIDTH, fd);
        len = strlen(acc[j].password);
        acc[j].password[len-1] = '\0';

        j++;
    }

    // Scorro l'array per individuare la presenza dell'username usr
    int indice = -1;
    if(p == 0)
    {
        for(j = 0; j < i; j++)
        {
            if(strcmp(acc[j].username, usr) == 0)
            {
                trovato = 0;
                break;
            }
        }
    } else {
        // in questo ramo il chiamante avrà già verificato l'esistenza dell'username
        // ricavo l'indice dell'username
        for(j = 0; j < i; j++)
        {
            if(strcmp(acc[j].username, usr) == 0)
            {
                indice = j;
                break;
            }
        }
        if(strcmp(acc[indice].password, psw) == 0)
        {
            trovato = 0;
        }
           
    }
    fclose(fd);
    free(buf);
    free(acc);
    return trovato;
}

int writeIntoFileAccounts(char* str1, char* str2)
{
    FILE* fd; int ret;
    // Se la cartella non esiste la creo
    mkdir("serverfiles", 0700);
    fd = fopen("serverfiles/accounts.txt", "a");
    if(fd < 0)
    {
        return -1;
    }
    ret = fprintf(fd, "%s\n%s\n", str1, str2);
    if(ret < 0)
    {
        fclose(fd);
        return -1;
        
    }
    fclose(fd);
    return 0;
}

void salvaRegistroSuFile()
{
    // Se la cartella non esiste la creo
    mkdir("serverfiles", 0700);
    char* percorso;
    percorso = malloc(strlen("serverfiles/serverRegisterList.txt")+1);
    strcpy(percorso, "serverfiles/serverRegisterList.txt");
    // aggiungo b alle opzioni di fopen perché scrivo su file binario
    FILE* fd = fopen(percorso, "w");
    struct regEntry* r = serverRegisterList;
    while(r != NULL)
    {   // ld sta per long decimal
        fprintf(fd, "%s %d %ld %ld\n", r->user_dest, r->port,r->timestamp_login, r->timestamp_logout);
        r = r->pun;
    }
    fclose(fd);
    free(percorso);
}

void leggiRegistroDaFile()
{
    mkdir("serverfiles", 0700);
    char* percorso; // char* testoLetto;
    percorso = malloc(strlen("serverfiles/serverRegisterList.txt")+1);
    strcpy(percorso, "serverfiles/serverRegisterList.txt");
    FILE* fd = fopen(percorso, "r");
    if(fd == NULL)
    {
        printf("Registro vuoto!\n");
        return;
    }
    struct regEntry temp;
    while(feof(fd) == 0)
    {
        int p;
        fscanf(fd, "%s %d %ld %ld\n", 
        temp.user_dest, &p, &temp.timestamp_login, &temp.timestamp_logout);
        inserisciElementoRList(temp.user_dest, p, temp.timestamp_login, temp.timestamp_logout);
    }
    fclose(fd);
    free(percorso);
    return;
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

void inviaOpaco16Bit(int sock, uint16_t num)
{
    int ret;
    num = htons(num);
    ret = send(sock, (void*)&num, sizeof(uint16_t), 0);
    if(ret < 0)
    {
        perror("Errore in fase di invio");
        exit(1);
    }
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

void spedisciComando(int sock, char* str)
{
    int ret;
    char cmd[8]; // Tutti i comandi da inviare al server stanno su 8 byte
    strcpy(cmd, str);
    ret = send(sock, (void*)cmd, sizeof(cmd), 0);
    if(ret < 0)
    {
        perror("Errore in fase di invio comando");
        exit(1);
    }
    return;
}
// Restituisce il numero di byte letti oppure -1 in caso di errore
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

void spedisciStringa(int sd, uint8_t* usr)
{
    uint16_t len, ret;
    len = strlen((char*)usr) + 1;
    inviaOpaco16Bit(sd, len);

    ret = send(sd, (void*)usr, len, 0);
    if(ret < 0)
    {
        perror("Errore in fase di send:");
        exit(1);
    }
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

void setupConnessioneTCPModoServer(int* sock, struct sockaddr_in* ind_serv, int port)
{
    int ret;
    struct sockaddr_in stateCopy;
    stateCopy = *ind_serv;
    *sock = socket(AF_INET, SOCK_STREAM, 0);
    if(*sock < 0)
    {
        perror("Errore nella creazione del socket");
        exit(1);
    }
    // Nel caso di utilizzo di socket TCP, il sistema operativo
    // in seguito ad una disconnessione del server anche avendo chiuso correttamente 
    // i socket non rilascia immediatamente la porta locale
    // Questo pezzetto di codice permette di riutilizzare porte che si trovano nello stato TIME_WAIT
    const int enable = 1;
    if (setsockopt(*sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0)
    {
        perror("setsockopt(SO_REUSEADDR) failed");
        exit(1);
    }
    memset(&stateCopy, 0,  sizeof(stateCopy));
    stateCopy.sin_family = AF_INET;
    stateCopy.sin_port = htons(port);
    inet_pton(AF_INET, INDIRIZZO_IP, &stateCopy.sin_addr);
    // Assegno indirizzo IP e porta al socket
    ret = bind(*sock, (struct sockaddr*)&stateCopy, sizeof(stateCopy));
    if(ret < 0)
    {
        perror("Errore in fase di bind");
        exit(1);
    }
    ret = listen(*sock, 10);
    if(ret < 0)
    {
        perror("Errore in fase di listen");
        exit(1);
    }
    *ind_serv = stateCopy;
}

void reimpostaConsegnaMessaggiServer(int time, char* user)
{
    // Scorro la lista dei messaggi destinati a user
    struct message* p;
    for(p = serverMessageList; p != NULL; p = p->pun)
    {
        if(strcmp((char*)p->userDest, user) == 0)
        {
           if(p->epochTimeRicezione > time)
                p->epochTimeRicezione = -1;
        }
    }
}

void serverLoginInArrivoDaDevice()
{
    // Il passo 2 del protocollo di login lato server richiede una risposta UASK
    char* username;
    char* password;
    int ret;
    spedisciComando(clientSock, "UASK");

    // Ricevo l'username
    riceviStringa(clientSock, &username);
    // Controllo esistenza dell'username
    if(usernameExists(username, NULL, 0) != 0)
    {
        // Invio UASK
        spedisciComando(clientSock, "UASK");
        return;
    }

    // Invio l'ACK che consiste anche nella richiesta della password
    spedisciComando(clientSock, "PSSW");
    // Mi aspetto di ricevere la password
    riceviStringa(clientSock, &password);
    // Controllo se la password è giusta
    if(usernameExists(username, password, 1) != 0)
    {
        spedisciComando(clientSock, "ERR");
        close(clientSock);
        return;
    }
    // Se siamo qui la password è corretta
    spedisciComando(clientSock, "PSOK");
    // Ricevo la clientPort
    uint16_t clPort;
    clPort = riceviOpaco16Bit(clientSock);

    // Occorre controllare se il client è già online
    if(isUsernameOnline(username) == 0)
    {
        // Si chiede al device di controllare il file
        spedisciComando(clientSock, "SEEF");
        char* cmd;
        // Lui risponde con l'esito del controllo sul file
        riceviComando(clientSock, &cmd);
        if(strcmp(cmd, "FOK") == 0)
        {
            // Questo è il caso in cui il device si è disconnesso improvvisamente
            // Ricevo il timestamp di logout
            int tlog = riceviOpaco32Bit(clientSock);
            // Devo reimpostare come messaggi da consegnare solo quelli contrassegnati
            // erroneamente come tali dal server nel mentre che il device era offline
            // senza fare out
            reimpostaConsegnaMessaggiServer(tlog, username);
            // Devo aggiornare la vecchia entry, si ferma al primo online che trova
            struct regEntry* r = searchDeviceByUsername(username);
            r->timestamp_logout = tlog;
        } else {
            // Arriva un FNOK
            spedisciComando(clientSock, "ERR");
            close(clientSock);
            return;
        }
    } else {
        // Non c'è bisogno del file
        spedisciComando(clientSock, "NOF");
    }
    ret = portAvailable(clPort);
    if(ret == -1)
    {
        spedisciComando(clientSock, "ERR");
        close(clientSock);
        return;
    }     
    else
    {
        // Scrivo una nuova entry nel registro
        inserisciElementoRList(username, clPort, time(NULL), -1);
        spedisciComando(clientSock, "GNT");
    }
    // Lato device si verificherà se è già stato registrato un login
    free(username);
    free(password);
    close(clientSock);
    return;
}

void serverRegistraNuovoDevice()
{
    int ret; 
    char* rcvUser;
    char* rcvPsw;
    
    // Ricevo l'username e controllo nel file se è uno di quelli già registrati
    riceviStringa(clientSock, &rcvUser);
    if(usernameExists(rcvUser, NULL, 0) == 0)
    {
        // Invio un NAK
        spedisciComando(clientSock, "NAK");
        close(clientSock);
        free(rcvUser);
        return;
    } else {
        // Invio un ACK
        spedisciComando(clientSock, "ACK");
        // Se sono qua l'username esiste
        // Ricevo la password
        riceviStringa(clientSock, &rcvPsw);
        // Memorizzo nel file la registrazione
        ret = writeIntoFileAccounts(rcvUser, rcvPsw);
        if(ret < 0)
            // Invio un NAK
            spedisciComando(clientSock, "NAK");
        else
            spedisciComando(clientSock, "ACK");
    }
    close(clientSock);
    free(rcvUser);
    free(rcvPsw);
    return;
}

void rilasciaDevice()
{
    char* rcvUser;

    // Ricevo lo username
    riceviStringa(clientSock, &rcvUser);

    // Ricavo il puntatore alla entry del registro
    // e modifico il campo
    
    struct regEntry* p = searchDeviceByUsername(rcvUser);
    // Modifico il campo timestamp_logout e ci metto l'ora corrente
    // Prima però devo chiamare la funzione isUsernameOnline per sapere
    // se l'utente che sto disconnettendo non era già stato disconnesso
    // precedentamente. Questo posso farlo semplicemente con la chiamata
    // sotto perché si ipotizza che un username non possa essere loggato
    // contemporaneamente su più di una porta
    if(isUsernameOnline(rcvUser) == 0)
        (p->timestamp_logout) = time(NULL);

    // ho dei dubbi in merito, chiude tutte le altre connessioni se lo faccio
    // Chiudo il socket di comunicazione
    close(clientSock);
    free(rcvUser);
    return;
}

void esc()
{
   
    // Chiudo tutti i descrittori relativi ai socket di ascolto

    // Bisogna dire a tutti i client connessi con ME 
    // di chiudere
    // però tra di loro devono continuare a parlare

    sleep(1);
    // salvo registro su file
    salvaRegistroSuFile();
    close(clientSock);
    printf("Server terminato.\n");

}

void stampaServerMessageList()
{
    struct message* p = serverMessageList;
    printf("Elenco dei messaggi ricevuti dal server\n");
    while(p != NULL)
    {
        printf("---------------------------------------\n");
         // Converto elemento in formato TIMESTAMP
        char *timestampLogin; char* timestampLogout;
        convertFromEpoch(p->epochTimeInvio, &timestampLogin);
        if(p->epochTimeRicezione == -1)
            timestampLogout = "N/A";
        else
            convertFromEpoch(p->epochTimeRicezione, &timestampLogout);
        printf("Da: %s a: %s\n", p->userSend, p->userDest);
        printf("Sessione del: %d\n", p->orarioSessione);
        printf("Istante invio: %s\n", timestampLogin);
        printf("Istante ricezione: %s\n", timestampLogout);
        printf("Testo del messaggio: %s\n", p->payload);
        printf("---------------------------------------\n");
        p = p->pun;
    }
    printf("Elenco completato\n");
}
// buf è il payload del messaggio. dim è la dimensione del payload del messaggio
// da calcolare prima di chiamare questa funzione
void serverInviaMessaggio(int clientSock, char* sndusr, char* dstusr, char* buf, uint32_t dim, time_t timeInvio)
{
    struct message msg;
    int ret;
    // Il payload del messaggio contiene il carattere di terminazione stringa
    // Invio il messaggio previo confezionamento
    msg.dimensionePayload = dim;
    msg.payload = (uint8_t*)malloc(sizeof(uint8_t)*msg.dimensionePayload);
    strcpy((char*)msg.payload, buf);
    strcpy((char*)msg.userSend, sndusr);
    strcpy((char*)msg.userDest, dstusr);
    msg.epochTimeInvio = timeInvio;
    msg.epochTimeRicezione = -1;
    msg.pun = NULL;

    // Scelto il protocollo BINARY
    // Per i campi di tipo array di char non c'è bisogno di utilizzare funzioni
    // di conversione, perché ciascun carattere sta su 8 bit
    // La conversione in network order è necessaria solo per i tipi numerici

    // Invio username mittente
    spedisciStringa(clientSock, msg.userSend);
    // Invio username destinatario
    spedisciStringa(clientSock, msg.userDest);
    // Invio orarioSessione
    inviaOpaco32Bit(clientSock, msg.orarioSessione);
    // Invio dstPort
    inviaOpaco16Bit(clientSock, msg.destPort);
    // Invio epochTimeInvio
    inviaOpaco32Bit(clientSock, msg.epochTimeInvio);
    // Invio epochTimeRicezione
    inviaOpaco32Bit(clientSock, msg.epochTimeRicezione);

    // Invio del payload
    // Invio della dimensione del payload
    inviaOpaco32Bit(clientSock, msg.dimensionePayload);
    // Invio del payload
    ret = send(clientSock, (void*)msg.payload, dim, 0);
    if(ret < 0)
    {
        perror("Errore in fase di invio");
        exit(1);
    }
}

void serverInviaMessaggioRidoto(int clientSock, char* sndusr, char* dstusr, int timeInvio)
{
    // Invio username mittente
    spedisciStringa(clientSock, (uint8_t*)sndusr);
    // Invio username destinatario
    spedisciStringa(clientSock, (uint8_t*)dstusr);
    // Invio epochTimeInvio
    inviaOpaco32Bit(clientSock, timeInvio);
}

void serverImmagazzinaMessaggio(struct message msg)
{
    struct message* p; struct message* q;
    for(q = serverMessageList; q != NULL; q = q->pun)
        p = q;
    
    q = malloc(sizeof(struct message));
    if(q == NULL)
        printf("errore malloc");
    
    strcpy((char*)q->userSend, (char*)msg.userSend);
    strcpy((char*)q->userDest, (char*)msg.userDest);
    // Questo assegnamento permetterà al server
    // di distinguere poi i primi messaggi all'interno di una sessione
    struct regEntry* z = searchDeviceByUsername((char*)q->userSend);
    q->orarioSessione = z->timestamp_login;
    // -----
    q->epochTimeInvio = msg.epochTimeInvio;
    // Se la sessione è una di quelle fallite
    // il messaggio va contrassegnato come NON consegnato!
    if(isAFailedSession(q->orarioSessione) == 1)
        q->epochTimeRicezione = -1;
    else
        q->epochTimeRicezione = msg.epochTimeRicezione;
    q->dimensionePayload = msg.dimensionePayload;
    
    q->payload = (uint8_t*)malloc(sizeof(uint8_t)*msg.dimensionePayload);
    strcpy((char*)q->payload, (char*)msg.payload);
    q->pun = NULL;
    if(serverMessageList == NULL)
        serverMessageList = q;
    else
        p->pun = q;
}

// Funzione che esamina il messaggio per scoprire se è la prima
// volta che src tenta di inviare a dst
// Ritorna 0 se prima volta, -1 altrimenti
int serverEsaminaMessaggio(struct message msg)
{
    // Si esamina la lista serverMessageList
    struct message* p = serverMessageList;
    while(p != NULL)
    {
        if( ( strcmp((char*)p->userSend, (char*)msg.userSend) == 0 ) && ( strcmp((char*)p->userDest, (char*)msg.userDest) == 0 ) )
        {
            struct regEntry* r = searchDeviceByUsername((char*)p->userSend);
            if(r->timestamp_login == p->orarioSessione)
                return -1; // ne ho trovato almeno uno ma della sessione corrente!
        }
        p = p->pun;
    }
    return 0;
}
// Conta il numero di elementi di serverRegisterList che non hanno effettuato logout
int contaUtentiOnline()
{
    int conta = 0;
    // Esamina la lista serverRegisterList
    struct regEntry* q;
    for(q = serverRegisterList; q != NULL; q = q->pun)
        if(q->timestamp_logout == -1)
            conta++;
    return conta;
}

void inserisciListaMessagiDaNotificare(struct message m)
{
    struct message* p; struct message* q;
    for(q = serverListaMsgDaNotificare; q != NULL; q = q->pun)
        p = q;
    q = malloc(sizeof(struct message));
    
    strcpy((char*)q->userSend, (char*)m.userSend);
    strcpy((char*)q->userDest, (char*)m.userDest);
    q->epochTimeInvio = m.epochTimeInvio;
    q->epochTimeRicezione = m.epochTimeRicezione;
    q->destPort = m.destPort;
    q->orarioSessione = m.orarioSessione;
    q->dimensionePayload = m.dimensionePayload;
    q->payload = (uint8_t*)malloc(sizeof(uint8_t)*m.dimensionePayload);
    strcpy((char*)q->payload, (char*)m.payload);

    q->pun = NULL;
    if(serverListaMsgDaNotificare == NULL)
        serverListaMsgDaNotificare = q;
    else 
        p->pun = q;
    return;
}

// Ritorna 0 se l'handshake è riuscito, -1 altrimenti
int serverHandshakeDestinatario(struct message msg, uint16_t* pt)
{
    int tentativi = 0; // numero di tentativi completati
    int ret; char* cmd;
    // Fare l'handshake significa
    // 1- vedere se l'username dst da registro risulta online
    if(isUsernameOnline((char*)msg.userDest) == -1)
        return -1;
    // 2- ricavo il puntatore alla lista degli utenti connessi
    // e la porta del destinatario
    struct regEntry* p = searchDeviceByUsername((char*)msg.userDest);
    uint16_t port = p->port;
    *pt = p->port;
    // 3- invio il messaggio al device alla porta ricavata del registro
    // lo devo inviare tramite il socket clDestSocket

    // apro il socket TCP NON BLOCCANTE
    clDestSocket = socket(AF_INET, SOCK_STREAM, 0);
    if(clDestSocket < 0)
    {
        perror("Errore apertura socket clDestSocket:");
        exit(1);
    }

    // Preparo l'indirizzo del destinatario
    memset(&endPoint_addr, 0, sizeof(endPoint_addr));
    endPoint_addr.sin_family = AF_INET;
    endPoint_addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &endPoint_addr.sin_addr);

    while(tentativi < 3)
    {
        ret = connect(clDestSocket, (struct sockaddr*)&endPoint_addr, sizeof(endPoint_addr));
        if(ret == 0)
            break;
        else
        {
            tentativi++;
            printf("Tentativo di connessione n %d fallito\n", tentativi);
            // Aspetto 2 secondi prima di rieseguire un nuovo tentativo
            sleep(2);
        }
            // qui è scaduto il timeout di 2 secondi
    }
    if(tentativi == 3) // se ho esaurito i 3 tentativi
    {
        close(clDestSocket);
        return -1;
    }
    // Invio il messaggio a device dst
    spedisciComando(clDestSocket, "CHT");
    serverInviaMessaggio(clDestSocket, (char*)msg.userSend, (char*)msg.userDest, (char*)msg.payload, msg.dimensionePayload, time(NULL));
    riceviComando(clDestSocket, &cmd);// Ricevo ACK dal device dst
    close(clDestSocket);
    free(cmd);
    return 0;
}

void serverRiceviMessaggio(struct message* msg)
{
    int ret;
    uint16_t len;

    len = riceviOpaco16Bit(clientSock);
    ret = recv(clientSock, (void*)msg->userSend, len, 0);
    if(ret < 0)
    {
        perror("(Server). Errore in fase di ricezione");
        exit(1);
    }

    len = riceviOpaco16Bit(clientSock);
    ret = recv(clientSock, (void*)msg->userDest, len, 0);
    if(ret < 0)
    {
        perror("(Server). Errore in fase di ricezione");
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
        perror("Errore in fase di ricezione 4:");
        exit(1);
    }
}

void serverGestisciChat()
{
    struct message msg; int ret; int hsStatus;
    uint16_t destPort;
    serverRiceviMessaggio(&msg);
    ret = serverEsaminaMessaggio(msg);
    // Se non mai ricevuto un messaggio da src per dst
    // allora devo eseguire handshake con dst
    // significa che src ha inviato per la prima
    // volta il messaggio a dst
    if(ret == 0)
    {
        printf("Eseguo un handshake..\n");
        hsStatus = serverHandshakeDestinatario(msg, &destPort);
        if(hsStatus == 0)
        {
            // se l'handshake ha avuto successo, il messaggio viene contrassegnato
            // come ricevuto sul server
            msg.epochTimeRicezione = time(NULL);
            // manda due pacchetti ON e numero di porta
            spedisciComando(clientSock, "ON");
            inviaOpaco16Bit(clientSock, destPort);
        }    
        else
        {
            spedisciComando(clientSock, "OFF");
            // aggiorno la entry relativa nel registro ponendo il timestamp_logout a quello corrente
            struct regEntry* p = searchDeviceByUsername((char*)msg.userDest);
            // lo faccio solo se presente nel registro
            if(p!=NULL) p->timestamp_logout = time(NULL);
            // catalogare quella sessione come fallita
            p = searchDeviceByUsername((char*)msg.userSend);
            inserisciListaSession(p->timestamp_login);
        }       
    } else {
        // uno sguardo al registro il server glielo dà
        // prima di contrassegnare l'epochTimeRicezione, almeno per gestire le disconnessioni
        // attese. in caso di disconnessione inattesa in questo scenario il timestamp
        // di ricezione viene comunque registrato.
        // il risultato è che i messaggi pendenti del destinatario mentre era disconnesso
        // se è crashato non possono essere recuperati, si rimedia guardando la cronologia dei messaggi
        struct regEntry* re = searchDeviceByUsername((char*)msg.userDest);
        if(re != NULL) // anche se l'utente esiste non è detto che la porta sia la stessa. dst potrebbe              
        {              // essere uscito ed avere cambiato porta
            // se la porta coincide
            msg.epochTimeRicezione = time(NULL);
            inviaOpaco16Bit(clientSock, re->port);      
        } else
            inviaOpaco16Bit(clientSock, 2); // porta che indica il caso in cui l'utente è disconnesso
    }
    serverImmagazzinaMessaggio(msg);
    close(clientSock);
    
}

void serverInviaUtentiOnline()
{
    // Invio il numero di utenti
    int nuser = contaUtentiOnline();
    inviaOpaco32Bit(clientSock, nuser);
    // Scorro la lista
    struct regEntry* p;
    for(p = serverRegisterList; p != NULL; p = p->pun)
        if(p->timestamp_logout == -1)
            spedisciStringa(clientSock, (uint8_t*)p->user_dest);
    close(clientSock);
}
// Questa funzione viene chiamata ogni volta che si trova un messaggio non
// consegnato inviato da parte di user nel buffer del server per l'utente
// che ha richiesto l'hanging
void inserisciNuovaNotifica(char* user, uint32_t lastTime)
{
    // Se non esite l'username passato come primo parametro crea un nuovo
    // elemento nella lista serverNotifiersList

    // Cerco l'elemento nella lista
    struct notificationEntry* ne;
    for(ne = serverNotifiersList; ne != NULL; ne = ne->pun)
    {
        if(strcmp(ne->user, user) == 0)
        {
            // Modifico l'elemento e mi fermo. Subito!
            ne->count++;
            ne->lastTimestamp = lastTime;
            return;
        }
    }

    // Se l'elemento non esiste, devo aggiungerlo alla lista
    // e il conto dei messaggi iniziale è 1
    // 1 perché la chiamata viene fatta solo quando c'è almeno un messaggio
    // non consegnato

    struct notificationEntry* p; struct notificationEntry* q;
    for(q = serverNotifiersList; q != NULL; q = q->pun)
        p = q;
    q = malloc(sizeof(struct notificationEntry));
    strcpy(q->user, user);
    q->count = 1;
    q->lastTimestamp = lastTime;
    q->pun = NULL;
    if(serverNotifiersList == NULL)
        serverNotifiersList = q;
    else
        p->pun = q;
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

int countListaNotifiers()
{
    int count = 0;
    struct notificationEntry* n;
    for(n = serverNotifiersList; n != NULL; n = n->pun)
        count++;
    return count;
}
void serverGestisciNotifiche()
{
    char* rcvUsr; struct message* l;
    int count = 0; // totale di iterazioni nel ciclo di invio che devono essere eseguite
    // Comando HAN già ricevuto
    // Ricevo l'username
    riceviStringa(clientSock, &rcvUsr);

    // Distruggo la lista
    distruggiListaNotifiers(&serverNotifiersList);

    // Guardo la lista dei messaggi
    for(l = serverMessageList; l != NULL; l = l->pun)
    {
        if(strcmp((char*)l->userDest, rcvUsr) == 0)
        {
             if(l->epochTimeRicezione == -1)
             {
                inserisciNuovaNotifica((char*)l->userSend, l->epochTimeInvio);
             }
        }
    }
    // Contare il numero degli elementi della serverNotifiersList
    count = countListaNotifiers();
    // Invio il numero di iterazioni
    inviaOpaco32Bit(clientSock, count);
    struct notificationEntry* p;
    for(p = serverNotifiersList; p != NULL; p = p->pun)
    {
        // Invio username
        spedisciStringa(clientSock, (uint8_t*)p->user);
        // Invio conto dei messaggi
        inviaOpaco32Bit(clientSock, p->count);
        // Invio lastTimestamp
        inviaOpaco32Bit(clientSock, p->lastTimestamp);
    }
    close(clientSock);
    free(rcvUsr);
    return;
}

void serverInviaNotificaLettura(struct message msg)
{
    int port; int ret;
    int orario = -1;
    
    // Chi è l'utente a cui devo inviare la notifica di lettura?
    // E' l'utente sorgente, che vedrà come notifica una coppia
    // di pacchetti del tipo
    // comando NFY + copia del messaggio che aveva già mandato

    // Controllo se il destinatario della notifica, quindi
    // il sorgente originale del messaggio è online
    if(isUsernameOnline((char*)msg.userSend) == -1)
    {
        msg.epochTimeRicezione = time(NULL);
        inserisciListaMessagiDaNotificare(msg);
        // La notifica verrà inviata non appena 
        // l'utente sorgente avvia una chat con il destinatario
        return;
    }
    // Ricavo la porta
    struct regEntry* p = searchDeviceByUsername((char*)msg.userSend);
    port = p->port;

    // Faccio il setup di una connessione TCP modo client
    clDestSocket = socket(AF_INET, SOCK_STREAM, 0);
    if(clDestSocket < 0)
    {
        perror("Errore nella creazione di clDestSocket");
        exit(1);
    }

    // Preparo l'indirizzo del destinatario
    memset(&endPoint_addr, 0, sizeof(endPoint_addr));
    endPoint_addr.sin_family = AF_INET;
    endPoint_addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &endPoint_addr.sin_addr);

    // Provo a connettermi
    ret = connect(clDestSocket, (struct sockaddr*)&endPoint_addr, sizeof(endPoint_addr));
    if(ret == -1)
    {
        inserisciListaMessagiDaNotificare(msg);
        return;
    }

    spedisciComando(clDestSocket, "NFY");
    orario = time(NULL);
    inviaOpaco32Bit(clDestSocket, orario);
    serverInviaMessaggio(clDestSocket, (char*)msg.userSend, (char*)msg.userDest, (char*)msg.payload, msg.dimensionePayload, msg.epochTimeInvio);
    close(clDestSocket);
    return;
}

void serverConsegnaBufferizzati()
{
    char* rcvUsername; int numIterazioni = 0;
    char* rcvU; char* rcvCom;
    // Già ricevuto comando SHW
    // Ricevo il sorgente dei messaggi (username)
    riceviStringa(clientSock, &rcvUsername);
    // Ricevo il destinatario dei messaggi (u)
    riceviStringa(clientSock, &rcvU);

    // Calcolo il numero dei messaggi da inviare

    // Guardo la lista dei messaggi bufferizzati
    struct message* m;
    for(m = serverMessageList; m != NULL; m = m->pun)
    {
        if(strcmp((char*)m->userSend, rcvUsername) == 0 && strcmp((char*)m->userDest, rcvU) == 0)
        {
            if(m->epochTimeRicezione == -1)
                numIterazioni++;
        }
    }

    // Invio il numero di messaggi
    inviaOpaco32Bit(clientSock, numIterazioni);

    // Guardo di nuovo la lista dei messaggi bufferizzati
    for(m = serverMessageList; m != NULL; m = m->pun)
    {
        if(strcmp((char*)m->userSend, rcvUsername) == 0 && strcmp((char*)m->userDest, rcvU) == 0)
        {
            if(m->epochTimeRicezione == -1)
            {
                serverInviaMessaggio(clientSock, (char*)m->userSend, (char*)m->userDest, (char*)m->payload, m->dimensionePayload, m->epochTimeInvio);
                // Devo registrare la consegna del messaggio
                m->epochTimeRicezione = time(NULL);
                // Siccome utilizzo socket TCP bloccante, l'ACK ricevuto dal server è relativo all'ultimo 
                // messaggio inviato
                riceviComando(clientSock, &rcvCom);
                serverInviaNotificaLettura(*m);
            }
        }
    }
    close(clientSock);
    return;
}

// Ritorna il numero di elementi della lista serverListaMsgDaNotificare
// con sorgente s e destinazione d
int contaElementiListaMsgDaNotificare(char* s, char* d)
{
    int count = 0;
    struct message* p;
    for(p = serverListaMsgDaNotificare; p != NULL; p = p->pun)
    {
        if((strcmp((char*)p->userSend, s) == 0) && (strcmp((char*)p->userDest, d) == 0))
            count++;
    }
    return count;
}

// Ritorna -1 se l'elemento non esiste, 0 altrimenti
int estraiPerValoreListaMsgDaNotificare(char* s, char* d, struct message* m)
{
    struct message* p = NULL; struct message* q = serverListaMsgDaNotificare;
    while(q != NULL)
    {
        if((strcmp((char*)q->userSend, s) == 0 && strcmp((char*)q->userDest, d) == 0))
            break;
        p = q;
        q = q->pun;
    }
    if(q == NULL)
        return -1;
    if(q == serverListaMsgDaNotificare)
        serverListaMsgDaNotificare = q->pun;
    else
        p->pun = q->pun;
    
    // Copio i campi del messaggio che devo restituire
    
    strcpy((char*)m->userSend, (char*)q->userSend);
    strcpy((char*)m->userDest, (char*)q->userDest);
    m->epochTimeInvio = q->epochTimeInvio;
    m->epochTimeRicezione = q->epochTimeRicezione;
   
    free(q);

    return 0;
}

void serverNotificaMittenteInChat()
{
    // A questo punto ho già ricevuto il comando
    // Mi deve arrivare l'username mittente e destinatario

    char* sender;
    riceviStringa(clientSock, &sender);
    char* dest;
    riceviStringa(clientSock, &dest);
    
    // Calcolo il numero di messaggi
    int c = contaElementiListaMsgDaNotificare(sender, dest);

    // Invio il numero di messaggi
    inviaOpaco32Bit(clientSock, c);

    // Attenzione: timestampRicezione nella lista dei messaggi
    // da notificare è l'orario di ricezione da parte
    // del destinatario! (che ha già scaricato il messaggio
    // mentre il sender era offline)

    int count = 0; 
    while(count < c)
    {
        // Guardo se esiste un messaggio da dest a sender
        // Se non esiste nemmeno uno, mi fermo
        // Se c'è lo estraggo dalla lista
        struct message msg;
        estraiPerValoreListaMsgDaNotificare(sender, dest, &msg);

        // Invio il riscontro che consiste in:
        // Orario di ricezione e copia del messaggio che avevo inviato
        // Invio l'orario di ricezione del destinatario originale
        inviaOpaco32Bit(clientSock, msg.epochTimeRicezione);
        // Invio il messaggio in formato ridotto
        serverInviaMessaggioRidoto(clientSock, (char*)msg.userSend, (char*)msg.userDest, msg.epochTimeInvio);
        count++;
    }
    close(clientSock);
    return;
}

// Trova il massimo tra tre interi
int doMax(int a, int b, int c)
{
    int max = a;
    if(b > max)
        max = b;
    if(c > max)
        max = c;
    return max;
}

void makeServerOn()
{
    fd_set master;
    fd_set read_fds;
    int fdmax;
    int select_ret;
    int i, addrlen;
    int nbytes; 
    char cmd[8]; // Comando ricevuto dal device: si è fissata dimensione predefinita di 1 byte!
    char buf[1024];
    // -------------------------

    FD_ZERO(&master);
    FD_ZERO(&read_fds);
    // metto i socket di ascolto nel set principale
    FD_SET(sockServer, &master);
    FD_SET(regSocket, &master);
    FD_SET(0, &master); // 0 è il descrittore di stdin

    // tengo traccia del maggiore tra i tre socket
    fdmax = doMax(sockServer, regSocket, 0);

    while(1)
    {
        read_fds = master;
        select_ret = select(fdmax + 1, &read_fds, NULL, NULL, NULL);
        if(select_ret < 0)
        {
            perror("Errore nella select: ");
            exit(1);
        }
        for(i = 0; i <= fdmax; i++) // scorro il set dei descrittori
        // controllo i descrittori pronti in lettura
        {
            if(FD_ISSET(i, &read_fds))
            {
                if(i == sockServer)
                {
                    addrlen = sizeof(cl_addr);
                    newsd = accept(sockServer, (struct sockaddr*)&cl_addr, (socklen_t*) &addrlen);
                    if(newsd < 0)
                    {
                        printf("Errore nella accept: ");
                        exit(1);
                    }
                    // Aggiungo il socket al master di lettura
                    FD_SET(newsd, &master);
                    if(newsd > fdmax) fdmax = newsd;
                } 
               
               if(i == regSocket)
                {
                    addrlen = sizeof(cl_addr);
                    newsd = accept(regSocket, (struct sockaddr*)&cl_addr, (socklen_t*) &addrlen);
                    if(newsd < 0)
                    {
                        printf("Errore nella accept: ");
                        exit(1);
                    }
                    // Aggiungo il socket al master di lettura
                    FD_SET(newsd, &master);
                    if(newsd > fdmax) fdmax = newsd;
                }
                if(i == newsd){
                    clientSock = i;
                    FD_CLR(i, &master);
                    nbytes = recv(clientSock, cmd, sizeof(cmd), 0);
                    if(nbytes < 0)
                    {
                        perror("(Server message) Errore nella recv");
                        exit(1);
                    }
                    if(strcmp(cmd, "SIG") == 0)
                    {
                        serverLoginInArrivoDaDevice();
                    }
                    if(strcmp(cmd, "REG") == 0)
                    {
                        serverRegistraNuovoDevice();
                    }

                    // Comandi in seguito al login
                    if(strcmp(cmd, "OUT") == 0)
                    {
                        rilasciaDevice();
                    }

                    if(strcmp(cmd, "TLK") == 0)
                    {
                        serverGestisciChat();
                    }

                    if(strcmp(cmd, "HAN") == 0)
                    {
                        serverGestisciNotifiche();
                    }

                    if(strcmp(cmd, "SHW") == 0)
                    {
                        serverConsegnaBufferizzati();
                    }
                    if(strcmp(cmd, "RNFY") == 0)
                    {
                        serverNotificaMittenteInChat();
                    }

                    if(strcmp(cmd, "GPON") == 0)
                    {
                        serverInviaUtentiOnline();
                    }
                }
                // Caso stdin
                if(i == 0)
                {
                    fgets(buf, 1024, stdin);
                    // Riconosco i possibili comandi lato server
                    if(strcmp(buf, "help\n") == 0)
                    {
                        printServerCmd();
                        printf("Comando?\n");
                    }
                        
                    else if(strcmp(buf, "list\n") == 0)
                    {
                        system("clear");
                        list();
                        printf("Comando?\n");
                       
                    }
                    else if(strcmp(buf, "debugmsglist\n") == 0)
                    {
                        system("clear");
                        stampaServerMessageList();
                        printf("Comando?\n");
                    }
                    else if(strcmp(buf, "debugreglist\n") == 0)
                    {
                        system("clear");
                        stampaRListCompleta();
                        printf("Comando?\n");
                    }
                    else if(strcmp(buf, "debugnotify\n") == 0)
                    {
                        system("clear");
                        stampaDebugNotifyList();
                        printf("Comando?\n");
                    }
                    else if(strcmp(buf, "esc\n") == 0)
                    {
                        // system("clear");
                        esc();
                        exit(0);
                    }
                    else
                    {
                        printf("Sintassi comando errata. Riprovare \n");
                        printf("Comando?\n");
                    }    
                }      
            } 
        }
   }
}
int main(int argc, char* argv[])
{
    int port; // porta del server

    if(argc < 2)
        port = 4242;
    else if(argc == 2)
    /* argv[1] è il parametro opzionale associato alla porta*/
    /* atoi effettua il parsing della stringa in argv[1] in un int */
        port = atoi(argv[1]);
    else
    {
        printf("ERRORE FATALE: Numero eccessivo di argomenti\n");
        exit(1);
    }
    
    // Faccio il setup di un socket TCP bloccante in ascolto sulla porta scelta
    setupConnessioneTCPModoServer(&sockServer, &sv_addr, port);
    // Creo, faccio la bind e metto in ascolto il socket TCP per le registrazioni
    // sulla porta 3125
    setupConnessioneTCPModoServer(&regSocket, &sv_addr_reg, 3125);
    printf("************* SERVER STARTED ***************** \n");

    // Recupero da file binario lo stato precedente del registro
    leggiRegistroDaFile();

    // Nel frattempo, il server dovrebbe anche gestire le interazioni da stdinput
    // per poter fare altre cose tipo scaricare i messaggi le notifiche etc
    // Intanto supponiamo uno scenario ristretto in cui si gestisce solo
    // login e registrazione
    printServerCmd();
    printf("Comando?\n");
    makeServerOn();
    exit(0);
    return 0;
}