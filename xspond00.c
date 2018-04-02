#define _POSIX_C_SOURCE 199506L
#define _REENTRANT

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>

#define true 1
#define false 0
#define LENGTH 513	/* max. delka vstupu. O 1 vetsi hodnota, nez kolik je pozadovano */

void * inputfunction();	/* funkce vlakna pro vstup shellu */
void * outputfunction();	/* funkce vlakna pro vystup shellu */
int parseInput(char *str, char **line[LENGTH]);	/* funkce parsujici vstup z radku do pole */
void stopchild(int sig);	/* funkce obsluhujici signal SIGCHLD */
void killchild(int sig);	/* funkce obsluhujici signal SIGINT */

int end = false;	/* priznak ukonceni programu */
char buffer[LENGTH];	/* buffer pro zpravy mezi vlakny */
pthread_mutex_t mutex;	/* mutex hlidajici provoz mezi vlakny */
pthread_cond_t cond;	/* podminka */
static int run = 1;	/* 0 - bezi inputfunction, 1 - bezi outputfunction */
pid_t child = 0;	/* PID ditete */

/*
 * Funkce pro 1. vlakno
 * stara se o plneni bufferu ze standardniho vstupu
 *
 * vraci NULL */
void *inputfunction() {
	size_t n=0, realn=0;	/* pomocne promenne pro velikosti nactenych dat funkci read() */
	char buf[LENGTH];		/* pomocny buffer vstupniho vlakna */
	char *tmp = NULL;
	int eof = false;		/* kontrola priznaku konce vstupu - radku */
	int sent = 0;			/* kvuli preteceni se touto promennou kontroluje, zda byla jiz data zaslana */

	while (!end) {	/* dokud uzivatel nenapise "exit" */
		/* uzamkneme mutex  - nyni ma rezii input vlakno */
		pthread_mutex_lock(&mutex);
		/* ceka na uvolneni podminky */
		while (run == 1) pthread_cond_wait(&cond, &mutex);
		/* pokud bylo zadano "exit", skoncime */
		if (end) break;
		/* nastaveni podminky, bezi input vlakno */
		run = 0;
		while (! eof) {	/* dokud neni konec radku */
			/* nejprve alokace pameti pro vstup z klavesnice */
			tmp = (char *)calloc(LENGTH+1, 1);	/* +1 kvuli znaku \0 */
			n = read(0, tmp, LENGTH);	/* ctu vstup */
			if (n == -1) printf("Chyba vstupu\n");
			/* v pripade, ze nacteme mene nez LENGTH znaku, posleme je - dal uz vstup nepokracuje */
			else if (n < (LENGTH) && eof == false) {
				strncpy(buf, tmp, n);
				realn = n;	/* skutecne poslane znaky */
				eof=true;	/* dale nepokracujeme */
			/* mame dlouhy vstup */
			} else if (sent < 1) {
				printf("Prilis dlouhy vstup (>%d znaku)\n", LENGTH-1);
				sent++;
			}
			free(tmp);
		}
		/* zkopirujeme nacteny retezec do hlavniho bufferu */
		strncpy(buffer, (const char*)buf, realn);
		
		/* predame rizeni output vlaknu */
		run = 1;
		pthread_cond_signal(&cond);
		pthread_mutex_unlock(&mutex);

		/* resetujeme ridici promenne */
		sent = 0;
		eof = false;
	}
	/* konec inputu, korektne ukoncime */
	pthread_exit(NULL);
	return NULL;
}

/*
 * Funkce pro 2. vlakno
 * zpracovava buffer, provadi prislusne akce a vypisuje na obrazovku
 */
void *outputfunction() {
	int length = 0, l=0;	/* delka bufferu a pocet parametru (pomocna promenna) */
	char *line[LENGTH];		/* pole jednotlivych retezcu ze vstupu */
	int status=0;			/* (zbytecna) promenna pro waitpid */

	while (!end) {	/* dokud uzivatel nenapise "exit" */
		/* uzamkneme mutex - nyni ma rezii output vlakno */
		pthread_mutex_lock(&mutex);
		/* ceka na uvolneni podminky */
		while (run == 0) pthread_cond_wait(&cond, &mutex);
		/* nastaveni podminky, bezi output vlakno */
		run = 1;

		length = strlen(buffer);	/* delka bufferu */
		l=0;

		/* pokud je v bufferu "exit", skoncime" */
		if (!strcmp("exit\n", buffer)) {
			end = true;	/* konec */
			/* predani rizeni inputu, at o tom take vi */
			run = 0;
			pthread_cond_signal(&cond);
			pthread_mutex_unlock(&mutex);
			break;	/* vyskocime ze smycky */
		}

		/* pokud je retezec vetsi nez 0, pracujeme
		 * nulovou delku muze mit napr. pri stisku ctrl+d
		 */
		if (length > 0) {
			l = parseInput(buffer, (char ***)&line);	/* zpracuje buffer */
			/* prvnim parametrem musi byt nazev programu
			 * pokud byl zadan jen newline (\n), tak i to je treba osetrit */
			if (line[0] != NULL && (l > -1)) {
				int fd=0;	/* standardni deskriptor je stdin */
				int background = false;	/* program spusten v pozadi? ne */
				pid_t pid;	/* PID vytvoreneho procesu */
				int io = 0;	/* 0 - normalni; 1 - stdin > soubor; 2 - soubor < stdout */

				/* pokud je vice retezcu v line[] nez 1 a ten posledni je &
				 * pak program spustime na pozadi
				 */
				if ((l > 0) && !strcmp(line[l], "&")) {	/* posledni znak je '&', proces neceka na ukonceni potomka */
					background = true;	/* spustit na pozadi */
					/* odstranime posledni znak '&' z argumentu a uvolnime pamet */
					free(line[l]);
					line[l]=NULL;
					l--;	/* zmensime pocet parametru o 1, znak '&' byl odebran */
				}
				/* pokud je vice retezcu v line[] nez 2 a predposledni retezec >,
				 * pak poslednim retezcem je nazev souboru, do ktereho bude presmerovan standardni vystup
				 */
				else if ((l > 1) && !strcmp(line[l-1], ">")) {	/* standardni vystup bude presmerovan do souboru */
					fd = open(line[l], O_CREAT|O_WRONLY|O_TRUNC, S_IRUSR|S_IWUSR);	/* filedeskriptor otevreme pro zapis */
					/* odstranime posledni 2 parametry ze seznamu argumentu */
					free(line[l]); free(line[l-1]);
					line[l]=NULL; line[l-1]=NULL;
					l -= 2;
					io = 1;	/* I/O je 1 - stdin > soubor */
				/* pokud je vice retezcu v line[] nez 2 a predposledni retezec je <,
				 * pak poslednim retezcem je nazev souboru, z ktereho bude presmerovan vystup na standardni vstup
				 */
				} else if ((l > 1) && !strcmp(line[l-1], "<")) {	/* standardni vstup bude presmerovan ze souboru */
					fd = open(line[l], O_RDONLY);	/* filedeskriptor otevreme pro cteni */
					/* odstranime posledni 2 parametry ze seznamu argumentu */
					free(line[l]); free(line[l-1]);
					line[l]=NULL; line[l-1]=NULL;
					l -= 2;
					io = 2;	/* I/O - soubor < stdout */
				}

				/* vytvorime novy proces */
				pid = fork();

				switch(pid) {
					case -1:
						printf("chyba ve vytvareni noveho procesu");
						exit(EXIT_FAILURE);
						break;
					case 0:		/* potomek */
						if (io == 1) {	/* stdin -> soubor */
							close(1);
							dup(fd);
						}
						if (io == 2) {	/* stdout < soubor */
							close(0);
							dup(fd);
						}
						if ((execvp(line[0], line) == -1)) {	/* provede prikaz */
							printf("Prikaz nelze provest\n");
							exit(EXIT_FAILURE);
						}
						/* korektne uzavreme deskriptory */
						if (io == 1 || io == 2) close(fd);
						break;
					default:	/* rodic */
						child=pid;

						/* pokud nejsme v pozadi, pak lze program ukoncit stisknutim ctrl+c */
						if (!background) {
							(void) signal(SIGINT, killchild);	/* bezici proces lze ukoncit stisknutim ctrl+c */
							waitpid(pid, &status, 0);
						} else {	/* proces je na pozadi, kontrolujeme jeho ukonceni pomoci signalu SIGCHILD */
							(void) signal(SIGCHLD, stopchild);
							/* neceka se na ukonceni procesu */
						}
						break;
				}
			} /* end if */
		} /* end if */
	
		/* vymazani promennych */
		memset((char *)buffer, 0, length);	/* smazani bufferu */
		/* smazani pole retezcu s argumenty */
		while (l >= 0) {
			free(line[l]);
			line[l]=NULL;
			l--;
		}
		
		/* vypsani promptu - opet cekame na vstup od input vlakna */
		write(1, "$ ", 2);

		/* predani rizeni input vlaknu */
		run = 0;
		pthread_cond_signal(&cond);
		pthread_mutex_unlock(&mutex);
	}
	/* korektne ukoncime */
	pthread_exit(NULL);
	return NULL;
}

/*
 * Rozparsuje vstup na radku do jednotlivych polozek v poli line
 * vraci pocet prvku v poli
 */
int parseInput(char *str, char **line[LENGTH]) {
	char * tmp;
	char * copy;	/* kopie str, strtok() meni parametr! */
	int i=0;		/* index v poli argumentu */

	/* vytvoreni kopie pro strok, ktery meni 1. parametr */
	copy=(char *)calloc(1, strlen(str));
	strncpy(copy, str, strlen(str)-1);
	
	/* v cyklu nacteme do pole line[] jednotlive casti retezce oddelene mezerou */
	tmp = strtok(copy, " ");
	while (tmp != NULL) {
		line[i]=(char *)calloc(1, strlen(tmp)+1);	/* tady to hlasi warning, ale melo by to byt v poradku */
		strcpy((char *)line[i++], tmp);
		copy = NULL;
		tmp = strtok(copy, " ");
	}
	/* uvolnime pamet */
	free(copy);
	return --i;
}

/*
 * Zpracovava signal SIGCHLD poslany potomky
 */
void stopchild(int sig) {
	printf("[Proces %d ukoncen]\n", (int)child);
	(void) signal(SIGCHLD, SIG_IGN);
}

/*
 * Zpracovava signal SIGINT
 * vznikne nejcasteji stisknutim ctrl+c
 */
void killchild(int sig) {
	kill(child, sig);
	(void) signal(SIGINT, killchild);
}

/*
 * Zpracovava signal SIGINT
 * ale jinak nez killchild() - neposila potomkovi signal k ukonceni
 */
void start(int sig) {
	(void) signal(SIGINT, start);
}

/* HLAVNI FUNKCE */
int main(int argc, char* argv[]) {
	pthread_t input, output;	/* 2 vlakna - jedno pro cteni vstupu, druhe pro zpracovavani */
	int tmp;

	(void) signal(SIGINT, start);	/* pri stisku ctrl+c UZ NA ZACATKU je treba se o signal postarat */

	/* vytvoreni vlakna pro vstup, starat se o nej bude funkce inputfunction */
	tmp = pthread_create(&input, NULL, inputfunction, NULL);
	if  (tmp != 0) {
		perror("Chyba pri vytvareni input vlakna\n");
		exit(EXIT_FAILURE);
	}
	/* vytvoreni vlakna pro vystup, starat se o nej bude funkce outputfunction */
	tmp = pthread_create(&output, NULL, outputfunction, NULL);
	if  (tmp != 0) {
		perror("Chyba pri vytvareni output vlakna\n");
		exit(EXIT_FAILURE);
	}
	/* vlakna ukoncena, tak je vratime systemu */
	pthread_join(input, NULL);
	pthread_join(output, NULL);

	return 0;

}

