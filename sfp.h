#ifndef SFP_H
#define SFP_H

// Bibliotecas necessárias para os tipos (int, char, etc)
#include <sys/types.h>

// --- Configurações do Protocolo ---
#define PORTA_SERVIDOR 9876
#define TAM_BLOCO 16             // [cite: 116] Leitura/Escrita sempre em blocos de 16 bytes
#define MAX_PATH 256             // Tamanho seguro para nomes de arquivos
#define MAX_DIR_ENTRIES 40       // [cite: 124] No máximo 40 nomes
#define TAM_LIST_BUFFER 2048     // Espaço para os nomes do listDir

// Códigos de Operação (para o switch/case)
typedef enum {
    REQ_READ, 
    REQ_WRITE, 
    REQ_CREATE_DIR, 
    REQ_REM_DIR, 
    REQ_LIST_DIR,
    
    REP_READ, 
    REP_WRITE, 
    REP_CREATE_DIR, 
    REP_REM_DIR, 
    REP_LIST_DIR
} TipoMsg;

// Estrutura auxiliar para o ListDir [cite: 227]
typedef struct {
    int inicio;
    int fim;
    int eh_arquivo; // 0 = diretório, 1 = arquivo
} EntryPos;

// ============================================================================
// ESTRUTURAS DE MENSAGEM
// ============================================================================

// 1. LEITURA (READ) [cite: 205]
typedef struct {
    int tipo;               // TipoMsg
    int owner;              // ID do processo (1..5)
    char path[MAX_PATH];
    int len_path;           // strlen
    char payload[TAM_BLOCO];// Vazio no pedido
    int offset;             // Posição de leitura
} MsgReadReq;

typedef struct {
    int tipo;
    int owner;
    char path[MAX_PATH];
    int len_path;
    char payload[TAM_BLOCO]; // Dados lidos vêm aqui
    int offset;              // Retorna o valor ou NEGATIVO se erro [cite: 206]
} MsgReadRep;

// 2. ESCRITA (WRITE) [cite: 208]
typedef struct {
    int tipo;
    int owner;
    char path[MAX_PATH];
    int len_path;
    char payload[TAM_BLOCO]; // Dados a escrever
    int offset;
} MsgWriteReq;

typedef struct {
    int tipo;
    int owner;
    char path[MAX_PATH];
    int len_path;
    char payload[TAM_BLOCO]; // Vazio na resposta
    int offset;              // Retorna o valor ou NEGATIVO se erro [cite: 210]
} MsgWriteRep;

// 3. CRIAÇÃO DE DIRETÓRIO (DC - Create) [cite: 219]
typedef struct {
    int tipo;
    int owner;
    char path[MAX_PATH];
    int len_path;
    char dirname[MAX_PATH];
    int len_dirname;
} MsgCreateDirReq;

typedef struct {
    int tipo;
    int owner;
    char path[MAX_PATH];    // Novo path completo
    int len_path;           // Tamanho da nova string ou negativo se erro
} MsgCreateDirRep;

// 4. REMOÇÃO DE DIRETÓRIO (DR - Remove) [cite: 221]
typedef struct {
    int tipo;
    int owner;
    char path[MAX_PATH];
    int len_path;
    char dirname[MAX_PATH]; // Nome a remover
    int len_dirname;
} MsgRemDirReq;

typedef struct {
    int tipo;
    int owner;
    char path[MAX_PATH];    // Novo path sem o nome
    int len_path;           // Tamanho ou negativo se erro
} MsgRemDirRep;

// 5. LISTAR DIRETÓRIO (DL - List) [cite: 226]
typedef struct {
    int tipo;
    int owner;
    char path[MAX_PATH];
    int len_path;
} MsgListDirReq;

typedef struct {
    int tipo;
    int owner;
    // "retorna os nomes... em um unico char array... e fstlstpositions" [cite: 230]
    char allfilenames[TAM_LIST_BUFFER];     
    EntryPos fstlstpositions[MAX_DIR_ENTRIES]; 
    int nrnames;                            // Quantidade de itens
} MsgListDirRep;

// ============================================================================
// UNION GENÉRICA (Facilita o envio via UDP)
// ============================================================================
typedef union {
    int tipo; // Acesso rápido ao tipo da mensagem
    MsgReadReq      read_req;
    MsgReadRep      read_rep;
    MsgWriteReq     write_req;
    MsgWriteRep     write_rep;
    MsgCreateDirReq create_dir_req;
    MsgCreateDirRep create_dir_rep;
    MsgRemDirReq    rem_dir_req;
    MsgRemDirRep    rem_dir_rep;
    MsgListDirReq   list_dir_req;
    MsgListDirRep   list_dir_rep;
} MensagemSFP;

#endif // SFP_H