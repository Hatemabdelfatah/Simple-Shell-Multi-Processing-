#include <stdio.h>      
#include <stdlib.h>    
#include <string.h>     
#include <unistd.h>     
#include <sys/types.h>  
#include <sys/wait.h>   
#include <signal.h>     
#include <fcntl.h>     
#include <errno.h>     
#include <ctype.h>      

#define MAX_LINE 1024      // Maximum input length and buffer size
#define INIT_TOKENS 100    // Initial capacity for tokens array

// Function declarations
void on_child_exit();                    // Reaps terminated child processes and logs them
void setup_environment();                // Changes directory to HOME (used at startup)
void shell();                            // Main shell loop: prints prompt (with current directory), reads input, processes commands
char **parse_input(const char *input);   // Splits the input string into tokens (handling quotes)
char *expand_variable(const char *token);  // Expands environment variables in a token (e.g., $HOME)
char **process_tokens(char **tokens);    // Processes tokens: expands variables and further splits tokens if needed
void execute_shell_builtin(char **tokens);  // Executes built-in commands: cd, echo, export
void execute_command(char **tokens, int bg);  // Executes external commands (foreground or background)

//-------------------------------------------------------------
// Main function: Registers the SIGCHLD handler, sets up the environment,
// then enters the shell's interactive loop.
int main() {
    // Set up the signal handler for SIGCHLD to handle background processes exiting.
    signal(SIGCHLD, on_child_exit);
    // Set the initial environment; currently, this changes the directory to "/" (or HOME as needed).
    setup_environment();
    // Enter the shell loop which handles user commands continuously.
    shell();
    return 0;
}

//-------------------------------------------------------------
// on_child_exit: A signal handler for SIGCHLD that performs cleanup of terminated child processes.
// It uses a non-blocking wait (WNOHANG) and logs each termination to a file ("log.txt").
void on_child_exit() {
    int saved_errno = errno;  // Preserve errno to avoid side-effects during signal handling.
    pid_t pid;
    int status;
    // Loop to reap all child processes that have terminated.
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        // Open log file in append mode, creating it if it doesn't exist.
        int fd = open("log.txt", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd != -1) {
            const char *msg = "Child process was terminated\n";
            // Write a log message about the terminated child process.
            write(fd, msg, strlen(msg));
            close(fd);
        }
    }
    errno = saved_errno;  // Restore the original errno value.
}

//-------------------------------------------------------------
// setup_environment: Prepares the initial environment for the shell.
// Currently, it attempts to change the working directory to "/".
// Future modifications could extend this to further environment settings.
void setup_environment() {
    if (chdir("/") != 0) {
        // If chdir fails, print an error message.
        perror("chdir");
    }
}

//-------------------------------------------------------------
// shell: Implements the main interactive loop for the shell.
// It displays a prompt (including the current directory), reads user input,
// processes commands (built-in and external), and handles background execution.
void shell() {
    char input[MAX_LINE];
    int ret;
    
    // Infinite loop to continuously prompt and process commands.
    while (1) {
        // Retrieve and display the current working directory in the prompt.
        {
            char cwd[MAX_LINE];
            if(getcwd(cwd, sizeof(cwd)) != NULL)
                // Display prompt in the format "myshell:<current_directory> > ".
                printf("myshell:%s> ", cwd);
            else
                printf("myshell> ");
            fflush(stdout);  // Flush the output to ensure prompt appears immediately.
        }
       
        // Read user input up to 1023 characters or until a newline is encountered.
        ret = scanf("%1023[^\n]", input);
        if(ret != 1) {
            // If no valid input is read (e.g., user just pressed Enter), consume the newline.
            getchar();
            continue;
        }
        getchar();  // Consume the newline character left by scanf.
        // If the input is an empty string, continue to the next iteration (re-prompt).
        if(strlen(input) == 0)
            continue;
        
        // Tokenize the input string into individual arguments/words.
        char **tokens = parse_input(input);
        if(tokens[0] == NULL) {
            // If tokenization results in no tokens, free the tokens array and re-prompt.
            free(tokens);
            continue;
        }
        
        // If the user enters "exit", clean up allocated memory and break out of the loop.
        if(strcmp(tokens[0], "exit") == 0) {
            for (int i = 0; tokens[i] != NULL; i++)
                free(tokens[i]);
            free(tokens);
            break;
        }
        
        // Check if the command is a built-in command (cd, echo, or export).
        if(strcmp(tokens[0], "cd") == 0 ||
           strcmp(tokens[0], "echo") == 0 ||
           strcmp(tokens[0], "export") == 0) {
            // Execute the built-in command without forking a new process.
            execute_shell_builtin(tokens);
            // Free memory allocated for tokens before continuing.
            for (int i = 0; tokens[i] != NULL; i++)
                free(tokens[i]);
            free(tokens);
            continue;
        }
        
        // Process tokens to expand any environment variables and split tokens with whitespace.
        char **processed_tokens = process_tokens(tokens);
        // Free the original tokens after processing.
        for (int i = 0; tokens[i] != NULL; i++)
            free(tokens[i]);
        free(tokens);
        
        // Check if the command should run in the background.
        int bg = 0;
        int count = 0;
        while (processed_tokens[count] != NULL)
            count++;
        if (count > 0 && strcmp(processed_tokens[count-1], "&") == 0) {
            bg = 1;  // Background flag set if last token is "&".
            free(processed_tokens[count-1]);  // Remove the "&" token.
            processed_tokens[count-1] = NULL;
        }
        
        // Execute the external command using the processed tokens.
        execute_command(processed_tokens, bg);
        
        // Free the memory allocated for the processed tokens.
        for (int i = 0; processed_tokens[i] != NULL; i++)
            free(processed_tokens[i]);
        free(processed_tokens);
    }
}

//-------------------------------------------------------------
// parse_input: Splits the input string into an array of tokens (words) based on spaces/tabs.
// It respects text enclosed in double quotes to ensure that quoted strings are treated as one token.
char **parse_input(const char *input) {
    int tokens_cap = INIT_TOKENS;
    char **tokens = malloc(tokens_cap * sizeof(char *));
    if (!tokens) {
        fprintf(stderr, "allocation error\n");
        exit(EXIT_FAILURE);
    }
    int token_index = 0;
    char current_token[MAX_LINE];  // Temporary buffer to hold characters for the current token.
    int ct_index = 0;              // Index in the current token buffer.
    int in_quotes = 0;             // Flag to track whether we are inside double quotes.
    
    // Iterate through each character in the input string.
    for (int i = 0; input[i] != '\0'; i++) {
        char c = input[i];
        if (c == '\"') {
            // Toggle the in_quotes flag when a double quote is encountered.
            in_quotes = !in_quotes;
        } else if ((c == ' ' || c == '\t') && !in_quotes) {
            // When encountering whitespace outside quotes, finish the current token.
            if (ct_index > 0) {  // Only add non-empty tokens.
                current_token[ct_index] = '\0';
                tokens[token_index++] = strdup(current_token);
                ct_index = 0;
                // Reallocate tokens array if the capacity is reached.
                if (token_index >= tokens_cap) {
                    tokens_cap += INIT_TOKENS;
                    tokens = realloc(tokens, tokens_cap * sizeof(char *));
                    if (!tokens) {
                        fprintf(stderr, "allocation error\n");
                        exit(EXIT_FAILURE);
                    }
                }
            }
        } else {
            // Add the character to the current token.
            current_token[ct_index++] = c;
        }
    }
    // After the loop, add any remaining token to the tokens array.
    if (ct_index > 0) {
        current_token[ct_index] = '\0';
        tokens[token_index++] = strdup(current_token);
    }
    tokens[token_index] = NULL;  // Terminate the tokens array with a NULL pointer.
    return tokens;
}

//-------------------------------------------------------------
// expand_variable: Searches for environment variable patterns (e.g., $VAR) in a token
// and replaces them with their corresponding values obtained via getenv.
// This function builds the result string dynamically.
char *expand_variable(const char *token) {
    size_t capacity = 1024;  // Initial capacity for the result string.
    char *result = malloc(capacity);
    if (!result) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    result[0] = '\0';  // Initialize as an empty string.
    size_t len = 0;    // Current length of the result string.
    
    // Process each character in the input token.
    for (size_t i = 0; token[i] != '\0'; i++) {
        if (token[i] == '$') {
            i++;  // Skip the '$' character.
            char varname[128];  // Buffer to hold the variable name.
            int j = 0;
            // Extract characters that form the variable name (alphanumeric or underscore).
            while (token[i] != '\0' && (token[i] == '_' || isalnum(token[i]))) {
                varname[j++] = token[i++];
            }
            varname[j] = '\0';
            i--;  // Step back to ensure the next iteration does not skip a character.
            char *value = getenv(varname);
            if (value == NULL)
                value = "";  // If variable is not found, use an empty string.
            size_t vlen = strlen(value);
            // Increase buffer size if necessary to hold the new value.
            while (len + vlen + 1 > capacity) {
                capacity *= 2;
                result = realloc(result, capacity);
                if (!result) {
                    perror("realloc");
                    exit(EXIT_FAILURE);
                }
            }
            // Concatenate the environment variable value to the result string.
            strcat(result, value);
            len += vlen;
        } else {
            // For normal characters, ensure there is enough capacity and append the character.
            if (len + 2 > capacity) {
                capacity *= 2;
                result = realloc(result, capacity);
                if (!result) {
                    perror("realloc");
                    exit(EXIT_FAILURE);
                }
            }
            result[len] = token[i];
            result[len+1] = '\0';
            len++;
        }
    }
    return result;
}

//-------------------------------------------------------------
// process_tokens: Takes an array of tokens, expands any environment variables,
// and further splits tokens if the expansion results in embedded whitespace.
// Returns a new array of tokens ready for command execution.
char **process_tokens(char **tokens) {
    int newSize = INIT_TOKENS;
    char **new_tokens = malloc(newSize * sizeof(char *));
    if (!new_tokens) {
        fprintf(stderr, "allocation error\n");
        exit(EXIT_FAILURE);
    }
    int count = 0;
    // Iterate over each original token.
    for (int i = 0; tokens[i] != NULL; i++) {
        // Expand any environment variables in the token.
        char *expanded = expand_variable(tokens[i]);
        // Check if the expanded token contains any whitespace.
        if (strchr(expanded, ' ') != NULL || strchr(expanded, '\t') != NULL) {
            // Duplicate the expanded token to safely use strtok.
            char *temp = strdup(expanded);
            // Use strtok to split the token by spaces or tabs.
            char *word = strtok(temp, " \t");
            while (word != NULL) {
                // If necessary, reallocate the new tokens array.
                if (count >= newSize) {
                    newSize += INIT_TOKENS;
                    new_tokens = realloc(new_tokens, newSize * sizeof(char *));
                    if (!new_tokens) {
                        fprintf(stderr, "allocation error\n");
                        exit(EXIT_FAILURE);
                    }
                }
                // Duplicate each word and add it to the new tokens array.
                new_tokens[count++] = strdup(word);
                word = strtok(NULL, " \t");
            }
            free(temp);    // Free the temporary duplicated string.
            free(expanded);  // Free the expanded token buffer.
        } else {
            // If there is no embedded whitespace, add the expanded token as is.
            if (count >= newSize) {
                newSize += INIT_TOKENS;
                new_tokens = realloc(new_tokens, newSize * sizeof(char *));
                if (!new_tokens) {
                    fprintf(stderr, "allocation error\n");
                    exit(EXIT_FAILURE);
                }
            }
            new_tokens[count++] = expanded;
        }
    }
    new_tokens[count] = NULL;  // Terminate the new tokens array.
    return new_tokens;
}

//-------------------------------------------------------------
// execute_shell_builtin: Handles execution of built-in shell commands (cd, echo, export).
// These commands are processed directly without forking a new process.
void execute_shell_builtin(char **tokens) {
    if (strcmp(tokens[0], "cd") == 0) {
        // Handle 'cd' command: if no argument or "~", change to HOME directory.
        if (tokens[1] == NULL || strcmp(tokens[1], "~") == 0) {
            char *home = getenv("HOME");
            if (home == NULL)
                home = "/";
            if (chdir(home) != 0)
                perror("cd");
        } else {
            char new_path[MAX_LINE];
    
            // If the directory starts with '~', replace it with the HOME directory.
            if (tokens[1][0] == '~') {
                char *home = getenv("HOME");
                if (home) {
                    // Append the rest of the path after '~' to the HOME directory.
                    snprintf(new_path, sizeof(new_path), "%s%s", home, tokens[1] + 1);
                } else {
                    perror("HOME not set");
                    return;
                }
            } else {
                // Otherwise, use the directory provided directly.
                strncpy(new_path, tokens[1], sizeof(new_path) - 1);
                new_path[sizeof(new_path) - 1] = '\0';
            }
    
            // Attempt to change directory to the specified path.
            if (chdir(new_path) != 0)
                perror("cd");
        }
    }
    else if (strcmp(tokens[0], "echo") == 0) {
        // Handle 'echo' command: Print out the arguments after expanding any variables.
        if (tokens[1] != NULL) {
            char buffer[MAX_LINE] = "";
            // Loop through all tokens after "echo".
            for (int i = 1; tokens[i] != NULL; i++) {
                char *expanded = expand_variable(tokens[i]);
                strcat(buffer, expanded);
                // Add a space between tokens if it's not the last token.
                if (tokens[i+1] != NULL)
                    strcat(buffer, " ");
                free(expanded);
            }
            // Print the final concatenated string.
            printf("%s\n", buffer);
        }
    }
    else if (strcmp(tokens[0], "export") == 0) {
        // Handle 'export' command: Set an environment variable.
        if (tokens[1] != NULL) {
            char *eq = strchr(tokens[1], '=');
            if (eq == NULL) {
                // If no '=' is found, the argument is invalid.
                fprintf(stderr, "export: invalid argument\n");
            } else {
                // Split the string at '=' to separate variable name and value.
                *eq = '\0';
                char *var = tokens[1];
                char *value = eq + 1;
                if (setenv(var, value, 1) != 0)
                    perror("export");
            }
        } else {
            // If no argument is provided, print an error message.
            fprintf(stderr, "export: missing argument\n");
        }
    }
}

//-------------------------------------------------------------
// execute_command: Creates a child process using fork() and executes external commands via execvp().
// For foreground commands, the parent waits until the child completes; for background commands, it does not wait.
void execute_command(char **tokens, int background) {
    pid_t pid = fork();
    if (pid < 0) {
        // If fork() fails, print an error message.
        perror("fork");
        return;
    }
    if (pid == 0) {  // Child process branch.
        // Execute the command using execvp; if it fails, print error and exit.
        if (execvp(tokens[0], tokens) == -1) {
            perror("execvp");
        }
        exit(EXIT_FAILURE);  // Exit if execution fails.
    } else {  // Parent process branch.
        if (!background) {
            int status;
            // Wait for the child process to complete if running in the foreground.
            if (waitpid(pid, &status, 0) == -1)
                perror("waitpid");
            else if (WIFSIGNALED(status))
                fprintf(stderr, "Child terminated abnormally by signal %d\n", WTERMSIG(status));
        }
        // If background, do not wait (child will be handled by the SIGCHLD handler).
    }
}