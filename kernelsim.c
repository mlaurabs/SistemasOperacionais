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

// --- CORES ---
#define K_RESET  "\x1B[0m"
#define K_RED    "\x1B[31m"
#define K_GREEN  "\x1B[32m"
#define K_YELLOW "\x1B[33m"
#define K_BLUE   "\x1B[34m"
#define K_MAGENTA "\x1B[35m"
#define K_CYAN   "\x1B[36m"
#define K_WHITE  "\x1B[37m"

#define N_PROCESSOS 5
#define MAXIMO 20         
#define TEMPO_FATIA 1
#define IC_PERIOD_US 500000 
#define KEY_BASE 1234     

#define SIG_SYSCALL SIGRTMIN 

typedef enum {
    PRONTO, EXECUTANDO, BLOQUEADO, FINALIZADO
} Estado;

const char* EstadoStr(Estado e) {
    switch(e) {
        case PRONTO: return "READY";
        case EXECUTANDO: return "EXEC";
        case BLOQUEADO: return "BLOCK";
        case FINALIZADO: return "EXIT";
        default: return "???";
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

// SETUP REDE
void SetupRede() {
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) { perror("Socket"); exit(1); }
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
    printf(K_GREEN ">>> [SCHED] Dispatch A%d (PID %d)" K_RESET "\n", idx+1, proc->pid);
    kill(proc->pid, SIGCONT);
    proc->estado = EXECUTANDO;
    idx_executando = idx;
}

void PararExecutando() {
    if (idx_executando >= 0) {
        Processo *proc = &processos[idx_executando];
        if (proc->estado == EXECUTANDO) {
            printf(K_YELLOW ">>> [SCHED] Preempt A%d" K_RESET "\n", idx_executando+1);
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

// ============================================================================
// APLICAÇÃO (FILHO)
// ============================================================================
void ExecutarAplicacao(int id_processo) { 
    int shmid = shmget(KEY_BASE + id_processo, sizeof(MensagemSFP), 0666);
    if (shmid < 0) { perror("App shmget"); exit(1); }
    
    MensagemSFP *shm = (MensagemSFP *)shmat(shmid, NULL, 0);
    
    srand(time(NULL) ^ (getpid() << 8));
    int pc = 0;
    int owner_id = id_processo + 1; 
    char my_home[20];
    sprintf(my_home, "/A%d", owner_id);

    printf("   [APP A%d] Iniciado (PID %d). Home: %s\n", owner_id, getpid(), my_home);

    while (pc < MAXIMO) {
        usleep(500000); 

        if ((rand() % 100) < 20) {
            int tipo_op = rand() % 5; 
            memset(shm, 0, sizeof(MensagemSFP));
            shm->read_req.owner = owner_id; 

            if (tipo_op == 0) { 
                shm->read_req.tipo = REQ_READ;
                sprintf(shm->read_req.path, "%s/dados.txt", my_home);
                shm->read_req.len_path = strlen(shm->read_req.path);
                shm->read_req.offset = (rand() % 4) * 16;
                printf(K_CYAN "   [A%d] SYSCALL READ %s off=%d" K_RESET "\n", owner_id, shm->read_req.path, shm->read_req.offset);
            } 
            else if (tipo_op == 1) { 
                shm->write_req.tipo = REQ_WRITE;
                sprintf(shm->write_req.path, "%s/dados.txt", my_home);
                shm->write_req.len_path = strlen(shm->write_req.path);
                shm->write_req.offset = (rand() % 4) * 16;
                sprintf(shm->write_req.payload, "D-A%d-%02d", owner_id, rand()%99);
                printf(K_CYAN "   [A%d] SYSCALL WRITE %s" K_RESET "\n", owner_id, shm->write_req.path);
            }
            else if (tipo_op == 2) { 
                shm->create_dir_req.tipo = REQ_CREATE_DIR;
                strcpy(shm->create_dir_req.path, my_home);
                sprintf(shm->create_dir_req.dirname, "sub_%d", rand()%100);
                shm->create_dir_req.len_dirname = strlen(shm->create_dir_req.dirname);
                printf(K_CYAN "   [A%d] SYSCALL MKDIR %s/%s" K_RESET "\n", owner_id, my_home, shm->create_dir_req.dirname);
            }
            else if (tipo_op == 3) { 
                shm->rem_dir_req.tipo = REQ_REM_DIR;
                strcpy(shm->rem_dir_req.path, my_home);
                sprintf(shm->rem_dir_req.dirname, "sub_%d", rand()%100);
                shm->rem_dir_req.len_dirname = strlen(shm->rem_dir_req.dirname);
                printf(K_CYAN "   [A%d] SYSCALL RMDIR %s/%s" K_RESET "\n", owner_id, my_home, shm->rem_dir_req.dirname);
            }
            else { 
                shm->list_dir_req.tipo = REQ_LIST_DIR;
                strcpy(shm->list_dir_req.path, my_home);
                printf(K_CYAN "   [A%d] SYSCALL LIST %s" K_RESET "\n", owner_id, my_home);
            }

            kill(getppid(), SIG_SYSCALL);
            pause(); 
            
            // Verificação de Erro/Sucesso
            int status = 0; 
            if (shm->tipo == REP_READ || shm->tipo == REP_WRITE) {
                if (shm->read_rep.offset < 0) status = -1;
            } else if (shm->tipo == REP_CREATE_DIR || shm->tipo == REP_REM_DIR) {
                if (shm->create_dir_rep.len_path < 0) status = -1;
            } else if (shm->tipo == REP_LIST_DIR) {
                if (shm->list_dir_rep.nrnames < 0) status = -1;
            }

            if (status == 0) printf(K_GREEN "   [A%d] Retornou: SUCESSO" K_RESET "\n", owner_id);
            else printf(K_RED "   [A%d] Retornou: ERRO (Cod negativo)" K_RESET "\n", owner_id);
        }
        pc++;
    }
    shmdt(shm);
    exit(0);
}

// INTERCONTROLLER
void CriarInterController(pid_t pid_kernel) {
    if (fork() == 0) {
        srand(time(NULL));
        while(1) {
            usleep(IC_PERIOD_US); 
            kill(pid_kernel, SIGALRM);
            if ((rand() % 100) < 10) kill(pid_kernel, SIGUSR1);
            if ((rand() % 100) < 2) kill(pid_kernel, SIGUSR2);
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

    printf(K_WHITE ">>> KERNEL: Criando %d processos..." K_RESET "\n", N_PROCESSOS);
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
    printf(">>> KERNEL STARTADO (V3.1 - Logs Ativados).\n");

    int p = DesenfileirarReady();
    if (p >= 0) IniciarProcesso(p);

    while(1) {
        pause(); 

        MensagemSFP msg_rede;
        while (recvfrom(sockfd, &msg_rede, sizeof(msg_rede), 0, NULL, NULL) > 0) {
            int eh_arquivo = (msg_rede.tipo == REP_READ || msg_rede.tipo == REP_WRITE);
            
            // LOG REATIVADO: Para debug
            int owner = msg_rede.read_rep.owner;
            if (eh_arquivo) {
                EnfileirarRespArquivo(msg_rede);
                printf(K_MAGENTA "[NET] <<< Recebido SFSS (Arq) para A%d. Pendentes=%d" K_RESET "\n", owner, len_fa);
            } else {
                EnfileirarRespDir(msg_rede);
                printf(K_MAGENTA "[NET] <<< Recebido SFSS (Dir) para A%d. Pendentes=%d" K_RESET "\n", owner, len_fd);
            }
        }

        if (flag_syscall) {
            sigprocmask(SIG_BLOCK, &mask_timer, NULL);
            flag_syscall = 0;
            if (idx_executando >= 0) {
                Processo *proc = &processos[idx_executando];
                printf(K_BLUE "[KERNEL] Syscall A%d -> Send SFSS..." K_RESET "\n", idx_executando+1);

                if (sendto(sockfd, proc->shm_addr, sizeof(MensagemSFP), 0, 
                           (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
                    perror("Kernel sendto");
                } 

                proc->estado = BLOQUEADO;
                printf(K_RED "[KERNEL] Bloqueando A%d (Aguardando I/O)" K_RESET "\n", idx_executando+1);
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
                
                printf(K_YELLOW "[IRQ1] Interrupção de Disco! Verificando A%d..." K_RESET "\n", id_dono+1);
                
                if (id_dono >= 0 && id_dono < N_PROCESSOS && processos[id_dono].estado == BLOQUEADO) {
                     memcpy(processos[id_dono].shm_addr, &resposta, sizeof(MensagemSFP));
                     processos[id_dono].estado = PRONTO;
                     EnfileirarReady(id_dono);
                     printf(K_GREEN "[KERNEL] A%d Desbloqueado -> Ready Queue" K_RESET "\n", id_dono+1);
                } else {
                     printf(K_RED "[KERNEL] ERRO/IGNORE: A%d não estava BLOQUEADO." K_RESET "\n", id_dono+1);
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
                
                printf(K_YELLOW "[IRQ2] Interrupção de Dir! Verificando A%d..." K_RESET "\n", id_dono+1);

                if (id_dono >= 0 && id_dono < N_PROCESSOS && processos[id_dono].estado == BLOQUEADO) {
                    memcpy(processos[id_dono].shm_addr, &resposta, sizeof(MensagemSFP));
                    processos[id_dono].estado = PRONTO;
                    EnfileirarReady(id_dono);
                    printf(K_GREEN "[KERNEL] A%d Desbloqueado -> Ready Queue" K_RESET "\n", id_dono+1);
                } else {
                     printf(K_RED "[KERNEL] ERRO/IGNORE: A%d não estava BLOQUEADO." K_RESET "\n", id_dono+1);
                }
            }
            sigprocmask(SIG_UNBLOCK, &mask_timer, NULL);
        }

        if (flag_irq0) {
            flag_irq0 = 0;
            if (idx_executando >= 0 && processos[idx_executando].estado == EXECUTANDO) {
                processos[idx_executando].pc++;
                PararExecutando(); 
            }
            int prox = DesenfileirarReady();
            if (prox >= 0) IniciarProcesso(prox);
        }

        if (flag_ctrlc) {
            flag_ctrlc = 0;
            printf("\n--- STATUS KERNEL (FINALIZANDO) ---\n");
            for(int i=0; i<N_PROCESSOS; i++) {
                printf("A%d: PC=%d Estado=%s\n", i+1, processos[i].pc, EstadoStr(processos[i].estado));
                shmctl(processos[i].shmid, IPC_RMID, NULL);
                kill(processos[i].pid, SIGKILL);
            }
            exit(0);
        }

        if (flag_filho_terminou) {
            flag_filho_terminou = 0;
            while(waitpid(-1, NULL, WNOHANG) > 0);
        }
    }
}
