#ifndef VM_VM_H
#define VM_VM_H
#include <hash.h>  // hash include
#include <stdbool.h>

#include "threads/palloc.h"

enum vm_type {
  /* page not initialized */
  VM_UNINIT = 0,
  /* page not related to the file, aka anonymous page */
  VM_ANON = 1,
  /* page that realated to the file */
  VM_FILE = 2,
  /* page that hold the page cache, for project 4 */
  VM_PAGE_CACHE = 3,

  /* Bit flags to store state */

  /* Auxillary bit flag marker for store information. You can add more
   * markers, until the value is fit in the int. */
  VM_MARKER_0 = (1 << 3),
  VM_MARKER_1 = (1 << 4),

  /* DO NOT EXCEED THIS VALUE. */
  VM_MARKER_END = (1 << 31),
};

#include "vm/anon.h"
#include "vm/file.h"
#include "vm/uninit.h"
#ifdef EFILESYS
#include "filesys/page_cache.h"
#endif

struct page_operations;
struct thread;

#define VM_TYPE(type) ((type) & 7)

/* The representation of "page".
 * This is kind of "parent class", which has four "child class"es, which are
 * uninit_page, file_page, anon_page, and page cache (project4).
 * DO NOT REMOVE/MODIFY PREDEFINED MEMBER OF THIS STRUCTURE. */
struct page {
  const struct page_operations *operations;
  void *va;            /* Address in terms of user space */
  struct frame *frame; /* Back reference for frame */

  /* Your implementation */

  /* Per-type data are binded into the union.
   * Each function automatically detects the current union */
  union {
    struct uninit_page uninit;
    struct anon_page anon;
    struct file_page file;
#ifdef EFILESYS
    struct page_cache page_cache;
#endif
  };
  struct hash_elem hash_elem;  // hash_elem 추가
  bool writable;               // bool 변수 추기
  bool accessible;
};

/* The representation of "frame" */
struct frame {
  void *kva;                    // 커널 가상 주소 (실제 물리 메모리를 가리킴)
  struct page *page;            // 이 프레임을 사용하는 페이지
  struct list_elem frame_elem;  // frame_table에 들어갈 때 사용
};

/* The function table for page operations.
 * This is one way of implementing "interface" in C.
 * Put the table of "method" into the struct's member, and
 * call it whenever you needed. */
struct page_operations {
  bool (*swap_in)(struct page *, void *);
  bool (*swap_out)(struct page *);
  void (*destroy)(struct page *);
  enum vm_type type;
};

#define swap_in(page, v) (page)->operations->swap_in((page), v)
#define swap_out(page) (page)->operations->swap_out(page)
#define destroy(page) \
  if ((page)->operations->destroy) (page)->operations->destroy(page)

/* Representation of current process's memory space.
 * We don't want to force you to obey any specific design for this struct.
 * All designs up to you for this. */

struct supplemental_page_table {
  struct hash spt_hash;  // 해시 구조체 추가
};

/* process.c load_segment의 vm_alloc_page_with_initializer의 네 번째 인자인
 * lazy_load_segment 함수에 전달될 인자 aux구조체 */
struct file_loader {
  size_t page_read_bytes;
  size_t page_zero_bytes;
  off_t ofs;
  struct file *file;
};

#include "threads/thread.h"
void supplemental_page_table_init(struct supplemental_page_table *spt);
bool supplemental_page_table_copy(struct supplemental_page_table *dst,
                                  struct supplemental_page_table *src);
void supplemental_page_table_kill(struct supplemental_page_table *spt);
struct page *spt_find_page(struct supplemental_page_table *spt,
                           void *va);
bool spt_insert_page(struct supplemental_page_table *spt, struct page *page);
void spt_remove_page(struct supplemental_page_table *spt, struct page *page);

void vm_init(void);
bool vm_try_handle_fault(struct intr_frame *f, void *addr, bool user,
                         bool write, bool not_present);

#define vm_alloc_page(type, upage, writable) \
  vm_alloc_page_with_initializer((type), (upage), (writable), NULL, NULL)
bool vm_alloc_page_with_initializer(enum vm_type type, void *upage,
                                    bool writable, vm_initializer *init, void *aux);
void vm_dealloc_page(struct page *page);
bool vm_claim_page(void *va);
enum vm_type page_get_type(struct page *page);

uint64_t page_hash(const struct hash_elem *e, void *aux);  // 선언
bool page_less(const struct hash_elem *a, const struct hash_elem *b, void *aux);
void hash_action_destroy(struct hash_elem *hash_elem_, void *aux);
bool is_valid_stack_access(void *addr, uintptr_t rsp);
void vm_stack_growth(void *addr);
#endif /* VM_VM_H */