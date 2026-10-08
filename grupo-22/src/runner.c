#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>   // essencial para a função waitpid()
#include <sys/time.h>
#include "../include/structs.h"

// função para enviar mensagens para o fifo central do controller
void write_to_controller(Message msg) {
    // abre o FIFO global apenas para escrita (O_WRONLY)
    int fifo_fd = open("fifo_controller", O_WRONLY);
    // se devolver -1, significa que o ficheiro não existe ou não pode ser aberto
    if (fifo_fd == -1) {
        // se falhar (ex: o controller não está a correr), o perror imprime o erro detalhado do sistema
        perror("Erro ao abrir fifo_controller (O controller está a correr?)");
        exit(1);    // termina imediatamente o runner com código de erro (1)
    }
    // tenta escrever a estrutura Message completa no tubo
    if (write(fifo_fd, &msg, sizeof(Message)) == -1) {
        perror("Erro a escrever no fifo_controller"); // se a escrita falhar por algum motivo

        exit(1);    // mata o processo
    }
    close(fifo_fd); // fecha a ligação deste runner ao tubo principal do servidor
}

// ponto partida do programa cliente (runner)
int main(int argc, char *argv[]) {
    // validação mínima de argumentos (precisa de pelo menos o ./runner e a flag -e, -c ou -s)
    if (argc < 2) {
        char msg_erro[] = "Uso: runner -e <user-id> \"<command>\" | -c | -s\n";
        write(STDOUT_FILENO, msg_erro, strlen(msg_erro));   // escreve no ecrã sem usar o printf
        return 1;   // termina o programa por uso incorreto
    }

    Message msg;    // cria a variável "msg" baseada na nossa estrutura
    msg.pid = getpid(); // obtém o ID único que o Sistema Operativo deu a este processo runner específico
    msg.operation = argv[1][1]; // extrai 'e', 'c' ou 's', ou seja, em vez de strcmp, vai buscar diretamente a 2ª letra do argumento
    msg.status = 0; // 0 = pedido inicial (ainda não executou)

    // parser dos argumentos
    // PREPARAÇÃO DA MENSAGEM
    // se a operação for executar ('e') e existirem exatamente 4 argumentos totais na linha de comandos
    if (msg.operation == 'e' && argc == 4) {
        msg.user_id = atoi(argv[2]);    // converte a string do ID do utilizador num número inteiro (ex: "101" -> 101)

        // copia com segurança a sting do comando para dentro da estrutura, respeitando o limite máximo
        strncpy(msg.command, argv[3], MAX_CMD_SIZE - 1);
        msg.command[MAX_CMD_SIZE - 1] = '\0';   // tranca a última posição do array com '\0' para evitar Buffer Overflow

    // se a operação for status ('c') ou shutdown ('s')
    } else if (msg.operation == 'c' || msg.operation == 's') {
        msg.user_id = -1;   // coloca um ID falso/inócuo porque não vai ser registado nenhum utilizador nestes casos
        msg.command[0] = '\0';  // esvazia a string do comando (começa logo com o terminador)
        
    // se falhar todas as verificações acima (ex: -e mas sem meter o comando)
    } else {
        char msg_erro[] = "Argumentos inválidos.\n";
        write(STDOUT_FILENO, msg_erro, strlen(msg_erro));   // escreve no ecrã
        return 1;   // mata o programa
    }

    char fifo_name[64]; // array para guardar o nome do tubo privado deste runner

    // constrói a string dinamicamente usando o PID. exemplo de resultado: "fifo_runner_9999"
    sprintf(fifo_name, "fifo_runner_%d", msg.pid);
    // pede ao kenel para criar fisicamente este ficheiro FIFO. permissões 0666 (leitura/escrita para todos)
    if (mkfifo(fifo_name, 0666) == -1) {
        perror("Erro ao criar fifo do runner"); // se falhar a criação do tubo
        exit(1);
    }

    char buf[128];  // buffer genérico para criar strings de texto para imprimir no ecrã
    int len;    // vairável para guardar quantos caracteres foram escritos no buffer

    write_to_controller(msg);   // chama a nossa função para atirar o pedido acabado de criar para o servidor
    
    // comportamento consoante a operação
    // BLOCO DA OPERAÇÃO 'e'
    if (msg.operation == 'e') {
        // prepara e imprime a mensagem a dizer que submetemos o pedido
        len = sprintf(buf, "[runner] command %d submitted\n", msg.pid);
        write(STDOUT_FILENO, buf, len);

        // bloqueia à espera da autorização do controller
        // BLOQUEIO ATIVO: abre o seu FIFO privado em modo leitura e fica à espera que o orquestrador escreva algo
        int fd_read = open(fifo_name, O_RDONLY);
        char response[64];  // array para guardar a resposta do servidor ("OK" ou "NO")
        int bytes_read = read(fd_read, response, sizeof(response) - 1); // lê os dados
        close(fd_read); // recebeu a proposta, fecha logo a porta

        if (bytes_read > 0) {   // se efetivamente leu algo no servidor
            response[bytes_read] = '\0';    // transforma os bytes lidos numa string válida
            
            // se o servidor deu luz verde ("OK")
            if (strncmp(response, "OK", 2) == 0) {
                len = sprintf(buf, "[runner] executing command %d...\n", msg.pid);
                write(STDOUT_FILENO, buf, len); // avisa o utilizador que vai começar
                
                pid_t child_pid = fork();   // clona o processo. nasce o filho

                // CÓDIGO FILHO
                if (child_pid == 0) {
                    char *args[64]; // array de ponteiros onde vamos guardar cada palavra solta do comando  
                    int i = 0;  // indice para saber em que posição do array 'args' estamos
                    char *cmd_ptr = msg.command;    // ponteiro que vai varrer a string do comando

                    // o nosso Parser Manual que transforma "ls -l" em ["ls", "-l", NULL]
                    while (*cmd_ptr != '\0') {
                        //  ignorar espaços em branco no início ou entre palavras
                        while (*cmd_ptr == ' ') cmd_ptr++;  // salta espaços extra
                        if (*cmd_ptr == '\0') break;    // prevenção de segurança

                        //  se o argumento começar com aspas duplas ou simples
                        if (*cmd_ptr == '"' || *cmd_ptr == '\'') {
                            char quote_type = *cmd_ptr; // memoriza se é aspa dupla ou plica
                            cmd_ptr++;  // salta a aspa 
                            args[i++] = cmd_ptr;    // guarda o ínico da palavra
                            
                            // procura a aspa final para fechar o argumento
                            while (*cmd_ptr != '\0' && *cmd_ptr != quote_type) {
                                cmd_ptr++;  // avança até fechar a aspa
                            }
                            if (*cmd_ptr != '\0') {
                                *cmd_ptr = '\0';    // substitui a aspa de fecho por \0 (fatia a string)
                                cmd_ptr++;  // avança para a próxima iteração
                            }
                        } else {
                            //  palavra normal (sem aspas)
                            args[i++] = cmd_ptr;    // guarda a morada da primeira palavra
                            // vai avançando letra a letra até encontrar um espaço
                            while (*cmd_ptr != '\0' && *cmd_ptr != ' ') {
                                cmd_ptr++;
                            }
                            if (*cmd_ptr != '\0') {
                                *cmd_ptr = '\0';    // substitui o espaço por \0 (fatia a string)
                                cmd_ptr++;  // avança
                            }
                        }
                    }
                    args[i] = NULL; // último argumento tem de ser NULL para o execvp não rebentar

                    // substitui a memória do processo filho pelo comando pedido
                    execvp(args[0], args);
                    
                    // se chegar aqui, o execvp falhou
                    perror("Erro ao executar o comando");
                    exit(1);

                // CÓDIGO PAI (runner original)
                } else if (child_pid > 0) {
                    int status;
                    // fica bloquado à espera que o filho termine (evita cirar processos zombie)
                    waitpid(child_pid, &status, 0); 

                    // atualiza a estrutura para avisar que a tarefa acabou
                    msg.status = 1; // filho acabou. o pai muda a flag para 1 (terminado)
                    write_to_controller(msg);   // envia para o servidor processar a saída
                    
                    len = sprintf(buf, "[runner] command %d finished\n", msg.pid);
                    write(STDOUT_FILENO, buf, len); // avisa o utiizador que acabou
                } else {
                    perror("Erro no fork"); // só cai aqui se o Sistema Operativo não tiver memória para criar processos
                }
            }
            // tratamento caso o servidor esteja a encerrar e mande "NO" em vez de "OK"
            else if (strncmp(response, "NO", 2) == 0) {
                char shutdown_msg[] = "[runner] Request rejected: Controller is shutting down.\n";
                write(STDOUT_FILENO, shutdown_msg, strlen(shutdown_msg));
            }
        }
    // BLOCO DA OPERAÇÃO 'c' (status)
    } else if (msg.operation == 'c') {
        int fd_read = open(fifo_name, O_RDONLY);    // abre o seu tubo privado
        char response[4096];    // buffer grande para receber a lista inteira do controller
        int bytes_read = read(fd_read, response, sizeof(response) - 1); // fica bloquado até o servidor enviar a lista
        close(fd_read);

        if (bytes_read > 0) {
            response[bytes_read] = '\0';    // transforma os bytes crus numa lista
            write(STDOUT_FILENO, response, bytes_read); // imprime a lista no ecrã
        }
    // BLOCO DA OPERAÇÃO 's' (Shutdown)
    } else if (msg.operation == 's') {
        len = sprintf(buf, "[runner] sent shutdown notification\n");
        write(STDOUT_FILENO, buf, len);
        
        len = sprintf(buf, "[runner] waiting for controller to shutdown...\n");
        write(STDOUT_FILENO, buf, len);

        int fd_read = open(fifo_name, O_RDONLY);    // abre o tubo privado 
        char response[64];
        //bloqueia aqui até o servidor terminar de limpar tudo e enviar o "END"
        int bytes_read = read(fd_read, response, sizeof(response) - 1);
        close(fd_read);

        if (bytes_read > 0) {   // recebeu a mensagem final "END"
            len = sprintf(buf, "[runner] controller exited.\n");
            write(STDOUT_FILENO, buf, len);
        }
    }
    // LIMPEZA OBRIGATÓRIA
    unlink(fifo_name);  // apaga o ficheiro do FIFO privado do disco 
    return 0;   // termina o programa limpo e com sucesso
}