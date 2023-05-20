.section ".rodata", "r"

.global font_data_start
font_data_start:
.incbin "../font.ttf"
.font_data_end:

.global font_size
font_size:
.quad .font_data_end - font_data_start
