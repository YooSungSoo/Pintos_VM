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
static bool anon_swap_in(struct page *page, void *kva) {
  struct anon_page *anon_page = &page->anon;

  /* anon_page 구조체 안에 swap_out된 페이지가 저장된 디스크 스왑 영역 저장되어
   * 있음 */
  int page_no = anon_page->swap_index;

  /* 해당 스왑 슬롯 사용 여부 검증 */
  if (bitmap_test(swap_table, page_no) == false) {
    return false;
  }

  /* 해당 스왑 영역의 데이터를 가상 주소 공간 kva에 write */
  for (int i = 0; i < SECTOR_PER_PAGE; ++i) {
    disk_read(swap_disk, page_no * SECTOR_PER_PAGE + i,
              (uint8_t *)kva + DISK_SECTOR_SIZE * i);
  }

  /* 해당 swap slot = false */
  bitmap_set(swap_table, page_no, false);
  anon_page->swap_index = -1;
  return true;
}

/* Swap out the page by writing contents to the swap disk.
ANON 페이지를 스왑 아웃한다.
디스크에 백업 파일이 없으므로, 디스크 상에 스왑 공간을 만들어 그곳에 페이지를
저장한다.*/
static bool anon_swap_out(struct page *page) {
  struct anon_page *anon_page = &page->anon;

  /* bitmap table을 순회해 false값을 가진 비트(페이지 할당이 가능한 swap_slot)
   * 검색 */
  int page_no = bitmap_scan(swap_table, 0, 1, false);

  if (page_no == BITMAP_ERROR) {
    return false;
  }

  /* 한 페이지를 디스크에 써 주기 위해 SECTORS_PER_PAGE개의 섹터에 저장
  이 때, 디스크에 각 섹터의 크기 DISK_SECTOR_SIZE만큼 써 줌 */
  uint8_t *src = (uint8_t *)page->frame->kva;
  for (int i = 0; i < SECTOR_PER_PAGE; ++i) {
    disk_write(swap_disk, page_no * SECTOR_PER_PAGE + i,
               src + DISK_SECTOR_SIZE * i);
  }

  /* swap_table의 해당 페이지에 대한 swap slot의 비트 true,
    해당 페이지의 PTE에서 present bit 0으로 설정
    이후 프로세스가 이 페이지에 접근하면 page fault가 뜸 */
  bitmap_set(swap_table, page_no, true);
  pml4_clear_page(thread_current()->pml4, page->va);

  /* 페이지의 swap_index 값을 이 페이지가 저장된 swap slot으로 바꿔줌 */
  anon_page->swap_index = page_no;

  return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy(struct page *page) {
  struct anon_page *anon_page = &page->anon;
}