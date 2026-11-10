#!/bin/bash

keymap -c keymap_drawer.config.yaml parse -c 10 -z ./config/boards/shields/sweep/sweep.keymap >sweep_keymap.yaml

TARGET_FILE="sweep_keymap.yaml"
NEW_FIRST="layout: { dts_layout: sweep_layouts.dtsi }"

# 첫 줄을 제외한 나머지를 읽고, 앞에 NEW_HEADER 붙여서 덮어쓰기
tail -n +2 "$TARGET_FILE" | {
    echo $NEW_FIRST
    cat
} >"$TARGET_FILE.tmp" && mv "$TARGET_FILE.tmp" "$TARGET_FILE"

keymap -c keymap_drawer.config.yaml draw sweep_keymap.yaml >sweep_keymap.svg
