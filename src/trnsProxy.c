/**
    Handle multiple socket connections with select and fd_set on Linux
*/
  
#include <stdio.h>
#include <string.h>   //strlen
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>   //close
#include <arpa/inet.h>    //close
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/time.h> //FD_SET, FD_ISSET, FD_ZERO macros
#include <fcntl.h>
#include "transp-proxy/buffer.h"
#include <pthread.h>

#define TRUE   1
#define FALSE  0
#define PORT 8080
#define MAX_CLIENTS 500
#define BUFF_SIZE 1024
#define WELCOME_MSG "Transparent Proxy v1.0 \r\n"

typedef struct client {
    int fd; // accept fd
    struct buffer writeBuffController;
    uint8_t writeBuff[BUFF_SIZE];
    struct buffer readBuffController;
    uint8_t readBuff[BUFF_SIZE];
    int originFd; // origin server fd
} client;

typedef struct addrinfo_params {
    char *site;
    char *port;
    struct addrinfo options;
    struct addrinfo *origin_servers;
} addrinfo_params;

void* name_resolver (void* args){
    int *ret_value = (int *)malloc(sizeof(int));
    addrinfo_params *params = (addrinfo_params*) args;
    *ret_value = getaddrinfo(params->site, params->port, &(params->options), &(params->origin_servers));
    return ret_value;
}

int main(int argc , char *argv[]) {
    int opt = TRUE;
    int master_socket, addrlen, new_socket, activity, i;
    pthread_t name_resolver_thread;
    int *resolver_ret_value = NULL;

    addrinfo_params params;
    params.site = "www.google.com";
    params.port = "80";
    memset(&(params.options), 0, sizeof(struct addrinfo));
    // TODO: Change to IPv6 (AF_INET6) when using sockets for ipv6
    params.options.ai_family = AF_INET;

    if(pthread_create(&name_resolver_thread, NULL, name_resolver, &params) != 0){
        perror("thread create failed");
        exit(EXIT_FAILURE);
    }

    client client_socket[MAX_CLIENTS];
    for(int i = 0; i < MAX_CLIENTS; i++) {
        client_socket[i].fd = 0;
        client_socket[i].originFd = 0;
        bufferInit(&client_socket[i].readBuffController, BUFF_SIZE, client_socket[i].readBuff);
        bufferInit(&client_socket[i].writeBuffController, BUFF_SIZE, client_socket[i].writeBuff);
    }

    // create a master socket - socket pasivo
    if ((master_socket = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }
  
    // set master socket to allow multiple connections , this is just a good habit, it will work without this
    if (setsockopt(master_socket, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }
  
    // type of socket created
    struct sockaddr_in address;
    address.sin_family = AF_INET; // IPv4
    address.sin_addr.s_addr = INADDR_ANY; // aceptar cualquier direccion ip y puerto
    address.sin_port = htons(PORT);
      
    // bind the socket to localhost port 8080
    if (bind(master_socket, (struct sockaddr *)&address, sizeof(address))<0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
    printf("Listening on port %d \n", PORT);
     
    // try to specify maximum of 5 pending connections for the master socket
    if (listen(master_socket, 5) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }
      
    // accept the incoming connection
    addrlen = sizeof(address);
    puts("Waiting for connections ...");
     
    // set of socket descriptors
    fd_set readfds;
    fd_set writefds;

    while(TRUE) {
        // clear the socket set
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);

        // add master socket to set
        FD_SET(master_socket, &readfds);
        int max_sd = master_socket;
        int sd, osfd;
        int valread;
         
        // check if there are child sockets to set and set them
        for (i = 0; i < MAX_CLIENTS; i++) {
            // socket descriptor
            sd = client_socket[i].fd;
            osfd = client_socket[i].originFd;
             
            // if valid socket descriptor then add to select
            if(sd > 0 && osfd > 0) {                
                if (bufferCanWrite(&client_socket[i].readBuffController))
                    FD_SET(sd, &readfds); // cliente escribe y leemos en readBuf

                if (bufferCanWrite(&client_socket[i].writeBuffController))
                    FD_SET(osfd, &readfds); // origin escribe y leemos en writeBuf

                if (bufferCanRead(&client_socket[i].writeBuffController))
                    FD_SET(sd, &writefds); // cliente lee de writeBuf

                if (bufferCanRead(&client_socket[i].readBuffController))
                    FD_SET(osfd, &writefds); // origin lee de readBuf
            }
             
            // highest file descriptor number, need it for the select function
            if(sd > max_sd || osfd > max_sd)
                max_sd = sd > osfd ? sd : osfd;
        }
  
        // wait for an activity on one of the sockets , timeout is NULL , so wait indefinitely
        activity = select(max_sd + 1, &readfds, &writefds, NULL, NULL);
    
        if ((activity < 0) && (errno!=EINTR))
            printf("select error");
          
        // If something happened on the master socket , then its an incoming connection
        if (FD_ISSET(master_socket, &readfds)) {
            if ((new_socket = accept(master_socket, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
                if (errno != EWOULDBLOCK && errno != EAGAIN) {
                    perror("accept");
                    exit(EXIT_FAILURE);
                }
            } else {
                // inform user of socket number - used in send and receive commands
                printf("New connection , socket fd is %d , ip is : %s , port : %d \n" , new_socket , inet_ntoa(address.sin_addr), ntohs(address.sin_port));
                
                // add new socket to array of sockets
                for (i = 0; i < MAX_CLIENTS; i++) {
                    // if position is empty
                    if (client_socket[i].fd == 0) {
                        client_socket[i].fd = new_socket;
                        fcntl(new_socket, F_SETFL, O_NONBLOCK);

                        // create origin server socket
                        if ((client_socket[i].originFd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)) == 0) {
                            perror("origin socket failed");
                            exit(EXIT_FAILURE);
                        }
                        // If name has not been resolved yet (no ret value), then wait for thread to finish it
                        if(resolver_ret_value == NULL){
                            if(pthread_join(name_resolver_thread, (void *)&resolver_ret_value) != 0 || *resolver_ret_value != 0){
                                perror("name resolver thread failed");
                                exit(EXIT_FAILURE);
                            }
                            puts("Resolved origin server name");
                        }
                        // connecting to google, if in progress, waiting in select to write
                        if ((connect(client_socket[i].originFd, params.origin_servers->ai_addr, sizeof(struct sockaddr))) == -1) {
                            if (errno != EINPROGRESS && errno != EAGAIN) {
                                perror("origin server connect failed");
                                exit(EXIT_FAILURE);
                            }
                        }

                        printf("Adding to list of sockets as %d\n" , i);
                        break;
                    }
                }
            }
        }
          
        // else its some IO operation on some other socket :)
        for (i = 0; i < MAX_CLIENTS; i++) {
            sd = client_socket[i].fd;
            osfd = client_socket[i].originFd;
            
            buffer *br = &client_socket[i].readBuffController;
            buffer *bw = &client_socket[i].writeBuffController;
            ssize_t wbytes;
            ssize_t rbytes;
              
            if (FD_ISSET(sd, &readfds)) {
                wbytes = bufferFreeSpace(br);
                // check if it was for closing
                if ((valread = recv(sd, getWritePtr(br), wbytes, 0)) == 0) {
                    // somebody disconnected, get his details and print
                    getpeername(sd , (struct sockaddr*)&address, (socklen_t*)&addrlen);
                    printf("Host disconnected , ip %s , port %d \n", inet_ntoa(address.sin_addr) , ntohs(address.sin_port));
                      
                    // close the socket and mark as 0 in list for reuse
                    close(sd);
                    close(osfd);
                    client_socket[i].fd = 0;
                    client_socket[i].originFd = 0;
                }
                else if (valread > 0)
                    advanceWritePtr(br, valread);
                else if (errno != EWOULDBLOCK && errno != EAGAIN)
                    perror("read from client");
            }

            if (FD_ISSET(osfd, &readfds)) {
                wbytes = bufferFreeSpace(bw);
                if ((valread = recv(osfd, getWritePtr(bw), wbytes, 0)) == 0) {
                    getpeername(osfd, (struct sockaddr*)&address, (socklen_t*)&addrlen);
                    printf("Google disconnected , ip %s , port %d \n", inet_ntoa(address.sin_addr) , ntohs(address.sin_port));
                    
                    close(sd);
                    close(osfd);
                    client_socket[i].fd = 0;
                    client_socket[i].originFd = 0;
                }
                else if (valread > 0) 
                    advanceWritePtr(bw, valread);
                else if (errno != EWOULDBLOCK && errno != EAGAIN)
                    perror("read from origin");
            }

            if (FD_ISSET(osfd, &writefds)) {
                if((rbytes = send(osfd, getReadPtr(br), bufferPendingRead(br), 0)) >= 0)
                    advanceReadPtr(br, rbytes);
                else if (errno != EWOULDBLOCK && errno != EAGAIN)
                    perror("write to origin");
            }

            if (FD_ISSET(sd, &writefds)) {
                if ((rbytes = send(sd, getReadPtr(bw), bufferPendingRead(bw), 0)) >= 0)
                    advanceReadPtr(bw, rbytes);
                else if (errno != EWOULDBLOCK && errno != EAGAIN)
                    perror("write to client");
            }
        }
    }
    freeaddrinfo(params.origin_servers);
    free(resolver_ret_value);

    return 0;
}