#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>

#include "config.h"
#include "thpool.h"

extern sqlite3 *db;
typedef struct {
	int fd;
	char ipaddr[128];
} p2p_t;

int loc_fd, inc_fd;
struct sockaddr_storage inc_addr;
socklen_t inc_len = sizeof(inc_addr);

ThreadPool thpool;
pthread_t net_thread;

int num_threads = NUM_THREADS;
int pidfile;
char *port = (char *)DEFAULT_PORT;
int queue_length = QUEUE_LENGTH;

char clientaddr[128] = { '\0' };
sqlite3 *db;
time_t start_time;
char *term;
static int c_count = 0;

void clean_string(char *);// handle escape characters
int client_count(int);// count clients num
void console_help();// print help info
void *get_in_addr(struct sockaddr *);// get ip addr
int recv_msg(int, char *);
int send_msg(int, char *);
int validate_int(char *);
void print_stats();
void stat_handler();
void shutdown_handler();
void *p2p(void *);
void *tcp_listen();

int main(int argc, char *argv[])
{
    struct addrinfo hints, *result;
    int yes = 1;
    char command[512] = { '\0' };
    int i = 0;
    sqlite3_stmt *stmt;
    char query[256] = { '\0' };

    // signal handlers
    signal(SIGHUP, (void (*)(int))shutdown_handler);//  close the terminal
    signal(SIGINT, (void (*)(int))shutdown_handler);//  CTRL+C
    signal(SIGTERM, (void (*)(int))shutdown_handler);// kill (1)

    signal(SIGUSR1, (void (*)(int))stat_handler);
    signal(SIGUSR2, (void (*)(int))stat_handler);
    // get the name of the terminal
    term = strdup(ttyname(1));

    fprintf(stdout, "%s: %s initializing%s...  \n", SERVER_NAME, INFO_MSG, SERVER_NAME);

    start_time = time(NULL);// start timing

    //======handle arguments from a command======
    for(i = 1; i < argc; i++) {
        if(strcmp("-h", argv[i]) == 0 || strcmp("--help", argv[i]) == 0) {
            fprintf(stdout, "usage: %s [-h | --help] [-p | --port port] [-q | --queue queue_length]"
                "[-t | --threads thread_count]\n\n", SERVER_NAME);
            fprintf(stdout, "%s descriptions:\n", SERVER_NAME);
            fprintf(stdout, "\t-h | --help:            help - help info\n");
            fprintf(stdout, "\t-p | --port:            port - server port(default: %s)\n", 
                DEFAULT_PORT);
            fprintf(stdout, "\t-q | --queue:   queue_length - backlog(default: %d)\n", 
                QUEUE_LENGTH);
            fprintf(stdout, "\t-t | --threads: thread_count - maximum number of threads(concurrent"
                "threads) (default: %d)\n", NUM_THREADS);
            fprintf(stdout, "\n");
            exit(0);
        } // option -h | --help
        else if(strcmp("-p", argv[i]) == 0 || strcmp("--port", argv[i]) == 0) {
            if(argv[i+1] != NULL) {
                if(validate_int(argv[i+1])) {
                    if(atoi(argv[i+1]) >= 0 && atoi(argv[i+1]) <= MAX_PORT) {
                        port = argv[i+1];
                        i++;
                    }
                    else
                        fprintf(stderr, "%s: %s port is not in range(0-%d), recover default port %s\n", 
                            SERVER_NAME, ERROR_MSG, MAX_PORT, DEFAULT_PORT);
                }
                else
                    fprintf(stderr, "%s: %s invalid port, recover default port %s\n", SERVER_NAME, 
                        ERROR_MSG, DEFAULT_PORT);
            }
            else
                fprintf(stderr, "%s: %s no port value, recover default port %s\n", 
                    SERVER_NAME, ERROR_MSG, DEFAULT_PORT);
        } // option -p | --port
        else if(strcmp("-q", argv[i]) == 0 || strcmp("--queue", argv[i]) == 0) {
            if(argv[i+1] != NULL) {
                if(validate_int(argv[i+1])) {
                    if(atoi(argv[i+1]) >= 1) {
                        queue_length = atoi(argv[i+1]);
                        i++;
                    }
                    else
                        fprintf(stderr, "%s: %s invalid queue length, recover default queue length %d\n",
                            SERVER_NAME, ERROR_MSG, QUEUE_LENGTH);
                }
                else
                    fprintf(stderr, "%s: %s invalid queue length, recover default queue length %d\n", 
                        SERVER_NAME, ERROR_MSG, QUEUE_LENGTH);
            }
            else
                fprintf(stderr, "%s: %s no queue length value, recover default queue length %d\n", 
                    SERVER_NAME, ERROR_MSG, QUEUE_LENGTH);
        } // option -q | --queue
        else if(strcmp("-t", argv[i]) == 0 || strcmp("--threads", argv[i]) == 0) {
            if(argv[i+1] != NULL) {
                if(validate_int(argv[i+1])) {
                    if(atoi(argv[i+1]) >= 1) {
                        num_threads = atoi(argv[i+1]);
                        i++;
                    }
                    else
                        fprintf(stderr, "%s: %s invalid threads num, recover default threads num %d\n",
                            SERVER_NAME, ERROR_MSG, NUM_THREADS);
                }
                else
                    fprintf(stderr, "%s: %s invalid threads num, recover default threads num %d\n",
                        SERVER_NAME, ERROR_MSG, NUM_THREADS);
            }
            else
                fprintf(stderr, "%s: %s no threads num value, recover default threads num %d\n", 
                    SERVER_NAME, ERROR_MSG, NUM_THREADS);
        } // option -t | --threads
        else {
            fprintf(stderr, "%s: %s unknown option'%s' , lookup '%s -h or --help'\n", SERVER_NAME,
                ERROR_MSG, argv[i], SERVER_NAME);
            exit(-1);
        }
    }

    //======prepare and clean the sqlite3======
    sqlite3_open(DB_FILE, &db);
    if(db == NULL) {
        fprintf(stderr, "%s: %s sqlite: cannot open sqlite3 %s\n", SERVER_NAME, ERROR_MSG, DB_FILE);
        exit(-1);
    }
    // delete all records from a table
    // usage DELETE FROM table_name WHERE [condition];
    sprintf(query, "DELETE FROM files");
    // tranlate a string sql into a prepared statement obj
    sqlite3_prepare_v2(db, query, strlen(query) + 1, &stmt, NULL);
    // execute the prepared statemented (has been compiled)
    if(sqlite3_step(stmt) != SQLITE_DONE) {
        fprintf(stderr, "%s: %s sqlite3: failed ！ \n", SERVER_NAME, ERROR_MSG);
        exit(-1);
    }
    // destroy the statement obj
    sqlite3_finalize(stmt);

    //======initialize server socket======
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    // indicate that the address will be used after bind, otherwise, used for connect
    hints.ai_flags = AI_PASSIVE;
    // getaddrinfo is adaptive to ipv4 and ipv6
    if((getaddrinfo(NULL, port, &hints, &result)) != 0) {
        fprintf(stderr, "%s: %s getaddrinfo failed \n", SERVER_NAME, ERROR_MSG);
        exit(-1);
    }
    // we directly use the first addrinfo struct ptr
    if((loc_fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol)) == -1) {
        fprintf(stderr, "%s: %s socket failed \n", SERVER_NAME, ERROR_MSG);
        exit(-1);
    }
    if(setsockopt(loc_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
        fprintf(stderr, "%s: %s SO_REUSEADDR set failed \n", SERVER_NAME, ERROR_MSG);
        exit(-1);
    }

    if((bind(loc_fd, result->ai_addr, result->ai_addrlen)) == -1) {
        if(atoi(port) < PRIVILEGED_PORT)
            fprintf(stderr, "%s: %s bind failed , invalid port \n", SERVER_NAME, ERROR_MSG);
        else
            fprintf(stderr, "%s: %s bind failed \n", SERVER_NAME, ERROR_MSG);
        exit(-1);
    }
    // destroy unuseful addrinfo structs
    freeaddrinfo(result);

    listen(loc_fd, queue_length);
    // initialize thread pool
    thpool.Init(num_threads);
    // child thread does the server socket listening
    pthread_create(&net_thread, NULL, (void *(*)(void *))&tcp_listen, NULL);
    fprintf(stdout, "%s: %s server initialize success , config info： [PID: %d] [PORT: %s]"
        "[QUEUE_LENGTH: %d] [THREADS_NUM: %d]\n", SERVER_NAME, OK_MSG, getpid(), port, 
        queue_length, num_threads);
	fprintf(stdout, "%s: %s input 'help' for more help info \n", SERVER_NAME, INFO_MSG);
    fprintf(stdout, "%s: %s input 'stop' or  Ctrl+C stop the server \n", 
        SERVER_NAME, INFO_MSG);

    // loop for user command
    while(1) {
        fgets(command, sizeof(command), stdin);
        clean_string((char *)&command);
        if(strcmp(command, "clear") == 0)
            system("clear");
        else if(strcmp(command, "help") == 0)
            console_help();
        else if(strcmp(command, "stat") == 0)
            print_stats();
        else if(strcmp(command, "stop") == 0)
            break;
        else
            fprintf(stderr, "%s: %s unknown command '%s', input 'help' for help \n", 
                SERVER_NAME, ERROR_MSG, command);
    }
    // stop the server
    kill(getpid(), SIGINT);
}

void clean_string(char *str)
{
	int i = 0;
	int index = 0;
	char buffer[1024];
	for(i = 0; i < strlen(str); i++)
	{
		if(str[i] != '\b' && str[i] != '\n' && str[i] != '\r')
			buffer[index++] = str[i];
	}
	memset(str, 0, sizeof(str));
	buffer[index] = '\0';
	strcpy(str, buffer);
}

int client_count(int change)
{
	c_count += change;
	return c_count;
}

void console_help()
{
	fprintf(stdout, "%s help:\n", SERVER_NAME);
	fprintf(stdout, "\tclear - clear the termimal info\n");
	fprintf(stdout, "\t help - get the help info\n");
	fprintf(stdout, "\t stat - get the current state\n");
	fprintf(stdout, "\t stop - stop the server\n");
}

void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET)
        return &(((struct sockaddr_in*)sa)->sin_addr);
    else
        return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int recv_msg(int fd, char *message)
{
	int b_received = 0;
	int b_total = 0;
	char buffer[1024];
	memset(buffer, '\0', sizeof(buffer));
	
	b_received = recv(fd, buffer, sizeof(buffer), 0);
	b_total += b_received;
	strcpy(message, buffer);
	return b_total;
}

int send_msg(int fd, char *message)
{
	return send(fd, message, strlen(message), 0);
}

int validate_int(char *string)
{
	int isInt = 1;
	int j = 0;
    for(j = 0; j < strlen(string); j++) {
        if(isInt == 1) {
            if(!isdigit(string[j]))
                isInt = 0;
        }
    }
	return isInt;
}

void print_stats()
{
    // print runtime
    int hours, minutes, seconds;
    char runtime[32] = { '\0' };
    char tpusage[32] = { '\0' };
    seconds = (int)difftime(time(NULL), start_time);
    minutes = seconds / 60;
    hours = minutes / 60;
    minutes = minutes % 60;
    seconds = seconds % 60;
    sprintf(runtime, "%02d:%02d:%02d", hours, minutes, seconds);

    // thread pool is available enough
    if(client_count(0) < (num_threads * TP_UTIL)) {
        fprintf(stdout, "%s: %s ", SERVER_NAME, OK_MSG);
        sprintf(tpusage, "[online users: %d/%d]", client_count(0), num_threads);
    }
    // thread pool is nearly full
    else if(((double)client_count(0) >= ((double)num_threads * TP_UTIL)) 
        && client_count(0) <= num_threads) {
        // print warning
        fprintf(stdout, "%s: %s ", SERVER_NAME, WARN_MSG);
        sprintf(tpusage, "\033[1;33m[online users: %d/%d]\033[0m", 
            client_count(0), num_threads);
    }
    // thread pool is unavailable
    else {
        // print error
        fprintf(stdout, "%s: %s ", SERVER_NAME, ERROR_MSG);
        sprintf(tpusage, "\033[1;31m[online users: %d/%d]\033[0m", 
            client_count(0), num_threads);
    }
    fprintf(stdout, "server running： [PID: %d] [RUNTIME: %s] [PORT: %s] [QUEUE_LENGTH: %d] %s\n", 
        getpid(), runtime, port, queue_length, tpusage);
}

// handle the SIGUSR1 | SIGUSR2
void stat_handler()
{
    freopen(term, "w", stdout);
    // print the server stat
    print_stats();
    // redirect stdout to /dev/null
    freopen("/dev/null", "w", stdout);
}

void shutdown_handler()
{
    // cancel the tcp listen thread
    pthread_cancel(net_thread);
    fprintf(stdout, "\n");

    // close the sqlite3
    if(sqlite3_close(db) != SQLITE_OK) {
        fprintf(stderr, "%s: %s sqlite: close sqlite3 failed.\n", SERVER_NAME, ERROR_MSG);
        exit(-1);
    }

    // close the server socket write|read
    if(shutdown(loc_fd, 2) == -1) {
        fprintf(stderr, "%s: %s server socket shutdown failed.\n", SERVER_NAME, ERROR_MSG);
        exit(-1);
    }
    // release the socket description
    if(close(loc_fd) == -1) {
        fprintf(stderr, "%s: %s server socket close fialed.\n", SERVER_NAME, ERROR_MSG);
        exit(-1);
    }

    // destroy the thread pool
    thpool.Destroy();
    fprintf(stdout, "%s: %s delete %d clients, server shutdown.\n", SERVER_NAME, OK_MSG, 
        client_count(0));
    exit(0);
}

void *p2p(void *args)
{
	char in[512],out[512] = { '\0' };
	p2p_t params = *((p2p_t *)(args));
	char *filename, *filehash, *filesize;
	long int f_size = 0;
	char peeraddr[128] = { '\0' };
	strcpy(peeraddr, params.ipaddr);
	int user_fd = params.fd;
	char query[256];
	int status;
	int flag=0;
	sqlite3_stmt *stmt;
	
	sprintf(out, "%s: %s \n", SERVER_NAME, USER_MSG);
	send_msg(user_fd, out);

	// wait for client's message
	while((strcmp(in, "CONNECT")) != 0 && (strcmp(in, "QUIT") != 0)) {
		recv_msg(user_fd, (char *)&in);
		clean_string((char *)&in);

        // if the message is CONNECT, reply the ACCEPT, which simulate the
        // three-way handshake
		if(strcmp(in, "CONNECT") == 0) {
			fprintf(stdout, "%s: %s , %s sends a msg , reply a ack msg [fd: %d]\n", 
                SERVER_NAME, OK_MSG,peeraddr, user_fd);
			sprintf(out, "ACCEPT\n");
			send_msg(user_fd, out);
		}
	}

	//waiting for futher message
	while(strcmp(in, "QUIT") != 0) {
		memset(in, 0, sizeof(in));
		memset(out, 0, sizeof(out));
		memset(query, 0, sizeof(query));

		// get the message
		recv_msg(user_fd, (char *)&in);
		clean_string((char *)&in);

		// msg format: ADD <filename> <hashcode> <filesize>
		if(strncmp(in, "ADD", 3) == 0) {
            // split the parameters
			strtok(in, " ");
			filename = strtok(NULL, " ");
			flag=0;

			if(filename != NULL) {
				filehash = strtok(NULL, " ");

				if(filehash != NULL) {
					filesize = strtok(NULL, " ");

					if((filesize != NULL) && (validate_int(filesize) == 1)) {
						f_size = atoi(filesize);
                        // sqlite3:insert string
						sprintf(query, "INSERT INTO files VALUES('%s', '%s', '%ld', '%s')", 
                            filename, filehash, f_size, peeraddr);
						sqlite3_prepare_v2(db, query, strlen(query) + 1, &stmt, NULL);

						if((status = sqlite3_step(stmt)) != SQLITE_DONE) {
							if(status == SQLITE_CONSTRAINT) {
								fprintf(stderr, "%s: %s sqlite: add failed , file into has existed\n", 
                                    SERVER_NAME, ERROR_MSG);
								sprintf(out, "ERROR add file failed , file info has existed\n");
								send_msg(user_fd, out);
							}
							else {
								fprintf(stderr, "%s: %s sqlite: add failed \n", SERVER_NAME, ERROR_MSG);
								sprintf(out, "ERROR add file failed , unknown reason\n");
								send_msg(user_fd, out);
							}
						}
						sqlite3_finalize(stmt);
						
						if(status == SQLITE_DONE) {
							fprintf(stdout, "%s: %s  client%s add a file to server db %20s [hashcode: %20s]" 
                            "[size: %10ld]\n", SERVER_NAME, INFO_MSG, peeraddr, filename, filehash, f_size);
							// return OK
							sprintf(out, "OK\n");
							send_msg(user_fd, out);
						}
					} // (filesize != NULL) && validate_int(filesize) == 1
					else
						flag=1;
				} // filehash != NULL
				else
					flag=1;
			} // filename != NULL
			else
				flag=1;
			
			// error format
			if(flag) {
				fprintf(stderr, "%s: %s add file failed，error format \n", SERVER_NAME, ERROR_MSG);
				sprintf(out, "ERROR add file failed , error format\n");
				send_msg(user_fd, out);
			}

		} // ADD command
		
		// msg format: DELETE [filename] [hashcode]
		else if(strncmp(in, "DELETE", 6) == 0) {
			strtok(in, " ");
			filename = strtok(NULL, " ");
			flag=0;
			
			if(filename != NULL) {
				filehash = strtok(NULL, " ");
				if(filehash != NULL) {
                    //sqlite3: delete string
					sprintf(query, "DELETE FROM files WHERE file='%s' AND hash='%s' AND peer='%s'", 
                        filename, filehash, peeraddr);
					sqlite3_prepare_v2(db, query, strlen(query) + 1, &stmt, NULL);
					if(sqlite3_step(stmt) != SQLITE_DONE) {
						fprintf(stderr, "%s: %s sqlite: delete file failed \n", SERVER_NAME, ERROR_MSG);
						sprintf(out, "ERROR delete file failed , unknown reason \n");
						send_msg(user_fd, out);	
					}
					sqlite3_finalize(stmt);
					
					fprintf(stdout, "%s: %s client%s delete a file from server db '%s'('%s') \n", 
                        SERVER_NAME, OK_MSG, peeraddr, filename, filehash);
					sprintf(out, "OK\n");
					send_msg(user_fd, out);
				} // filehash != NULL
				else
					flag=1;
			} // filename != NULL
			else
				flag=1;
			// error format
			if(flag) {
				fprintf(stderr, "%s: %s delete file failed , error format\n", SERVER_NAME, ERROR_MSG);
				sprintf(out, "ERROR delete file failed , error format\n");
				send_msg(user_fd, out);
			}
		} // DELETE command
		
		// msg format : LIST
		else if(strcmp(in, "LIST") == 0) {
            //sqlite3: query string
			sprintf(query, "SELECT DISTINCT file,size,peer FROM files ORDER BY file ASC");
			sqlite3_prepare_v2(db, query, strlen(query) + 1, &stmt, NULL);
			while((status = sqlite3_step(stmt)) != SQLITE_DONE) {
				if(status == SQLITE_ERROR) {
					fprintf(stderr, "%s: %s sqlite: database error \n", SERVER_NAME, ERROR_MSG);
					sprintf(out, "ERROR database error \n");
					send_msg(user_fd, out);
				}
                //exclude the owner file info
				else if(strcmp(peeraddr,(char *) sqlite3_column_text(stmt, 2))) {					
					sprintf(out, "%s %d\n", sqlite3_column_text(stmt, 0), sqlite3_column_int(stmt, 1));
					send_msg(user_fd, out);
				}
			}
		sqlite3_finalize(stmt);
		sprintf(out, "OK\n");
		send_msg(user_fd, out);
		} // LIST command

		// msg format : QUIT
		else if(strcmp(in, "QUIT") == 0) {
			continue;
		} // QUIT command

		// msg format : REQUEST [filename]
		else if(strncmp(in, "REQUEST", 7) == 0) {
			strtok(in, " ");
			filename = strtok(NULL, " ");
			if(filename != NULL) {
				sprintf(query, "SELECT peer,size FROM files WHERE file='%s' ORDER BY peer ASC", 
                    filename);
				sqlite3_prepare_v2(db, query, strlen(query) + 1, &stmt, NULL);
				while((status = sqlite3_step(stmt)) != SQLITE_DONE) {
					if(status == SQLITE_ERROR) {
						fprintf(stderr, "%s: %s sqlite: database error \n", SERVER_NAME, ERROR_MSG);						
						sprintf(out, "ERROR database error\n");
						send_msg(user_fd, out);
					}	
					else {
						sprintf(out, "%s %ld\n", sqlite3_column_text(stmt, 0), 
                            (long int)sqlite3_column_int(stmt, 1));
						send_msg(user_fd, out);
					}
				}
				sqlite3_finalize(stmt);
				
				sprintf(out, "OK\n");
				send_msg(user_fd, out);				
			} // filename != NULL
			else {
				sprintf(out, "ERROR get the file info failed \n");
				send_msg(user_fd, out);
			}
		} // REQUEST command
		else {
			sprintf(out, "ERROR error format\n");
			send_msg(user_fd, out);
		}
	} // command is not QUIT


	memset(out, 0, sizeof(out));
	sprintf(out, "GOODBYE\n");
	send_msg(user_fd, out);

	fprintf(stdout, "%s: %s client %s logout [online users: %d/%d]\n", 
        SERVER_NAME, OK_MSG, peeraddr, client_count(-1), NUM_THREADS);
	
	sprintf(query, "DELETE FROM files WHERE peer='%s'", peeraddr);
	sqlite3_prepare_v2(db, query, strlen(query) + 1, &stmt, NULL);
	if(sqlite3_step(stmt) != SQLITE_DONE) {
		fprintf(stderr, "%s: %s client %s delete failed [fd: %d]\n", SERVER_NAME, ERROR_MSG, 
            peeraddr, user_fd);
		return (void *)-1;
	}
	sqlite3_finalize(stmt);
    // release the client socket description
	if(close(user_fd) == -1) {
		fprintf(stderr, "%s: %s close client sockset failed [fd: %d]\n", SERVER_NAME, 
            ERROR_MSG, user_fd);
		return (void *)-1;
	}

	return (void *)0;
}

// accept the client connection
void *tcp_listen()
{
    p2p_t params;
    char out[512] = { '\0' };

    while(1) {
        if((inc_fd = accept(loc_fd, (struct sockaddr *)&inc_addr, &inc_len)) == -1) {
            fprintf(stderr, "%s: %s accept failed \n", SERVER_NAME, ERROR_MSG);
            return (void *)-1;
        }
        else {
            inet_ntop(inc_addr.ss_family, get_in_addr((struct sockaddr *)&inc_addr), clientaddr,
                sizeof(clientaddr));

            fprintf(stdout, "%s: %s  %s is attempt to connect the server [socket fd: %d]" 
                " [online users: %d/%d]\n",SERVER_NAME, INFO_MSG, clientaddr, inc_fd,
                client_count(1),num_threads);

            if(((double)client_count(0) >= ((double)num_threads * TP_UTIL)) 
                && (client_count(0) <= num_threads)) {
                if(client_count(0) == num_threads)
                    fprintf(stdout, "%s: %s thread pool is unavailable [online users: %d/%d]\n",
                     SERVER_NAME, WARN_MSG, client_count(0), num_threads);
                else
                    fprintf(stdout, "%s: %s threads pool is nearly unavailable [online users: %d/%d]\n", 
                        SERVER_NAME, WARN_MSG, client_count(0), num_threads);
            }
            else if((client_count(0)) > num_threads) {
                fprintf(stderr, "%s: %s thread pool has unavailable , there are continuous clients attempt"
                "to connect [online users: %d/%d]\n", SERVER_NAME, ERROR_MSG, client_count(0), num_threads);
                sprintf(out, "%s: %s server is unavailable , please try again later\n", SERVER_NAME, USER_MSG);
                send_msg(inc_fd, out);
            }
            params.fd = inc_fd;
            strcpy(params.ipaddr, clientaddr);
            // add a work to thread pool
            thpool.AddWork(new Job(p2p, (void *)&params)); 
        }
    }
}