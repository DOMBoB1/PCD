#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>

#define PORT 8080
#define BUF_SIZE 1024

void compile_and_run_code(const char *filename, char *result, size_t result_size) {
    char compile_cmd[BUF_SIZE];
    char run_cmd[BUF_SIZE];
    char buffer[BUF_SIZE];
    FILE *fp;
    int status;

    // Create compile command
    snprintf(compile_cmd, sizeof(compile_cmd), "gcc %s -o output 2>&1", filename);

    // Compile the code
    fp = popen(compile_cmd, "r");
    if (fp == NULL) {
        snprintf(result, result_size, "Failed to run compile command.\n");
        return;
    }

    // Read the compile output
    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
        strncat(result, buffer, result_size - strlen(result) - 1);
    }

    status = pclose(fp);
    if (status != 0) {
        // Compilation failed, return the compile output
        return;
    }

    // Create run command
    snprintf(run_cmd, sizeof(run_cmd), "./output 2>&1");

    // Run the compiled code
    fp = popen(run_cmd, "r");
    if (fp == NULL) {
        snprintf(result, result_size, "Failed to run the program.\n");
        return;
    }

    // Read the program output
    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
        strncat(result, buffer, result_size - strlen(result) - 1);
    }

    pclose(fp);

    // Clean up
    remove("output");
}

void handle_client(int client_socket) {
    char buffer[BUF_SIZE];
    char result[BUF_SIZE * 4] = {0}; // Allocate enough space for the result
    ssize_t n;

    // Receive code from client
    n = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) {
        perror("recv");
        close(client_socket);
        return;
    }
    buffer[n] = '\0';

    // Save the received code to a file
    FILE *code_file = fopen("code.c", "w");
    if (code_file == NULL) {
        perror("fopen");
        close(client_socket);
        return;
    }
    fprintf(code_file, "%s", buffer);
    fclose(code_file);

    // Compile and run the code
    compile_and_run_code("code.c", result, sizeof(result));

    // Send the result back to the client
    send(client_socket, result, strlen(result), 0);

    // Close the client socket
    close(client_socket);

    // Clean up
    remove("code.c");
}

int main() {
    int server_socket, client_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    // Create socket
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    // Set socket options
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Bind socket to an address and port
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);
    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    // Listen for incoming connections
    if (listen(server_socket, 5) == -1) {
        perror("listen");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    printf("Server is listening on port %d\n", PORT);

    // Accept and handle incoming connections
    while (1) {
        client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_socket == -1) {
            perror("accept");
            continue;
        }

        if (!fork()) {
            // Child process
            close(server_socket);
            handle_client(client_socket);
            exit(EXIT_SUCCESS);
        }

        // Parent process
        close(client_socket);
        waitpid(-1, NULL, WNOHANG); // Clean up zombie processes
    }

    close(server_socket);
    return 0;
}
