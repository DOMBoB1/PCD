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
    char result[BUF_SIZE * 4] = {0};
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
    fseek(file, 0, SEEK_END);
    size_t file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char *code = (char *)malloc(file_size + 1);
    if (!code) {
        perror("malloc");
        close(sock);
        fclose(file);
        exit(EXIT_FAILURE);
    }

    fread(code, 1, file_size, file);
    code[file_size] = '\0';
    fclose(file);

    // Send code to server
    send(sock, code, file_size, 0);
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
    if (!output_file) {
        perror("fopen output");
        close(sock);
        exit(EXIT_FAILURE);
    }
    fwrite(result, 1, n, output_file);
    fclose(output_file);

    printf("%s\n", result);

    // Close socket
    close(sock);

    // In cazul in care o sa avem nevoie sa avem mai multe fisiere, golim buffer-ul
    memset(result, 0, sizeof(result));

    return 0;
}
