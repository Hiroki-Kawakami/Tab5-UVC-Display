#!/bin/sh
font_offset=$((4 * 1024 * 1024)) # font partition offset
esptool.py --chip esp32p4 write_flash $font_offset EmbeddedJP/EmbeddedJP.ttf
