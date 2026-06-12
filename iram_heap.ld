    /* Heap WAMR en IRAM0 — libère 192 KB de DRAM
     * iram0_0_seg : 415 KB / 43 KB utilisés = 372 KB libres
     * Sans bloc SECTIONS {} : Zephyr insère ce contenu dans son propre SECTIONS {}
     */
    .wamr_iram_heap (NOLOAD) :
    {
        *(.wamr_iram_heap)
        *(.wamr_iram_heap.*)
    } > iram0_0_seg
