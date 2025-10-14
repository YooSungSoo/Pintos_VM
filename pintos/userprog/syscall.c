#include "userprog/syscall.h"

#include <stdio.h>
#include <syscall-nr.h>

#include "devices/input.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "filesys/off_t.h"
#include "intrinsic.h"
#include "string.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/loader.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/gdt.h"
#include "userprog/process.h"
#include "vm/vm.h"

#define FDT_SIZE 512
typedef int pid_t;

void syscall_entry(void);
void syscall_handler(struct intr_frame*);

/* System call function declarations */
void exit(int status);
bool create(const char* file, unsigned initial_size);
bool remove(const char* file);
int open(const char* file);
int filesize(int fd);
int read(int fd, void* buffer, unsigned size);
int write(int fd, const void* buffer, unsigned size);
void seek(int fd, unsigned position);
unsigned tell(int fd);
void close(int fd);
bool copy_in(void* dst, const void* usrc, size_t size);
bool copy_in_string(char* dst, const char* us, size_t dst_sz, size_t* out_len);
struct lock filesys_lock;
int exec(const char* cmd_line);
pid_t fork(const char* thread_name, struct intr_frame* if_);
int wait(pid_t pid);
int dup2(int oldfd, int newfd);
void* mmap(void* addr, size_t length, int writable, int fd, off_t offset);
void munmap(void* addr);
void* check_and_get_page(const void* uaddr);

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void syscall_init(void) {
  write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48 | ((uint64_t)SEL_KCSEG)
                                                               << 32);
  write_msr(MSR_LSTAR, (uint64_t)syscall_entry);
  write_msr(MSR_SYSCALL_MASK,
            FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
  lock_init(&filesys_lock);
}

/* The main system call interface */
void syscall_handler(struct intr_frame* f UNUSED) {
  thread_current()->user_rsp = (void*)f->rsp;
  int syscall_number = (int)f->R.rax;

  switch (syscall_number) {
    case SYS_HALT: {
      power_off();
      break;
    }
    case SYS_EXIT: {
      int status = (int)f->R.rdi;
      exit(status);
      break;
    }
    case SYS_WRITE: {
      f->R.rax =
          write((int)f->R.rdi, (const void*)f->R.rsi, (unsigned)f->R.rdx);
      break;
    }
    case SYS_READ: {
      f->R.rax = read((int)f->R.rdi, (void*)f->R.rsi, (unsigned)f->R.rdx);
      break;
    }
    case SYS_SEEK: {
      int fd = (int)f->R.rdi;
      unsigned position = (unsigned)f->R.rsi;
      seek(fd, position);
      break;
    }
    case SYS_CREATE: {
      f->R.rax = create((const char*)f->R.rdi, (unsigned)f->R.rsi);
      break;
    }
    case SYS_REMOVE: {
      f->R.rax = remove((const char*)f->R.rdi);
      break;
    }
    case SYS_FILESIZE: {
      f->R.rax = filesize((int)f->R.rdi);
      break;
    }
    case SYS_TELL: {
      int fd = (int)f->R.rdi;
      f->R.rax = tell(fd);
      break;
    }
    case SYS_EXEC: {
      exec((const char*)f->R.rdi);
      break;
    }
    case SYS_OPEN: {
      f->R.rax = open((const char*)f->R.rdi);
      break;
    }
    case SYS_CLOSE: {
      close((int)f->R.rdi);
      break;
    }
    case SYS_FORK: {
      f->R.rax = fork((const char*)f->R.rdi, f);
      break;
    }
    case SYS_WAIT: {
      pid_t pid = (pid_t)f->R.rdi;
      f->R.rax = wait(pid);
      break;
    }
    case SYS_DUP2: {
      int oldfd = (int)f->R.rdi;
      int newfd = (int)f->R.rsi;
      f->R.rax = dup2(oldfd, newfd);
      break;
    }
    case SYS_MMAP: {
      void* addr = (void*)f->R.rdi;
      off_t length = (off_t)f->R.rsi;
      int writable = (int)f->R.rdx;
      int fd = (int)f->R.r10;
      off_t offset = (off_t)f->R.r8;

      // 1. fd 유효성 검사
      if (fd < 2 || fd >= FDT_SIZE) {
        f->R.rax = 0;  // MAP_FAILED
        break;
      }

      // 2. 현재 스레드의 fdt에서 file 가져오기
      struct thread* curr = thread_current();
      struct file* file = curr->fdt[fd];

      // 3. file 유효성 검사
      if (file == NULL || file == STDIN_MARKER || file == STDOUT_MARKER) {
        f->R.rax = 0;  // MAP_FAILED
        break;
      }

      // 4. file_reopen으로 독립적인 파일 참조 생성
      struct file* reopened_file = file_reopen(file);
      if (reopened_file == NULL) {
        f->R.rax = 0;  // MAP_FAILED
        break;
      }

      f->R.rax = (uintptr_t)do_mmap(addr, length, writable, reopened_file, offset);
      break;
    }
    case SYS_MUNMAP: {
      do_munmap((void*)f->R.rdi);
      break;
    }
    default: {
      printf("system call 오류 : 알 수 없는 시스템콜 번호 %d\n",
             syscall_number);
      thread_exit();
    }
  }
}

void exit(int status) {
  struct thread* curr = thread_current();
#ifdef USERPROG
  curr->exit_status = status;
  printf("%s: exit(%d)\n", curr->name, curr->exit_status);
#endif
  thread_exit();
}

bool create(const char* file, unsigned initial_size) {
  if (file == NULL) {
    exit(-1);
  }

  char fname[NAME_MAX + 1];
  size_t fname_len = 0;

  if (!copy_in_string(fname, file, sizeof fname, &fname_len)) {
    return false;
  }

  fname[fname_len] = '\0';

  if (fname_len == 0) {
    return false;
  }
  lock_acquire(&filesys_lock);
  bool ok = filesys_create(fname, initial_size);
  lock_release(&filesys_lock);

  return ok;
}

bool remove(const char* file) {
  if (file == NULL) {
    exit(-1);
  }

  char fname[NAME_MAX + 1];
  size_t fname_len = 0;

  if (!copy_in_string(fname, file, sizeof fname, &fname_len)) {
    return false;
  }

  fname[fname_len] = '\0';

  if (fname_len == 0) {
    return false;
  }

  bool ok = filesys_remove(fname);
  return ok;
}

void seek(int fd, unsigned position) {
  if (!fd || fd < 2 || fd >= FDT_SIZE) return;

  struct thread* curr = thread_current();
  struct file* file = curr->fdt[fd];

  if (file == NULL) return;

  lock_acquire(&filesys_lock);
  file_seek(file, position);
  lock_release(&filesys_lock);
}

unsigned tell(int fd) {
  if (!fd || fd < 2 || fd >= FDT_SIZE) return -1;

  struct thread* curr = thread_current();
  struct file* file = curr->fdt[fd];

  if (file == NULL) return -1;

  return file_tell(file);
}

int write(int fd, const void* buffer, unsigned size) {
  if (fd < 0 || fd >= FDT_SIZE) return -1;
  if ((size == 0) || (buffer == NULL)) return 0;

  const size_t CHUNK_SIZE = 4096;

  struct thread* curr = thread_current();
  struct file* file = curr->fdt[fd];
  if (file == NULL || file == STDIN_MARKER) return -1;

  if (file == STDOUT_MARKER) {
    // 큰 데이터는 청크 단위로 나누어 출력

    size_t remaining = size;
    size_t offset = 0;

    while (remaining > 0) {
      size_t chunk_size = remaining > CHUNK_SIZE ? CHUNK_SIZE : remaining;

      void* kbuff = palloc_get_page(PAL_ZERO);
      if (kbuff == NULL) {
        exit(-1);
      }

      if (!copy_in(kbuff, (const char*)buffer + offset, chunk_size)) {
        palloc_free_page(kbuff);
        exit(-1);
      }

      putbuf(kbuff, chunk_size);
      palloc_free_page(kbuff);

      offset += chunk_size;
      remaining -= chunk_size;
    }
    return size;
  }
  size_t remaining = size;
  size_t offset = 0;
  int total_written = 0;

  while (remaining > 0) {
    size_t chunk_size = remaining > CHUNK_SIZE ? CHUNK_SIZE : remaining;

    void* kbuff = palloc_get_page(PAL_ZERO);
    if (kbuff == NULL) {
      exit(-1);
    }

    if (!copy_in(kbuff, (const char*)buffer + offset, chunk_size)) {
      palloc_free_page(kbuff);
      exit(-1);
    }
    lock_acquire(&filesys_lock);
    int bytes_written = file_write(file, kbuff, chunk_size);
    lock_release(&filesys_lock);

    palloc_free_page(kbuff);

    if (bytes_written != chunk_size) {
      total_written += bytes_written;
      break;
    }

    total_written += bytes_written;
    offset += chunk_size;
    remaining -= chunk_size;
  }

  return total_written;
}

int read(int fd, void* buffer, unsigned size) {
  if (!fd || fd < 0 || fd >= FDT_SIZE) return -1;
  int bytes_read = 0;
  struct thread* curr = thread_current();
  struct file* file = curr->fdt[fd];

  if (file == NULL || file == STDOUT_MARKER) return -1;

  if (file == STDIN_MARKER) {
    // stdin에서 읽기 전에 버퍼 유효성 검사
    for (unsigned i = 0; i < size; i++) {
      if (!is_user_vaddr((uint8_t*)buffer + i)) {
        exit(-1);
      }
      if (check_and_get_page((uint8_t*)buffer + i) == NULL) {
        exit(-1);
      }
    }

    // stdin에서 읽기
    for (unsigned i = 0; i < size; i++) {
      *((uint8_t*)buffer + i) = (uint8_t)input_getc();
    }
    bytes_read = size;
  } else {
    // 잘못된 fd인 경우 리턴

    // 버퍼가 유효한 사용자 주소인지 확인
    for (unsigned i = 0; i < size; i++) {
      if (!is_user_vaddr((uint8_t*)buffer + i)) {
        exit(-1);
      }
      if (check_and_get_page((uint8_t*)buffer + i) == NULL) {
        exit(-1);
      }
    }

    // 버퍼가 writable인지 확인 (페이지 단위로)
    for (unsigned i = 0; i < size; i += PGSIZE) {
      void* page_addr = pg_round_down((uint8_t*)buffer + i);
      uint64_t* pte = pml4e_walk(curr->pml4, (uint64_t)page_addr, 0);

      // PTE가 없거나 writable이 아니면 종료!
      if (pte == NULL || !(*pte & PTE_W)) {
        exit(-1);
      }
    }

    // 마지막 바이트가 속한 페이지도 확인
    if (size > 0) {
      void* last_page = pg_round_down((uint8_t*)buffer + size - 1);
      void* first_page = pg_round_down(buffer);

      if (last_page != first_page) {
        uint64_t* pte = pml4e_walk(curr->pml4, (uint64_t)last_page, 0);
        if (pte == NULL || !(*pte & PTE_W)) {
          exit(-1);
        }
      }
    }

    // file_read() 함수 호출
    lock_acquire(&filesys_lock);
    bytes_read = file_read(file, buffer, size);
    lock_release(&filesys_lock);
  }

  return bytes_read;
}

int open(const char* file) {
  char kname[NAME_MAX + 1];
  size_t len = 0;

  if (!copy_in_string(kname, file, sizeof kname, &len)) {
    return -1;
  }

  kname[len] = '\0';

  lock_acquire(&filesys_lock);
  struct file* f = filesys_open(kname);
  lock_release(&filesys_lock);

  if (f == NULL) {
    return -1;
  }

  struct thread* curr = thread_current();

  for (int fd = 2; fd < FDT_SIZE; fd++) {
    if (curr->fdt[fd] == NULL) {
      curr->fdt[fd] = f;
      return fd;
    }
  }

  file_close(f);
  return -1;
}

int filesize(int fd) {
  if (!fd || fd < 2 || fd >= FDT_SIZE) return -1;

  struct thread* curr = thread_current();
  struct file* file = curr->fdt[fd];

  if (file == NULL) return -1;

  return file_length(file);
}

void close(int fd) {
  if (fd < 2 || fd >= FDT_SIZE) {
    return;
  }

  struct thread* curr = thread_current();
  struct file* file = curr->fdt[fd];

  if (file == NULL) {
    return;
  }

  if (file == STDIN_MARKER || file == STDOUT_MARKER) {
    curr->fdt[fd] = NULL;
    return;
  }

  if (file_should_close(file)) {
    file_close(file);
  }

  curr->fdt[fd] = NULL;
}

// 반환값이 의미없긴 한데 introduction에 맞춰서 int로 설정
int exec(const char* cmd_line) {
  char kernel_file[256];
  size_t i = 0;

  if (!copy_in_string(kernel_file, cmd_line, sizeof kernel_file, &i)) {
    exit(-1);
  }
  kernel_file[i] = '\0';

  process_exec(kernel_file);
}

/* 유저 포인터 `usrc`로부터 size 바이트를 커널 버퍼 `dst`로 복사한다.
   성공하면 true, 실패하면 false를 반환한다. */
bool copy_in(void* dst, const void* usrc, size_t size) {
  for (size_t i = 0; i < size; i++) {
    const void* user_addr = (const char*)usrc + i;

    if (!is_user_vaddr(user_addr)) {
      return false;
    }

    void* kva = check_and_get_page(user_addr);
    if (kva == NULL) {
      return false;
    }

    ((char*)dst)[i] = *(char*)kva;
  }
  return true;
}

/*
 * copy_in_string()
 * - 유저 포인터 us가 가리키는 NUL-종단 문자열을 커널 버퍼 dst로 복사한다.
 * - 페이지 경계마다 검증(pml4_get_page)하며 잘못된 포인터/매핑 실패 시
 * exit(-1).
 * - dst_sz 바이트 안에서 반드시 '\0'을 만나야 하며, 만나지 못하면 false를
 * 반환(과다 길이).
 * - 성공 시 true를 반환하고, out_len가 비-NULL이면 NUL 제외 길이를 기록한다.
 * - 64비트 주소 체계를 가정한다.
 */
bool copy_in_string(char* dst, const char* us, size_t dst_sz, size_t* out_len) {
  /* (1) 파라미터 가드 */
  if (dst == NULL || dst_sz == 0) return false;
  if (us == NULL || !is_user_vaddr(us)) exit(-1);  // bad ptr → 종료

  struct thread* curr = thread_current();

  /* (2) 바이트 단위 복사 */
  for (size_t i = 0; i < dst_sz; i++) {
    const char* up = (const char*)us + i;

    if (!is_user_vaddr(up)) exit(-1);  // 커널 경계 넘어가면 종료
    char* kva = check_and_get_page(up);
    if (kva == NULL) exit(-1);  // 미매핑 → 종료

    char c = *kva;  // 안전한 한 바이트 로드
    dst[i] = c;     // 커널 버퍼에 기록

    if (c == '\0') {
      // 문자열 끝
      if (out_len) *out_len = i;  // 널 전까지 길이
      return true;
    }
  }

  /* (3) 버퍼 초과: NUL을 못 만남 */
  return false;
}

pid_t fork(const char* thread_name, struct intr_frame* if_) {
  // 1. 주소 유효성 검사
  if (thread_name == NULL || !is_user_vaddr(thread_name) ||
      !pml4_get_page(thread_current()->pml4, thread_name)) {
    exit(-1);
  }

  // 2. 전체 문자열 유효성 검사
  int len = 0;
  int MAX_LEN = 16;  // 최대 길이 제한(16자)
  while (len < MAX_LEN) {
    if (!is_user_vaddr(thread_name + len) ||
        !pml4_get_page(thread_current()->pml4, thread_name + len)) {
      exit(-1);
    }
    if (thread_name[len] == '\0') break;
    len++;
  }

  // 3. 자식 프로세스 생성 (올바른 인터럽트 프레임 전달)
  pid_t child_pid = process_fork(thread_name, if_);

  return child_pid;
}

int wait(pid_t pid) { return process_wait(pid); }

int dup2(int oldfd, int newfd) {
  if (oldfd < 0 || oldfd >= FDT_SIZE) return -1;
  if (newfd < 0 || newfd >= FDT_SIZE) return -1;

  if (oldfd == newfd) return newfd;
  struct thread* curr = thread_current();

  if (curr->fdt[newfd] != NULL) close(newfd);

  struct file* file = curr->fdt[oldfd];
  if (file == NULL) return -1;

  curr->fdt[newfd] = file;

  if (file != STDIN_MARKER && file != STDOUT_MARKER) {
    file_add_ref(file);
  }

  return newfd;
}

void* check_and_get_page(const void* uaddr) {
  // 1. User 주소 범위 확인
  if (!is_user_vaddr(uaddr)) {
    return NULL;
  }

  struct thread* curr = thread_current();

  // 2. 페이지 매핑 확인
  void* kva = pml4_get_page(curr->pml4, uaddr);

  // 3. 매핑되어 있으면 바로 반환
  if (kva != NULL) {
    return kva;
  }

  // SPT에서 페이지 찾기
  void* page_addr = pg_round_down(uaddr);
  struct page* page = spt_find_page(&curr->spt, page_addr);

  if (page != NULL) {
    // SPT에 있음 -> Lazy loading 필요
    if (vm_claim_page(page_addr)) {
      kva = pml4_get_page(curr->pml4, uaddr);
      return kva;
    }
    // Claim 실패
    return NULL;
  }

  // SPT에서 못찾으면 stack growth 시도
  if (is_valid_stack_access(page_addr, (uintptr_t)curr->user_rsp)) {
    vm_stack_growth(page_addr);
    kva = pml4_get_page(curr->pml4, uaddr);
    return kva;
  }

  return NULL;  // 모두 실패
}