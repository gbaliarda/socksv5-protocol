#include "include/clientargs.h"

#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

static void
serialize_request(struct client_request_args *args, struct client_serialized_request *request);

static void
serialize_config_data(struct client_request_args *args, struct client_serialized_request *request);

#define MAX_BYTES_DATA 65536 + 3

int
main(const int argc, char **argv) {
    struct client_request_args  args;
    struct sockaddr_in          sin4;
    struct sockaddr_in6         sin6;
    enum ip_version             ip_version;

    parse_args(argc, argv, &args, &sin4, &sin6, &ip_version);

    // socket -> connect -> send -> recv -> close

    int sock_fd;

    if(ip_version == ipv4){
        if((sock_fd = socket(sin4.sin_family, SOCK_STREAM, IPPROTO_TCP)) < 0){
            perror("client socket ipv4 creation");
            return 1;
        }
    
        if(connect(sock_fd, (struct sockaddr *)&sin4, sizeof(sin4)) < 0){
            perror("client socket ipv4 connect");
            return 1;
        }
    } else {
        if((sock_fd = socket(sin6.sin6_family, SOCK_STREAM, IPPROTO_TCP)) < 0){
            perror("client socket ipv6 creation");
            return 1;
        }
    
        if(connect(sock_fd, (struct sockaddr *)&sin6, sizeof(sin6)) < 0){
            perror("client socket ipv6 connect");
            return 1;
        }
    }

    struct client_serialized_request request;
    serialize_request(&args, &request);

    if(send(sock_fd, &request, sizeof(request) - DATA_SIZE + request.dlen, 0) < 0){
        perror("client socket send");
        return 1;
    }

    /*
    if(send(sock_fd, &args, sizeof(args) - sizeof(args.data) + args.dlen, 0) < 0){
        perror("client socket send");
        return 1;
    }
    */

    uint8_t buf[MAX_BYTES_DATA];

    static uint8_t combinedlen[2] = {0};
    static uint8_t numeric_data_array[4] = {0};
    static uint16_t dlen;
    static uint32_t numeric_response;


    
    long n; 
    while ((n = recv(sock_fd, buf, MAX_BYTES_DATA, 0)) != 0) { // no termino de mandar sigue recibiendo 
        if (n < 0) {
            perror("client socket recv");
            abort();
        }
    }


    // termine de recibir

    //Valores posibles del campo status en la response del protocolo
    enum monitor_resp_status {
        monitor_resp_status_ok              = 0x00,
        monitor_resp_status_invalid_version = 0x01,
        monitor_resp_status_invalid_method  = 0x02,
        monitor_resp_status_invalid_target  = 0x03,
        monitor_resp_status_invalid_data    = 0x04,
        monitor_resp_status_error_auth      = 0x05,
        monitor_resp_status_server_error    = 0x06,
    };

    switch (buf[0]) {
        case monitor_resp_status_ok:
            if (args.method == get) {
                combinedlen[0] =  buf[1];
                combinedlen[1] =  buf[2]; 
                dlen = ntohs(*(uint16_t*)combinedlen); // obtengo el dlen
                switch (args.target.get_target) {
                    case historic_connections:      // recibe uint32 (4 bytes)
                    case concurrent_connections:    // recibe uint32 (4 bytes)
                    case transferred_bytes:         // recibe uint32 (4 bytes)
                        for (int i = 0, j = 3; i < 4; i++) {
                            numeric_data_array[i] = buf[j++];
                        }
                        numeric_response = ntohl(*(uint32_t*)numeric_data_array);
                        if(args.target.get_target == historic_connections) {
                            printf("The amount of historic connections is: %d\n", numeric_response);
                        } else {
                            printf("The amount of %s is: %d",  args.target.get_target == concurrent_connections ? "concurrent connections" : "transferred bytes", numeric_response);
                        }
                        break;
                    case proxy_users_list:
                    case admin_users_list: 
                        printf("Printing %s user list:  \n", args.target.get_target == proxy_users_list ? "proxy" : "admin");
                        for (uint16_t i = 3; i < dlen + 3; i++) {
                            if (buf[i] == 0) {
                                putchar('\n');
                            } else {
                                putchar(buf[i]);
                            }
                        }
                        putchar('\n'); // el ultimo nombre de la lista no tiene \0
                        break;
                default:
                    break;
                }
            }
            else {
                printf("Tu configuracion al servidor ha sido exitosa!\n");
            }
            break;
        case monitor_resp_status_invalid_version:
            printf("La version de la request que ha mandado es incorrecta!\n");
            break;
        case monitor_resp_status_invalid_method:
            printf("El metodo de la request que ha mandado es incorrecta!\n");
            break;
        case monitor_resp_status_invalid_target:
            printf("El target de la request que ha mandado es incorrecta!\n");
            break;
        case monitor_resp_status_invalid_data:
            printf("La data de la request que ha mandado es incorrecta!\n");
            break;
        case monitor_resp_status_error_auth:
            printf("El token de la request que ha mandado es incorrecta!\n");
            break;
        case monitor_resp_status_server_error:
            printf("El servidor no pudo resolver tu request!\n");
            break;
        default:
            break;
    }

    if(close(sock_fd) < 0){
        perror("client socket close");
        return 1;
    }

    return 0;
}

static void
serialize_request(struct client_request_args *args, struct client_serialized_request *request){
    // Fields without unions
    request->version = 1;
    memcpy(request->token, args->token, TOKEN_SIZE);
    request->method = args->method;
    // request.dlen = htons(args.dlen);
    request->dlen = args->dlen;

    // Fields with unions
    switch(request->method){
        case get:
            request->target = args->target.get_target;
            memcpy(request->data, &args->data.optional_data, sizeof(uint8_t));
            break;
        case config:
            request->target = args->target.config_target;
            serialize_config_data(args, request);
            break;
        default:
            // no deberia llegar aqui
            break;
    }
}

static void
serialize_config_data(struct client_request_args *args, struct client_serialized_request *request){
    uint8_t disector_value;
    size_t username_len;
    size_t extra_param_len;
    
    switch(request->target){
        case toggle_disector:
            disector_value = (uint8_t)args->data.disector_data_params;
            memcpy(request->data, &disector_value, sizeof(disector_value));
            break;
        case add_proxy_user:
            username_len = strlen(args->data.add_proxy_user_params.user);
            memcpy(request->data, args->data.add_proxy_user_params.user, username_len);
            
            request->data[username_len] = args->data.add_proxy_user_params.separator;
            
            extra_param_len = strlen(args->data.add_proxy_user_params.pass);
            memcpy(request->data + username_len + 1, args->data.add_proxy_user_params.pass, extra_param_len);

            break;
        case add_admin_user:
            username_len = strlen(args->data.add_admin_user_params.user);
            memcpy(request->data, args->data.add_admin_user_params.user, username_len);
            
            request->data[username_len] = args->data.add_admin_user_params.separator;
            
            extra_param_len = strlen(args->data.add_admin_user_params.token);
            memcpy(request->data + username_len + 1, args->data.add_admin_user_params.token, extra_param_len);

            break;
        case del_proxy_user:
        case del_admin_user:
            memcpy(request->data, args->data.user, request->dlen);
            break;
    }
}