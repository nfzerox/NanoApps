/* 3 MiB of known bytes, linked into the image so the .hbapp itself is ~3 MiB. */
    .section .rodata.bigload_blob, "a"
    .balign 4
    .global bigload_blob
bigload_blob:
    .incbin "build/blob.bin"
    .global bigload_blob_end
bigload_blob_end:
