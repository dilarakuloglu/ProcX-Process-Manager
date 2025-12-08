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

void start_process(char *command, int mode);
int parse_command(char *command, char **argv);

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

// ---- INSIDE main() LOOP ----

void start_process(char *command, int mode) {
  char *argv[20];
  int child_status;
  int arguman_count = parse_command(command, argv);
  if (arguman_count == 0) {
    printf("COMMAND NOT FOUND !");
    return;
  }
  printf("main process : argv[0] = '%s'\n", argv[0]);
  printf("argv[1] = '%s'\n", argv[1]);

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
    printf("%d\n", shared_data->processes[process_idx].pid);
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
    printf(" Process :count %d\n", shared_data->process_count);
  }
}

void init_shared_memory() {
  int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
  if (fd == -1) {
    perror("shm_open failed");
    exit(1);
  }

  if (ftruncate(fd, SHM_SIZE) == -1) {
    perror("ftruncate failed");
    exit(1);
  }

  shared_data = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  if (shared_data == MAP_FAILED) {
    perror("mmap failed");
    exit(1);
  }

  // shm_open() yeni shared memory oluşturur bu RAM de yer açar ama içerik
  // rastgele olur. o yüzden tüm ProcessInfo temizlenmeli.

  memset(shared_data, 0, SHM_SIZE);
}

int main(void) {
  shm_unlink("/procx_shm");
  // eski bozuk shared memory objesini siliyor, ftruncate Invalid argument
  // hatası bu sayede kaybolmuş oldu.
  init_shared_memory();
  char command[256];
  while (true) {
    ProcessMode mode = ATTACHED;
    printf("ENTER A COMMAND : "); // sleep 5
    if (fgets(command, sizeof(command), stdin) == NULL) {
      printf("\nExiting ProcX...\n");
      break; // EOF
    }
    size_t len = strlen(command);
    if (len > 0 && command[len - 1] == '\n') {
      command[len - 1] = '\0';
      len--;
    }
    trim(command); // baştaki ve sondaki boşluklar silinecek. "   sleep 5"

    if (command[0] == '\0')
      continue;

    // detached kontrolü
    len = strlen(command);
    if (len > 0 && command[len - 1] == '&') {
      mode = DETACHED;
      command[len - 1] = '\0';
      trim(command); // "sleep 2   & " de & kaldırıltsan sonra fazla boşluklar
                     // silinecek.
    }

    if (command[0] == '\0')
      continue;

    start_process(command, mode);
  }
  return 0;
}
