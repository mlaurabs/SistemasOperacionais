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

// --- CONFIGURAÇÕES ---
#define N_PROCESSOS 5
#define MAXIMO 50         
#define TEMPO_FATIA 1
#define IC_PERIOD_US 500000 
#define KEY_BASE 1234     

// --- MAPA DE SINAIS (CORREÇÃO #2) ---
// IRQ0 = SIGALRM (Timer)
// IRQ1 = SIGUSR1 (Arquivo pronto)
// IRQ2 = SIGUSR2 (Diretório pronto)
#define SIG_SYSCALL SIGRTMIN // Syscall separada das interrupções de HW

typedef enum {
    PRONTO,
    EXECUTANDO, 
    BLOQUEADO, 
    FINALIZADO
} Estado;

typedef struct {
    pid_t pid;
    int pc; // PC agora será atualizado pelo Kernel
    Estado estado;
    
    int shmid;            
    MensagemSFP *shm_addr; 
    
    int acesso_D1; // Acessos a ARQUIVOS
    int acesso_D2; // Acessos a DIRETÓRIOS
} Processo;

Processo processos[N_PROCESSOS];

// --- GLOBAIS ---
volatile sig_atomic_t flag_irq0 = 0; 
volatile sig_atomic_t flag_irq1 = 0; 
volatile sig_atomic_t flag_irq2 = 0; 
volatile sig_atomic_t flag_syscall = 0;
volatile sig_atomic_t flag_ctrlc = 0;
volatile sig_atomic_t flag_filho_terminou = 0;

int idx_executando = -1;

// --- REDE ---
int sockfd;
struct sockaddr_in servaddr;

// --- FILAS DE RESPOSTAS ---
MensagemSFP fila_resp_arq[20];
int head_fa=0, tail_fa=0, len_fa=0;

MensagemSFP fila_resp_dir[20];
int head_fd=0, tail_fd=0, len_fd=0;

// --- FILA READY ---
int ready_queue[N_PROCESSOS];
int rq_head = 0, rq_tail = 0, rq_len = 0;

// ============================================================================
// FUNÇÕES DE FILA
// ============================================================================

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
// GERENCIAMENTO DE PROCESSOS
// ============================================================================

void SetupRede() {
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }
    
    // Non-blocking para não travar o Kernel
    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(PORTA_SERVIDOR); 
    
    // CORREÇÃO #5: Usar Loopback explícito em vez de INADDR_ANY para envio
    if (inet_aton("127.0.0.1", &servaddr.sin_addr) == 0) {
        fprintf(stderr, "Erro ao configurar IP localhost\n");
        exit(1);
    }
}

void IniciarProcesso(int idx) {
    if (idx < 0 || idx >= N_PROCESSOS) return;
    Processo *proc = &processos[idx];
    if (proc->estado != PRONTO) return;

    // CORREÇÃO #3: Enviar SIGCONT explicitamente
    kill(proc->pid, SIGCONT);
    proc->estado = EXECUTANDO;
    idx_executando = idx;
    
    // Debug visual
    // printf("Kernel: Retomando A%d (PC=%d)\n", idx+1, proc->pc);
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

// ============================================================================
// HANDLERS
// ============================================================================
void HandlerIRQ0(int sig) { (void)sig; flag_irq0 = 1; }
void HandlerIRQ1(int sig) { (void)sig; flag_irq1 = 1; } // SIGUSR1
void HandlerIRQ2(int sig) { (void)sig; flag_irq2 = 1; } // SIGUSR2
void HandlerSyscall(int sig) { (void)sig; flag_syscall = 1; } // SIGRTMIN
void HandlerCtrlC(int sig) { (void)sig; flag_ctrlc = 1; }
void HandlerFilhoTerminado(int sig) { (void)sig; flag_filho_terminou = 1; }

// ============================================================================
// CÓDIGO DA APLICAÇÃO (FILHO)
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

    while (pc < MAXIMO) {
        // Simula trabalho
        usleep(500000); // 0.5s

        // Probabilidade de Syscall (20%)
        if ((rand() % 100) < 20) {
            int tipo_op = rand() % 5; 
            
            memset(shm, 0, sizeof(MensagemSFP));
            
            if (tipo_op == 0) { // READ
                shm->read_req.tipo = REQ_READ;
                shm->read_req.owner = owner_id;
                sprintf(shm->read_req.path, "%s/dados.txt", my_home);
                shm->read_req.len_path = strlen(shm->read_req.path);
                shm->read_req.offset = (rand() % 4) * 16;
                printf("[A%d] Syscall READ %s off=%d\n", owner_id, shm->read_req.path, shm->read_req.offset);
            } 
            else if (tipo_op == 1) { // WRITE
                shm->write_req.tipo = REQ_WRITE;
                shm->write_req.owner = owner_id;
                sprintf(shm->write_req.path, "%s/dados.txt", my_home);
                shm->write_req.len_path = strlen(shm->write_req.path);
                shm->write_req.offset = (rand() % 4) * 16;
                sprintf(shm->write_req.payload, "D-A%d-%02d", owner_id, rand()%99);
                printf("[A%d] Syscall WRITE %s\n", owner_id, shm->write_req.path);
            }
            else if (tipo_op == 2) { // CREATE
                shm->create_dir_req.tipo = REQ_CREATE_DIR;
                shm->create_dir_req.owner = owner_id;
                strcpy(shm->create_dir_req.path, my_home);
                sprintf(shm->create_dir_req.dirname, "sub_%d", rand()%100);
                printf("[A%d] Syscall MKDIR %s/%s\n", owner_id, my_home, shm->create_dir_req.dirname);
            }
            else if (tipo_op == 3) { // REMOVE
                shm->rem_dir_req.tipo = REQ_REM_DIR;
                shm->rem_dir_req.owner = owner_id;
                strcpy(shm->rem_dir_req.path, my_home);
                sprintf(shm->rem_dir_req.dirname, "sub_%d", rand()%100);
                printf("[A%d] Syscall RMDIR %s/%s\n", owner_id, my_home, shm->rem_dir_req.dirname);
            }
            else { // LIST
                shm->list_dir_req.tipo = REQ_LIST_DIR;
                shm->list_dir_req.owner = owner_id;
                strcpy(shm->list_dir_req.path, my_home);
                printf("[A%d] Syscall LIST %s\n", owner_id, my_home);
            }

            // CORREÇÃO #2: Usa SIG_SYSCALL (SIGRTMIN) em vez de SIGUSR1
            kill(getppid(), SIG_SYSCALL);
            
            // CORREÇÃO #4 e #7: Pausa segura
            // O sinal SIGSTOP virá do Kernel imediatamente.
            // Quando receber SIGCONT, volta aqui.
            pause(); 
            
            // Retorno
            printf("   >>> [A%d] Acordou. Resposta tipo: %d\n", owner_id, shm->tipo);
        }
        pc++;
    }
    shmdt(shm);
    exit(0);
}

// ============================================================================
// INTERCONTROLLER
// ============================================================================
void CriarInterController(pid_t pid_kernel) {
    if (fork() == 0) {
        srand(time(NULL));
        while(1) {
            usleep(IC_PERIOD_US); // 500ms
            
            // IRQ0: TimeSlice (SIGALRM)
            kill(pid_kernel, SIGALRM);

            // IRQ1: Arquivo (SIGUSR1) - Prob 10%
            if ((rand() % 100) < 10) { 
                kill(pid_kernel, SIGUSR1);
            }

            // IRQ2: Diretório (SIGUSR2) - Prob 2%
            if ((rand() % 100) < 2) { 
                kill(pid_kernel, SIGUSR2);
            }
        }
        exit(0);
    }
}

// ============================================================================
// MAIN - KERNEL SIM
// ============================================================================
int main() {
    pid_t pid_kernel = getpid();
    SetupRede();

    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    // Instalação Correta dos Sinais (CORREÇÃO #2)
    sa.sa_handler = HandlerIRQ0; sigaction(SIGALRM, &sa, NULL);
    sa.sa_handler = HandlerIRQ1; sigaction(SIGUSR1, &sa, NULL); // USR1 = Arq
    sa.sa_handler = HandlerIRQ2; sigaction(SIGUSR2, &sa, NULL); // USR2 = Dir
    sa.sa_handler = HandlerSyscall; sigaction(SIG_SYSCALL, &sa, NULL); // RTMIN = Syscall
    sa.sa_handler = HandlerCtrlC; sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = HandlerFilhoTerminado; sigaction(SIGCHLD, &sa, NULL);

    // --- CRIAR PROCESSOS ---
    for(int i=0; i<N_PROCESSOS; i++) {
        int shmid = shmget(KEY_BASE + i, sizeof(MensagemSFP), IPC_CREAT | 0666);
        if (shmid < 0) { perror("shmget"); exit(1); }
        
        processos[i].shmid = shmid;
        processos[i].shm_addr = (MensagemSFP *)shmat(shmid, NULL, 0);
        memset(processos[i].shm_addr, 0, sizeof(MensagemSFP));

        pid_t pid = fork();
        if (pid == 0) {
            ExecutarAplicacao(i); 
        } else {
            processos[i].pid = pid;
            processos[i].estado = PRONTO;
            processos[i].pc = 0;
            EnfileirarReady(i);
        }
    }

    CriarInterController(pid_kernel);

    // Inicia primeiro processo
    int p = DesenfileirarReady();
    if (p >= 0) IniciarProcesso(p);

    printf(">>> KERNEL T2 (Corrigido) PID=%d\n", pid_kernel);
    printf(">>> Mapa: IRQ0=ALRM, IRQ1=USR1, IRQ2=USR2, Syscall=RTMIN\n");

    while(1) {
        pause(); 

        // 1. CHECAR REDE (Buffer de entrada)
        MensagemSFP msg_rede;
        while (recvfrom(sockfd, &msg_rede, sizeof(msg_rede), 0, NULL, NULL) > 0) {
            int eh_arquivo = (msg_rede.tipo == REP_READ || msg_rede.tipo == REP_WRITE);
            
            if (eh_arquivo) {
                EnfileirarRespArquivo(msg_rede);
                // printf("Kernel: Net -> Fila Arq (A%d)\n", msg_rede.read_rep.owner);
            } else {
                EnfileirarRespDir(msg_rede);
                // printf("Kernel: Net -> Fila Dir (A%d)\n", msg_rede.create_dir_rep.owner);
            }
        }

        // 2. TRATAR SYSCALL
        if (flag_syscall) {
            flag_syscall = 0;
            if (idx_executando >= 0) {
                Processo *proc = &processos[idx_executando];
                MensagemSFP *msg_envio = proc->shm_addr;
                
                // Envia para SFSS
                if (sendto(sockfd, msg_envio, sizeof(MensagemSFP), 0, 
                           (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
                    perror("Kernel sendto");
                } 

                // Bloqueia
                proc->estado = BLOQUEADO;
                kill(proc->pid, SIGSTOP); // Manda SIGSTOP pro filho parar no pause()
                
                if (msg_envio->tipo == REQ_READ || msg_envio->tipo == REQ_WRITE)
                    proc->acesso_D1++;
                else
                    proc->acesso_D2++;

                idx_executando = -1;
            }
        }

        // 3. TRATAR IRQ1 (Arquivo)
        if (flag_irq1) {
            flag_irq1 = 0;
            MensagemSFP resposta;
            if (DesenfileirarRespArquivo(&resposta)) {
                // Owner vem 1..5, converte para 0..4
                int id_dono = resposta.read_rep.owner - 1; 
                
                if (id_dono >= 0 && id_dono < N_PROCESSOS) {
                    Processo *proc = &processos[id_dono];
                    memcpy(proc->shm_addr, &resposta, sizeof(MensagemSFP));
                    
                    if (proc->estado == BLOQUEADO) {
                        proc->estado = PRONTO;
                        EnfileirarReady(id_dono);
                        printf("Kernel: IRQ1 -> A%d pronto\n", proc->pid);
                    }
                }
            }
        }

        // 4. TRATAR IRQ2 (Diretório)
        if (flag_irq2) {
            flag_irq2 = 0;
            MensagemSFP resposta;
            if (DesenfileirarRespDir(&resposta)) {
                int id_dono = resposta.create_dir_rep.owner - 1;
                
                if (id_dono >= 0 && id_dono < N_PROCESSOS) {
                    Processo *proc = &processos[id_dono];
                    memcpy(proc->shm_addr, &resposta, sizeof(MensagemSFP));
                    
                    if (proc->estado == BLOQUEADO) {
                        proc->estado = PRONTO;
                        EnfileirarReady(id_dono);
                        printf("Kernel: IRQ2 -> A%d pronto\n", proc->pid);
                    }
                }
            }
        }

        // 5. TRATAR IRQ0 (TimeSlice)
        if (flag_irq0) {
            flag_irq0 = 0;
            
            // CORREÇÃO #1: Atualizar PC do processo que estava rodando
            // Como é simulação, incrementamos o PC apenas para estatística
            if (idx_executando >= 0 && processos[idx_executando].estado == EXECUTANDO) {
                processos[idx_executando].pc++;
            }

            PararExecutando();
            
            int prox = DesenfileirarReady();
            if (prox >= 0) {
                IniciarProcesso(prox);
            }
        }

        // 6. DEBUG (Ctrl+C)
        if (flag_ctrlc) {
            flag_ctrlc = 0;
            printf("\n--- STATUS KERNEL ---\n");
            for(int i=0; i<N_PROCESSOS; i++)
                printf("A%d (PID %d): PC~=%d Estado=%d (1=Exec, 2=Block) | Arq=%d Dir=%d\n", 
                       i+1, processos[i].pid, processos[i].pc, processos[i].estado, 
                       processos[i].acesso_D1, processos[i].acesso_D2);
            printf("Filas Pendentes: Arq=%d, Dir=%d\n", len_fa, len_fd);
        }

        if (flag_filho_terminou) {
            flag_filho_terminou = 0;
            while(waitpid(-1, NULL, WNOHANG) > 0);
        }
    }
}