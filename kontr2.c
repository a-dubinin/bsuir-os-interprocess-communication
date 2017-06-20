/**
 * Программа для работы со средствами
 * межпроцессного взаимодействия.
 * Разработчик - Дубинин А. В. (http://dubinin.net)
 * 04.03.2017
 */

// Подключение файлов из библиотек
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sem.h>
#include <sys/mman.h>

// Объявление символических констант
#define MODE_SEM     0600
#define MODE_SHM     0777
#define NUM_PROC     2
#define NUM_ROW_PART 75
#define NUM_ROW_ALL  1000
#define STR_LEN      255
#define P_CREATE_ERR -1
#define SHM_NAME     "shm_obj_01.shm"

/*
 * Объявление символических констант
 * (сообщения пользовательского интерфейса, потока вывода ошибок)
 */
#define MSG_ERR_SIGUSR1     "Ошибка: невозможно обработать сигнал USR1"
#define MSG_ERR_SIGUSR2     "Ошибка: невозможно обработать сигнал USR2"
#define MSG_ERR_UNKNOWN_SIG "Ошибка: недопустимый сигнал %d\n"
#define MSG_ERR_SEM         "Ошибка: невозможно создать набор семафоров"
#define MSG_ERR_SHM         "Ошибка: невозможно создать объект разделяемой памяти"
#define MSG_ERR_PROCESS     "Ошибка: невозможно создать процесс"
#define MSG_PATTERN_ROW     "Row %4d | Pid %5d | %ld (мксек)\n"

/*
 * Прототипы функций sigHandler, createProcess, executeChildProcess,
 * executeParentProcess, writeRowToShm, readRowFromShm
 */
static void sigHandler(const int);
pid_t createProcess();
void executeChildProcess(const int, const int, struct sembuf, struct sembuf);
void executeParentProcess(const int, pid_t []);
void writeRowToShm(const int, const int);
void readRowFromShm(const int);

/*
 * Объявление и инициализация начальными значениями
 * флагов записи и чтения в/из разделяемой памяти
 */
int startWrite = 0;
int startRead  = 0;

void main()
{
    // Объявление переменных
    int semId;            // дескриптор наборов семафоров
    int shmId;            // дескриптор объекта разделяемой памяти
    pid_t pidNew;         // id нового созданного процесса
    pid_t pids[NUM_PROC]; // массив идентификаторов дочерних процессов

    // Объедение для выполнения операций над набором семафоров
    union semun {
       int val;
       struct semid_ds *buf;
       unsigned short *array;
    } ctlArg;

    /*
     * Установка предопределенных реакций на сигналы:
     * SIGUSR1
     * SIGUSR2
     */
    if (signal(SIGUSR1, sigHandler) == SIG_ERR) {
        perror(MSG_ERR_SIGUSR1);
        exit(EXIT_FAILURE);
    }
    if (signal(SIGUSR2, sigHandler) == SIG_ERR) {
        perror(MSG_ERR_SIGUSR2);
        exit(EXIT_FAILURE);
    }

    // Создание и получение доступа к наборам семафоров
    if ((semId = semget(IPC_PRIVATE, 2, MODE_SEM|IPC_CREAT|IPC_EXCL)) < 0) {
        perror(MSG_ERR_SEM);
        exit(EXIT_FAILURE);
    }

    // Инициализация начальными значениями семафоров в наборе
    ctlArg.val = 0;
    semctl(semId, 0, SETVAL, ctlArg);
    semctl(semId, 1, SETVAL, ctlArg);
    
    // Структуры для определения p и v операций над семафором
    struct sembuf writeP = {1, -1, 0};
    struct sembuf writeV = {1, 1, 0};

    // Создание и открытие объекта разделяемой памяти
    if ((shmId = shm_open(SHM_NAME, O_RDWR|O_CREAT, MODE_SHM)) < 0) {
        perror(MSG_ERR_SHM);
        exit(EXIT_FAILURE);
    }

    // Создание дочерних процессов
    pidNew = createProcess();
    if (pidNew == 0) {
        pids[0] = getpid();
        executeChildProcess(semId, shmId, writeP, writeV);
    } else {
        pids[0] = pidNew;
        pidNew = createProcess();
        if (pidNew == 0) {
            pids[1] = getpid();
            executeChildProcess(semId, shmId, writeV, writeP);
        } else {
            pids[1] = pidNew;
            executeParentProcess(shmId, pids);
        }
    }

    // Удаление объекта разделяемой памяти и набора семафоров
    shm_unlink(SHM_NAME);
    semctl(semId, 0, IPC_RMID, NULL);
    return;
}

/*
 * Статическая функция установки действий
 * на реагирование сигналов.
 *
 * sig - параметр-константа типа integer
 */
static void sigHandler(const int sig)
{
    switch (sig) {
        case SIGUSR1:
            startRead = 1;
            break;
        case SIGUSR2:
            startWrite = 1;
            break;
        default:
            printf(MSG_ERR_UNKNOWN_SIG, sig);
    }
    return;
}

/*
 * Функция создания нового процесса.
 * Возвращает значение типа pid_t.
 */
pid_t createProcess()
{
    pid_t pid = fork();
    if (pid == P_CREATE_ERR) {
        perror(MSG_ERR_PROCESS);
        exit(EXIT_FAILURE);
    } else {
        return pid;
    }
}

/*
 * Функция выполнения дочернего процесса.
 *
 * semId - параметр-константа типа int
 * shmId - параметр-константа типа int
 * lock - параметр-структура sembuf
 * unlock - параметр-структура sembuf
 */
void executeChildProcess(
    const int semId,
    const int shmId,
    struct sembuf lock,
    struct sembuf unlock
) {
    int curRow; // номер текущей строки
    int i;      // параметр цикла

    while (1) {
        if (startWrite) {
            semop(semId, &lock, 1); // блокировка доступа к разделяемой памяти
            lseek(shmId, 0, SEEK_END); // установка указателя

            // Чтение №строки из семафора-счетчика
            curRow = semctl(semId, 0, GETVAL, NULL);

            // Запись в разделяемую память по 75 строк
            for (i = 0; i < NUM_ROW_PART; i++) {
                curRow++;
                if (curRow > NUM_ROW_ALL) {
                    semop(semId, &unlock, 1);
                    // Посылка родительскому процессу сигнала SIGUSR1
                    kill(getppid(), SIGUSR1);
                    return;
                }
                writeRowToShm(shmId, curRow);
            }

            // Запись №строки в семафор-счетчик
            semctl(semId, 0, SETVAL, curRow);
            semop(semId, &unlock, 1); // разблокировка доступа
        }
    }
}

/*
 * Функция выполнения родительского процесса.
 *
 * shmId - параметр-константа типа int
 * pids - параметр-массив элементов типа pid_t
 */
void executeParentProcess(const int shmId, pid_t pids[])
{
    int i, j; // параметры циклов

    // Посылка дочерним процессам сигнала SIGUSR2
    kill(pids[0], SIGUSR2);
    kill(pids[1], SIGUSR2);

    i = 0;
    while (i < NUM_ROW_ALL) {
        if (startRead) {
            lseek(shmId, i * STR_LEN, SEEK_SET); // установка указателя

            // Чтение из разделяемой памяти по 75 строк
            for (j = 0; j < NUM_ROW_PART; j++) {
                i++;
                if (i > NUM_ROW_ALL) {
                    break;
                }
                readRowFromShm(shmId);
            }
        }
    }
}

/*
 * Функция записи в разделяемую память строки.
 *
 * shmId - параметр-константа типа int
 * curRow - параметр-константа типа int
 */
void writeRowToShm(const int shmId, const int curRow)
{
    // Объявление и инициализация переменных
    char row[STR_LEN];          // строка для записи в разделяемую память
    pid_t pid = getpid();       // id процесса
    struct timeval currentTime; // текущее время
    gettimeofday(&currentTime, NULL);

    sprintf(
        row,
        MSG_PATTERN_ROW,
        curRow,
        pid,
        currentTime.tv_usec
    );
    write(shmId, row, STR_LEN);
    return;
}

/*
 * Функция чтения строки из разделяемой памяти.
 *
 * shmId - параметр-константа типа int
 */
void readRowFromShm(const int shmId)
{
    char row[STR_LEN]; // строка

    read(shmId, row, STR_LEN);
    printf("%s", row);
    return;
}
