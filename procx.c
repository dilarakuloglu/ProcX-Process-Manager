#include <stdio.h>      // printf, perror vs.
#include <stdlib.h>     // exit, malloc, free
#include <string.h>
#include <signal.h>     // pid_t, kill, sinyal tipleri
#include <unistd.h>     // fork, getpid, exec, pid_t
#include <sys/types.h>  // pid_t, key_t, mode_t (PID tanımı burada da var)
#include <time.h>       // time_t, time(), ctime()
#include <fcntl.h>      // O_CREAT, O_RDWR
#include <sys/mman.h>   // shm_open, mmap

typedef enum {
    ATTACHED = 0,
    DETACHED = 1
} ProcessMode;

typedef enum {
    RUNNING = 0,
    TERMINATED = 1
} ProcessStatus;

typedef struct {
    pid_t pid; // Process ID
    pid_t owner_pid; // Başlatan instance'ın PID'si
    char command[256]; // Çalıştırılan komut  // dinamik bellek yok çünkü başka procx instance’ı o pointer’ın işaret ettiği yeri göremez.
    ProcessMode mode; // Attached (0) veya Detached (1)
    ProcessStatus status; // Running (0) veya Terminated (1)
    time_t start_time; // Başlangıç zamanı
    int is_active; // Aktif mi? (1: Evet, 0: Hayır)
} ProcessInfo;

// Paylaşılan bellek yapısı
typedef struct {
    ProcessInfo processes[50]; // Maksimum 50 process  // bu PCB gibi iş görüyormuş.
    int process_count; // Aktif process sayısı
} SharedData;

// birden fazla terminalde çalışan ProcX in instanceları aynı process listesini görsün diye. Global state yani. peer-to-peer
// en kritik IPC tekniği , en hızlısı çünkü kopya 0 tüm instancelar aynı RAM den yürütülüyor o yüzden senkronizasyon lazım.

#define SHM_NAME "/procx_shm"
#define SHM_SIZE sizeof(SharedData)
SharedData *shared_data = NULL; // global pointer buradan RAM e erişecek.

// Mesaj yapısı
typedef struct {
    long msg_type; // Mesaj tipi
    int command; // Komut (START/TERMINATE)
    pid_t sender_pid; // Gönderen PID
    pid_t target_pid; // Hedef process PID
} Message;

// Shared memory senkronize veri tutar ama olayı bildirmez .Terminal 1 de işlem yapınca terminal 2 nin anında öğrenmesi için message queue lazım.

// O_CREATE -> yoksa oluştur ,O_RDWR ->hem oku hem yaz , 0666-> dosya izinleri
//shm_open() → bir shared memory nesnesi oluşturur. macos ta /var/run altında , eğer zaten varsa yeniden açar. bir dosya tanımlayıcısı "int fd" verir.
//int shm_open(const char *name, int oflag, mode_t mode);
//ftruncate() → boyut belirler , shared memory nesnesi dosya gibi olduğundan önce boyut belirlenmeli
//mmap() → RAM’e bağlar(pointer döner)
// void* mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
// Sistem çağrılarında hata aldıysan asla devam etme.

int parse_command(char *command, char **argv) {
    

}



void start_process(char *command, int mode) {
    pid_t pid = fork();

    if (pid<0) {
        perror("fork failed");
        return;
    }
    if (pid==0) {
        if (mode == DETACHED) {
            setsid();
        }

        //execvp(argv[0], argv);

    } else {
        // Parent process

        // shared_data ya eklenecek
    }
}



void init_shared_memory() {
    int fd =shm_open(SHM_NAME, O_CREAT | O_RDWR,0666 );
    if (fd == -1) {
        perror("shm_open failed");
        exit(1);
    }

    if (ftruncate(fd,SHM_SIZE)== -1) {
        perror("ftruncate failed");
        exit(1);
    }

    shared_data = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (shared_data == MAP_FAILED) {
        perror("mmap failed");
        exit(1);
    }

    // shm_open() yeni shared memory oluşturur bu RAM de yer açar ama içerik rastgele olur.
    // o yüzden tüm ProcessInfo temizlenmeli.

    if (shared_data->process_count < 0 || shared_data->process_count >50) {
        memset(shared_data, 0, SHM_SIZE);
    }
}

int main(void) {
    init_shared_memory();

    return 0;
}