#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>     // biblioteca essencial para chamade de sistema POSIX (read, write, close, fork)
#include <fcntl.h>      // define constantes como O_RDONLY, O_WRONLY, O_CREAT, etc
#include <sys/stat.h>   // necessário para a função mkfifo() e as permissões (0666)
#include <sys/time.h>   // necessário para o gettimeofday() e estrutura timeval
#include <string.h>
#include <errno.h>      // permite aceder à variável global 'errno' para identificar erros específicos
#include "../include/structs.h"     // a nossa estrutura 'Message' que padroniza a comunicção

// FILA DE ESPERA (Tarefas pendentes)
Message wait_queue[MAX_QUEUE];  // array estático que funciona como fila de espera para as tarefas que não têm vaga
int queue_size = 0;     // contador que indica quantos processos estão na fila

// TAREFAS EM EXECUÇÃO
Message executing_tasks[MAX_QUEUE];     // array que guarda as tarefas que estão a correr neste momento

// LIMITES E CONTAGENS
int max_parallel;   // limite máximo de tarefas a correr ao mesmo tempo (passado nos argumentos)
int current_running = 0;    // quantas tarefas estão efetivamente a correr agora

// GESTÃO DE ENCERRAMENTO (SHUTDOWN)
int shutting_down = 0;  // flag booleana: 0 = funcionamento normal, 1 = a encerrar
int pid_of_shutdown_runner = -1;    // guarda o PID do cliente que pediu o '-s' para lhe responder no fim

// função que escreve os resultados finais no ficheiro histórico
void log_task(int user_id, pid_t pid, double duration) {
    // abre o ficheiro. O_CREAT se não existir. O_APPEND escreve sempre no fim. 0666 dá permissões de leitura/escrita
    int fd = open("log_tasks.txt", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd != -1) {
        char buffer[256];
        // o sprintf formata a string e devolve o número de bytes escritos no buffer
        int bytes = sprintf(buffer, "user-id: %d | command-id: %d | duration: %.4f s\n", user_id, pid, duration);
        // escreve fisicamente no ficheiro usando a chamada de sistema low-level
        write(fd, buffer, bytes);   // escreve no ficheiro do disco
        close(fd); // boa prática: libertar sempre o descritor do ficheiro
    } else {
        perror("Erro ao abrir log_tasks.txt");
    }
}
// funçaõ que desbloqueia os runners adormecidos
void send_authorization(int runner_pid) {
    char fifo_name[64];
    // constrói o nome do FIFO privado correspondente ao cliente (ex: "fifo_runner_1234")
    sprintf(fifo_name, "fifo_runner_%d", runner_pid);
    
    // abre o FIFO privado do cliente para escrita
    int fd = open(fifo_name, O_WRONLY);
    if (fd != -1) {
        write(fd, "OK", 2); // envia os 2 bytes mágicos que desbloqueiam o 'runner'
        close(fd);
    }
}

// limpa a tarefa da lista de execução quando ela acaba
void remove_from_executing(pid_t pid) {
    for (int i = 0; i < current_running; i++) { // percorre as tarefas ativas
        if (executing_tasks[i].pid == pid) {    // encontra a tarefa que acabou
            // puxa os restantes para a esquerda para não deixar buracos no array
            for (int j = i; j < current_running - 1; j++) {
                executing_tasks[j] = executing_tasks[j + 1];    // substitui o antigo pelo que está à frente
            }
            break;  // já encontrou e removeu, não precisa de continuar à procura
        }
    }
}

// para tirar o primeiro runner da fila para executar e avançar todos uma casa para a esquerda
// copia a mensagem da posição 0, puxa toda a fila restante uma casa para a esquerda, e reduz o tamanho da fila 
Message pop_from_queue() {
    Message next_task = wait_queue[0];  // guarda a tarefa mais antiga (indice 0)
    // como tiramos o índice 0, temos de arrastar a fila toda para a esuqerda
    for (int i = 0; i < queue_size - 1; i++) {
        wait_queue[i] = wait_queue[i + 1];  // arrasta todoa a fila uma posição para a esquerda
    }
    queue_size--;   // informa que a lista de espera tem menos uma pessoa
    return next_task;   // devolve a tarefa para ela ir para execução
}

int main(int argc, char *argv[]) {
    char log_buf[256];
    int log_len;

    // validação escrita de argumentos
    if (argc < 3) {
        fprintf(stderr, "Uso: %s <parallel-commands> <sched-policy>\n", argv[0]);
        return 1;
    }

    // atoi converte a string do terminal (ex: "2") num inteiro para o limite
    max_parallel = atoi(argv[1]);
    char *sched_policy = argv[2];   // guard a política apesar de o código usar sempre FIFO
    
    // mkfifo cria o tubo de comunicação global. 0666 = permissões rw-rw-rw
    // IMPORTANTE: "&& errno != EEXIST" ignora o erro se o FIFO já existir de execuções anteriores
    if (mkfifo("fifo_controller", 0666) == -1 && errno != EEXIST) {
        perror("Erro ao criar FIFO");
        return 1;
    }

    log_len = sprintf(log_buf, "[controller] Running with parallel limit: %d | Policy: %s\n", max_parallel, sched_policy);
    write(STDOUT_FILENO, log_buf, log_len); //imprime para o terminal

    // A JOGADA DE MESTRE: O_RDWR evita o "Busy Waiting", impedindo que o read() retorne 0 constantemente
    int fd_controller = open("fifo_controller", O_RDWR);
    Message msg;

    while (1) {
        // bloqueia aqui até receber uma mensagem com o tamnaho exato da nossa struct Message 
        if (read(fd_controller, &msg, sizeof(Message)) > 0) {
            
            // BLOCO EXECUTE 
            if (msg.operation == 'e') {
                // ignora novos pedidos se estiver a meio de um shutdown e envia "NO" para não bloquear
                // PREVENÇÃO DURANTE SHUTDOWN: se estamos a fechar e é um pedido NOVO (status 0)
                if (shutting_down == 1 && msg.status == 0) {
                    char fifo_name[64];
                    sprintf(fifo_name, "fifo_runner_%d", msg.pid);
                    int fd_rej = open(fifo_name, O_WRONLY);
                    if (fd_rej != -1) {
                        write(fd_rej, "NO", 2); // rejeita o cliente para ele não ficar pendurado
                        close(fd_rej);
                    }
                    continue;   // volta ao início do read, ignorando o resto
                }

                // PEDIDO NOVO A ENTRAR (status 0)
                if (msg.status == 0) { 
                    gettimeofday(&msg.arrival_time, NULL);  // marca o relógio de entrada

                    log_len = sprintf(log_buf, "[controller] Tarefa %d recebida: \"%s\" (Utilizador %d)\n", msg.pid, msg.command, msg.user_id);
                    write(STDOUT_FILENO, log_buf, log_len);

                    // se há vagas, deixamos o processo entrar imediatamente. Se o limite foi atingido, guardamos o estado na fila e não enviamos o 'OK' e o runner fica bloqueado
                    // AVALIAÇÃO DE CONCORRÊNCIA
                    if (current_running < max_parallel) {
                        // há vagas entra na fila de execução
                        executing_tasks[current_running] = msg; 
                        current_running++;
                        send_authorization(msg.pid);    // dá o "OK" para o fork() aparecer
                    } else {
                        // não há vagas. vai para a fila de espera
                        if (queue_size < MAX_QUEUE) {
                            wait_queue[queue_size] = msg;
                            queue_size++;
                            
                            log_len = sprintf(log_buf, "[controller] Task %d queued.\n", msg.pid);
                            write(STDOUT_FILENO, log_buf, log_len);
                        }
                    }
                } 
                // TAREFA TERMINOU (status 1 enviado pelo pai runner)
                else if (msg.status == 1) { 
                    struct timeval end_time;
                    gettimeofday(&end_time, NULL);  // marca o relógio de saída
                    double duration = 0;

                    // procura o processo que terminou e calcula o tempo total
                    for (int i = 0; i < current_running; i++) {
                        if (executing_tasks[i].pid == msg.pid) {
                            // subtrai os segundos, e adiciona a subtração dos microsegundos (convertida para segundos)
                            duration = (end_time.tv_sec - executing_tasks[i].arrival_time.tv_sec) + 
                                       (end_time.tv_usec - executing_tasks[i].arrival_time.tv_usec) / 1000000.0;
                            break;
                        }
                    }

                    remove_from_executing(msg.pid); // liberta a vaga
                    current_running--;
                    
                    log_len = sprintf(log_buf, "[controller] Tarefa %d concluída: \"%s\"\n", msg.pid, msg.command);
                    write(STDOUT_FILENO, log_buf, log_len); 

                    log_task(msg.user_id, msg.pid, duration);   // escreve no ficheiro
                    
                    // ESCALONAMENTO: se a vaga abriu e há gente na fila, põe o próximo a correr
                    if (queue_size > 0) {
                        Message next_msg = pop_from_queue();    // tira o mais antifo (FIFO)
                        executing_tasks[current_running] = next_msg;    // mete em execução
                        current_running++;
                        send_authorization(next_msg.pid);   // dá-le o cobiçado "OK"
                    }
                }
            } 

            // BLOCO STATUS
            else if (msg.operation == 'c') {
                char response[4096] = "--- Executing ---\n";    // buffer grande para evitar overflows
                char temp[128];
                
                // adicionar os que estão a executar
                // concatena (strcat) as strings de todos os processos a correr
                for (int i = 0; i < current_running; i++) {
                    sprintf(temp, "user-id %d - command-id %d\n", executing_tasks[i].user_id, executing_tasks[i].pid);
                    strcat(response, temp);
                }
                
                // adicionar os que estão agendados na fila
                strcat(response, "--- Scheduled ---\n");
                // concatens (strcat) as strings de todos os processos na fila
                for (int i = 0; i < queue_size; i++) {
                    sprintf(temp, "user-id %d - command-id %d\n", wait_queue[i].user_id, wait_queue[i].pid);
                    strcat(response, temp);
                }

                // envio da resposta para o pipe privado do cliente
                // responde ao cliente que pediu o status
                char fifo_name[64];
                sprintf(fifo_name, "fifo_runner_%d", msg.pid);
                int fd_resp = open(fifo_name, O_WRONLY);
                if (fd_resp != -1) {
                    write(fd_resp, response, strlen(response)); // envia a string gigante criada
                    close(fd_resp);
                }
            }

            // quando recebemos um -s, ativamos a flag shutting_down = 1
            // BLOCO SHUTDOWN
            else if (msg.operation == 's') {
                // inicia o processo de shutdown sem fechar já o ciclo
                shutting_down = 1;  // ativa a flag. o servidor agora sabe que está em modo de fecho
                pid_of_shutdown_runner = msg.pid;   // regista quem pediu o encerramento
            }
            
            // CONDIÇÃO DE SAÍDA SEGURA (Grateful Exit)
            // se ativarem o shutdown E não há tarefas a correr E a fila está vazia
            if (shutting_down == 1 && current_running == 0 && queue_size == 0) {
                log_len = sprintf(log_buf, "[controller] A encerrar o orquestrador.\n");
                write(STDOUT_FILENO, log_buf, log_len);
                
                // avisa o cliente que pediu o '-s' que já fechamos em segurança
                if (pid_of_shutdown_runner != -1) {
                    char fifo_name[64];
                    sprintf(fifo_name, "fifo_runner_%d", pid_of_shutdown_runner);
                    int fd_resp = open(fifo_name, O_WRONLY);
                    if (fd_resp != -1) {
                        write(fd_resp, "END", 3);
                        close(fd_resp);
                    }
                }
                break;  // finalmente, quebra o while(1) e o processo termina
            }
        }
    }

    // LIMPEZA FINAL
    close(fd_controller);   // fecha o descritor de ficheiro principal
    unlink("fifo_controller");  // destói/apaga o ficheiro FIFO do sistema de ficheiros para não deixar lixo
    return 0;
}