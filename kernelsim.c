#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "sfp.h" 

#define N_PROCESSOS 5
#define MAXIMO 20         
#define IC_PERIOD_US 500000 
#define KEY_BASE 1234     

#define SIG_SYSCALL SIGRTMIN 

typedef enum {
    PRONTO, EXECUTANDO, BLOQUEADO, FINALIZADO
} Estado;

const char* EstadoStr(Estado e) {
    switch(e) {
        case PRONTO: return "PRONTO"; 
        case EXECUTANDO: return "EXECUTANDO";
        case BLOQUEADO: return "BLOQUEADO";
        case FINALIZADO: return "FINALIZADO";
        default: return "???";
    }
}

// --- NOVO: Função para imprimir nome da syscall ---
const char* NomeSyscall(int tipo) {
    switch(tipo) {
        case REQ_READ: return "READ";
        case REQ_WRITE: return "WRITE";
        case REQ_CREATE_DIR: return "CREATE_DIR";
        case REQ_REM_DIR: return "REM_DIR";
        case REQ_LIST_DIR: return "LIST_DIR";
        default: return "UNKNOWN";
    }
}

typedef struct {
    pid_t pid;
    int pc; 
    Estado estado;
    int shmid;            
    MensagemSFP *shm_addr; 
    int acesso_D1; 
    int acesso_D2; 
} Processo;

Processo processos[N_PROCESSOS];

// --- GLOBAIS ---
volatile sig_atomic_t flag_irq0 = 0, flag_irq1 = 0, flag_irq2 = 0;
volatile sig_atomic_t flag_syscall = 0, flag_ctrlc = 0, flag_filho_terminou = 0;
int idx_executando = -1;

// --- REDE ---
int sockfd;
struct sockaddr_in servaddr;

// --- FILAS ---
MensagemSFP fila_resp_arq[20];
int head_fa=0, tail_fa=0, len_fa=0;
MensagemSFP fila_resp_dir[20];
int head_fd=0, tail_fd=0, len_fd=0;

int ready_queue[N_PROCESSOS];
int rq_head = 0, rq_tail = 0, rq_len = 0;

// --- FUNÇÕES DE FILA ---
void EnfileirarReady(int idx) {
    if (rq_len < N_PROCESSOS) {
        ready_queue[rq_tail] = idx;
        rq_tail = (rq_tail + 1) % N_PROCESSOS;
        rq_len++;
    }
}
int DesenfileirarReady() {
    if (rq_len == 0) return -1;
    int idx = ready_queue[rq_head];
    rq_head = (rq_head + 1) % N_PROCESSOS;
    rq_len--;
    return idx;
}
void EnfileirarRespArquivo(MensagemSFP msg) {
    if (len_fa < 20) {
        fila_resp_arq[tail_fa] = msg;
        tail_fa = (tail_fa + 1) % 20;
        len_fa++;
    }
}
int DesenfileirarRespArquivo(MensagemSFP *msg_dest) {
    if (len_fa == 0) return 0; 
    *msg_dest = fila_resp_arq[head_fa];
    head_fa = (head_fa + 1) % 20;
    len_fa--;
    return 1; 
}
void EnfileirarRespDir(MensagemSFP msg) {
    if (len_fd < 20) {
        fila_resp_dir[tail_fd] = msg;
        tail_fd = (tail_fd + 1) % 20;
        len_fd++;
    }
}
int DesenfileirarRespDir(MensagemSFP *msg_dest) {
    if (len_fd == 0) return 0;
    *msg_dest = fila_resp_dir[head_fd];
    head_fd = (head_fd + 1) % 20;
    len_fd--;
    return 1;
}

// ============================================================================
// SETUP REDE 
// ============================================================================
void SetupRede() {
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) { 
        perror("Socket creation failed"); 
        exit(1); 
    }

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in me;
    memset(&me, 0, sizeof(me));
    me.sin_family = AF_INET;
    me.sin_port = htons(0); 
    me.sin_addr.s_addr = htonl(INADDR_ANY); 

    if (bind(sockfd, (struct sockaddr*)&me, sizeof(me)) < 0) {
        perror("Bind falhou");
        exit(1);
    }

    socklen_t len = sizeof(me);
    if (getsockname(sockfd, (struct sockaddr *)&me, &len) == 0) {
        printf(">>> [REDE] Kernel escutando na porta UDP %d\n", ntohs(me.sin_port));
    }

    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
    
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(PORTA_SERVIDOR); 
    inet_aton("127.0.0.1", &servaddr.sin_addr);
}

void IniciarProcesso(int idx) {
    if (idx < 0 || idx >= N_PROCESSOS) return;
    Processo *proc = &processos[idx];
    if (proc->estado != PRONTO) return;
    kill(proc->pid, SIGCONT);
    proc->estado = EXECUTANDO;
    idx_executando = idx;
}

void PararExecutando() {
    if (idx_executando >= 0) {
        Processo *proc = &processos[idx_executando];
        if (proc->estado == EXECUTANDO) {
            kill(proc->pid, SIGSTOP);
            proc->estado = PRONTO;
            EnfileirarReady(idx_executando);
        }
        idx_executando = -1;
    }
}

// HANDLERS
void HandlerIRQ0(int sig) { (void)sig; flag_irq0 = 1; }
void HandlerIRQ1(int sig) { (void)sig; flag_irq1 = 1; } 
void HandlerIRQ2(int sig) { (void)sig; flag_irq2 = 1; } 
void HandlerSyscall(int sig) { (void)sig; flag_syscall = 1; } 
void HandlerCtrlC(int sig) { (void)sig; flag_ctrlc = 1; }
void HandlerFilhoTerminado(int sig) { (void)sig; flag_filho_terminou = 1; }

void ImprimirEstado() {
    printf("\n******* Estado Atual *******\n");
    for (int i = 0; i < N_PROCESSOS; i++) {
        Processo *p = &processos[i];
        if (p->estado == BLOQUEADO) {
            char op = '?';
            int dev = 0;
            int tipo_req = p->shm_addr->read_req.tipo; 
            
            if (tipo_req == REQ_READ) { op = 'R'; dev = 1; }
            else if (tipo_req == REQ_WRITE) { op = 'W'; dev = 1; }
            else if (tipo_req == REQ_CREATE_DIR) { op = 'C'; dev = 2; }
            else if (tipo_req == REQ_REM_DIR) { op = 'D'; dev = 2; }
            else if (tipo_req == REQ_LIST_DIR) { op = 'L'; dev = 2; }

            printf("A%d(pid=%d): PC=%d - ESTADO=%s - (espera D%d op=%c)\n",
                   i + 1, p->pid, p->pc, EstadoStr(p->estado), dev, op);
        } else {
            printf("A%d(pid=%d): PC=%d - ESTADO=%s\n",
                   i + 1, p->pid, p->pc, EstadoStr(p->estado));
        }
    }
    printf("Filas (Respostas): Arq=%d Dir=%d\n", len_fa, len_fd);
    printf("******************************\n\n");
}

void ExecutarAplicacao(int id_processo) { 
    int shmid = shmget(KEY_BASE + id_processo, sizeof(MensagemSFP), 0666);
    if (shmid < 0) { perror("App shmget"); exit(1); }
    MensagemSFP *shm = (MensagemSFP *)shmat(shmid, NULL, 0);
    srand(time(NULL) ^ (getpid() << 8));
    
    int pc = 0;
    int owner_id = id_processo + 1; 
    char my_home[20];
    sprintf(my_home, "/A%d", owner_id);
    int offsets_possiveis[] = {0, 16, 32, 48, 64, 80, 96};

    printf("   [APP A%d] Iniciado (PID %d). Home: %s\n", owner_id, getpid(), my_home);

    while (pc < MAXIMO) {
        usleep(500000); 
        int d = rand();
        if ((d % 100) < 15) { 
            memset(shm, 0, sizeof(MensagemSFP));
            int offset_escolhido = offsets_possiveis[rand() % 7];

            if (d % 2 != 0) { // Arquivo
                if ((rand() % 2) == 0) { 
                    shm->read_req.tipo = REQ_READ;
                    shm->read_req.owner = owner_id; 
                    sprintf(shm->read_req.path, "%s/dados.txt", my_home);
                    shm->read_req.len_path = strlen(shm->read_req.path);
                    shm->read_req.offset = offset_escolhido;
                } else { 
                    shm->write_req.tipo = REQ_WRITE;
                    shm->write_req.owner = owner_id; 
                    sprintf(shm->write_req.path, "%s/dados.txt", my_home);
                    shm->write_req.len_path = strlen(shm->write_req.path);
                    shm->write_req.offset = offset_escolhido;
                    sprintf(shm->write_req.payload, "D-A%d-PC%02d", owner_id, pc);
                }
            } else { // Diretório
                int sub_op = rand() % 3;
                if (sub_op == 0) { 
                    shm->create_dir_req.tipo = REQ_CREATE_DIR;
                    shm->create_dir_req.owner = owner_id; 
                    strcpy(shm->create_dir_req.path, my_home);
                    sprintf(shm->create_dir_req.dirname, "sub_%d", rand()%50);
                    shm->create_dir_req.len_dirname = strlen(shm->create_dir_req.dirname);
                } else if (sub_op == 1) { 
                    shm->rem_dir_req.tipo = REQ_REM_DIR;
                    shm->rem_dir_req.owner = owner_id; 
                    strcpy(shm->rem_dir_req.path, my_home);
                    sprintf(shm->rem_dir_req.dirname, "sub_%d", rand()%50);
                    shm->rem_dir_req.len_dirname = strlen(shm->rem_dir_req.dirname);
                } else { 
                    shm->list_dir_req.tipo = REQ_LIST_DIR;
                    shm->list_dir_req.owner = owner_id; 
                    strcpy(shm->list_dir_req.path, my_home);
                }
            }
            kill(getppid(), SIG_SYSCALL);
            pause(); 
        }
        usleep(500000); 
        pc++;
    }
    shmdt(shm);
    exit(0);
}

void CriarInterController(pid_t pid_kernel) {
    if (fork() == 0) {
        srand(time(NULL));
        while(1) {
            usleep(IC_PERIOD_US); 
            kill(pid_kernel, SIGALRM);
            if ((rand() % 100) < 40) kill(pid_kernel, SIGUSR1);
            if ((rand() % 100) < 20) kill(pid_kernel, SIGUSR2);
        }
        exit(0);
    }
}

// MAIN
int main() {
    pid_t pid_kernel = getpid();
    SetupRede();

    sigset_t mask_timer;
    sigemptyset(&mask_timer);
    sigaddset(&mask_timer, SIGALRM);

    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sa.sa_handler = HandlerIRQ0; sigaction(SIGALRM, &sa, NULL);
    sa.sa_handler = HandlerIRQ1; sigaction(SIGUSR1, &sa, NULL); 
    sa.sa_handler = HandlerIRQ2; sigaction(SIGUSR2, &sa, NULL); 
    sa.sa_handler = HandlerSyscall; sigaction(SIG_SYSCALL, &sa, NULL); 
    sa.sa_handler = HandlerCtrlC; sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = HandlerFilhoTerminado; sigaction(SIGCHLD, &sa, NULL);

    printf(">>> KERNEL: Criando %d processos...\n", N_PROCESSOS);
    for(int i=0; i<N_PROCESSOS; i++) {
        int shmid = shmget(KEY_BASE + i, sizeof(MensagemSFP), IPC_CREAT | 0666);
        if (shmid < 0) { perror("shmget"); exit(1); }
        
        processos[i].shmid = shmid;
        processos[i].shm_addr = (MensagemSFP *)shmat(shmid, NULL, 0);
        memset(processos[i].shm_addr, 0, sizeof(MensagemSFP));

        pid_t pid = fork();
        if (pid == 0) {
            ExecutarAplicacao(i); 
            exit(0); 
        } else {
            kill(pid, SIGSTOP); 
            processos[i].pid = pid;
            processos[i].estado = PRONTO;
            processos[i].pc = 0;
            EnfileirarReady(i);
            printf("   + Processo A%d criado (PID %d) e PAUSADO.\n", i+1, pid);
        }
    }

    CriarInterController(pid_kernel);
    printf(">>> KERNEL STARTADO (V5.2 - Syscall Name Print).\n");

    int p = DesenfileirarReady();
    if (p >= 0) IniciarProcesso(p);

    ImprimirEstado();

    while(1) {
        pause(); 

        MensagemSFP msg_rede;
        ssize_t bytes_rec;
        while ((bytes_rec = recvfrom(sockfd, &msg_rede, sizeof(msg_rede), 0, NULL, NULL)) > 0) {
            int eh_arquivo = (msg_rede.tipo == REP_READ || msg_rede.tipo == REP_WRITE);
            int owner = msg_rede.read_rep.owner;
            printf(">>> [REDE] Resposta recebida! Bytes: %ld | Tipo: %d | Owner: A%d\n", bytes_rec, msg_rede.tipo, owner);

            if (eh_arquivo) {
                EnfileirarRespArquivo(msg_rede);
            } else {
                EnfileirarRespDir(msg_rede);
            }
        }
        
        if (flag_syscall) {
            sigprocmask(SIG_BLOCK, &mask_timer, NULL);
            flag_syscall = 0;
            if (idx_executando >= 0) {
                Processo *proc = &processos[idx_executando];
                
                // --- CORREÇÃO: Print com nome da syscall ---
                printf("[KERNEL] A%d fez syscall (%s). Enviando REQ ao SFSS...\n", 
                       idx_executando+1, NomeSyscall(proc->shm_addr->tipo));

                if (sendto(sockfd, proc->shm_addr, sizeof(MensagemSFP), 0, 
                           (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
                    perror("Kernel sendto error");
                } 

                proc->estado = BLOQUEADO;
                printf("[KERNEL] A%d BLOQUEADO (aguardando I/O)\n", idx_executando+1);
                kill(proc->pid, SIGSTOP); 
                
                if (proc->shm_addr->tipo == REQ_READ || proc->shm_addr->tipo == REQ_WRITE)
                    proc->acesso_D1++;
                else
                    proc->acesso_D2++;

                idx_executando = -1;
                int prox = DesenfileirarReady();
                if (prox >= 0) IniciarProcesso(prox);
            }
            sigprocmask(SIG_UNBLOCK, &mask_timer, NULL);
        }

        if (flag_irq1) {
            sigprocmask(SIG_BLOCK, &mask_timer, NULL);
            flag_irq1 = 0;
            MensagemSFP resposta;
            if (len_fa > 0) {
                DesenfileirarRespArquivo(&resposta);
                int id_dono = resposta.read_rep.owner - 1; 
                if (id_dono >= 0 && id_dono < N_PROCESSOS && processos[id_dono].estado == BLOQUEADO) {
                     memcpy(processos[id_dono].shm_addr, &resposta, sizeof(MensagemSFP));
                     processos[id_dono].estado = PRONTO;
                     EnfileirarReady(id_dono);
                     printf("[IRQ1-ARQ] A%d DESBLOQUEADO!\n", id_dono+1);
                }
            }
            sigprocmask(SIG_UNBLOCK, &mask_timer, NULL);
        }

        if (flag_irq2) {
            sigprocmask(SIG_BLOCK, &mask_timer, NULL);
            flag_irq2 = 0;
            MensagemSFP resposta;
            if (len_fd > 0) {
                DesenfileirarRespDir(&resposta);
                int id_dono = resposta.create_dir_rep.owner - 1; 
                if (id_dono >= 0 && id_dono < N_PROCESSOS && processos[id_dono].estado == BLOQUEADO) {
                    memcpy(processos[id_dono].shm_addr, &resposta, sizeof(MensagemSFP));
                    processos[id_dono].estado = PRONTO;
                    EnfileirarReady(id_dono);
                    printf("[IRQ2-DIR] A%d DESBLOQUEADO!\n", id_dono+1);
                }
            }
            sigprocmask(SIG_UNBLOCK, &mask_timer, NULL);
        }

        if (flag_irq0) {
            flag_irq0 = 0;
            if (idx_executando >= 0 && processos[idx_executando].estado == EXECUTANDO) {
                processos[idx_executando].pc++;
                printf(">>> [CPU] A%d PC=%d\n", idx_executando + 1, processos[idx_executando].pc);

                if (processos[idx_executando].pc >= MAXIMO) {
                    processos[idx_executando].estado = FINALIZADO;
                    printf(">>> [KERNEL] A%d FINALIZADO.\n", idx_executando + 1);
                    kill(processos[idx_executando].pid, SIGKILL); 
                    idx_executando = -1; 
                } else {
                    PararExecutando(); 
                }
            }
            int prox = DesenfileirarReady();
            if (prox >= 0) IniciarProcesso(prox);
        }

        if (flag_ctrlc) {
            flag_ctrlc = 0;
            ImprimirEstado();
        }

        if (flag_filho_terminou) {
            flag_filho_terminou = 0;
            while(waitpid(-1, NULL, WNOHANG) > 0);
        }

        int contagem_finalizados = 0;
        for (int i = 0; i < N_PROCESSOS; i++) {
            if (processos[i].estado == FINALIZADO) {
                contagem_finalizados++;
            }
        }

        if (contagem_finalizados == N_PROCESSOS) {
            printf("\n******* FIM DA SIMULAÇÃO (Todos os processos terminaram) *******\n");
            for(int i=0; i<N_PROCESSOS; i++) shmctl(processos[i].shmid, IPC_RMID, NULL);
            system("pkill -P $(ps -o pid= --ppid $$) >/dev/null 2>&1");
            exit(0);
        }
    }
    return 0;
}
