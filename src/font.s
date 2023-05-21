.section ".rodata", "r"

.global _font_data_start
_font_data_start:
.incbin "../font.ttf"
.font_data_end:

.global _font_size
_font_size:
.quad .font_data_end - _font_data_start
