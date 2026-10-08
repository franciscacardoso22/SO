#!/bin/bash

echo "--- Compilar o projeto ---"
make clean
make

echo "--- Iniciar o controller (Limite = 2) ---"
./bin/controller 2 fifo &
sleep 1

echo "--- Submeter 3 tarefas de 5 segundos cada ---"
./bin/runner -e 101 "sleep 5" &
./bin/runner -e 102 "sleep 5" &
./bin/runner -e 103 "sleep 5" &

sleep 1

echo "--- Verificar o Status ---"
./bin/runner -c

echo "--- A aguardar que as tarefas terminem... ---"
sleep 10

echo "--- Conteúdo do log final ---"
cat log_tasks.txt

echo "--- Enviar sinal de Shutdown ---"
./bin/runner -s

echo "--- Teste concluído com sucesso ---"

