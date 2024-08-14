#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>   // posix api for system calls
#include <string.h>   // string manipulation commnds like : strdup, strcmp, strlen, strtok, memset, memmove
#include <sys/wait.h> // wait call
#include <fcntl.h>    //file related ops : open , O_RDONLY, O_WRONLY
#include <termios.h>

#define BUFFER_SIZE 4096
#define HISTORY_SIZE 1000
#define PIPE_SIZE 4096

char *history[HISTORY_SIZE]; // stores pointer to prev stored commands
int history_count = 0;
int history_index = 0;

void add_to_history(const char *command)
{
    if (history_count < HISTORY_SIZE)
    {
        history[history_count++] = strdup(command); // copy cmd and create a pointer to it
    }
    else // history is full
    {
        free(history[0]);                                                   // remove oldest
        memmove(history, history + 1, (HISTORY_SIZE - 1) * sizeof(char *)); // shifting history array to left.
        history[HISTORY_SIZE - 1] = strdup(command);
    }
    history_index = history_count;
}

void enable_raw_mode()
{
    struct termios term;
    tcgetattr(STDIN_FILENO, &term); // STDIN_FILENO & stdin are different. stdin is a global "pointer" for standard stream of input, many function like 'putchar' directly use this globally declared pointer. STDIN_FILENO is ( a macro ) the default standard file "descriptor", these represent every processes individual input stream. -> https://man7.org/linux/man-pages/man3/stdin.3.html
    term.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &term);
}

void disable_raw_mode()
{
    struct termios term;
    tcgetattr(STDIN_FILENO, &term);
    term.c_lflag |= ICANON | ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &term);
}

void parse_command(char *input, char *argv[], char **input_file, char **output_file, int *append)
{
    char *start = input;
    // tokenization
    const char *delimeter = " ";
    unsigned idx = 0;

    char *token = strtok(start, delimeter);
    while (token != NULL)
    {
        if (strcmp(token, ">") == 0)
        {
            token = strtok(NULL, delimeter); // pass NULL so that token iterates on the previous given string only
            *output_file = token;
            *append = 0;
        }
        else if (strcmp(token, ">>") == 0)
        {
            token = strtok(NULL, delimeter);
            *output_file = token;
            *append = 1;
        }
        else if (strcmp(token, "<") == 0)
        {
            token = strtok(NULL, delimeter);
            *input_file = token;
        }
        else if (strcmp(token, " ") == 0)
        {
            token = strtok(NULL, delimeter);
        }
        else
        {
            argv[idx++] = token;
        }

        token = strtok(NULL, delimeter);
    }

    argv[idx] = NULL;
}

bool parse_pipe(char *argv[], char *child01_argv[], char *child02_argv[])
{
    unsigned idx = 0, split_idx = 0;
    bool contains_pipe = false;

    while (argv[idx] != NULL)
    {
        if (strcmp(argv[idx], "|") == 0)
        {
            split_idx = idx;
            contains_pipe = true;
            break;
        }
        idx++;
    }

    if (!contains_pipe)
    {
        return false;
    }

    for (idx = 0; idx < split_idx; idx++)
    {
        child01_argv[idx] = argv[idx];
    }
    child01_argv[idx] = NULL;

    idx = split_idx + 1;
    int j = 0;
    while (argv[idx] != NULL)
    {
        child02_argv[j++] = argv[idx++];
    }
    child02_argv[j] = NULL;

    return true;
}

void exec_with_pipe(char *child01_argv[], char *child02_argv[])
{
    int pipefd[2]; // create array to hold file descriptors

    if (pipe(pipefd) == -1) // Create a pipe
    {
        perror("pipe() failed");
        exit(EXIT_FAILURE);
    }

    // Create 1st child
    if (fork() == 0)
    {
        // Redirect STDOUT to output part of pipe
        dup2(pipefd[1], STDOUT_FILENO); // changing the file descriptor's output stream to pipe's input stream
        close(pipefd[0]);
        close(pipefd[1]);

        execvp(child01_argv[0], child01_argv);
        perror("Fail to execute first command");
        exit(EXIT_FAILURE);
    }

    // Create 2nd child
    if (fork() == 0)
    {
        // Redirect STDIN to input part of pipe
        dup2(pipefd[0], STDIN_FILENO); // changing the file descriptor's input stream to pipe's output stream
        close(pipefd[1]);
        close(pipefd[0]);

        execvp(child02_argv[0], child02_argv);
        perror("Fail to execute second command");
        exit(EXIT_FAILURE);
    }

    close(pipefd[0]);
    close(pipefd[1]);

    // Wait for child processes to finish
    wait(0);
    wait(0);
}

void execute_command(char *argv[], char *input_file, char *output_file, int append)
{

    pid_t pid = fork(); // creating child process to execute command
    if (pid == -1)
    {
        perror("fork");
    }
    else if (pid == 0) // means this is the child process
    {
        if (input_file)
        {
            int fd = open(input_file, O_RDONLY); // create a file description and file descriptor referencing it.
            if (fd == -1)
            {
                perror("open input file");
                exit(EXIT_FAILURE);
            }
            dup2(fd, STDIN_FILENO);
            close(fd);
        }

        if (output_file)
        {
            int fd;
            if (append)
            {
                fd = open(output_file, O_WRONLY | O_CREAT | O_APPEND, 0644); // open in write mode if exists else create then apend
            }
            else
            {
                fd = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644); // open in write mode if exists else create then truncate
            }

            if (fd == -1)
            {
                perror("open output file");
                exit(EXIT_FAILURE);
            }
            dup2(fd, STDOUT_FILENO); // duplicate file descriptor
            close(fd);
        }
        else if (execvp(argv[0], argv) == -1) // execute commands
        {
            perror("Execvp error : ");
        }

        exit(EXIT_FAILURE);
    }
    else // means this the parent process
    {
        wait(NULL); // wait here for child process to finish the command
    }
}

void print_prompt()
{
    printf("\rIn_a_nutshell> ");
    fflush(stdout);
}

int main()
{
    char input[BUFFER_SIZE]; // unparsed input
    char *argv[BUFFER_SIZE]; // parsed argument vector
    char *child01_argv[PIPE_SIZE], *child02_argv[PIPE_SIZE];
    char *input_file = NULL;
    char *output_file = NULL;
    int append = 0;
    int pos = 0, length = 0; // possition of the cursor
    int c;                   // current character

    enable_raw_mode();

    while (1) // loop for keep taking commands until "exit"
    {
        print_prompt();
        memset(input, 0, BUFFER_SIZE);

        while (1) // loop to take single command
        {
            c = getchar();
            if (c == '\n') // command has ended
            {
                input[length] = '\0'; // marker that shows the command ends here
                add_to_history(input);
                break;
            }
            else if (c == 127 || c == '\b') // handling backspace
            {
                if (pos > 0 && length > 0)
                {
                    pos--;
                    length--;
                    memmove(&input[pos], &input[pos + 1], length - pos);
                    input[length] = '\0';
                    printf("\b \b");
                    printf("%s", &input[pos]);
                    printf(" \b");
                    for (int i = pos; i < length; i++)
                    {
                        printf("\b");
                    }
                    fflush(stdout);
                }
            }
            else if (c == 27) // Handle escape sequences for arrow keys
            {
                c = getchar(); // skip the '[' character
                c = getchar(); // now take the escape character

                if (c == 'A') // Up arrow
                {
                    if (history_index > 0)
                    {
                        history_index--;
                        strcpy(input, history[history_index]);
                        length = pos = strlen(input);
                        printf("\r\033[K");
                        print_prompt();
                        printf("%s", input);
                        fflush(stdout);
                        length = strlen(history[history_index]);
                        for (int i = pos; i < length; i++)
                        {
                            printf("\b");
                        }
                        fflush(stdout);
                    }
                }
                else if (c == 'B') // Down arrow
                {
                    if (history_index < history_count)
                    {
                        history_index++;
                        if (history_index == history_count)
                        {
                            input[0] = '\0';
                            pos = length = 0;
                        }
                        else
                        {
                            strcpy(input, history[history_index]);
                            length = pos = strlen(input);
                        }
                        printf("\r\033[K");
                        print_prompt();
                        printf("%s", input);
                        fflush(stdout);
                        length = strlen(history[history_index]);
                        for (int i = pos; i < length; i++)
                        {
                            printf("\b");
                        }
                        fflush(stdout);
                    }
                }
                else if (c == 'C') // Right arrow
                {
                    if (pos < length)
                    {
                        pos++;
                        printf("\033[C"); // move cursor right
                        fflush(stdout);
                    }
                }
                else if (c == 'D') // Left arrow
                {
                    if (pos > 0)
                    {
                        pos--;
                        printf("\033[D"); // move cursor left
                        fflush(stdout);
                    }
                }
            }
            else
            {
                memmove(&input[pos + 1], &input[pos], length - pos); // shift all characters from pos+1 to length towards right
                input[pos] = c;
                pos++;
                length++;
                printf("\033[K");              // Clear line from cursor onwards
                printf("%s", &input[pos - 1]); // print cleared line again
                for (int i = pos; i < length; i++)
                {
                    printf("\b"); // to move cursor back to pos
                }
                fflush(stdout);
            }
        }

        printf("\n");

        // Trim leading spaces
        char *start = input;
        while (*start == ' ')
        {
            start++;
        }

        // on "exit" kill/end program.
        if (strcmp(input, "exit") == 0)
        {
            break;
        }

        // handle cd
        if (strncmp(start, "cd ", 3) == 0)
        {
            char *dir = start + 3;
            if (chdir(dir) != 0)
            {
                perror("cd error : ");
            }
            continue;
        }

        input_file = NULL;
        output_file = NULL;
        append = 0;

        parse_command(start, argv, &input_file, &output_file, &append);

        if (parse_pipe(argv, child01_argv, child02_argv))
        {
            exec_with_pipe(child01_argv, child02_argv);
        }
        else
            execute_command(argv, input_file, output_file, append);
        pos = 0; // stating cursor at 0
        length = 0;
    }

    disable_raw_mode();
    return 0;
}

// addition : & to continue parent process to take more commands wile child keeps execution.
