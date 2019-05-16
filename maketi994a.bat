sdcc --code-size 8192 --opt-code-size ti99kb2_sdcc.c
rem sdcc --code-size 8192 ti99kb2_sdcc.c
rem sdcc --code-size 8192 --no-pack-iram ti99kb2_sdcc.c
packihx ti99kb2_sdcc.ihx > ti994a_2_sdcc.hex
pause prompt