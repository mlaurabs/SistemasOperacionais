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

#include "sfp.h" // Nosso contrato

#define ROOT_DIR "SFSS-root-dir"

// Função auxiliar para montar o caminho real no disco
// Ex: Recebe "/A1/teste.txt" -> Transforma em "SFSS-root-dir/A1/teste.txt"
void ConstruirCaminho(char *buffer, const char *caminho_req) {
    snprintf(buffer, sizeof(buffer), "%s%s", ROOT_DIR, caminho_req);
}

// ---------------------------------------------------------------------------
// 1. TRATAR LEITURA (READ)
// ---------------------------------------------------------------------------
void TratarRead(MensagemSFP *msg) {
    char caminho_real[MAX_PATH + 20];
    ConstruirCaminho(caminho_real, msg->read_req.path);

    FILE *f = fopen(caminho_real, "rb");
    if (!f) {
        // Erro: Arquivo não existe ou sem permissão
        printf("SFSS: Erro ao abrir leitura '%s': %s\n", caminho_real, strerror(errno));
        msg->read_rep.tipo = REP_READ;
        msg->read_rep.offset = -1; // Código de erro conforme PDF
        return; // Retorna com erro
    }

    // Tenta ir para o offset
    if (fseek(f, msg->read_req.offset, SEEK_SET) != 0) {
        printf("SFSS: Erro seek '%s': %s\n", caminho_real, strerror(errno));
        msg->read_rep.tipo = REP_READ;
        msg->read_rep.offset = -2; // Erro de seek (fora do arquivo?)
        fclose(f);
        return;
    }

    // Lê os 16 bytes (TAM_BLOCO)
    memset(msg->read_rep.payload, 0, TAM_BLOCO); // Limpa buffer
    size_t lidos = fread(msg->read_rep.payload, 1, TAM_BLOCO, f);

    fclose(f);

    // Prepara resposta
    msg->read_rep.tipo = REP_READ;
    // O PDF diz: "na ausência de erros, offset tem valor igual ao requisitado"
    // Se leu 0 bytes (EOF), ainda não é necessariamente erro de protocolo, mas
    // se o cliente pediu além do fim, lidos será 0.
    if (lidos == 0 && msg->read_req.offset > 0) {
         // Talvez indicar EOF? Mas o PDF pede offset negativo se erro.
         // Vamos manter o offset original se conseguiu ler 0 (fim de arquivo) ou >0.
    }
    msg->read_rep.offset = msg->read_req.offset;
    
    printf("SFSS: Read OK em '%s' (Off=%d, Lidos=%ld)\n", msg->read_req.path, msg->read_req.offset, lidos);
}

// ---------------------------------------------------------------------------
// 2. TRATAR ESCRITA (WRITE)
// ---------------------------------------------------------------------------
void TratarWrite(MensagemSFP *msg) {
    char caminho_real[MAX_PATH + 20];
    ConstruirCaminho(caminho_real, msg->write_req.path);

    // Verifica se arquivo existe para saber o modo de abertura
    FILE *f = fopen(caminho_real, "r+b"); // Leitura/Escrita Binária
    if (!f) {
        // Se não existe, cria (w+b)
        f = fopen(caminho_real, "w+b");
        if (!f) {
            printf("SFSS: Erro ao criar/abrir escrita '%s': %s\n", caminho_real, strerror(errno));
            msg->write_rep.tipo = REP_WRITE;
            msg->write_rep.offset = -1; 
            return;
        }
    }

    // Lógica de "Gaps" (PDF Obs1): Se offset > tamanho atual, preencher com espaços (0x20)
    fseek(f, 0, SEEK_END);
    long tamanho_atual = ftell(f);

    if (msg->write_req.offset > tamanho_atual) {
        // Preencher o buraco com espaços
        fseek(f, tamanho_atual, SEEK_SET);
        long buraco = msg->write_req.offset - tamanho_atual;
        char *espacos = malloc(buraco);
        memset(espacos, 0x20, buraco); // 0x20 = espaço
        fwrite(espacos, 1, buraco, f);
        free(espacos);
    }

    // Posiciona e escreve o Payload
    fseek(f, msg->write_req.offset, SEEK_SET);
    fwrite(msg->write_req.payload, 1, TAM_BLOCO, f);
    fclose(f);

    printf("SFSS: Write OK em '%s' (Off=%d)\n", msg->write_req.path, msg->write_req.offset);

    // Prepara resposta
    msg->write_rep.tipo = REP_WRITE;
    msg->write_rep.offset = msg->write_req.offset; // Sucesso
}

// ---------------------------------------------------------------------------
// 3. TRATAR CRIAR DIRETÓRIO (DC)
// ---------------------------------------------------------------------------
void TratarCreateDir(MensagemSFP *msg) {
    char caminho_real[MAX_PATH * 2];
    char caminho_novo[MAX_PATH * 2];

    ConstruirCaminho(caminho_real, msg->create_dir_req.path);
    
    // Monta o path completo do novo diretório
    snprintf(caminho_novo, sizeof(caminho_novo),"%s/%s", caminho_real, msg->create_dir_req.dirname);

    printf("SFSS: Criando Dir '%s'\n", caminho_novo);

    // Permissões 0700 (rwx------)
    if (mkdir(caminho_novo, 0700) == 0) {
        // Sucesso
        msg->create_dir_rep.tipo = REP_CREATE_DIR;
        
        // Retorna o novo path concatenado (conforme PDF)
        snprintf(msg->create_dir_rep.path,sizeof(msg->create_dir_rep.path),"%s/%s",msg->create_dir_req.path, msg->create_dir_req.dirname);
        msg->create_dir_rep.len_path = strlen(msg->create_dir_rep.path);
    } else {
        // Erro
        perror("SFSS mkdir error");
        msg->create_dir_rep.tipo = REP_CREATE_DIR;
        msg->create_dir_rep.len_path = -1; // Código de erro
    }
}

// ---------------------------------------------------------------------------
// 4. TRATAR REMOVER (DR)
// ---------------------------------------------------------------------------
void TratarRemoveDir(MensagemSFP *msg) {
    char caminho_real[MAX_PATH * 2];
    ConstruirCaminho(caminho_real, msg->rem_dir_req.path);

    char alvo[MAX_PATH * 2];
    snprintf(alvo, sizeof(alvo), "%s/%s", caminho_real, msg->rem_dir_req.dirname);

    printf("SFSS: Removendo '%s'\n", alvo);

    // remove() do C apaga arquivos e diretórios (se vazios)
    if (remove(alvo) == 0) {
        msg->rem_dir_rep.tipo = REP_REM_DIR;
        // Retorna o path "pai" limpo
        strcpy(msg->rem_dir_rep.path, msg->rem_dir_req.path);
        msg->rem_dir_rep.len_path = strlen(msg->rem_dir_rep.path);
    } else {
        perror("SFSS remove error");
        msg->rem_dir_rep.tipo = REP_REM_DIR;
        msg->rem_dir_rep.len_path = -1;
    }
}

// ---------------------------------------------------------------------------
// 5. TRATAR LISTAR DIRETÓRIO (DL)
// ---------------------------------------------------------------------------
void TratarListDir(MensagemSFP *msg) {
    char caminho_real[MAX_PATH + 20];
    ConstruirCaminho(caminho_real, msg->list_dir_req.path);

    DIR *d;
    struct dirent *dir;
    d = opendir(caminho_real);

    msg->list_dir_rep.tipo = REP_LIST_DIR;
    msg->list_dir_rep.nrnames = 0;
    msg->list_dir_rep.allfilenames[0] = '\0'; // Inicia string vazia

    if (d) {
        int char_count = 0;
        int idx = 0;

        while ((dir = readdir(d)) != NULL && idx < MAX_DIR_ENTRIES) {
            // Ignora "." e ".." para limpar a saída (opcional, mas bom)
            if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0)
                continue;

            int len_nome = strlen(dir->d_name);

            // Verifica se cabe no buffer allfilenames
            if (char_count + len_nome + 1 >= TAM_LIST_BUFFER) break;

            // Copia nome para o bufferzão
            strcat(msg->list_dir_rep.allfilenames, dir->d_name);
            
            // Preenche a struct de posições
            msg->list_dir_rep.fstlstpositions[idx].inicio = char_count;
            msg->list_dir_rep.fstlstpositions[idx].fim = char_count + len_nome;
            
            // Tenta adivinhar se é arquivo ou diretório
            // (DT_DIR nem sempre funciona em todos sistemas de arquivos, mas no linux lab costuma funcionar)
            if (dir->d_type == DT_DIR) 
                msg->list_dir_rep.fstlstpositions[idx].eh_arquivo = 0;
            else 
                msg->list_dir_rep.fstlstpositions[idx].eh_arquivo = 1;

            char_count += len_nome;
            idx++;
        }
        msg->list_dir_rep.nrnames = idx;
        closedir(d);
        printf("SFSS: ListDir OK em '%s' -> %d itens encontrados\n", msg->list_dir_req.path, idx);
    } else {
        printf("SFSS: Erro opendir '%s': %s\n", caminho_real, strerror(errno));
        msg->list_dir_rep.nrnames = -1; // Erro
    }
}

// ---------------------------------------------------------------------------
// MAIN
// ---------------------------------------------------------------------------
int main() {
    int sockfd;
    struct sockaddr_in servaddr, cliaddr;
    MensagemSFP msg_buffer;
    
    // 1. Cria socket UDP
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    memset(&servaddr, 0, sizeof(servaddr));
    memset(&cliaddr, 0, sizeof(cliaddr));

    // 2. Configura Endereço do Servidor
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(PORTA_SERVIDOR);

    // 3. Bind
    if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    // 4. Cria diretório raiz se não existir
    struct stat st = {0};
    if (stat(ROOT_DIR, &st) == -1) {
        if(mkdir(ROOT_DIR, 0700) == 0){
            printf(">>> Diretório raiz '%s' criado com sucesso.\n", ROOT_DIR);
        } else {
            perror(">>> Erro ao criar diretório raiz");
            exit(EXIT_FAILURE);
        }
    }

    printf(">>> SFSS rodando na porta %d...\n", PORTA_SERVIDOR);
    printf(">>> Aguardando requisições...\n");

    while (1) {
        socklen_t len = sizeof(cliaddr);
        
        // 5. Recebe Mensagem (Bloqueante)
        ssize_t n = recvfrom(sockfd, &msg_buffer, sizeof(MensagemSFP), 
                             MSG_WAITALL, (struct sockaddr *)&cliaddr, &len);
        
        if (n < 0) {
            perror("recvfrom error");
            continue;
        }

        // 6. Processa a Mensagem (Dispatch)
        // O tipo está no início da Union, então podemos acessar msg_buffer.tipo
        // ou msg_buffer.read_req.tipo (são o mesmo espaço de memória)
        switch (msg_buffer.tipo) {
            case REQ_READ:
                TratarRead(&msg_buffer);
                break;
            case REQ_WRITE:
                TratarWrite(&msg_buffer);
                break;
            case REQ_CREATE_DIR:
                TratarCreateDir(&msg_buffer);
                break;
            case REQ_REM_DIR:
                TratarRemoveDir(&msg_buffer);
                break;
            case REQ_LIST_DIR:
                TratarListDir(&msg_buffer);
                break;
            default:
                printf("SFSS: Tipo de mensagem desconhecido: %d\n", msg_buffer.tipo);
                continue; // Ignora
        }

        // 7. Envia Resposta de volta para quem mandou
        sendto(sockfd, &msg_buffer, sizeof(MensagemSFP), 
               MSG_CONFIRM, (const struct sockaddr *)&cliaddr, len);
    }

    return 0;
}