/**
 * HTTP Server in C that serves HTML files and handles PHP scripts
 * 
 * This server handles GET requests and can:
 * - Serve static HTML files from a "www" directory
 * - Execute PHP scripts using the PHP CLI
 * 
 * Compile with: gcc -o http_server http_server.c
 * Run with: ./http_server
 * 
 * Before running, create a "www" directory in the same location as your executable
 * and place your HTML and PHP files there.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <ctype.h>

#define PORT 8080
#define BUFFER_SIZE 4096
#define MAX_PATH_LENGTH 256
#define WWW_DIRECTORY "./www"
#define PHP_CLI "/usr/bin/php"  // Path to PHP CLI executable

// Helper function to check if a file exists
int file_exists(const char *path) {
    struct stat buffer;
    return (stat(path, &buffer) == 0);
}

// Helper function to get file extension
const char* get_file_extension(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot || dot == filename) {
        return "";
    }
    return dot + 1;
}

// Helper function to determine content type based on file extension
const char* get_content_type(const char *extension) {
    if (strcmp(extension, "html") == 0 || strcmp(extension, "htm") == 0) {
        return "text/html";
    } else if (strcmp(extension, "css") == 0) {
        return "text/css";
    } else if (strcmp(extension, "js") == 0) {
        return "application/javascript";
    } else if (strcmp(extension, "jpg") == 0 || strcmp(extension, "jpeg") == 0) {
        return "image/jpeg";
    } else if (strcmp(extension, "png") == 0) {
        return "image/png";
    } else if (strcmp(extension, "gif") == 0) {
        return "image/gif";
    } else if (strcmp(extension, "txt") == 0) {
        return "text/plain";
    } else if (strcmp(extension, "php") == 0) {
        return "text/html";  // PHP output is typically HTML
    } else {
        return "application/octet-stream";
    }
}

// Parse HTTP request to extract the path
char* get_request_path(const char *request) {
    static char path[MAX_PATH_LENGTH];
    
    // Find the GET request line
    char *get_pos = strstr(request, "GET ");
    if (get_pos == NULL) {
        // Not a GET request
        strcpy(path, "/");
        return path;
    }
    
    // Extract the path
    char *path_start = get_pos + 4;  // Skip "GET "
    char *path_end = strstr(path_start, " ");
    if (path_end == NULL) {
        strcpy(path, "/");
        return path;
    }
    
    // Copy the path
    size_t path_length = path_end - path_start;
    if (path_length >= MAX_PATH_LENGTH) {
        path_length = MAX_PATH_LENGTH - 1;
    }
    strncpy(path, path_start, path_length);
    path[path_length] = '\0';
    
    // URL decode the path
    // Simple decoding for spaces only
    char *src = path;
    char *dst = path;
    while (*src) {
        if (strncmp(src, "%20", 3) == 0) {
            *dst++ = ' ';
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
    
    return path;
}

// Send a 404 Not Found response
void send_not_found(int client_socket) {
    const char *response = 
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/html\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<html><body>"
        "<h1>404 Not Found</h1>"
        "<p>The requested resource could not be found on this server.</p>"
        "</body></html>";
    
    write(client_socket, response, strlen(response));
}

// Send a 500 Internal Server Error response
void send_server_error(int client_socket) {
    const char *response = 
        "HTTP/1.1 500 Internal Server Error\r\n"
        "Content-Type: text/html\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<html><body>"
        "<h1>500 Internal Server Error</h1>"
        "<p>The server encountered an error while processing your request.</p>"
        "</body></html>";
    
    write(client_socket, response, strlen(response));
}

// Execute a PHP script and send the output to the client
void serve_php(int client_socket, const char *file_path) {
    printf("Executing PHP script: %s\n", file_path);
    
    // Create a pipe to capture the output of the PHP process
    int pipe_fd[2];
    if (pipe(pipe_fd) == -1) {
        perror("Failed to create pipe");
        send_server_error(client_socket);
        return;
    }
    
    // Fork a child process to execute the PHP script
    pid_t pid = fork();
    if (pid == -1) {
        // Fork failed
        perror("Failed to fork process");
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        send_server_error(client_socket);
        return;
    }
    
    if (pid == 0) {
        // This is the child process
        // Redirect stdout to the pipe
        close(pipe_fd[0]);  // Close read end
        dup2(pipe_fd[1], STDOUT_FILENO);
        close(pipe_fd[1]);
        
        // Execute the PHP script
        execl(PHP_CLI, PHP_CLI, file_path, NULL);
        
        // If execl returns, it failed
        perror("Failed to execute PHP");
        exit(EXIT_FAILURE);
    } else {
        // This is the parent process
        close(pipe_fd[1]);  // Close write end
        
        // Send HTTP headers
        const char *headers = 
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Connection: close\r\n"
            "\r\n";
        write(client_socket, headers, strlen(headers));
        
        // Read from the pipe and send to the client
        char buffer[BUFFER_SIZE];
        ssize_t bytes_read;
        while ((bytes_read = read(pipe_fd[0], buffer, BUFFER_SIZE)) > 0) {
            write(client_socket, buffer, bytes_read);
        }
        
        close(pipe_fd[0]);
        
        // Wait for the child process to finish
        int status;
        waitpid(pid, &status, 0);
        
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            printf("PHP script exited with status %d\n", WEXITSTATUS(status));
        }
    }
}

// Serve a static file
void serve_file(int client_socket, const char *file_path) {
    printf("Serving file: %s\n", file_path);
    
    // Get file extension
    const char *extension = get_file_extension(file_path);
    const char *content_type = get_content_type(extension);
    
    // Open the file
    int file_fd = open(file_path, O_RDONLY);
    if (file_fd == -1) {
        perror("Failed to open file");
        send_not_found(client_socket);
        return;
    }
    
    // Get file size
    struct stat file_stat;
    if (fstat(file_fd, &file_stat) == -1) {
        perror("Failed to get file stats");
        close(file_fd);
        send_server_error(client_socket);
        return;
    }
    
    // Send HTTP headers
    char headers[BUFFER_SIZE];
    snprintf(headers, BUFFER_SIZE,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n"
        "\r\n",
        content_type, file_stat.st_size);
    write(client_socket, headers, strlen(headers));
    
    // Send file content
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    while ((bytes_read = read(file_fd, buffer, BUFFER_SIZE)) > 0) {
        write(client_socket, buffer, bytes_read);
    }
    
    close(file_fd);
}

void handle_client(int client_socket) {
    char buffer[BUFFER_SIZE] = {0};
    
    // Read request
    ssize_t bytes_read = read(client_socket, buffer, BUFFER_SIZE - 1);
    if (bytes_read <= 0) {
        close(client_socket);
        return;
    }
    
    // Print request information
    printf("Received request:\n%s\n", buffer);
    
    // Get the requested path
    char *request_path = get_request_path(buffer);
    printf("Requested path: %s\n", request_path);
    
    // Construct the file path
    char file_path[MAX_PATH_LENGTH];
    snprintf(file_path, MAX_PATH_LENGTH, "%s%s", WWW_DIRECTORY, request_path);
    
    // If the path ends with a slash, try to serve index.html or index.php
    if (request_path[strlen(request_path) - 1] == '/') {
        char index_html_path[MAX_PATH_LENGTH];
        char index_php_path[MAX_PATH_LENGTH];
        
        snprintf(index_html_path, MAX_PATH_LENGTH, "%s/index.html", file_path);
        snprintf(index_php_path, MAX_PATH_LENGTH, "%s/index.php", file_path);
        
        if (file_exists(index_html_path)) {
            serve_file(client_socket, index_html_path);
        } else if (file_exists(index_php_path)) {
            serve_php(client_socket, index_php_path);
        } else {
            send_not_found(client_socket);
        }
    } else {
        // Check if the file exists
        if (file_exists(file_path)) {
            // Check if it's a PHP file
            const char *extension = get_file_extension(file_path);
            if (strcasecmp(extension, "php") == 0) {
                serve_php(client_socket, file_path);
            } else {
                serve_file(client_socket, file_path);
            }
        } else {
            send_not_found(client_socket);
        }
    }
    
    close(client_socket);
}

int main() {
    int server_fd, client_socket;
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    
    // Check if PHP is installed
    if (access(PHP_CLI, X_OK) != 0) {
        printf("Warning: PHP CLI (%s) not found or not executable.\n", PHP_CLI);
        printf("PHP scripts will not be processed correctly.\n");
        printf("Please install PHP or update the PHP_CLI path in the code.\n\n");
    }
    
    // Create www directory if it doesn't exist
    struct stat st = {0};
    if (stat(WWW_DIRECTORY, &st) == -1) {
        printf("Creating www directory...\n");
        mkdir(WWW_DIRECTORY, 0700);
        
        // Create a sample index.html file
        char index_path[MAX_PATH_LENGTH];
        snprintf(index_path, MAX_PATH_LENGTH, "%s/index.html", WWW_DIRECTORY);
        FILE *index_file = fopen(index_path, "w");
        if (index_file) {
            fprintf(index_file, 
                "<!DOCTYPE html>\n"
                "<html>\n"
                "<head>\n"
                "    <title>Welcome to C HTTP Server</title>\n"
                "</head>\n"
                "<body>\n"
                "    <h1>Welcome to C HTTP Server</h1>\n"
                "    <p>This is a sample HTML file being served by your C HTTP server.</p>\n"
                "    <p>Place your HTML files in the 'www' directory to serve them.</p>\n"
                "</body>\n"
                "</html>\n");
            fclose(index_file);
            printf("Created sample index.html file.\n");
        }
        
        // Create a sample PHP file
        char php_path[MAX_PATH_LENGTH];
        snprintf(php_path, MAX_PATH_LENGTH, "%s/info.php", WWW_DIRECTORY);
        FILE *php_file = fopen(php_path, "w");
        if (php_file) {
            fprintf(php_file, 
                "<?php\n"
                "    echo \"<h1>PHP is working!</h1>\";\n"
                "    echo \"<p>This is generated by PHP running on your C HTTP server.</p>\";\n"
                "    echo \"<h2>PHP Information</h2>\";\n"
                "    phpinfo();\n"
                "?>\n");
            fclose(php_file);
            printf("Created sample info.php file.\n");
        }
    }

    // Create socket file descriptor
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }
    
    // Set socket options to reuse address
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("Setsockopt failed");
        exit(EXIT_FAILURE);
    }
    
    // Setup server address structure
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);
    
    // Bind socket to the port
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }
    
    // Listen for connections
    if (listen(server_fd, 10) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }
    
    printf("Server started at http://localhost:%d\n", PORT);
    printf("Serving files from %s\n", WWW_DIRECTORY);
    printf("Try visiting: http://localhost:%d/ for the HTML sample\n", PORT);
    printf("Try visiting: http://localhost:%d/info.php for the PHP sample\n", PORT);
    printf("Press Ctrl+C to stop the server\n");
    
    // Accept and process connections
    while (1) {
        printf("Waiting for connections...\n");
        
        if ((client_socket = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen)) < 0) {
            perror("Accept failed");
            continue;
        }
        
        // Get client info
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(address.sin_addr), client_ip, INET_ADDRSTRLEN);
        printf("Client connected: %s:%d\n", client_ip, ntohs(address.sin_port));
        
        // Handle client
        handle_client(client_socket);
    }
    
    return 0;
}