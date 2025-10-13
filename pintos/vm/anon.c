/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "devices/disk.h"
#include "kernel/bitmap.h"
#include "threads/vaddr.h"
#include "vm/vm.h"

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static bool anon_swap_in(struct page *page, void *kva);
static bool anon_swap_out(struct page *page);
static void anon_destroy(struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
    .swap_in = anon_swap_in,
    .swap_out = anon_swap_out,
    .destroy = anon_destroy,
    .type = VM_ANON,
};

struct swap_anon *find_blank_swap();

/* 디스크에서 사용 가능한 swap slot과 사용 불가능한 swap slot을
  관리하는 자료구조
  bitmap 구조체 사용
    - bit를 저장하는 연속된 메모리 공간 위의 배열 객체
    - 각각의 비트가 swap_slot과 매칭, 1인 경우 swap out되어
    디스크의 swap 공간에 임시 저장되었다는 뜻 */
struct bitmap *swap_table;

/* PGSZIE == 1<<12byte (4kb) / DISK_SECTOR_SIZE == 512byte
  SECTOR_PER_PAGE == 8 */
const size_t SECTOR_PER_PAGE = PGSIZE / DISK_SECTOR_SIZE;

struct list swap_list;

/* Initialize the data for anonymous pages */
void vm_anon_init(void) {
  /* TODO: Set up the swap_disk. */
  list_init(&swap_list);
  swap_disk = disk_get(1, 1);
  disk_sector_t max_sector_size = disk_size(swap_disk);
  for (int i = 0; i < max_sector_size; i += SECTOR_PER_PAGE) {
    struct swap_anon *swap_anon = malloc(sizeof(struct swap_anon));
    swap_anon->page = NULL;
    swap_anon->use = false;
    for (int j = 0; j < SECTOR_PER_PAGE; j++) {
      swap_anon->sector[j] = i + j;
    }
    list_push_back(&swap_list, &swap_anon->swap_elem);
  }
}

/* Initialize the file mapping */
bool anon_initializer(struct page *page, enum vm_type type, void *kva) {
  /* Set up the handler */
  page->operations = &anon_ops;
  struct anon_page *anon_page = &page->anon;

  return true;
}

/* Swap in the page by read contents from the swap disk. */
static bool anon_swap_in(struct page *page, void *kva) {
  struct anon_page *anon_page = &page->anon;
  struct swap_anon *swap_anon = anon_page->swap_anon;
  for (int i = 0; i < SECTOR_PER_PAGE; i++) {
    disk_read(swap_disk, swap_anon->sector[i], (uint8_t *)kva + DISK_SECTOR_SIZE * i);
  }
  anon_page->swap_anon = NULL;
  swap_anon->use = false;

  return true;
}

struct swap_anon *find_blank_swap() {
  struct list_elem *e = list_head(&swap_list);
  while ((e = list_next(e)) != list_end(&swap_list)) {
    struct swap_anon *swap_anon = list_entry(e, struct swap_anon, swap_elem);
    if (!swap_anon->use) {
      return swap_anon;
    }
  }
  return NULL;
}

/* Swap out the page by writing contents to the swap disk.
ANON 페이지를 스왑 아웃한다.
디스크에 백업 파일이 없으므로, 디스크 상에 스왑 공간을 만들어 그곳에 페이지를
저장한다.*/
static bool anon_swap_out(struct page *page) {
  struct anon_page *anon_page = &page->anon;
  struct swap_anon *swap_anon = find_blank_swap();
  for (int i = 0; i < SECTOR_PER_PAGE; i++) {
    disk_write(swap_disk, swap_anon->sector[i], (uint8_t *)page->frame->kva + DISK_SECTOR_SIZE * i);
  }

  swap_anon->use = true;
  anon_page->swap_anon = swap_anon;
  pml4_clear_page(thread_current()->pml4, page->va);
  page->frame = NULL;

  return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy(struct page *page) {
  struct anon_page *anon_page = &page->anon;
}