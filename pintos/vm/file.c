/* file.c: Implementation of memory backed file object (mmaped object). */

#include "filesys/file.h"  // file_read_at, file_write_at

#include "threads/malloc.h"
#include "threads/synch.h"     // filesys_lock
#include "threads/thread.h"    // thread_current(), pml4
#include "threads/vaddr.h"     // PGSIZE
#include "userprog/process.h"  // struct vm_load_arg
#include "vm/vm.h"

static bool file_backed_swap_in(struct page *page, void *kva);
static bool file_backed_swap_out(struct page *page);
static void file_backed_destroy(struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
    .swap_in = file_backed_swap_in,
    .swap_out = file_backed_swap_out,
    .destroy = file_backed_destroy,
    .type = VM_FILE,
};

/* The initializer of file vm */
void vm_file_init(void) {
}

/* Initialize the file backed page */
bool file_backed_initializer(struct page *page, enum vm_type type, void *kva) {
  /* Set up the handler */
  page->operations = &file_ops;

  struct file_page *file_page = &page->file;

  struct file_page *aux = (struct file_page *)page->uninit.aux;
  file_page->file = aux->file;
  file_page->offset = aux->offset;
  file_page->page_read_bytes = aux->page_read_bytes;

  return true;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in(struct page *page, void *kva) {
  struct file_page *file_page UNUSED = &page->file;

  int read = file_read_at(file_page->file, page->frame->kva, file_page->page_read_bytes, file_page->offset);
  memset(page->frame->kva + read, 0, PGSIZE - read);
  return true;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out(struct page *page) {
  struct file_page *file_page UNUSED = &page->file;

  struct frame *frame = page->frame;

  if (pml4_is_dirty(thread_current()->pml4, page->va)) {
    file_write_at(file_page->file, page->frame->kva, file_page->page_read_bytes, file_page->offset);
    pml4_set_dirty(thread_current()->pml4, page->va, false);
  }
  page->frame->page = NULL;
  page->frame = NULL;
  pml4_clear_page(thread_current()->pml4, page->va);
  return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy(struct page *page) {
  struct file_page *file_page UNUSED = &page->file;

  if (pml4_is_dirty(thread_current()->pml4, page->va)) {
    file_write_at(file_page->file, page->va, file_page->page_read_bytes, file_page->offset);
    pml4_set_dirty(thread_current()->pml4, page->va, false);
  }

  if (page->frame) {
    list_remove(&page->frame->frame_elem);
    page->frame->page = NULL;
    page->frame = NULL;
  }

  pml4_clear_page(thread_current()->pml4, page->va);
}

/* Do the mmap */
void *do_mmap(void *addr, off_t length, int writable,
              struct file *file, off_t offset) {
  // 1. 기본 검증
  if (addr == NULL || pg_ofs(addr) != 0) {
    return NULL;  // page-aligned가 아님
  }

  if (length == 0) {
    return NULL;
  }

  if (file == NULL) {
    return NULL;
  }

  // 2. 파일 길이 확인
  off_t file_len = file_length(file);
  if (file_len == 0) {
    return NULL;
  }

  // 3. 매핑할 페이지 수 계산
  size_t page_count = (length + PGSIZE - 1) / PGSIZE;

  // 4. 기존 매핑과 겹치는지 확인
  struct thread *curr = thread_current();
  for (size_t i = 0; i < page_count; i++) {
    void *check_addr = addr + (i * PGSIZE);
    if (spt_find_page(&curr->spt, check_addr) != NULL) {
      return NULL;  // 이미 매핑된 페이지가 있음
    }
  }

  // 5. mmap_region 생성 및 리스트에 추가
  struct mmap_region *region = malloc(sizeof(struct mmap_region));
  if (region == NULL) {
    return NULL;
  }

  region->start_addr = addr;
  region->page_count = page_count;
  region->file = file;  // file_reopen으로 받은 파일
  list_push_back(&curr->mmap_list, &region->elem);

  // 6. 각 페이지를 lazy하게 할당
  off_t current_offset = offset;
  off_t remaining = length;

  for (size_t i = 0; i < page_count; i++) {
    void *page_addr = addr + (i * PGSIZE);

    // 이 페이지에서 읽을 바이트 수 계산
    off_t read_bytes = remaining > PGSIZE ? PGSIZE : remaining;
    off_t zero_bytes = PGSIZE - read_bytes;

    // aux 구조체 생성 (file_backed_initializer에 전달)
    struct file_loader *aux = malloc(sizeof(struct file_loader));
    if (aux == NULL) {
      goto rollback;
    }

    aux->file = file;
    aux->ofs = current_offset;
    aux->page_read_bytes = read_bytes;
    aux->page_zero_bytes = zero_bytes;

    // VM_FILE 타입으로 페이지 할당 (lazy)
    if (!vm_alloc_page_with_initializer(VM_FILE, page_addr, writable,
                                        NULL, aux)) {
      free(aux);
      goto rollback;
                                        }

    current_offset += read_bytes;
    remaining -= read_bytes;
  }

  // ReSharper disable once CppDFAMemoryLeak
  return addr;

rollback:
  // 실패 시 이미 할당된 페이지들 정리
  for (size_t j = 0; j < page_count; j++) {
    void *page_addr = addr + (j * PGSIZE);
    struct page *page = spt_find_page(&curr->spt, page_addr);
    if (page != NULL) {
      spt_remove_page(&curr->spt, page);
      free(page->uninit.aux);  // aux 메모리 해제
      free(page);
    }
  }

  // region 정리
  list_remove(&region->elem);
  free(region);

  return NULL;
}

/* Do the munmap */
void do_munmap(void *addr) {
  struct thread *curr = thread_current();

  // 1. mmap_list에서 해당 addr의 region 찾기
  struct mmap_region *region = NULL;
  struct list_elem *e;

  for (e = list_begin(&curr->mmap_list);
       e != list_end(&curr->mmap_list);
       e = list_next(e)) {
    struct mmap_region *r = list_entry(e, struct mmap_region, elem);
    if (r->start_addr == addr) {
      region = r;
      break;
    }
       }

  if (region == NULL) {
    return;  // 해당 주소에 매핑이 없음
  }

  // 2. 모든 페이지를 해제
  for (size_t i = 0; i < region->page_count; i++) {
    void *page_addr = addr + (i * PGSIZE);
    struct page *page = spt_find_page(&curr->spt, page_addr);

    if (page != NULL) {
      // destroy 호출 (file_backed_destroy가 dirty 체크 & write back 수행)
      destroy(page);
      // SPT에서 제거
      spt_remove_page(&curr->spt, page);
      // page 메모리 해제
      free(page);
    }
  }

  // 3. 파일 닫기
  file_close(region->file);

  // 4. region을 리스트에서 제거 및 메모리 해제
  list_remove(&region->elem);
  free(region);
}