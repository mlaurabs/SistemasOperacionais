#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>

#include "sfp.h" 

#define ROOT_DIR "SFSS-root-dir"

// funcao auxiliar para montar o caminho do arquivo
void ConstruirCaminho(char *buffer, size_t tam, const char *caminho_req) {
    snprintf(buffer, tam, "%s%s", ROOT_DIR, caminho_req);
}


// READ
void TratarRead(MensagemSFP *msg) {
    int req_owner = msg->read_req.owner;
    int req_offset = msg->read_req.offset;
    char req_path[MAX_PATH];
    strcpy(req_path, msg->read_req.path); 

    msg->read_rep.tipo = REP_READ;
    msg->read_rep.owner = req_owner;

    char caminho_real[MAX_PATH + 20];
    ConstruirCaminho(caminho_real, sizeof(caminho_real), req_path);

    FILE *f = fopen(caminho_real, "rb");
    if (!f) {
        printf("SFSS: Erro leitura '%s': %s\n", caminho_real, strerror(errno));
        msg->read_rep.offset = -1; // erro: Arquivo não existe
        return; 
    }

    if (fseek(f, req_offset, SEEK_SET) != 0) {
        printf("SFSS: Erro seek '%s': %s\n", caminho_real, strerror(errno));
        msg->read_rep.offset = -2; // erro: offset inválido
        fclose(f);
        return;
    }

    memset(msg->read_rep.payload, 0, TAM_BLOCO);
    size_t lidos = fread(msg->read_rep.payload, 1, TAM_BLOCO, f);
    fclose(f);

    msg->read_rep.offset = req_offset; 
    printf("SFSS: Read OK (A%d) '%s' Off=%d\n", req_owner, req_path, req_offset);
}

// WRITE
void TratarWrite(MensagemSFP *msg) {
    int req_owner = msg->write_req.owner;
    int req_offset = msg->write_req.offset;
    char req_path[MAX_PATH];
    char req_payload[TAM_BLOCO];
    strcpy(req_path, msg->write_req.path);
    memcpy(req_payload, msg->write_req.payload, TAM_BLOCO);

    msg->write_rep.tipo = REP_WRITE;
    msg->write_rep.owner = req_owner;

    char caminho_real[MAX_PATH + 20];
    ConstruirCaminho(caminho_real, sizeof(caminho_real), req_path);

    FILE *f = fopen(caminho_real, "r+b"); 
    if (!f) {
        f = fopen(caminho_real, "w+b");
        if (!f) {
            printf("SFSS: Erro escrita '%s': %s\n", caminho_real, strerror(errno));
            msg->write_rep.offset = -1; 
            return;
        }
    }

    fseek(f, 0, SEEK_END);
    long tamanho_atual = ftell(f);

    if (req_offset > tamanho_atual) {
        fseek(f, tamanho_atual, SEEK_SET);
        long buraco = req_offset - tamanho_atual;
        char *espacos = malloc(buraco);
        if (espacos) {
            memset(espacos, 0x20, buraco); 
            fwrite(espacos, 1, buraco, f);
            free(espacos);
        }
    }

    fseek(f, req_offset, SEEK_SET);
    fwrite(req_payload, 1, TAM_BLOCO, f);
    fclose(f);

    printf("SFSS: Write OK (A%d) '%s'\n", req_owner, req_path);
    msg->write_rep.offset = req_offset; 
}

// cria dir
void TratarCreateDir(MensagemSFP *msg) {
    int req_owner = msg->create_dir_req.owner;
    char req_path[MAX_PATH];
    char req_dirname[MAX_PATH];
    strcpy(req_path, msg->create_dir_req.path);
    strcpy(req_dirname, msg->create_dir_req.dirname);

    msg->create_dir_rep.tipo = REP_CREATE_DIR;
    msg->create_dir_rep.owner = req_owner;

    char caminho_real[MAX_PATH * 2];
    char caminho_novo[MAX_PATH * 2];

    ConstruirCaminho(caminho_real, sizeof(caminho_real), req_path);
    snprintf(caminho_novo, sizeof(caminho_novo),"%s/%s", caminho_real, req_dirname);

    printf("SFSS: Mkdir (A%d) '%s'\n", req_owner, caminho_novo);

    if (mkdir(caminho_novo, 0700) == 0 || errno == EEXIST) {
        snprintf(msg->create_dir_rep.path, sizeof(msg->create_dir_rep.path), 
                 "%s/%s", req_path, req_dirname);
        msg->create_dir_rep.len_path = strlen(msg->create_dir_rep.path);
    } else {
        perror("SFSS mkdir error");
        msg->create_dir_rep.len_path = -1; 
    }
}

// remove diretório
void TratarRemoveDir(MensagemSFP *msg) {
    int req_owner = msg->rem_dir_req.owner;
    char req_path[MAX_PATH];
    char req_dirname[MAX_PATH];
    strcpy(req_path, msg->rem_dir_req.path);
    strcpy(req_dirname, msg->rem_dir_req.dirname);

    msg->rem_dir_rep.tipo = REP_REM_DIR;
    msg->rem_dir_rep.owner = req_owner;

    char caminho_real[MAX_PATH * 2];
    ConstruirCaminho(caminho_real, sizeof(caminho_real), req_path);

    char alvo[MAX_PATH * 2];
    snprintf(alvo, sizeof(alvo), "%s/%s", caminho_real, req_dirname);

    printf("SFSS: Rmdir (A%d) '%s'\n", req_owner, alvo);

    if (remove(alvo) == 0) {
        strcpy(msg->rem_dir_rep.path, req_path);
        msg->rem_dir_rep.len_path = strlen(msg->rem_dir_rep.path);
    } else {
        // se falhar (ex: diretorio nao vazio ou nao existe), retorna erro
        // mas não vamos travar o servidor, apenas notificar.
        // perror("SFSS remove error"); 
        msg->rem_dir_rep.len_path = -1;
    }
}

// listar diretório
void TratarListDir(MensagemSFP *msg) {
    int req_owner = msg->list_dir_req.owner;
    char req_path[MAX_PATH];
    strcpy(req_path, msg->list_dir_req.path);

    msg->list_dir_rep.tipo = REP_LIST_DIR;
    msg->list_dir_rep.owner = req_owner;

    char caminho_real[MAX_PATH + 20];
    ConstruirCaminho(caminho_real, sizeof(caminho_real), req_path);

    DIR *d;
    struct dirent *dir;
    d = opendir(caminho_real);

    msg->list_dir_rep.nrnames = 0;

    if (d) {
        int char_count = 0;
        int idx = 0;

        while ((dir = readdir(d)) != NULL && idx < MAX_DIR_ENTRIES) {
            if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0)
                continue;

            int len_nome = strlen(dir->d_name);
            if (char_count + len_nome + 1 >= TAM_LIST_BUFFER) break;

            strcpy(&msg->list_dir_rep.allfilenames[char_count], dir->d_name);
            
            msg->list_dir_rep.fstlstpositions[idx].inicio = char_count;
            msg->list_dir_rep.fstlstpositions[idx].fim = char_count + len_nome;
            
            if (dir->d_type == DT_DIR) 
                msg->list_dir_rep.fstlstpositions[idx].eh_arquivo = 0;
            else 
                msg->list_dir_rep.fstlstpositions[idx].eh_arquivo = 1;

            char_count += len_nome + 1; 
            idx++;
        }
        msg->list_dir_rep.nrnames = idx;
        closedir(d);
        printf("SFSS: ListDir OK (A%d) '%s' -> %d itens\n", req_owner, req_path, idx);
    } else {
        // se erro ao abrir dir, retorna código negativo
        msg->list_dir_rep.nrnames = -1; 
    }
}

int main() {
    int sockfd;
    struct sockaddr_in servaddr, cliaddr;
    MensagemSFP msg_buffer;
    
    // cria o socket
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // configura endereço do servidor
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(PORTA_SERVIDOR); 

    // Bind
    if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    // cria diretórios iniciais se não existirem
    if (mkdir(ROOT_DIR, 0700) == 0) {
        printf(">>> Diretório raiz criado.\n");
    }

    char home_path[MAX_PATH];
    for (int i = 0; i <= 5; i++) {
        snprintf(home_path, sizeof(home_path), "%s/A%d", ROOT_DIR, i);
        mkdir(home_path, 0700);
    }

    printf(">>> SFSS rodando na porta %d\n", PORTA_SERVIDOR);

    while (1) {
        socklen_t len = sizeof(cliaddr);
        
        // impede que o servidor fique esperando encher o buffer se o pacote fragmentar
        ssize_t n = recvfrom(sockfd, &msg_buffer, sizeof(MensagemSFP), 
                             0, (struct sockaddr *)&cliaddr, &len);
        
        if (n < 0) {
            // ignora erros temporários
            if (errno != EAGAIN && errno != EWOULDBLOCK) perror("SFSS recv error");
            continue;
        }

        // processa a mensagem
        switch (msg_buffer.tipo) {
            case REQ_READ:       TratarRead(&msg_buffer); break;
            case REQ_WRITE:      TratarWrite(&msg_buffer); break;
            case REQ_CREATE_DIR: TratarCreateDir(&msg_buffer); break;
            case REQ_REM_DIR:    TratarRemoveDir(&msg_buffer); break;
            case REQ_LIST_DIR:   TratarListDir(&msg_buffer); break;
            default: 
                printf("SFSS: Tipo de mensagem desconhecido (%d)\n", msg_buffer.tipo);
                break;
        }

        // envia resposta de volta
        sendto(sockfd, &msg_buffer, sizeof(MensagemSFP), 
               0, (const struct sockaddr *)&cliaddr, len);
    }
    return 0;
}
