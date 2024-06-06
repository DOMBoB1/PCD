#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define PORT 8080
#define BUF_SIZE 1024

int main() {
    int sock;
    struct sockaddr_in server_addr;
    char buffer[BUF_SIZE];
    char result[BUF_SIZE * 4];
    ssize_t n;

    // Create socket
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    // Define server address
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    if (inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sock);
        exit(EXIT_FAILURE);
    }

    // Connect to server
    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("connect");
        close(sock);
        exit(EXIT_FAILURE);
    }

    // Get code from user
    printf("Enter C code (end with an empty line):\n");
    char *code = NULL;
    size_t len = 0;
    while (fgets(buffer, sizeof(buffer), stdin)) {
        if (strcmp(buffer, "\n") == 0) {
            break;
        }
        len += strlen(buffer);
        code = realloc(code, len + 1);
        strcat(code, buffer);
    }

    // Send code to server
    send(sock, code, strlen(code), 0);
    free(code);

    // Receive result from server
    n = recv(sock, result, sizeof(result) - 1, 0);
    if (n <= 0) {
        perror("recv");
        close(sock);
        exit(EXIT_FAILURE);
    }
    result[n] = '\0';

    // Print the result
    printf("Result from server:\n%s\n", result);

    // Close socket
    close(sock);

    return 0;
}
