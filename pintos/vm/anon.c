/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "devices/disk.h"
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

/* Initialize the data for anonymous pages */
void vm_anon_init(void) {
  /* TODO: Set up the swap_disk. */
  swap_disk = disk_get(1, 1);
  size_t swap_size = disk_size(swap_disk) / SECTOR_PER_PAGE;
  swap_table = bitmap_create(swap_size);
}

/* Initialize the file mapping */
bool anon_initializer(struct page *page, enum vm_type type, void *kva) {
  /* page struct 안의 union영역은 현재 uninit page
    ANON page 0초기화 -> union영역 0초기화됨*/
  struct uninit_page *uninit = &page->uninit;
  memset(uninit, 0, sizeof(struct uninit_page));

  /* Set up the handler */
  /* 해당 페이지는 이제 ANON*/
  page->operations = &anon_ops;

  /* 해당 페이지는 아직 물리 메모리 위에 있음 -> swap_index값 -1로 설정*/
  struct anon_page *anon_page = &page->anon;
  anon_page->swap_index = -1;

  return true;
}

/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in(struct page *page, void *kva) {
  struct anon_page *anon_page = &page->anon;
}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out(struct page *page) {
  struct anon_page *anon_page = &page->anon;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy(struct page *page) {
  struct anon_page *anon_page = &page->anon;
}