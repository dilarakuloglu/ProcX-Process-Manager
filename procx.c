#include <fcntl.h>  // O_CREAT, O_RDWR
#include <signal.h> // pid_t, kill, sinyal tipleri
#include <stdbool.h>
#include <stdio.h>  // printf, perror vs.
#include <stdlib.h> // exit, malloc, free
#include <string.h>
#include <sys/mman.h>  // shm_open, mmap
#include <sys/types.h> // pid_t, key_t, mode_t (PID tanımı burada da var)
#include <sys/wait.h>
#include <time.h>   // time_t, time(), ctime()
#include <unistd.h> // fork, getpid, exec, pid_t

typedef enum { ATTACHED = 0, DETACHED = 1 } ProcessMode;
typedef enum { RUNNING = 0, TERMINATED = 1 } ProcessStatus;

typedef struct {
  pid_t pid;       // Process ID
  pid_t owner_pid; // Başlatan instance'ın PID'si
  char
      command[256]; // Çalıştırılan komut  // dinamik bellek yok çünkü başka
                    // procx instance’ı o pointer’ın işaret ettiği yeri göremez.
  ProcessMode mode; // Attached (0) veya Detached (1)
  ProcessStatus status; // Running (0) veya Terminated (1)
  time_t start_time;    // Başlangıç zamanı
  int is_active;        // Aktif mi? (1: Evet, 0: Hayır)
} ProcessInfo;

// Paylaşılan bellek yapısı
typedef struct {
  ProcessInfo
      processes[50]; // Maksimum 50 process  // bu PCB gibi iş görüyormuş.
  int process_count; // Aktif process sayısı
} SharedData;

// birden fazla terminalde çalışan ProcX in instanceları aynı process listesini
// görsün diye. Global state yani. peer-to-peer en kritik IPC tekniği , en
// hızlısı çünkü kopya 0 tüm instancelar aynı RAM den yürütülüyor o yüzden
// senkronizasyon lazım.

#define SHM_NAME "/procx_shm"
#define SHM_SIZE sizeof(SharedData)
#define MAX_PROCESSES 50
SharedData *shared_data = NULL; // global pointer buradan RAM e erişecek.

// Mesaj yapısı
typedef struct {
  long msg_type;    // Mesaj tipi
  int command;      // Komut (START/TERMINATE)
  pid_t sender_pid; // Gönderen PID
  pid_t target_pid; // Hedef process PID
} Message;

int get_menu_choice();
void start_process_menu(void);
void start_process(char *command, int mode);
int parse_command(char *command, char **argv);
void trim(char *str);
void init_shared_memory();

// Shared memory senkronize veri tutar ama olayı bildirmez .Terminal 1 de işlem
// yapınca terminal 2 nin anında öğrenmesi için message queue lazım.

// O_CREATE -> yoksa oluştur ,O_RDWR ->hem oku hem yaz , 0666-> dosya izinleri
// shm_open() → bir shared memory nesnesi oluşturur. macos ta /var/run altında ,
// eğer zaten varsa yeniden açar. bir dosya tanımlayıcısı "int fd" verir. int
// shm_open(const char *name, int oflag, mode_t mode); ftruncate() → boyut
// belirler , shared memory nesnesi dosya gibi olduğundan önce boyut
// belirlenmeli mmap() → RAM’e bağlar(pointer döner)
// void* mmap(void *addr, size_t length, int prot, int flags, int fd, off_t
// offset); Sistem çağrılarında hata aldıysan asla devam etme.

// Kullanıcıdan alınıp child processte yürütülecek olan programı uygun argüman
// haline getirir.
// args[0]= sleep - args[1]= 10 - args[2]= NULL

int get_menu_choice() {
  char input[64];

  while (true) {
    printf("\n===== ProcX v1.0 =====\n");
    printf("1. Launch New Process\n");
    printf("2. List Active Processes\n");
    printf("3. Terminate a Process\n");
    printf("0. Exit\n");
    printf("Your choice: ");

    if (fgets(input, sizeof(input), stdin) == NULL) {
      printf("\nEOF received. Exiting...\n");
      return 0;
    }

    input[strcspn(input, "\n")] = '\0';
    trim(input);

    if (input[0] == '\0') {
      printf("Empty input is not allowed. Please try again.\n");
      continue;
    }

    if (strcmp(input, "0") == 0)
      return 0;
    if (strcmp(input, "1") == 0)
      return 1;
    if (strcmp(input, "2") == 0)
      return 2;
    if (strcmp(input, "3") == 0)
      return 3;

    printf("Invalid selection! Please enter 0, 1, 2 or 3.\n");
  }
}

void start_process_menu(void) {
  char command[256];
  char input[16];
  ProcessMode mode;

  printf("Enter a command to run: ");
  if (fgets(command, sizeof(command), stdin) == NULL) {
    printf("No input detected. Returning to menu.\n");
    return;
  }

  command[strcspn(command, "\n")] = '\0';
  trim(command);

  if (command[0] == '\0') {
    printf("Empty command. Nothing to run.\n");
    return;
  }

  while (true) {
    printf("Select mode (0 = Attached, 1 = Detached): ");

    if (fgets(input, sizeof(input), stdin) == NULL) {
      printf("No input detected. Cancelling process launch.\n");
      return;
    }

    input[strcspn(input, "\n")] = '\0';
    trim(input);

    if (strcmp(input, "0") == 0) {
      mode = ATTACHED;
      break;
    }

    if (strcmp(input, "1") == 0) {
      mode = DETACHED;
      break;
    }

    printf("Invalid mode! Please enter 0 or 1.\n");
  }
  start_process(command, mode);
}

int parse_command(char *command, char **argv) {
  for (int i = 0; i < 20; i++) {
    argv[i] = NULL;
  }

  char *token = strtok(command, " ");
  int i = 0;
  while (i < 19 && token != NULL) {
    argv[i++] = token;
    token = strtok(NULL, " ");
  }
  argv[i] = NULL; // execv nin okuyabileceği şekilde komut null-terminated oldu.
  return i;
}

void trim(char *str) {
  // kullanıcıdan alınan komutun başında ve sonundaki boşlukları temizler.
  char *start = str;
  while (*start == ' ' || *start == '\t')
    start++;

  if (*start == '\0') {
    str[0] = '\0';
    return;
  }

  char *end = start + strlen(start) - 1;
  while (end > start && (*end == ' ' || *end == '\t'))
    end--;

  *(end + 1) = '\0';

  if (start != str)
    memmove(str, start, strlen(start) + 1);
}

void start_process(char *command, int mode) {
  char *argv[20];
  int child_status;
  int arguman_count = parse_command(command, argv);
  if (arguman_count == 0) {
    printf("COMMAND NOT FOUND !");
    return;
  }
  pid_t pid = fork();

  if (pid < 0) {
    perror("fork failed");
    return;
  }
  if (pid == 0) {
    if (mode == DETACHED) {
      setsid(); // background child process
    }
    execvp(argv[0], argv);
    perror("execvp failed");
    exit(1);

  } else {
    // Parent, DETACHED çocukları beklemez ama kaydeder. Yani detached
    // modda bile shared memory’ye kaydı her zaman parent yapar. DETACHED mod
    // sadece terminal kapanınca ölmemesi anlamına gelir.
    // fork()tan dönen pid == child_pid !!!

    int process_idx = shared_data->process_count;
    if (process_idx >= MAX_PROCESSES) {
      fprintf(stderr, "process table full\n");
      return;
    }
    shared_data->processes[process_idx].pid = pid;
    shared_data->processes[process_idx].owner_pid = getpid();
    strcpy(shared_data->processes[process_idx].command, command);
    shared_data->processes[process_idx].mode = mode;
    shared_data->processes[process_idx].status = RUNNING;
    shared_data->processes[process_idx].is_active = 1;
    shared_data->processes[process_idx].start_time = time(NULL);

    if (mode == ATTACHED) {
      // Sadece attached modda parent child processi bekleyecek.
      waitpid(pid, &child_status, 0);
      shared_data->processes[process_idx].status = TERMINATED;
      shared_data->processes[process_idx].is_active = 0;
    }

    shared_data->process_count++;
    printf("\nProcess :count %d\n", shared_data->process_count);
  }
}

void init_shared_memory() {
  int fd = shm_open(SHM_NAME, O_RDWR, 0666);
  int first_instance = 0;

  // shared memory hiç yoksa oluştur.
  if (fd == -1) {
    fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd == -1) {
      perror("shm_open failed");
      exit(1);
    }
    first_instance = 1;

    if (ftruncate(fd, SHM_SIZE) == -1) {
      perror("ftruncate failed");
      exit(1);
    }
  }

  shared_data = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (shared_data == MAP_FAILED) {
    perror("mmap failed");
    exit(1);
  }

  close(fd);
  // shared memory ilk kez oluşturulmuşsa temizle.
  if (first_instance) {
    memset(shared_data, 0, SHM_SIZE);
    printf("Shared memory created and initialized.\n");
  } else {
    // mevcut shared memorye bağlanıldı tutarlılık kontrolü
    if (shared_data->process_count < 0 ||
        shared_data->process_count > MAX_PROCESSES) {
      printf("Shared memory corrupted — resetting.\n");
      memset(shared_data, 0, SHM_SIZE);
    }

    printf("Shared memory attached. Existing process count = %d\n",
           shared_data->process_count);
  }
}
// shm_open() yeni shared memory oluşturur bu RAM de yer açar ama içerik
// rastgele olur. o yüzden tüm ProcessInfo temizlenmeli.

int main(void) {
  shm_unlink(SHM_NAME); // multi-terminal modda silinecek.
  init_shared_memory();
  char command[256];

  while (true) {
    int choice = get_menu_choice();
    switch (choice) {
    case 0:
      printf("Shutting down ProcX...\n");
      return 0;
    case 1:
      printf("\n--- Launch New Process ---\n");
      start_process_menu();
      break;

    case 2:
      printf("\n--- Active Processes ---\n");
      // list_processes();
      break;
    case 3:
      printf("\n--- Terminate Process ---\n");
      // terminate_process_menu();
      break;
    }
  }

  return 0;
}
