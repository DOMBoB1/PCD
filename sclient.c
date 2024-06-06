#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define PORT 8080
#define BUF_SIZE 1024

int main(int argc, char *argv[]) {
    int sock;
    struct sockaddr_in server_addr;
    char buffer[BUF_SIZE];
    char result[BUF_SIZE * 4];
    ssize_t n;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <filename>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    FILE *file = fopen(argv[1], "r");
    if (!file) {
        perror("fopen");
        exit(EXIT_FAILURE);
    }

    // Create socket
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("socket");
        fclose(file);
        exit(EXIT_FAILURE);
    }

    // Define server address
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    if (inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sock);
        fclose(file);
        exit(EXIT_FAILURE);
    }

    // Connect to server
    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("connect");
        close(sock);
        fclose(file);
        exit(EXIT_FAILURE);
    }

    // Present menu to the user
    printf("Select language:\n1. C\n2. Python\n3. C++\n4. Java\nChoice: ");
    int choice;
    scanf("%d", &choice);
    getchar(); // Consume newline character left by scanf

    // Send choice to server
    snprintf(buffer, sizeof(buffer), "%d", choice);
    send(sock, buffer, strlen(buffer), 0);

    // Read code from file
    char *code = (char *)malloc(1);
    if (!code) {
        perror("malloc");
        close(sock);
        fclose(file);
        exit(EXIT_FAILURE);
    }
    code[0] = '\0';
    size_t len = 0;

    while (fgets(buffer, sizeof(buffer), file)) {
        len += strlen(buffer);
        char *new_code = realloc(code, len + 1);
        if (!new_code) {
            perror("realloc");
            free(code);
            close(sock);
            fclose(file);
            exit(EXIT_FAILURE);
        }
        code = new_code;
        strcat(code, buffer);
    }
    fclose(file);  // Close the file after reading

    // Send code to server
    size_t total_sent = 0;
    while (total_sent < strlen(code)) {
        ssize_t sent = send(sock, code + total_sent, strlen(code) - total_sent, 0);
        if (sent == -1) {
            perror("send");
            free(code);
            close(sock);
            exit(EXIT_FAILURE);
        }
        total_sent += sent;
    }
    free(code);

    // Receive result from server
    n = recv(sock, result, sizeof(result) - 1, 0);
    if (n <= 0) {
        perror("recv");
        close(sock);
        exit(EXIT_FAILURE);
    }
    result[n] = '\0';

    // Write the result to output.txt
    FILE *output_file = fopen("output.txt", "w");
    if (output_file < 0) {
        perror("fopen output");
        close(sock);
        exit(EXIT_FAILURE);
    }
    fwrite(result, strlen(result), strlen(result), output_file);
    
    //fprintf(output_file, "%s", result);
    fclose(output_file);
    printf(result);
    // Close socket
    close(sock);

    return 0;
}