#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#define REQ_FIFO "/tmp/dserv_req"
#define MAXLINE 1024

// Estrutura de um documento
typedef struct Doc {
    int id;
    char title[256];
    char authors[256];
    int year;
    char path[256];
    struct Doc* next;
} Doc;

// Variáveis globais que guardam o estado do servidor
Doc* head = NULL;
int next_id = 1;
char base_folder[256];
int cache_size = 0;
int running = 1;
int documentos_adicionados_na_sessao = 0;

// Estrutura para rastrear processos ativos na pesquisa paralela
typedef struct {
    pid_t pid;    // PID do processo filho
    int doc_id;   // ID do documento associado
} ProcessInfo;

// Funções para carregar/salvar o índice
void load_index() {
    int fd = open("index.db", O_RDONLY);
    if (fd < 0) return;

    char buffer[4096];
    ssize_t bytes_read;
    char *ptr = buffer;
    int remaining = 0;

    while ((bytes_read = read(fd, buffer + remaining, sizeof(buffer) - remaining - 1)) > 0) {
        bytes_read += remaining;
        buffer[bytes_read] = '\0';
        ptr = buffer;

        while (1) {
            char *line_end = strchr(ptr, '\n');
            if (!line_end) {
                remaining = bytes_read - (ptr - buffer);
                if (remaining > 0) memmove(buffer, ptr, remaining);
                break;
            }

            *line_end = '\0';
            Doc *d = malloc(sizeof(Doc));
            if (!d) break;

            // Parse manual da linha: id|title|authors|year|path
            char *token = strtok(ptr, "|");
            if (!token) { free(d); break; }
            d->id = atoi(token);
            
            token = strtok(NULL, "|");
            if (!token) { free(d); break; }
            strncpy(d->title, token, sizeof(d->title));
            
            token = strtok(NULL, "|");
            if (!token) { free(d); break; }
            strncpy(d->authors, token, sizeof(d->authors));
            
            token = strtok(NULL, "|");
            if (!token) { free(d); break; }
            d->year = atoi(token);
            
            token = strtok(NULL, "\n");
            if (!token) { free(d); break; }
            strncpy(d->path, token, sizeof(d->path));
            
            d->next = head;
            head = d;
            if (d->id >= next_id) next_id = d->id + 1;
            ptr = line_end + 1;
        }
    }
    close(fd);
}

void save_index() {
    int fd = open("index.db", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open");
        return;
    }

    char line[1024];
    for (Doc* p = head; p; p = p->next) {
        int len = snprintf(line, sizeof(line), "%d|%s|%s|%d|%s\n",
                          p->id, p->title, p->authors, p->year, p->path);
        if (write(fd, line, len) != len) {
            perror("write");
            break;
        }
    }
    close(fd);
}

void resposta(const char* client_fifo, const char* msg) {
    int fd = open(client_fifo, O_WRONLY);
    if (fd < 0) return;
    write(fd, msg, strlen(msg));
    close(fd);
}

void adicionarDocumento(char **args) {
    const char *cf = args[0];
    const char *relpath = args[4];

    if (documentos_adicionados_na_sessao >= cache_size) {
        resposta(cf, "Erro: Limite de documentos atingido nesta sessão\n");
        return;
    }

    char fullpath[512];
    snprintf(fullpath, sizeof(fullpath), "%s/%s", base_folder, relpath);

    if (access(fullpath, F_OK) != 0) {
        resposta(cf, "Erro: Caminho não existe\n");
        return;
    }

    Doc *d = malloc(sizeof(Doc));
    if (!d) {
        resposta(cf, "Erro: Memória insuficiente\n");
        return;
    }

    d->id = next_id++;
    strncpy(d->title, args[1], sizeof(d->title));
    strncpy(d->authors, args[2], sizeof(d->authors));
    d->year = atoi(args[3]);
    strncpy(d->path, relpath, sizeof(d->path));
    d->next = head;
    head = d;
    documentos_adicionados_na_sessao++;

    char resp[128];
    snprintf(resp, sizeof(resp), "Document %d indexed\n", d->id);
    resposta(cf, resp);
}

void consultarDocumento(char **args) {
    const char *cf = args[0];
    int key = atoi(args[1]);
    for (Doc* p = head; p; p = p->next) {
        if (p->id == key) {
            char buf[MAXLINE];
            snprintf(buf, sizeof(buf),
                     "Title: %s\nAuthors: %s\nYear: %d\nPath: %s\n",
                     p->title, p->authors, p->year, p->path);
            resposta(cf, buf);
            return;
        }
    }
    resposta(cf, "Chave não existe\n");
}

void removerDocumento(char **args) {
    const char *cf = args[0];
    int key = atoi(args[1]);
    Doc **pp = &head;
    while (*pp) {
        if ((*pp)->id == key) {
            Doc *tmp = *pp;
            *pp = tmp->next;
            free(tmp);
            char resp[64];
            snprintf(resp, sizeof(resp),
                     "Index entry %d deleted\n", key);
            resposta(cf, resp);
            return;
        }
        pp = &(*pp)->next;
    }
    resposta(cf, "Chave não existe\n");
}

void contaLinhasDocumento(char **args) {
    const char *cf = args[0];
    int key = atoi(args[1]);
    char *kw = args[2];
    for (Doc* p = head; p; p = p->next) {
        if (p->id == key) {
            char fullpath[512];
            snprintf(fullpath, sizeof(fullpath), "%s/%s", base_folder, p->path);

            if (access(fullpath, R_OK) != 0) {
                resposta(cf, "O documento não se encontra acessível para a pesquisa.\n");
                return;
            }

            int pipefd[2];
            pipe(pipefd);
            pid_t pid = fork();
            if (pid == 0) {
                close(pipefd[0]);
                dup2(pipefd[1], STDOUT_FILENO);
                execlp("grep", "grep", "-c", "-w", kw, fullpath, NULL);
                _exit(1);
            }
            close(pipefd[1]);
            char countbuf[32] = {0};
            read(pipefd[0], countbuf, sizeof(countbuf)-1);
            close(pipefd[0]);
            resposta(cf, countbuf);
            waitpid(pid, NULL, 0);
            return;
        }
    }
    resposta(cf, "Chave não existe\n");
}

int compare_ids(const void *a, const void *b) {
    return (*(int*)a - *(int*)b);
}

void pesquisaDocumentos(char **args, int nr_proc) {
    const char *cf = args[0];
    char *kw = args[1];

    int *ids = malloc(next_id * sizeof(int));
    if (!ids) {
        resposta(cf, "Erro: Memória insuficiente\n");
        return;
    }
    int count = 0;

    ProcessInfo *active_processes = malloc(nr_proc * sizeof(ProcessInfo));
    if (!active_processes) {
        free(ids);
        resposta(cf, "Erro: Memória insuficiente\n");
        return;
    }
    int processos_ativos = 0;
    Doc *doc_atual = head;

    while (doc_atual != NULL || processos_ativos > 0) {
        // Iniciar novos processos enquanto houver documentos e slots livres
        while (doc_atual != NULL && processos_ativos < nr_proc) {
            char current_path[512];
            snprintf(current_path, sizeof(current_path), "%s/%s", base_folder, doc_atual->path);

            pid_t pid = fork();
            if (pid == 0) {
                execlp("grep", "grep", "-q", "-w", kw, current_path, NULL);
                _exit(1);
            } else if (pid > 0) {
                active_processes[processos_ativos].pid = pid;
                active_processes[processos_ativos].doc_id = doc_atual->id;
                processos_ativos++;
                doc_atual = doc_atual->next;
            } else {
                free(ids);
                free(active_processes);
                resposta(cf, "Erro: Falha ao criar processo\n");
                return;
            }
        }

        // Esperar por qualquer processo filho terminar
        int status;
        pid_t pid_filho = waitpid(-1, &status, 0);
        if (pid_filho > 0) {
            for (int i = 0; i < processos_ativos; i++) {
                if (active_processes[i].pid == pid_filho) {
                    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                        ids[count++] = active_processes[i].doc_id;
                    }
                    // Remove o processo do array
                    active_processes[i] = active_processes[processos_ativos - 1];
                    processos_ativos--;
                    break;
                }
            }
        }
    }

    // Ordenar e formatar resposta
    qsort(ids, count, sizeof(int), compare_ids);

    size_t buffer_size = 3; // Para "[" e "]\n"
    for (int i = 0; i < count; i++) {
        buffer_size += snprintf(NULL, 0, "%d", ids[i]) + 1;
    }

    char *result = malloc(buffer_size);
    if (!result) {
        free(ids);
        free(active_processes);
        resposta(cf, "Erro: Memória insuficiente\n");
        return;
    }

    char *ptr = result;
    snprintf(ptr, buffer_size, "[");
    ptr += 1;

    for (int i = 0; i < count; i++) {
        ptr += snprintf(ptr, buffer_size - (ptr - result), "%d", ids[i]);
        if (i < count - 1) {
            ptr += snprintf(ptr, buffer_size - (ptr - result), ",");
        }
    }

    snprintf(ptr, buffer_size - (ptr - result), "]\n");
    resposta(cf, result);

    free(result);
    free(ids);
    free(active_processes);
}

void encerrarServidor(char **args) {
    char *cf = args[0];
    resposta(cf, "Server is shutting down\n");
    running = 0;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Modos de utilização:\n\n ./dserver <ficheiro> <tamanho>\n");
        exit(1);
    }
    strncpy(base_folder, argv[1], sizeof(base_folder));
    cache_size = atoi(argv[2]);
    umask(0);
    mkfifo(REQ_FIFO, 0666);
    load_index();

    char buf[MAXLINE];
    while (running) {
        int fd = open(REQ_FIFO, O_RDONLY);
        if (fd < 0) break;
        ssize_t n = read(fd, buf, sizeof(buf)-1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = '\0';

        char *args[6];
        int argc_req = 0;
        char *tok = strtok(buf, "|");
        while (tok && argc_req < 6) {
            args[argc_req++] = tok;
            tok = strtok(NULL, "|");
        }
        const char *op = args[0];
        if (strcmp(op, "ADICIONAR") == 0)            adicionarDocumento(args+1);
        else if (strcmp(op, "CONSULTAR") == 0)       consultarDocumento(args+1);
        else if (strcmp(op, "REMOVER") == 0)         removerDocumento(args+1);
        else if (strcmp(op, "CONTARLINHAS") == 0)    contaLinhasDocumento(args+1);
        else if (strcmp(op, "PROCURA") == 0) {
            int nr = (argc_req >= 4 ? atoi(args[3]) : 1);
            pesquisaDocumentos(args+1, nr);
        }
        else if (strcmp(op, "ENCERRARSERVIDOR") == 0) encerrarServidor(args+1);
    }
    save_index();
    unlink(REQ_FIFO);
    return 0;
}