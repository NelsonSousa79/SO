#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#define REQ_FIFO "/tmp/dserv_req"
char client_fifo[64];

void print_usage() {
    const char *msg = "Modos de utilização:\n\n dclient -a <Titulo> <Autores> <Ano> <Caminho>\n dclient -c <Chave>\n dclient -d <Chave>\n dclient -l <Linha> <Palavra>\n dclient -s <Palavra>\n dclient -s <Palavra> <NºProcessos>\n dclient -f\n";
    write(2, msg, strlen(msg));
}

int main(int argc, char *argv[]){
    if (argc < 2) { 
        print_usage();
         return 1;
    }

    snprintf(client_fifo, sizeof(client_fifo), "/tmp/cli_%d", getpid());
    umask(0);
    mkfifo(client_fifo,0666);

    char buf[1024] = {0};
    char *op;

    if (strcmp(argv[1], "-a") == 0 && argc == 6) {
        op = "ADICIONAR";
        snprintf(buf, sizeof(buf), "%s|%s|%s|%s|%s|%s|",op, client_fifo, argv[2], argv[3], argv[4], argv[5]);
    }
    else if (strcmp(argv[1], "-c") == 0 && argc == 3) {
        op = "CONSULTAR";
        snprintf(buf, sizeof(buf), "%s|%s|%s|", op, client_fifo, argv[2]);
    }
    else if (strcmp(argv[1], "-d") == 0 && argc == 3) {
        op = "REMOVER";
        snprintf(buf, sizeof(buf), "%s|%s|%s|", op, client_fifo, argv[2]);
    }
    else if (strcmp(argv[1], "-l") == 0 && argc == 4) {
        op = "CONTARLINHAS";
        snprintf(buf, sizeof(buf), "%s|%s|%s|%s|",op, client_fifo, argv[2], argv[3]);
    }
    else if (strcmp(argv[1], "-s") == 0 && (argc == 3 || argc == 4)) {
        op = "PROCURA";
        if (argc == 4)
            snprintf(buf, sizeof(buf), "%s|%s|%s||%s|", op, client_fifo, argv[2], argv[3]);
        else
            snprintf(buf, sizeof(buf), "%s|%s|%s|||", op, client_fifo, argv[2]);
    }
    else if (strcmp(argv[1], "-f") == 0 && argc == 2) {
        op = "ENCERRARSERVIDOR";
        snprintf(buf, sizeof(buf), "%s|%s|", op, client_fifo);
    }
    else {
        print_usage();
        unlink(client_fifo);
        return 1;
    }

    if (access(REQ_FIFO, F_OK) != 0) {
        char *aguardando = "Aguardando o servidor ser iniciado...\n";
        write(STDERR_FILENO, aguardando, strlen(aguardando));

        // Loop para aguardar o servidor ser iniciado
        while (access(REQ_FIFO, F_OK) != 0) {
            usleep(500000); // Aguarda 500ms antes de verificar novamente
        }
    }

    int fd = open(REQ_FIFO, O_WRONLY);
    if (fd < 0) {
        const char *erro = "Erro: Não foi possível abrir o FIFO do servidor.\n";
        write(STDERR_FILENO, erro, strlen(erro));
        unlink(client_fifo);
        return 1;
    }

    if (strlen(buf) >= 512) {
        char *erro = "Erro: Mensagem muito grande (limite: 512 bytes).\n";
        write(STDERR_FILENO, erro, strlen(erro));
        close(fd);
        return 1;
    }
    write(fd, buf, strlen(buf));
    close(fd);

    fd = open(client_fifo, O_RDONLY);
    if (fd < 0) {
        perror("open client_fifo");
        unlink(client_fifo);
        return 1;
    }

    char resp[1024];
    ssize_t n;
    while ((n = read(fd, resp, sizeof(resp) - 1)) > 0) {
        resp[n] = '\0'; // Garante que a string seja terminada
        printf("%s", resp); // Imprime a parte lida
    }
    if (n < 0) {
        perror("read");
    }

    close(fd);
    unlink(client_fifo);
    return 0;
}


//CORRIGIR O OUTPUT DO CLIENTE EM ./dclient -l 