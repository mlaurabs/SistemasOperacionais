#ifndef SFP_H
#define SFP_H

#include <sys/types.h>

// config do protocolo
#define PORTA_SERVIDOR 9881
#define TAM_BLOCO 16             // leitura/escrita sempre em blocos de 16 bytes
#define MAX_PATH 256             
#define MAX_DIR_ENTRIES 40       
#define TAM_LIST_BUFFER 2048     

// códigos de operação 
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

// estrutura auxiliar para o ListDir
typedef struct {
    int inicio;
    int fim;
    int eh_arquivo; // 0 = diretório, 1 = arquivo
} EntryPos;


// estruturas das mensagesn

// READ
typedef struct {
    int tipo;              
    int owner;              // ID do processo (1..5)
    char path[MAX_PATH];
    int len_path;          
    char payload[TAM_BLOCO];// vazio no pedido
    int offset;             // posição de leitura
} MsgReadReq;

typedef struct {
    int tipo;
    int owner;
    char path[MAX_PATH];
    int len_path;
    char payload[TAM_BLOCO]; // dados lidos vêm aqui
    int offset;              // retorna o valor ou negativo se erro
} MsgReadRep;

// WRITE
typedef struct {
    int tipo;
    int owner;
    char path[MAX_PATH];
    int len_path;
    char payload[TAM_BLOCO]; // dados pra escrever
    int offset;
} MsgWriteReq;

typedef struct {
    int tipo;
    int owner;
    char path[MAX_PATH];
    int len_path;
    char payload[TAM_BLOCO]; // vazio na resposta
    int offset;              // retorna o valor ou negativo se erro 
} MsgWriteRep;

// CREATE DIR
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
    char path[MAX_PATH];    // novo path completo
    int len_path;           // tamanho da nova string ou negativo se erro
} MsgCreateDirRep;

// REM DIR
typedef struct {
    int tipo;
    int owner;
    char path[MAX_PATH];
    int len_path;
    char dirname[MAX_PATH]; // nome a remover
    int len_dirname;
} MsgRemDirReq;

typedef struct {
    int tipo;
    int owner;
    char path[MAX_PATH];    // novo path sem o nome
    int len_path;           // tamanho ou negativo se erro
} MsgRemDirRep;

// LIST DIR
typedef struct {
    int tipo;
    int owner;
    char path[MAX_PATH];
    int len_path;
} MsgListDirReq;

typedef struct {
    int tipo;
    int owner;
    
    char allfilenames[TAM_LIST_BUFFER];     
    EntryPos fstlstpositions[MAX_DIR_ENTRIES]; 
    int nrnames;                            // quantidade de itens
} MsgListDirRep;

// union para envio via UDP)
typedef union {
    int tipo;
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

#endif 
